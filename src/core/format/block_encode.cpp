#include "core/format/block_encode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/format/pixel_transfer.h"

namespace wmgl {
namespace {

using Block = float[16][4];  // texels row-major (y * 4 + x), RGBA

float clamp01(float v) { return std::isnan(v) ? 0.0f : std::clamp(v, 0.0f, 1.0f); }
int unorm(float v, int max) { return static_cast<int>(std::lround(clamp01(v) * static_cast<float>(max))); }

// ------------------------------------------------------------------ S3TC

uint16_t to565(const float* c) {
    return static_cast<uint16_t>((unorm(c[0], 31) << 11) | (unorm(c[1], 63) << 5) | unorm(c[2], 31));
}
void from565(uint16_t v, float* c) {
    c[0] = static_cast<float>((v >> 11) & 31) / 31.0f;
    c[1] = static_cast<float>((v >> 5) & 63) / 63.0f;
    c[2] = static_cast<float>(v & 31) / 31.0f;
}

// BC1 colour: bounding-box endpoints, nearest palette entry per texel. With
// `transparent`, texels of alpha < 0.5 use the 3-colour mode's index 3.
void encodeBc1(const Block& px, bool transparent, uint8_t* out) {
    float lo[3] = {1, 1, 1}, hi[3] = {0, 0, 0};
    bool anyTransparent = false;
    for (const auto& p : px) {
        if (transparent && p[3] < 0.5f) {
            anyTransparent = true;
            continue;
        }
        for (int c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], clamp01(p[c]));
            hi[c] = std::max(hi[c], clamp01(p[c]));
        }
    }
    if (lo[0] > hi[0]) lo[0] = lo[1] = lo[2] = hi[0] = hi[1] = hi[2] = 0;  // all transparent
    uint16_t q0 = to565(hi), q1 = to565(lo);
    if (anyTransparent ? q0 > q1 : q0 < q1) std::swap(q0, q1);
    float pal[4][3];
    from565(q0, pal[0]);
    from565(q1, pal[1]);
    const bool four = q0 > q1;
    for (int c = 0; c < 3; ++c) {
        pal[2][c] = four ? (2 * pal[0][c] + pal[1][c]) / 3 : (pal[0][c] + pal[1][c]) / 2;
        pal[3][c] = four ? (pal[0][c] + 2 * pal[1][c]) / 3 : 0;
    }
    uint32_t indices = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        if (anyTransparent && px[i][3] < 0.5f) {
            best = 3;
        } else {
            float bestError = 1e30f;
            for (int k = 0; k < (four ? 4 : 3); ++k) {
                float e = 0;
                for (int c = 0; c < 3; ++c) e += (clamp01(px[i][c]) - pal[k][c]) * (clamp01(px[i][c]) - pal[k][c]);
                if (e < bestError) bestError = e, best = k;
            }
        }
        indices |= static_cast<uint32_t>(best) << (2 * i);
    }
    out[0] = static_cast<uint8_t>(q0);
    out[1] = static_cast<uint8_t>(q0 >> 8);
    out[2] = static_cast<uint8_t>(q1);
    out[3] = static_cast<uint8_t>(q1 >> 8);
    std::memcpy(out + 4, &indices, 4);  // little-endian host
}

// BC3 alpha (the BC4 layout): eight interpolated values between max and min.
void encodeBc3Alpha(const Block& px, uint8_t* out) {
    int lo = 255, hi = 0;
    for (const auto& p : px) {
        lo = std::min(lo, unorm(p[3], 255));
        hi = std::max(hi, unorm(p[3], 255));
    }
    out[0] = static_cast<uint8_t>(hi);
    out[1] = static_cast<uint8_t>(lo);
    float pal[8];
    pal[0] = static_cast<float>(hi);
    pal[1] = static_cast<float>(lo);
    for (int k = 1; k < 7; ++k) pal[k + 1] = (static_cast<float>(7 - k) * hi + static_cast<float>(k) * lo) / 7.0f;
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        const float a = static_cast<float>(unorm(px[i][3], 255));
        int best = 0;
        for (int k = 1; k < (hi > lo ? 8 : 1); ++k)
            if (std::fabs(pal[k] - a) < std::fabs(pal[best] - a)) best = k;
        bits |= static_cast<uint64_t>(best) << (3 * i);
    }
    for (int b = 0; b < 6; ++b) out[2 + b] = static_cast<uint8_t>(bits >> (8 * b));
}

// ------------------------------------------------------------------ BPTC

// Little-endian bit stream of one 128-bit block.
struct BitWriter {
    uint8_t* out;
    int at = 0;
    void put(uint32_t value, int bits) {
        for (int i = 0; i < bits; ++i, ++at)
            if ((value >> i) & 1) out[at >> 3] |= static_cast<uint8_t>(1u << (at & 7));
    }
};

constexpr int kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

// The 4-bit index whose weight is nearest t (0..1) along the endpoints.
int nearestWeight(float t) {
    const float w = std::clamp(t, 0.0f, 1.0f) * 64.0f;
    int best = 0;
    for (int k = 1; k < 16; ++k)
        if (std::fabs(static_cast<float>(kWeights4[k]) - w) < std::fabs(static_cast<float>(kWeights4[best]) - w)) best = k;
    return best;
}

// Indices of `n`-channel texel values projected onto e0 -> e1; the anchor
// (texel 0) must have a 0 high bit, else the endpoints swap.
template <int N>
void projectIndices(const float (&values)[16][N], float (&e0)[N], float (&e1)[N], int (&index)[16], bool& swapped) {
    float d[N], len = 0;
    for (int c = 0; c < N; ++c) len += (d[c] = e1[c] - e0[c]) * d[c];
    for (int i = 0; i < 16; ++i) {
        float t = 0;
        if (len > 0)
            for (int c = 0; c < N; ++c) t += (values[i][c] - e0[c]) * d[c];
        index[i] = nearestWeight(len > 0 ? t / len : 0.0f);
    }
    swapped = index[0] >= 8;
    if (swapped)
        for (int& k : index) k = 15 - k;
}

// BC7 mode 6: one subset, RGBA 7-bit endpoints plus a p-bit each, 4-bit indices.
void encodeBc7(const Block& px, uint8_t* out) {
    float values[16][4], e0[4] = {255, 255, 255, 255}, e1[4] = {0, 0, 0, 0};
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 4; ++c) {
            values[i][c] = static_cast<float>(unorm(px[i][c], 255));
            e0[c] = std::min(e0[c], values[i][c]);
            e1[c] = std::max(e1[c], values[i][c]);
        }
    // Endpoint 0 rounds down (p-bit 0), endpoint 1 up (p-bit 1).
    int q0[4], q1[4];
    for (int c = 0; c < 4; ++c) {
        q0[c] = static_cast<int>(e0[c]) >> 1;
        q1[c] = std::min(127, (static_cast<int>(e1[c]) + 1) >> 1);
        if (((q1[c] << 1) | 1) < static_cast<int>(e1[c])) q1[c] = 127;
        e0[c] = static_cast<float>(q0[c] << 1);
        e1[c] = static_cast<float>((q1[c] << 1) | 1);
    }
    int index[16];
    bool swapped;
    projectIndices<4>(values, e0, e1, index, swapped);
    int p0 = 0, p1 = 1;
    if (swapped) {
        std::swap(q0, q1);
        std::swap(p0, p1);
    }
    std::memset(out, 0, 16);
    BitWriter w{out};
    w.put(1u << 6, 7);  // mode 6
    for (int c = 0; c < 4; ++c) {
        w.put(static_cast<uint32_t>(q0[c]), 7);
        w.put(static_cast<uint32_t>(q1[c]), 7);
    }
    w.put(static_cast<uint32_t>(p0), 1);
    w.put(static_cast<uint32_t>(p1), 1);
    for (int i = 0; i < 16; ++i) w.put(static_cast<uint32_t>(index[i]), i == 0 ? 3 : 4);
}

// BC6H's half-float bit patterns as integers (sign-magnitude to signed for
// the signed format; negatives clamp to 0 for the unsigned one).
int halfInteger(float v, bool isSigned) {
    if (std::isnan(v)) v = 0;
    if (!isSigned) v = std::max(v, 0.0f);
    const uint16_t h = floatToHalf(std::clamp(v, -65504.0f, 65504.0f));
    const int magnitude = std::min(h & 0x7fff, 0x7bff);
    return (h & 0x8000) ? -magnitude : magnitude;
}

// BC6H mode 11: one region, 10-bit endpoints, 4-bit indices. Decoding
// maps an endpoint x to about 31x + 15 (unsigned) or 62x + 31 (signed) in
// half-float bits, so encoding inverts that.
void encodeBc6h(const Block& px, bool isSigned, uint8_t* out) {
    float values[16][3], lo[3], hi[3];
    for (int c = 0; c < 3; ++c) lo[c] = 1e9f, hi[c] = -1e9f;
    for (int i = 0; i < 16; ++i)
        for (int c = 0; c < 3; ++c) {
            values[i][c] = static_cast<float>(halfInteger(px[i][c], isSigned));
            lo[c] = std::min(lo[c], values[i][c]);
            hi[c] = std::max(hi[c], values[i][c]);
        }
    const float scale = isSigned ? 62.0f : 31.0f, bias = isSigned ? 31.0f : 15.0f;
    const int minX = isSigned ? -511 : 0, maxX = isSigned ? 511 : 1023;
    int q0[3], q1[3];
    float e0[3], e1[3];
    auto decoded = [&](int x) { return x == 0 ? 0.0f : static_cast<float>(x) * scale + (x < 0 ? -bias : bias); };
    for (int c = 0; c < 3; ++c) {
        q0[c] = std::clamp(static_cast<int>(std::floor((lo[c] - (lo[c] < 0 ? -bias : bias)) / scale)), minX, maxX);
        q1[c] = std::clamp(static_cast<int>(std::ceil((hi[c] - (hi[c] < 0 ? -bias : bias)) / scale)), minX, maxX);
        e0[c] = decoded(q0[c]);
        e1[c] = decoded(q1[c]);
    }
    int index[16];
    bool swapped;
    projectIndices<3>(values, e0, e1, index, swapped);
    if (swapped) std::swap(q0, q1);
    std::memset(out, 0, 16);
    BitWriter w{out};
    w.put(0x03, 5);  // mode 11
    for (const int* q : {q0, q1})
        for (int c = 0; c < 3; ++c) w.put(static_cast<uint32_t>(q[c]) & 0x3ff, 10);
    for (int i = 0; i < 16; ++i) w.put(static_cast<uint32_t>(index[i]), i == 0 ? 3 : 4);
}

// -------------------------------------------------------------- ETC2/EAC

void putBigEndian(uint64_t v, uint8_t* out) {
    for (int b = 0; b < 8; ++b) out[b] = static_cast<uint8_t>(v >> (56 - 8 * b));
}

constexpr int kEtcModifiers[8][2] = {{2, 8}, {5, 17}, {9, 29}, {13, 42}, {18, 60}, {24, 80}, {33, 106}, {47, 183}};

// ETC2 RGB in differential mode with one base colour for both sub-blocks
// (zero deltas, so no T/H/planar mode is triggered). With `punchthrough`
// (RGB8A1) and a texel of alpha < 0.5, the opaque bit is 0: index 2 is
// transparent and index 0 the unmodified base colour.
void encodeEtc2Rgb(const Block& px, bool punchthrough, uint8_t* out) {
    bool anyTransparent = false;
    float sum[3] = {0, 0, 0};
    int opaque = 0;
    for (const auto& p : px) {
        if (punchthrough && p[3] < 0.5f) {
            anyTransparent = true;
            continue;
        }
        ++opaque;
        for (int c = 0; c < 3; ++c) sum[c] += clamp01(p[c]);
    }
    int base5[3], base8[3];
    for (int c = 0; c < 3; ++c) {
        base5[c] = opaque ? unorm(sum[c] / static_cast<float>(opaque), 31) : 0;
        base8[c] = (base5[c] << 3) | (base5[c] >> 2);
    }
    // Pixel index (msb, lsb): opaque mode +a, +b, -a, -b; punch-through mode
    // base, +b, transparent, -b.
    auto modifier = [&](int table, int index) {
        const int a = kEtcModifiers[table][0], b = kEtcModifiers[table][1];
        if (anyTransparent) return index == 0 ? 0 : index == 1 ? b : -b;
        return index == 0 ? a : index == 1 ? b : index == 2 ? -a : -b;
    };
    int bestTable = 0;
    int bestIndex[16] = {};
    long bestError = -1;
    for (int table = 0; table < 8; ++table) {
        long error = 0;
        int index[16];
        for (int i = 0; i < 16; ++i) {
            index[i] = 2;
            if (anyTransparent && px[i][3] < 0.5f) continue;
            long best = -1;
            for (int k = 0; k < 4; ++k) {
                if (anyTransparent && k == 2) continue;
                long e = 0;
                for (int c = 0; c < 3; ++c) {
                    const int v = std::clamp(base8[c] + modifier(table, k), 0, 255);
                    const int d = v - unorm(px[i][c], 255);
                    e += d * d;
                }
                if (best < 0 || e < best) best = e, index[i] = k;
            }
            error += best;
        }
        if (bestError < 0 || error < bestError) {
            bestError = error;
            bestTable = table;
            std::memcpy(bestIndex, index, sizeof(index));
        }
    }
    uint64_t v = 0;
    v |= static_cast<uint64_t>(base5[0]) << 59;
    v |= static_cast<uint64_t>(base5[1]) << 51;
    v |= static_cast<uint64_t>(base5[2]) << 43;  // deltas 0
    v |= static_cast<uint64_t>(bestTable) << 37;
    v |= static_cast<uint64_t>(bestTable) << 34;
    v |= static_cast<uint64_t>(anyTransparent ? 0 : 1) << 33;  // differential / opaque bit
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            const int p = x * 4 + y, k = bestIndex[y * 4 + x];
            v |= static_cast<uint64_t>((k >> 1) & 1) << (16 + p);
            v |= static_cast<uint64_t>(k & 1) << p;
        }
    putBigEndian(v, out);
}

constexpr int kEacModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12}, {-2, -5, -8, -13, 1, 4, 7, 12},
    {-2, -4, -6, -13, 1, 3, 5, 12}, {-3, -6, -8, -12, 2, 5, 7, 11},  {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},  {-2, -6, -8, -10, 1, 5, 7, 9},
    {-2, -5, -8, -10, 1, 4, 7, 9},  {-2, -4, -8, -10, 1, 3, 7, 9},   {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},   {-4, -6, -8, -9, 3, 5, 7, 8},
    {-3, -5, -7, -9, 2, 4, 6, 8}};

// One EAC block over `targets` (texels row-major). kind 0: 8-bit alpha;
// 1: unsigned R11; 2: signed R11 (targets in the decoded integer range).
void encodeEac(const int (&targets)[16], int kind, uint8_t* out) {
    int lo = targets[0], hi = targets[0];
    for (int t : targets) lo = std::min(lo, t), hi = std::max(hi, t);
    // Decoded value of base codeword `base` with modifier `m` and multiplier `mult`.
    auto decode = [&](int base, int m, int mult) {
        if (kind == 0) return std::clamp(base + m * mult, 0, 255);
        if (kind == 1) return std::clamp(base * 8 + 4 + (mult ? m * mult * 8 : m), 0, 2047);
        return std::clamp(base * 8 + (mult ? m * mult * 8 : m), -1023, 1023);
    };
    const int mid = (lo + hi) / 2;
    const int base = kind == 0 ? mid : kind == 1 ? std::clamp((mid - 4) / 8, 0, 255) : std::clamp(mid / 8, -127, 127);
    int bestTable = 0, bestMult = 1, bestIndex[16] = {};
    long bestError = -1;
    for (int table = 0; table < 16; ++table)
        for (int mult = kind == 0 ? 1 : 0; mult < 16; ++mult) {
            long error = 0;
            int index[16];
            for (int i = 0; i < 16; ++i) {
                long best = -1;
                for (int k = 0; k < 8; ++k) {
                    const long d = decode(base, kEacModifiers[table][k], mult) - targets[i];
                    if (best < 0 || d * d < best) best = d * d, index[i] = k;
                }
                error += best;
            }
            if (bestError < 0 || error < bestError) {
                bestError = error;
                bestTable = table;
                bestMult = mult;
                std::memcpy(bestIndex, index, sizeof(index));
            }
        }
    uint64_t v = static_cast<uint64_t>(static_cast<uint8_t>(base)) << 56;
    v |= static_cast<uint64_t>(bestMult) << 52;
    v |= static_cast<uint64_t>(bestTable) << 48;
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) v |= static_cast<uint64_t>(bestIndex[y * 4 + x]) << (45 - 3 * (x * 4 + y));
    putBigEndian(v, out);
}

void encodeEacChannel(const Block& px, int channel, bool isSigned, uint8_t* out) {
    int targets[16];
    for (int i = 0; i < 16; ++i) {
        const float v = std::isnan(px[i][channel]) ? 0.0f : px[i][channel];
        targets[i] = isSigned ? static_cast<int>(std::lround(std::clamp(v, -1.0f, 1.0f) * 1023.0f)) : unorm(v, 2047);
    }
    encodeEac(targets, isSigned ? 2 : 1, out);
}

}  // namespace

bool blockEncodable(Storage s) {
    return (s >= Storage::BC1_RGBA && s <= Storage::BC3_RGBA_sRGB) ||
           (s >= Storage::BC6H_RGBFloat && s <= Storage::EAC_RG11Snorm);
}

void encodeBlocks(Storage s, const float* rgba, int width, int height, int depth, uint8_t* blocks) {
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    for (int z = 0; z < depth; ++z)
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx) {
                Block px;
                for (int y = 0; y < 4; ++y)
                    for (int x = 0; x < 4; ++x) {
                        const int sx = std::min(bx * 4 + x, width - 1), sy = std::min(by * 4 + y, height - 1);
                        std::memcpy(px[y * 4 + x], rgba + ((static_cast<size_t>(z) * height + sy) * width + sx) * 4, 16);
                    }
                switch (s) {
                case Storage::BC1_RGBA: case Storage::BC1_RGBA_sRGB: encodeBc1(px, true, blocks); blocks += 8; break;
                case Storage::BC2_RGBA: case Storage::BC2_RGBA_sRGB: {
                    uint64_t alpha = 0;
                    for (int i = 0; i < 16; ++i) alpha |= static_cast<uint64_t>(unorm(px[i][3], 15)) << (4 * i);
                    std::memcpy(blocks, &alpha, 8);
                    encodeBc1(px, false, blocks + 8);
                    blocks += 16;
                    break;
                }
                case Storage::BC3_RGBA: case Storage::BC3_RGBA_sRGB:
                    encodeBc3Alpha(px, blocks);
                    encodeBc1(px, false, blocks + 8);
                    blocks += 16;
                    break;
                case Storage::BC6H_RGBFloat: case Storage::BC6H_RGBUfloat:
                    encodeBc6h(px, s == Storage::BC6H_RGBFloat, blocks);
                    blocks += 16;
                    break;
                case Storage::BC7_RGBAUnorm: case Storage::BC7_RGBAUnorm_sRGB: encodeBc7(px, blocks); blocks += 16; break;
                case Storage::ETC2_RGB8: case Storage::ETC2_RGB8_sRGB: encodeEtc2Rgb(px, false, blocks); blocks += 8; break;
                case Storage::ETC2_RGB8A1: case Storage::ETC2_RGB8A1_sRGB: encodeEtc2Rgb(px, true, blocks); blocks += 8; break;
                case Storage::EAC_RGBA8: case Storage::EAC_RGBA8_sRGB: {
                    int alpha[16];
                    for (int i = 0; i < 16; ++i) alpha[i] = unorm(px[i][3], 255);
                    encodeEac(alpha, 0, blocks);
                    encodeEtc2Rgb(px, false, blocks + 8);
                    blocks += 16;
                    break;
                }
                case Storage::EAC_R11Unorm: case Storage::EAC_R11Snorm:
                    encodeEacChannel(px, 0, s == Storage::EAC_R11Snorm, blocks);
                    blocks += 8;
                    break;
                case Storage::EAC_RG11Unorm: case Storage::EAC_RG11Snorm:
                    encodeEacChannel(px, 0, s == Storage::EAC_RG11Snorm, blocks);
                    encodeEacChannel(px, 1, s == Storage::EAC_RG11Snorm, blocks + 8);
                    blocks += 16;
                    break;
                default: return;
                }
            }
}

}  // namespace wmgl
