#include "core/format/pixel_transfer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace wmgl {

// ------------------------------------------------------------ scalars

float halfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign << 31;
        } else {
            // Subnormal: normalise.
            int e = -1;
            uint32_t m = mantissa;
            do { ++e; m <<= 1; } while (!(m & 0x400));
            bits = (sign << 31) | (static_cast<uint32_t>(127 - 15 - e) << 23) | ((m & 0x3ff) << 13);
        }
    } else if (exponent == 31) {
        bits = (sign << 31) | 0x7f800000u | (mantissa << 13);
    } else {
        bits = (sign << 31) | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

uint16_t floatToHalf(float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    const uint32_t sign = (bits >> 16) & 0x8000;
    const int32_t exponent = static_cast<int32_t>((bits >> 23) & 0xff);
    uint32_t mantissa = bits & 0x7fffff;
    if (exponent == 0xff) return static_cast<uint16_t>(sign | 0x7c00 | (mantissa ? 0x200 : 0));
    int32_t e = exponent - 127 + 15;
    if (e >= 31) return static_cast<uint16_t>(sign | 0x7c00);
    if (e <= 0) {
        if (e < -10) return static_cast<uint16_t>(sign);
        mantissa |= 0x800000;
        const uint32_t shift = static_cast<uint32_t>(14 - e);
        uint32_t half = mantissa >> shift;
        const uint32_t rest = mantissa & ((1u << shift) - 1), mid = 1u << (shift - 1);
        if (rest > mid || (rest == mid && (half & 1))) ++half;
        return static_cast<uint16_t>(sign | half);
    }
    uint32_t half = (static_cast<uint32_t>(e) << 10) | (mantissa >> 13);
    const uint32_t rest = mantissa & 0x1fff;
    if (rest > 0x1000 || (rest == 0x1000 && (half & 1))) ++half;
    return static_cast<uint16_t>(sign | half);
}

namespace {

// Unsigned float with `mbits` mantissa and 5-bit exponent (10/11-bit floats).
float smallFloatToFloat(uint32_t v, int mbits) {
    const uint32_t exponent = (v >> mbits) & 0x1f, mantissa = v & ((1u << mbits) - 1);
    if (exponent == 0) return mantissa ? std::ldexp(static_cast<float>(mantissa), -14 - mbits) : 0.0f;
    if (exponent == 31) return mantissa ? NAN : INFINITY;
    return std::ldexp(1.0f + static_cast<float>(mantissa) / static_cast<float>(1u << mbits), static_cast<int>(exponent) - 15);
}

uint32_t floatToSmallFloat(float f, int mbits) {
    if (std::isnan(f)) return (31u << mbits) | 1u;
    if (f <= 0.0f) return 0;
    if (std::isinf(f)) return 31u << mbits;
    int exponent;
    const float frac = std::frexp(f, &exponent);  // f = frac * 2^exponent, frac in [0.5, 1)
    int e = exponent - 1 + 15;
    if (e >= 31) return (30u << mbits) | ((1u << mbits) - 1);  // clamp to max finite
    if (e <= 0) {
        const uint32_t m = static_cast<uint32_t>(std::lround(std::ldexp(f, 14 + mbits)));
        return std::min(m, (1u << mbits) - 1);
    }
    uint32_t m = static_cast<uint32_t>(std::lround((frac * 2.0f - 1.0f) * static_cast<float>(1u << mbits)));
    if (m == (1u << mbits)) {
        m = 0;
        ++e;
        if (e >= 31) return (30u << mbits) | ((1u << mbits) - 1);
    }
    return (static_cast<uint32_t>(e) << mbits) | m;
}

void decodeRGB9E5(uint32_t v, double* c) {
    const int exponent = static_cast<int>(v >> 27) - 15 - 9;
    c[0] = std::ldexp(static_cast<double>(v & 0x1ff), exponent);
    c[1] = std::ldexp(static_cast<double>((v >> 9) & 0x1ff), exponent);
    c[2] = std::ldexp(static_cast<double>((v >> 18) & 0x1ff), exponent);
    c[3] = 1.0;
}

uint32_t encodeRGB9E5(const double* in) {
    const double maxValue = 511.0 / 512.0 * 65536.0;
    double c[3];
    for (int i = 0; i < 3; ++i) c[i] = std::clamp(std::isnan(in[i]) ? 0.0 : in[i], 0.0, maxValue);
    const double maxC = std::max({c[0], c[1], c[2]});
    int expShared = std::max(-16, static_cast<int>(std::floor(std::log2(std::max(maxC, 1e-30))))) + 1 + 15;
    expShared = std::max(0, expShared);
    double denom = std::ldexp(1.0, expShared - 15 - 9);
    if (static_cast<int>(std::floor(maxC / denom + 0.5)) == 512) {
        ++expShared;
        denom *= 2.0;
    }
    uint32_t out = static_cast<uint32_t>(expShared) << 27;
    for (int i = 0; i < 3; ++i) out |= (static_cast<uint32_t>(std::floor(c[i] / denom + 0.5)) & 0x1ff) << (9 * i);
    return out;
}

double unormToDouble(uint64_t v, int bits) { return static_cast<double>(v) / static_cast<double>((1ull << bits) - 1); }

uint64_t doubleToUnorm(double f, int bits) {
    const double maxV = static_cast<double>((1ull << bits) - 1);
    if (std::isnan(f)) return 0;
    return static_cast<uint64_t>(std::llround(std::clamp(f, 0.0, 1.0) * maxV));
}

double snormToDouble(int64_t v, int bits) {
    return std::max(static_cast<double>(v) / static_cast<double>((1ll << (bits - 1)) - 1), -1.0);
}

int64_t doubleToSnorm(double f, int bits) {
    const double maxV = static_cast<double>((1ll << (bits - 1)) - 1);
    if (std::isnan(f)) return 0;
    return std::llround(std::clamp(f, -1.0, 1.0) * maxV);
}

template <class T>
T clampInt(double v) {
    if (std::isnan(v)) return 0;
    const double lo = static_cast<double>(std::numeric_limits<T>::min()), hi = static_cast<double>(std::numeric_limits<T>::max());
    return static_cast<T>(std::clamp(v, lo, hi));
}

// --------------------------------------------------- client formats

int formatComponents(GLenum format) {
    switch (format) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_ALPHA: case GL_RED_INTEGER: case GL_GREEN_INTEGER:
    case GL_BLUE_INTEGER: case GL_ALPHA_INTEGER: case GL_LUMINANCE: case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX:
    case GL_INTENSITY:
        return 1;
    case GL_RG: case GL_RG_INTEGER: case GL_LUMINANCE_ALPHA: case GL_DEPTH_STENCIL: return 2;
    case GL_RGB: case GL_BGR: case GL_RGB_INTEGER: case GL_BGR_INTEGER: return 3;
    case GL_RGBA: case GL_BGRA: case GL_RGBA_INTEGER: case GL_BGRA_INTEGER: return 4;
    default: return 0;
    }
}

bool isIntegerFormat(GLenum format) {
    switch (format) {
    case GL_RED_INTEGER: case GL_GREEN_INTEGER: case GL_BLUE_INTEGER: case GL_ALPHA_INTEGER: case GL_RG_INTEGER:
    case GL_RGB_INTEGER: case GL_BGR_INTEGER: case GL_RGBA_INTEGER: case GL_BGRA_INTEGER: return true;
    default: return false;
    }
}

// Which RGBA channel (0-3) each client component maps to; -1 for luminance-style.
void componentChannels(GLenum format, int* channel) {
    switch (format) {
    case GL_RED: case GL_RED_INTEGER: case GL_LUMINANCE: case GL_INTENSITY: channel[0] = 0; return;
    case GL_GREEN: case GL_GREEN_INTEGER: channel[0] = 1; return;
    case GL_BLUE: case GL_BLUE_INTEGER: channel[0] = 2; return;
    case GL_ALPHA: case GL_ALPHA_INTEGER: channel[0] = 3; return;
    case GL_RG: case GL_RG_INTEGER: channel[0] = 0; channel[1] = 1; return;
    case GL_LUMINANCE_ALPHA: channel[0] = 0; channel[1] = 3; return;
    case GL_RGB: case GL_RGB_INTEGER: channel[0] = 0; channel[1] = 1; channel[2] = 2; return;
    case GL_BGR: case GL_BGR_INTEGER: channel[0] = 2; channel[1] = 1; channel[2] = 0; return;
    case GL_RGBA: case GL_RGBA_INTEGER: channel[0] = 0; channel[1] = 1; channel[2] = 2; channel[3] = 3; return;
    case GL_BGRA: case GL_BGRA_INTEGER: channel[0] = 2; channel[1] = 1; channel[2] = 0; channel[3] = 3; return;
    default: channel[0] = 0; return;
    }
}

size_t typeBytes(GLenum type) {
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: return 1;
    case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_HALF_FLOAT: case GL_UNSIGNED_SHORT_5_6_5:
    case GL_UNSIGNED_SHORT_5_6_5_REV: case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV:
    case GL_UNSIGNED_SHORT_5_5_5_1: case GL_UNSIGNED_SHORT_1_5_5_5_REV: return 2;
    case GL_FLOAT_32_UNSIGNED_INT_24_8_REV: return 8;
    default: return 4;
    }
}

struct PackedLayout {
    int count = 0;
    int bits[4] = {};
    bool rev = false;
};

bool packedLayout(GLenum type, PackedLayout& p) {
    auto set = [&](std::initializer_list<int> bits, bool rev) {
        p.count = static_cast<int>(bits.size());
        int i = 0;
        for (int b : bits) p.bits[i++] = b;
        p.rev = rev;
        return true;
    };
    switch (type) {
    case GL_UNSIGNED_BYTE_3_3_2: return set({3, 3, 2}, false);
    case GL_UNSIGNED_BYTE_2_3_3_REV: return set({3, 3, 2}, true);
    case GL_UNSIGNED_SHORT_5_6_5: return set({5, 6, 5}, false);
    case GL_UNSIGNED_SHORT_5_6_5_REV: return set({5, 6, 5}, true);
    case GL_UNSIGNED_SHORT_4_4_4_4: return set({4, 4, 4, 4}, false);
    case GL_UNSIGNED_SHORT_4_4_4_4_REV: return set({4, 4, 4, 4}, true);
    case GL_UNSIGNED_SHORT_5_5_5_1: return set({5, 5, 5, 1}, false);
    case GL_UNSIGNED_SHORT_1_5_5_5_REV: return set({5, 5, 5, 1}, true);
    case GL_UNSIGNED_INT_8_8_8_8: return set({8, 8, 8, 8}, false);
    case GL_UNSIGNED_INT_8_8_8_8_REV: return set({8, 8, 8, 8}, true);
    case GL_UNSIGNED_INT_10_10_10_2: return set({10, 10, 10, 2}, false);
    case GL_UNSIGNED_INT_2_10_10_10_REV: return set({10, 10, 10, 2}, true);
    default: return false;
    }
}

uint64_t readWord(const uint8_t* p, size_t bytes, bool swap) {
    uint8_t tmp[8];
    std::memcpy(tmp, p, bytes);
    if (swap) std::reverse(tmp, tmp + bytes);
    uint64_t v = 0;
    std::memcpy(&v, tmp, bytes);  // little-endian host
    return v;
}

void writeWord(uint8_t* p, uint64_t v, size_t bytes, bool swap) {
    uint8_t tmp[8];
    std::memcpy(tmp, &v, 8);
    if (swap) std::reverse(tmp, tmp + bytes);
    std::memcpy(p, tmp, bytes);
}

// Reads one group of client data into RGBA (normalizing unless integer).
void readGroup(const uint8_t* p, GLenum format, GLenum type, bool integerDest, bool swap, Texel& t) {
    t = Texel();
    if (format == GL_DEPTH_COMPONENT) {
        switch (type) {
        case GL_UNSIGNED_BYTE: t.depth = unormToDouble(p[0], 8); break;
        case GL_UNSIGNED_SHORT: t.depth = unormToDouble(readWord(p, 2, swap), 16); break;
        case GL_UNSIGNED_INT: t.depth = unormToDouble(readWord(p, 4, swap), 32); break;
        case GL_BYTE: t.depth = snormToDouble(static_cast<int8_t>(p[0]), 8); break;
        case GL_SHORT: t.depth = snormToDouble(static_cast<int16_t>(readWord(p, 2, swap)), 16); break;
        case GL_INT: t.depth = snormToDouble(static_cast<int32_t>(readWord(p, 4, swap)), 32); break;
        case GL_HALF_FLOAT: t.depth = halfToFloat(static_cast<uint16_t>(readWord(p, 2, swap))); break;
        case GL_FLOAT: { const uint32_t v = static_cast<uint32_t>(readWord(p, 4, swap)); float f; std::memcpy(&f, &v, 4); t.depth = f; break; }
        default: break;
        }
        return;
    }
    if (format == GL_STENCIL_INDEX) {
        switch (type) {
        case GL_UNSIGNED_BYTE: case GL_BYTE: t.stencil = p[0]; break;
        case GL_UNSIGNED_SHORT: case GL_SHORT: t.stencil = static_cast<uint32_t>(readWord(p, 2, swap)); break;
        case GL_FLOAT: { const uint32_t v = static_cast<uint32_t>(readWord(p, 4, swap)); float f; std::memcpy(&f, &v, 4); t.stencil = static_cast<uint32_t>(f); break; }
        case GL_HALF_FLOAT: t.stencil = static_cast<uint32_t>(halfToFloat(static_cast<uint16_t>(readWord(p, 2, swap)))); break;
        default: t.stencil = static_cast<uint32_t>(readWord(p, 4, swap)); break;
        }
        return;
    }
    if (format == GL_DEPTH_STENCIL) {
        if (type == GL_UNSIGNED_INT_24_8) {
            const uint32_t v = static_cast<uint32_t>(readWord(p, 4, swap));
            t.depth = unormToDouble(v >> 8, 24);
            t.stencil = v & 0xff;
        } else {  // FLOAT_32_UNSIGNED_INT_24_8_REV
            const uint32_t d = static_cast<uint32_t>(readWord(p, 4, swap));
            float f;
            std::memcpy(&f, &d, 4);
            t.depth = f;
            t.stencil = static_cast<uint32_t>(readWord(p + 4, 4, swap)) & 0xff;
        }
        return;
    }
    int channel[4] = {0, 1, 2, 3};
    componentChannels(format, channel);
    const int n = formatComponents(format);
    if (format == GL_ALPHA || format == GL_ALPHA_INTEGER) t.c[3] = 0;  // filled below
    if (type == GL_UNSIGNED_INT_10F_11F_11F_REV) {
        const uint32_t v = static_cast<uint32_t>(readWord(p, 4, swap));
        t.c[0] = smallFloatToFloat(v & 0x7ff, 6);
        t.c[1] = smallFloatToFloat((v >> 11) & 0x7ff, 6);
        t.c[2] = smallFloatToFloat((v >> 22) & 0x3ff, 5);
        return;
    }
    if (type == GL_UNSIGNED_INT_5_9_9_9_REV) {
        decodeRGB9E5(static_cast<uint32_t>(readWord(p, 4, swap)), t.c);
        return;
    }
    PackedLayout packed;
    if (packedLayout(type, packed)) {
        const uint64_t word = readWord(p, typeBytes(type), swap);
        const int totalBits = static_cast<int>(typeBytes(type)) * 8;
        int shift = packed.rev ? 0 : totalBits;
        for (int i = 0; i < packed.count && i < 4; ++i) {
            const int bits = packed.bits[i];
            uint64_t v;
            if (packed.rev) { v = (word >> shift) & ((1ull << bits) - 1); shift += bits; }
            else { shift -= bits; v = (word >> shift) & ((1ull << bits) - 1); }
            t.c[channel[i]] = integerDest ? static_cast<double>(v) : unormToDouble(v, bits);
        }
        return;
    }
    double values[4] = {0, 0, 0, 0};
    const size_t s = typeBytes(type);
    for (int i = 0; i < n; ++i) {
        const uint8_t* q = p + i * s;
        double v = 0;
        switch (type) {
        case GL_UNSIGNED_BYTE: v = integerDest ? q[0] : unormToDouble(q[0], 8); break;
        case GL_BYTE: v = integerDest ? static_cast<int8_t>(q[0]) : snormToDouble(static_cast<int8_t>(q[0]), 8); break;
        case GL_UNSIGNED_SHORT: { const uint64_t x = readWord(q, 2, swap); v = integerDest ? static_cast<double>(x) : unormToDouble(x, 16); break; }
        case GL_SHORT: { const int16_t x = static_cast<int16_t>(readWord(q, 2, swap)); v = integerDest ? x : snormToDouble(x, 16); break; }
        case GL_UNSIGNED_INT: { const uint64_t x = readWord(q, 4, swap); v = integerDest ? static_cast<double>(x) : unormToDouble(x, 32); break; }
        case GL_INT: { const int32_t x = static_cast<int32_t>(readWord(q, 4, swap)); v = integerDest ? x : snormToDouble(x, 32); break; }
        case GL_HALF_FLOAT: v = halfToFloat(static_cast<uint16_t>(readWord(q, 2, swap))); break;
        case GL_FLOAT: { const uint32_t x = static_cast<uint32_t>(readWord(q, 4, swap)); float f; std::memcpy(&f, &x, 4); v = f; break; }
        default: break;
        }
        values[i] = v;
    }
    for (int i = 0; i < n; ++i) t.c[channel[i]] = values[i];
}

void writeGroup(uint8_t* p, GLenum format, GLenum type, bool integerSource, bool swap, const Texel& t) {
    if (format == GL_DEPTH_COMPONENT) {
        const double d = t.depth;
        switch (type) {
        case GL_UNSIGNED_BYTE: p[0] = static_cast<uint8_t>(doubleToUnorm(d, 8)); break;
        case GL_UNSIGNED_SHORT: writeWord(p, doubleToUnorm(d, 16), 2, swap); break;
        case GL_UNSIGNED_INT: writeWord(p, doubleToUnorm(d, 32), 4, swap); break;
        case GL_BYTE: p[0] = static_cast<uint8_t>(doubleToSnorm(d, 8)); break;
        case GL_SHORT: writeWord(p, static_cast<uint16_t>(doubleToSnorm(d, 16)), 2, swap); break;
        case GL_INT: writeWord(p, static_cast<uint32_t>(doubleToSnorm(d, 32)), 4, swap); break;
        case GL_HALF_FLOAT: writeWord(p, floatToHalf(static_cast<float>(d)), 2, swap); break;
        case GL_FLOAT: { const float f = static_cast<float>(d); uint32_t x; std::memcpy(&x, &f, 4); writeWord(p, x, 4, swap); break; }
        default: break;
        }
        return;
    }
    if (format == GL_STENCIL_INDEX) {
        switch (type) {
        case GL_UNSIGNED_BYTE: case GL_BYTE: p[0] = static_cast<uint8_t>(t.stencil); break;
        case GL_UNSIGNED_SHORT: case GL_SHORT: writeWord(p, t.stencil, 2, swap); break;
        case GL_FLOAT: { const float f = static_cast<float>(t.stencil); uint32_t x; std::memcpy(&x, &f, 4); writeWord(p, x, 4, swap); break; }
        case GL_HALF_FLOAT: writeWord(p, floatToHalf(static_cast<float>(t.stencil)), 2, swap); break;
        default: writeWord(p, t.stencil, 4, swap); break;
        }
        return;
    }
    if (format == GL_DEPTH_STENCIL) {
        if (type == GL_UNSIGNED_INT_24_8) {
            writeWord(p, (doubleToUnorm(t.depth, 24) << 8) | (t.stencil & 0xff), 4, swap);
        } else {
            const float f = static_cast<float>(t.depth);
            uint32_t x;
            std::memcpy(&x, &f, 4);
            writeWord(p, x, 4, swap);
            writeWord(p + 4, t.stencil & 0xff, 4, swap);
        }
        return;
    }
    int channel[4] = {0, 1, 2, 3};
    componentChannels(format, channel);
    const int n = formatComponents(format);
    if (type == GL_UNSIGNED_INT_10F_11F_11F_REV) {
        const uint32_t v = floatToSmallFloat(static_cast<float>(t.c[0]), 6) | (floatToSmallFloat(static_cast<float>(t.c[1]), 6) << 11) |
                           (floatToSmallFloat(static_cast<float>(t.c[2]), 5) << 22);
        writeWord(p, v, 4, swap);
        return;
    }
    if (type == GL_UNSIGNED_INT_5_9_9_9_REV) {
        writeWord(p, encodeRGB9E5(t.c), 4, swap);
        return;
    }
    PackedLayout packed;
    if (packedLayout(type, packed)) {
        const int totalBits = static_cast<int>(typeBytes(type)) * 8;
        uint64_t word = 0;
        int shift = packed.rev ? 0 : totalBits;
        for (int i = 0; i < packed.count && i < 4; ++i) {
            const int bits = packed.bits[i];
            const double c = t.c[channel[i]];
            const uint64_t v = integerSource ? std::min<uint64_t>(static_cast<uint64_t>(std::max(0.0, c)), (1ull << bits) - 1)
                                             : doubleToUnorm(c, bits);
            if (packed.rev) { word |= v << shift; shift += bits; }
            else { shift -= bits; word |= v << shift; }
        }
        writeWord(p, word, typeBytes(type), swap);
        return;
    }
    const size_t s = typeBytes(type);
    for (int i = 0; i < n; ++i) {
        uint8_t* q = p + i * s;
        const double c = t.c[channel[i]];
        switch (type) {
        case GL_UNSIGNED_BYTE: q[0] = integerSource ? clampInt<uint8_t>(c) : static_cast<uint8_t>(doubleToUnorm(c, 8)); break;
        case GL_BYTE: q[0] = static_cast<uint8_t>(integerSource ? clampInt<int8_t>(c) : static_cast<int8_t>(doubleToSnorm(c, 8))); break;
        case GL_UNSIGNED_SHORT: writeWord(q, integerSource ? clampInt<uint16_t>(c) : doubleToUnorm(c, 16), 2, swap); break;
        case GL_SHORT: writeWord(q, static_cast<uint16_t>(integerSource ? clampInt<int16_t>(c) : static_cast<int16_t>(doubleToSnorm(c, 16))), 2, swap); break;
        case GL_UNSIGNED_INT: writeWord(q, integerSource ? clampInt<uint32_t>(c) : doubleToUnorm(c, 32), 4, swap); break;
        case GL_INT: writeWord(q, static_cast<uint32_t>(integerSource ? clampInt<int32_t>(c) : static_cast<int32_t>(doubleToSnorm(c, 32))), 4, swap); break;
        case GL_HALF_FLOAT: writeWord(q, floatToHalf(static_cast<float>(c)), 2, swap); break;
        case GL_FLOAT: { const float f = static_cast<float>(c); uint32_t x; std::memcpy(&x, &f, 4); writeWord(q, x, 4, swap); break; }
        default: break;
        }
    }
}

// Maps GL RGBA semantics to stored channels (and back) for legacy formats.
void rgbaToStored(const FormatInfo& info, Texel& t) {
    switch (info.baseFormat) {
    case GL_ALPHA: t.c[0] = t.c[3]; break;
    case GL_LUMINANCE_ALPHA: t.c[1] = t.c[3]; break;
    default: break;
    }
    if (info.storedAlphaIsOne) t.c[3] = info.isInteger() ? 1.0 : 1.0;
}

void storedToRgba(const FormatInfo& info, Texel& t) {
    switch (info.baseFormat) {
    case GL_RED: t.c[1] = 0; t.c[2] = 0; t.c[3] = 1; break;
    case GL_RG: t.c[2] = 0; t.c[3] = 1; break;
    case GL_RGB: t.c[3] = 1; break;
    case GL_ALPHA: t.c[3] = t.c[0]; t.c[0] = t.c[1] = t.c[2] = 0; break;
    case GL_LUMINANCE: case GL_INTENSITY: t.c[1] = t.c[2] = 0; t.c[3] = 1; break;
    case GL_LUMINANCE_ALPHA: t.c[3] = t.c[1]; t.c[1] = t.c[2] = 0; break;
    default: break;
    }
}

}  // namespace

void mapRgbaToStored(const FormatInfo& info, Texel& t) { rgbaToStored(info, t); }

namespace {

template <void (*Map)(const FormatInfo&, Texel&)>
void mapTexels(const FormatInfo& info, float* rgba, size_t texels) {
    for (size_t i = 0; i < texels; ++i) {
        Texel t;
        for (int c = 0; c < 4; ++c) t.c[c] = rgba[i * 4 + c];
        Map(info, t);
        for (int c = 0; c < 4; ++c) rgba[i * 4 + c] = static_cast<float>(t.c[c]);
    }
}

}  // namespace

void mapRgbaToStored(const FormatInfo& info, float* rgba, size_t texels) { mapTexels<rgbaToStored>(info, rgba, texels); }
void mapStoredToRgba(const FormatInfo& info, float* rgba, size_t texels) { mapTexels<storedToRgba>(info, rgba, texels); }

// ------------------------------------------------------------- storage

void decodeStorage(Storage s, const uint8_t* p, Texel& t) {
    t = Texel();
    auto u8 = [&](int i) { return static_cast<double>(p[i]); };
    auto s8 = [&](int i) { return static_cast<double>(static_cast<int8_t>(p[i])); };
    auto u16 = [&](int i) { uint16_t v; std::memcpy(&v, p + 2 * i, 2); return v; };
    auto s16 = [&](int i) { int16_t v; std::memcpy(&v, p + 2 * i, 2); return v; };
    auto u32 = [&](int i) { uint32_t v; std::memcpy(&v, p + 4 * i, 4); return v; };
    auto s32 = [&](int i) { int32_t v; std::memcpy(&v, p + 4 * i, 4); return v; };
    auto f32 = [&](int i) { float v; std::memcpy(&v, p + 4 * i, 4); return static_cast<double>(v); };
    switch (s) {
    case Storage::R8Unorm: case Storage::R8Unorm_sRGB: t.c[0] = u8(0) / 255.0; break;
    case Storage::R8Snorm: t.c[0] = snormToDouble(static_cast<int8_t>(p[0]), 8); break;
    case Storage::R8Uint: t.c[0] = u8(0); break;
    case Storage::R8Sint: t.c[0] = s8(0); break;
    case Storage::R16Unorm: t.c[0] = unormToDouble(u16(0), 16); break;
    case Storage::R16Snorm: t.c[0] = snormToDouble(s16(0), 16); break;
    case Storage::R16Uint: t.c[0] = u16(0); break;
    case Storage::R16Sint: t.c[0] = s16(0); break;
    case Storage::R16Float: t.c[0] = halfToFloat(u16(0)); break;
    case Storage::RG8Unorm: case Storage::RG8Unorm_sRGB: t.c[0] = u8(0) / 255.0; t.c[1] = u8(1) / 255.0; break;
    case Storage::RG8Snorm: t.c[0] = snormToDouble(static_cast<int8_t>(p[0]), 8); t.c[1] = snormToDouble(static_cast<int8_t>(p[1]), 8); break;
    case Storage::RG8Uint: t.c[0] = u8(0); t.c[1] = u8(1); break;
    case Storage::RG8Sint: t.c[0] = s8(0); t.c[1] = s8(1); break;
    case Storage::R32Uint: t.c[0] = u32(0); break;
    case Storage::R32Sint: t.c[0] = s32(0); break;
    case Storage::R32Float: t.c[0] = f32(0); break;
    case Storage::RG16Unorm: t.c[0] = unormToDouble(u16(0), 16); t.c[1] = unormToDouble(u16(1), 16); break;
    case Storage::RG16Snorm: t.c[0] = snormToDouble(s16(0), 16); t.c[1] = snormToDouble(s16(1), 16); break;
    case Storage::RG16Uint: t.c[0] = u16(0); t.c[1] = u16(1); break;
    case Storage::RG16Sint: t.c[0] = s16(0); t.c[1] = s16(1); break;
    case Storage::RG16Float: t.c[0] = halfToFloat(u16(0)); t.c[1] = halfToFloat(u16(1)); break;
    case Storage::RGBA8Unorm: case Storage::RGBA8Unorm_sRGB:
        for (int i = 0; i < 4; ++i) t.c[i] = u8(i) / 255.0;
        break;
    case Storage::BGRA8Unorm: t.c[0] = u8(2) / 255.0; t.c[1] = u8(1) / 255.0; t.c[2] = u8(0) / 255.0; t.c[3] = u8(3) / 255.0; break;
    case Storage::RGBA8Snorm: for (int i = 0; i < 4; ++i) t.c[i] = snormToDouble(static_cast<int8_t>(p[i]), 8); break;
    case Storage::RGBA8Uint: for (int i = 0; i < 4; ++i) t.c[i] = u8(i); break;
    case Storage::RGBA8Sint: for (int i = 0; i < 4; ++i) t.c[i] = s8(i); break;
    case Storage::RGB10A2Unorm: case Storage::RGB10A2Uint: {
        const uint32_t v = u32(0);
        const bool norm = s == Storage::RGB10A2Unorm;
        const uint32_t c[4] = {v & 0x3ff, (v >> 10) & 0x3ff, (v >> 20) & 0x3ff, v >> 30};
        for (int i = 0; i < 4; ++i) t.c[i] = norm ? unormToDouble(c[i], i == 3 ? 2 : 10) : static_cast<double>(c[i]);
        break;
    }
    case Storage::RG11B10Float: {
        const uint32_t v = u32(0);
        t.c[0] = smallFloatToFloat(v & 0x7ff, 6);
        t.c[1] = smallFloatToFloat((v >> 11) & 0x7ff, 6);
        t.c[2] = smallFloatToFloat((v >> 22) & 0x3ff, 5);
        break;
    }
    case Storage::RGB9E5Float: decodeRGB9E5(u32(0), t.c); break;
    case Storage::RG32Uint: t.c[0] = u32(0); t.c[1] = u32(1); break;
    case Storage::RG32Sint: t.c[0] = s32(0); t.c[1] = s32(1); break;
    case Storage::RG32Float: t.c[0] = f32(0); t.c[1] = f32(1); break;
    case Storage::RGBA16Unorm: for (int i = 0; i < 4; ++i) t.c[i] = unormToDouble(u16(i), 16); break;
    case Storage::RGBA16Snorm: for (int i = 0; i < 4; ++i) t.c[i] = snormToDouble(s16(i), 16); break;
    case Storage::RGBA16Uint: for (int i = 0; i < 4; ++i) t.c[i] = u16(i); break;
    case Storage::RGBA16Sint: for (int i = 0; i < 4; ++i) t.c[i] = s16(i); break;
    case Storage::RGBA16Float: for (int i = 0; i < 4; ++i) t.c[i] = halfToFloat(u16(i)); break;
    case Storage::RGBA32Uint: for (int i = 0; i < 4; ++i) t.c[i] = u32(i); break;
    case Storage::RGBA32Sint: for (int i = 0; i < 4; ++i) t.c[i] = s32(i); break;
    case Storage::RGBA32Float: for (int i = 0; i < 4; ++i) t.c[i] = f32(i); break;
    case Storage::Depth16Unorm: t.depth = unormToDouble(u16(0), 16); break;
    case Storage::Depth32Float: t.depth = f32(0); break;
    case Storage::Stencil8: t.stencil = p[0]; break;
    case Storage::Depth32Float_Stencil8: t.depth = f32(0); t.stencil = p[4]; break;
    default: break;
    }
}

void encodeStorage(Storage s, const Texel& t, uint8_t* p) {
    auto w16 = [&](int i, uint16_t v) { std::memcpy(p + 2 * i, &v, 2); };
    auto w32 = [&](int i, uint32_t v) { std::memcpy(p + 4 * i, &v, 4); };
    auto wf = [&](int i, double v) { const float f = static_cast<float>(v); std::memcpy(p + 4 * i, &f, 4); };
    switch (s) {
    case Storage::R8Unorm: case Storage::R8Unorm_sRGB: p[0] = static_cast<uint8_t>(doubleToUnorm(t.c[0], 8)); break;
    case Storage::R8Snorm: p[0] = static_cast<uint8_t>(doubleToSnorm(t.c[0], 8)); break;
    case Storage::R8Uint: p[0] = clampInt<uint8_t>(t.c[0]); break;
    case Storage::R8Sint: p[0] = static_cast<uint8_t>(clampInt<int8_t>(t.c[0])); break;
    case Storage::R16Unorm: w16(0, static_cast<uint16_t>(doubleToUnorm(t.c[0], 16))); break;
    case Storage::R16Snorm: w16(0, static_cast<uint16_t>(doubleToSnorm(t.c[0], 16))); break;
    case Storage::R16Uint: w16(0, clampInt<uint16_t>(t.c[0])); break;
    case Storage::R16Sint: w16(0, static_cast<uint16_t>(clampInt<int16_t>(t.c[0]))); break;
    case Storage::R16Float: w16(0, floatToHalf(static_cast<float>(t.c[0]))); break;
    case Storage::RG8Unorm: case Storage::RG8Unorm_sRGB: for (int i = 0; i < 2; ++i) p[i] = static_cast<uint8_t>(doubleToUnorm(t.c[i], 8)); break;
    case Storage::RG8Snorm: for (int i = 0; i < 2; ++i) p[i] = static_cast<uint8_t>(doubleToSnorm(t.c[i], 8)); break;
    case Storage::RG8Uint: for (int i = 0; i < 2; ++i) p[i] = clampInt<uint8_t>(t.c[i]); break;
    case Storage::RG8Sint: for (int i = 0; i < 2; ++i) p[i] = static_cast<uint8_t>(clampInt<int8_t>(t.c[i])); break;
    case Storage::R32Uint: w32(0, clampInt<uint32_t>(t.c[0])); break;
    case Storage::R32Sint: w32(0, static_cast<uint32_t>(clampInt<int32_t>(t.c[0]))); break;
    case Storage::R32Float: wf(0, t.c[0]); break;
    case Storage::RG16Unorm: for (int i = 0; i < 2; ++i) w16(i, static_cast<uint16_t>(doubleToUnorm(t.c[i], 16))); break;
    case Storage::RG16Snorm: for (int i = 0; i < 2; ++i) w16(i, static_cast<uint16_t>(doubleToSnorm(t.c[i], 16))); break;
    case Storage::RG16Uint: for (int i = 0; i < 2; ++i) w16(i, clampInt<uint16_t>(t.c[i])); break;
    case Storage::RG16Sint: for (int i = 0; i < 2; ++i) w16(i, static_cast<uint16_t>(clampInt<int16_t>(t.c[i]))); break;
    case Storage::RG16Float: for (int i = 0; i < 2; ++i) w16(i, floatToHalf(static_cast<float>(t.c[i]))); break;
    case Storage::RGBA8Unorm: case Storage::RGBA8Unorm_sRGB:
        for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(doubleToUnorm(t.c[i], 8));
        break;
    case Storage::BGRA8Unorm:
        p[0] = static_cast<uint8_t>(doubleToUnorm(t.c[2], 8));
        p[1] = static_cast<uint8_t>(doubleToUnorm(t.c[1], 8));
        p[2] = static_cast<uint8_t>(doubleToUnorm(t.c[0], 8));
        p[3] = static_cast<uint8_t>(doubleToUnorm(t.c[3], 8));
        break;
    case Storage::RGBA8Snorm: for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(doubleToSnorm(t.c[i], 8)); break;
    case Storage::RGBA8Uint: for (int i = 0; i < 4; ++i) p[i] = clampInt<uint8_t>(t.c[i]); break;
    case Storage::RGBA8Sint: for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(clampInt<int8_t>(t.c[i])); break;
    case Storage::RGB10A2Unorm:
        w32(0, static_cast<uint32_t>(doubleToUnorm(t.c[0], 10) | (doubleToUnorm(t.c[1], 10) << 10) |
                                     (doubleToUnorm(t.c[2], 10) << 20) | (doubleToUnorm(t.c[3], 2) << 30)));
        break;
    case Storage::RGB10A2Uint: {
        auto c = [&](int i, uint32_t maxV) { return std::min<uint32_t>(clampInt<uint32_t>(t.c[i]), maxV); };
        w32(0, c(0, 1023) | (c(1, 1023) << 10) | (c(2, 1023) << 20) | (c(3, 3) << 30));
        break;
    }
    case Storage::RG11B10Float:
        w32(0, floatToSmallFloat(static_cast<float>(t.c[0]), 6) | (floatToSmallFloat(static_cast<float>(t.c[1]), 6) << 11) |
                   (floatToSmallFloat(static_cast<float>(t.c[2]), 5) << 22));
        break;
    case Storage::RGB9E5Float: w32(0, encodeRGB9E5(t.c)); break;
    case Storage::RG32Uint: for (int i = 0; i < 2; ++i) w32(i, clampInt<uint32_t>(t.c[i])); break;
    case Storage::RG32Sint: for (int i = 0; i < 2; ++i) w32(i, static_cast<uint32_t>(clampInt<int32_t>(t.c[i]))); break;
    case Storage::RG32Float: for (int i = 0; i < 2; ++i) wf(i, t.c[i]); break;
    case Storage::RGBA16Unorm: for (int i = 0; i < 4; ++i) w16(i, static_cast<uint16_t>(doubleToUnorm(t.c[i], 16))); break;
    case Storage::RGBA16Snorm: for (int i = 0; i < 4; ++i) w16(i, static_cast<uint16_t>(doubleToSnorm(t.c[i], 16))); break;
    case Storage::RGBA16Uint: for (int i = 0; i < 4; ++i) w16(i, clampInt<uint16_t>(t.c[i])); break;
    case Storage::RGBA16Sint: for (int i = 0; i < 4; ++i) w16(i, static_cast<uint16_t>(clampInt<int16_t>(t.c[i]))); break;
    case Storage::RGBA16Float: for (int i = 0; i < 4; ++i) w16(i, floatToHalf(static_cast<float>(t.c[i]))); break;
    case Storage::RGBA32Uint: for (int i = 0; i < 4; ++i) w32(i, clampInt<uint32_t>(t.c[i])); break;
    case Storage::RGBA32Sint: for (int i = 0; i < 4; ++i) w32(i, static_cast<uint32_t>(clampInt<int32_t>(t.c[i]))); break;
    case Storage::RGBA32Float: for (int i = 0; i < 4; ++i) wf(i, t.c[i]); break;
    case Storage::Depth16Unorm: w16(0, static_cast<uint16_t>(doubleToUnorm(t.depth, 16))); break;
    case Storage::Depth32Float: wf(0, t.depth); break;
    case Storage::Stencil8: p[0] = static_cast<uint8_t>(t.stencil); break;
    case Storage::Depth32Float_Stencil8: wf(0, t.depth); p[4] = static_cast<uint8_t>(t.stencil); p[5] = p[6] = p[7] = 0; break;
    default: break;
    }
}

// ------------------------------------------------------------ layout

size_t groupBytes(GLenum format, GLenum type) {
    if (type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV) return 8;
    PackedLayout packed;
    if (packedLayout(type, packed) || type == GL_UNSIGNED_INT_10F_11F_11F_REV || type == GL_UNSIGNED_INT_5_9_9_9_REV ||
        type == GL_UNSIGNED_INT_24_8)
        return typeBytes(type);
    const int n = formatComponents(format);
    return n ? typeBytes(type) * static_cast<size_t>(n) : 0;
}

TransferLayout transferLayout(GLenum format, GLenum type, int width, int height, int depth, const PixelStoreParams& ps) {
    TransferLayout l;
    l.groupBytes = groupBytes(format, type);
    if (!l.groupBytes) return l;
    // Packed types are one element of their full size; others are n
    // components of sizeof(type) (GL 3.3 section 3.7.4).
    PackedLayout packedInfo;
    const bool packed = packedLayout(type, packedInfo) || type == GL_UNSIGNED_INT_10F_11F_11F_REV ||
                        type == GL_UNSIGNED_INT_5_9_9_9_REV || type == GL_UNSIGNED_INT_24_8 ||
                        type == GL_FLOAT_32_UNSIGNED_INT_24_8_REV;
    const size_t s = packed ? l.groupBytes : typeBytes(type);
    const size_t n = packed ? 1 : static_cast<size_t>(formatComponents(format));
    const size_t a = static_cast<size_t>(std::max(1, ps.alignment));
    const size_t rowLength = static_cast<size_t>(ps.rowLength > 0 ? ps.rowLength : width);
    size_t rowElements = n * rowLength;
    if (s < a) rowElements = (a / s) * ((s * n * rowLength + a - 1) / a);
    l.rowStride = rowElements * s;
    const size_t imageHeight = static_cast<size_t>(ps.imageHeight > 0 ? ps.imageHeight : height);
    l.imageStride = l.rowStride * imageHeight;
    l.skipBytes = static_cast<size_t>(ps.skipImages) * l.imageStride + static_cast<size_t>(ps.skipRows) * l.rowStride +
                  static_cast<size_t>(ps.skipPixels) * l.groupBytes;
    if (width > 0 && height > 0 && depth > 0)
        l.bytesNeeded = l.skipBytes + static_cast<size_t>(depth - 1) * l.imageStride +
                        static_cast<size_t>(height - 1) * l.rowStride + static_cast<size_t>(width) * l.groupBytes;
    return l;
}

bool validTransferFormatType(GLenum format, GLenum type, bool compatibility) {
    const bool integer = isIntegerFormat(format);
    switch (format) {
    case GL_RED: case GL_GREEN: case GL_BLUE: case GL_RG: case GL_RGB: case GL_BGR: case GL_RGBA: case GL_BGRA:
    case GL_RED_INTEGER: case GL_GREEN_INTEGER: case GL_BLUE_INTEGER: case GL_RG_INTEGER: case GL_RGB_INTEGER:
    case GL_BGR_INTEGER: case GL_RGBA_INTEGER: case GL_BGRA_INTEGER: case GL_DEPTH_COMPONENT: case GL_STENCIL_INDEX:
    case GL_DEPTH_STENCIL: case GL_ALPHA:
        break;
    case GL_LUMINANCE: case GL_LUMINANCE_ALPHA: case GL_ALPHA_INTEGER:
        if (compatibility) break;
        return false;
    default: return false;
    }
    switch (type) {
    case GL_UNSIGNED_BYTE: case GL_BYTE: case GL_UNSIGNED_SHORT: case GL_SHORT: case GL_UNSIGNED_INT: case GL_INT:
        return format != GL_DEPTH_STENCIL;
    case GL_HALF_FLOAT: case GL_FLOAT:
        return !integer && format != GL_DEPTH_STENCIL;
    case GL_UNSIGNED_BYTE_3_3_2: case GL_UNSIGNED_BYTE_2_3_3_REV: case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV:
        return format == GL_RGB || format == GL_RGB_INTEGER;
    case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV: case GL_UNSIGNED_SHORT_5_5_5_1:
    case GL_UNSIGNED_SHORT_1_5_5_5_REV: case GL_UNSIGNED_INT_8_8_8_8: case GL_UNSIGNED_INT_8_8_8_8_REV:
    case GL_UNSIGNED_INT_10_10_10_2: case GL_UNSIGNED_INT_2_10_10_10_REV:
        return format == GL_RGBA || format == GL_BGRA || format == GL_RGBA_INTEGER || format == GL_BGRA_INTEGER;
    case GL_UNSIGNED_INT_10F_11F_11F_REV: case GL_UNSIGNED_INT_5_9_9_9_REV:
        return format == GL_RGB;
    case GL_UNSIGNED_INT_24_8: case GL_FLOAT_32_UNSIGNED_INT_24_8_REV:
        return format == GL_DEPTH_STENCIL;
    default: return false;
    }
}

bool transferCompatible(GLenum format, GLenum type, const FormatInfo& info) {
    const bool integerFormat = isIntegerFormat(format);
    if (info.isDepthOrStencil()) {
        if (info.isDepth() && info.isStencil())
            return format == GL_DEPTH_STENCIL || format == GL_DEPTH_COMPONENT || format == GL_STENCIL_INDEX;
        if (info.isDepth()) return format == GL_DEPTH_COMPONENT;
        return format == GL_STENCIL_INDEX;
    }
    if (format == GL_DEPTH_COMPONENT || format == GL_DEPTH_STENCIL || format == GL_STENCIL_INDEX) return false;
    if (integerFormat != info.isInteger()) return false;
    return true;
}

// -------------------------------------------------------------- bulk

namespace {

// Packed client types whose bits are the storage format's: transferred
// unchanged, since decoding and re-encoding would canonicalize them (RGB9E5
// has several encodings of one value; glCopyImageSubData and readback must
// see the bits that were uploaded).
// Floating-point data in its own component type is copied too: conversion
// would canonicalize NaN payloads, which views in integer formats see
// (KHR-GL43.texture_view.view_classes).
bool rawTransfer(GLenum format, GLenum type, Storage storage) {
    switch (storage) {
    case Storage::RGB9E5Float: return format == GL_RGB && type == GL_UNSIGNED_INT_5_9_9_9_REV;
    case Storage::RG11B10Float: return format == GL_RGB && type == GL_UNSIGNED_INT_10F_11F_11F_REV;
    case Storage::R16Float: return format == GL_RED && type == GL_HALF_FLOAT;
    case Storage::RG16Float: return format == GL_RG && type == GL_HALF_FLOAT;
    case Storage::RGBA16Float: return format == GL_RGBA && type == GL_HALF_FLOAT;
    case Storage::R32Float: return format == GL_RED && type == GL_FLOAT;
    case Storage::RG32Float: return format == GL_RG && type == GL_FLOAT;
    case Storage::RGBA32Float: return format == GL_RGBA && type == GL_FLOAT;
    // SNORM has two encodings of -1.0 (-128 and -127).
    case Storage::R8Snorm: return format == GL_RED && type == GL_BYTE;
    case Storage::RG8Snorm: return format == GL_RG && type == GL_BYTE;
    case Storage::RGBA8Snorm: return format == GL_RGBA && type == GL_BYTE;
    case Storage::R16Snorm: return format == GL_RED && type == GL_SHORT;
    case Storage::RG16Snorm: return format == GL_RG && type == GL_SHORT;
    case Storage::RGBA16Snorm: return format == GL_RGBA && type == GL_SHORT;
    default: return false;
    }
}

// Client groups whose bytes are the stored texel's bytes: the client format
// names the stored channels in order (alpha and luminance formats store into
// R or RG) and the type is the storage's component, so a row is copied as is.
bool identityTransfer(GLenum format, GLenum type, const FormatInfo& info) {
    if (info.storedAlphaIsOne || info.compressed || info.isDepthOrStencil()) return false;
    const GLenum base = info.baseFormat;
    const bool integer = info.isInteger();
    const bool channels =
        integer ? (format == GL_RED_INTEGER && base == GL_RED) || (format == GL_RG_INTEGER && base == GL_RG) ||
                      (format == GL_RGBA_INTEGER && base == GL_RGBA)
                : format == base && (base == GL_RED || base == GL_RG || base == GL_RGBA || base == GL_ALPHA ||
                                     base == GL_LUMINANCE || base == GL_LUMINANCE_ALPHA);
    if (!channels) return false;
    using S = Storage;
    switch (info.storage) {
    case S::R8Unorm: case S::RG8Unorm: case S::RGBA8Unorm: case S::RGBA8Unorm_sRGB: case S::R8Unorm_sRGB:
    case S::RG8Unorm_sRGB: return type == GL_UNSIGNED_BYTE;
    case S::R16Unorm: case S::RG16Unorm: case S::RGBA16Unorm: return type == GL_UNSIGNED_SHORT;
    case S::R8Uint: case S::RG8Uint: case S::RGBA8Uint: return type == GL_UNSIGNED_BYTE;
    case S::R8Sint: case S::RG8Sint: case S::RGBA8Sint: return type == GL_BYTE;
    case S::R16Uint: case S::RG16Uint: case S::RGBA16Uint: return type == GL_UNSIGNED_SHORT;
    case S::R16Sint: case S::RG16Sint: case S::RGBA16Sint: return type == GL_SHORT;
    case S::R32Uint: case S::RG32Uint: case S::RGBA32Uint: return type == GL_UNSIGNED_INT;
    case S::R32Sint: case S::RG32Sint: case S::RGBA32Sint: return type == GL_INT;
    default: return false;
    }
}

// unpackPixels' byte shuffle into RGBA8 (unsigned normalized, RGB or RGBA
// base format): for each stored channel, the byte of the client group it
// comes from, or kShuffleZero / kShuffleOne.
constexpr int8_t kShuffleZero = -1, kShuffleOne = -2;
bool byteShuffle(GLenum format, GLenum type, bool alphaIsOne, int8_t map[4]) {
    const int8_t O = kShuffleOne;
    auto set = [&](int8_t r, int8_t g, int8_t b, int8_t a) {
        map[0] = r;
        map[1] = g;
        map[2] = b;
        map[3] = alphaIsOne ? O : a;
        return true;
    };
    if (type == GL_UNSIGNED_BYTE) {
        switch (format) {
        case GL_RGB: return set(0, 1, 2, O);
        case GL_BGR: return set(2, 1, 0, O);
        case GL_RGBA: return set(0, 1, 2, 3);
        case GL_BGRA: return set(2, 1, 0, 3);
        // Conversion to RGB copies L into R, G and B (GL 2.1 section 3.6.5).
        case GL_LUMINANCE: return set(0, 0, 0, O);
        case GL_LUMINANCE_ALPHA: return set(0, 0, 0, 1);
        default: return false;
        }
    }
    // The first component is in the most significant byte: on a little-endian
    // host, the last byte in memory (the REV orders are UNSIGNED_BYTE groups).
    if (type == GL_UNSIGNED_INT_8_8_8_8) {
        if (format == GL_RGBA) return set(3, 2, 1, 0);
        if (format == GL_BGRA) return set(1, 2, 3, 0);
        return false;
    }
    if (type == GL_UNSIGNED_INT_8_8_8_8_REV) {
        if (format == GL_RGBA) return set(0, 1, 2, 3);
        if (format == GL_BGRA) return set(2, 1, 0, 3);
    }
    return false;
}

// unpackPixels' tables for 16-bit packed groups into RGBA8: the generic
// conversion (readGroup, clamping, rgbaToStored, encodeStorage) of every
// value, built once per (format, type, base format, alpha) and kept.
const uint32_t* packed16Table(GLenum format, GLenum type, const FormatInfo& info) {
    switch (type) {
    case GL_UNSIGNED_SHORT_5_6_5: case GL_UNSIGNED_SHORT_5_6_5_REV:
        if (format != GL_RGB) return nullptr;
        break;
    case GL_UNSIGNED_SHORT_4_4_4_4: case GL_UNSIGNED_SHORT_4_4_4_4_REV: case GL_UNSIGNED_SHORT_5_5_5_1:
    case GL_UNSIGNED_SHORT_1_5_5_5_REV:
        if (format != GL_RGBA && format != GL_BGRA) return nullptr;
        break;
    default: return nullptr;
    }
    static std::mutex lock;
    static std::unordered_map<uint64_t, std::unique_ptr<uint32_t[]>> tables;
    const uint64_t key = (static_cast<uint64_t>(format) << 40) ^ (static_cast<uint64_t>(type) << 16) ^
                         (static_cast<uint64_t>(info.baseFormat) << 2) ^ (info.storedAlphaIsOne ? 1u : 0u) ^
                         (info.storage == Storage::RGBA8Unorm_sRGB ? 2u : 0u);
    std::lock_guard<std::mutex> guard(lock);
    std::unique_ptr<uint32_t[]>& table = tables[key];
    if (!table) {
        table.reset(new uint32_t[65536]);
        for (uint32_t v = 0; v < 65536; ++v) {
            const uint16_t group = static_cast<uint16_t>(v);
            Texel t;
            readGroup(reinterpret_cast<const uint8_t*>(&group), format, type, false, false, t);
            for (double& c : t.c) c = std::clamp(c, 0.0, 1.0);
            rgbaToStored(info, t);
            encodeStorage(info.storage, t, reinterpret_cast<uint8_t*>(&table[v]));
        }
    }
    return table.get();
}

}  // namespace

bool storedLayout(GLenum format, GLenum type, const FormatInfo& info) {
    // Byte swapping is the caller's to exclude (PixelStoreParams::swapBytes).
    return groupBytes(format, type) == storageBytes(info.storage) &&
           (identityTransfer(format, type, info) || rawTransfer(format, type, info.storage));
}

void unpackPixels(const void* src, GLenum format, GLenum type, int width, int height, int depth,
                  const PixelStoreParams& ps, const FormatInfo& info, void* out) {
    const TransferLayout l = transferLayout(format, type, width, height, depth, ps);
    const size_t texelBytes = storageBytes(info.storage);
    const bool integerDest = info.isInteger();
    const bool clampDepth = info.isDepth() && (info.type != ComponentType::Float || ps.clampFloatDepth);
    auto* dst = static_cast<uint8_t*>(out);
    const auto* base = static_cast<const uint8_t*>(src) + l.skipBytes;
    const bool swap = ps.swapBytes && typeBytes(type) > 1;
    Texel t;
    // Rows that need no conversion are copied whole; BGRA bytes into RGBA8
    // swap two bytes per texel; RGB bytes into RGBA8 gain an opaque alpha.
    // RGBA and BGRA groups of UNSIGNED_INT_8_8_8_8_REV are the bytes of
    // UNSIGNED_BYTE groups on a little-endian host.
    const GLenum byteType = (type == GL_UNSIGNED_INT_8_8_8_8_REV && (format == GL_RGBA || format == GL_BGRA)) ? GL_UNSIGNED_BYTE
                                                                                                           : type;
    const bool rgba8 = info.storage == Storage::RGBA8Unorm || info.storage == Storage::RGBA8Unorm_sRGB;
    const size_t rowBytes = static_cast<size_t>(width) * texelBytes;
    auto rowAt = [&](int z, int y) { return base + static_cast<size_t>(z) * l.imageStride + static_cast<size_t>(y) * l.rowStride; };
    if (!swap && l.groupBytes == texelBytes && !(ps.clampFloatDepth && info.isDepth()) &&
        (identityTransfer(format, byteType, info) || rawTransfer(format, type, info.storage))) {
        for (int z = 0; z < depth; ++z)
            for (int y = 0; y < height; ++y, dst += rowBytes) std::memcpy(dst, rowAt(z, y), rowBytes);
        return;
    }
    if (!swap && format == GL_BGRA && byteType == GL_UNSIGNED_BYTE && l.groupBytes == 4 && rgba8 &&
        info.baseFormat == GL_RGBA) {
        for (int z = 0; z < depth; ++z)
            for (int y = 0; y < height; ++y) {
                const uint8_t* row = rowAt(z, y);
                for (int x = 0; x < width; ++x, row += 4, dst += 4) {
                    uint32_t v;
                    std::memcpy(&v, row, 4);
                    v = (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16);
                    std::memcpy(dst, &v, 4);
                }
            }
        return;
    }
    if (format == GL_RGB && type == GL_UNSIGNED_BYTE && l.groupBytes == 3 && rgba8 && info.baseFormat == GL_RGB) {
        // Four texels a step as one byte shuffle (pshufb, tbl), while 16
        // bytes can be read within the row; the rest one at a time.
        typedef uint8_t Bytes16 __attribute__((ext_vector_type(16)));
        const Bytes16 opaque = (Bytes16)(0xff);
        for (int z = 0; z < depth; ++z)
            for (int y = 0; y < height; ++y) {
                const uint8_t* row = rowAt(z, y);
                int x = 0;
                for (; x + 6 <= width; x += 4, row += 12, dst += 16) {
                    Bytes16 in;
                    std::memcpy(&in, row, 16);
                    const Bytes16 out = __builtin_shufflevector(in, opaque, 0, 1, 2, 16, 3, 4, 5, 16, 6, 7, 8, 16, 9, 10, 11, 16);
                    std::memcpy(dst, &out, 16);
                }
                for (; x < width; ++x, row += 3, dst += 4) {
                    dst[0] = row[0];
                    dst[1] = row[1];
                    dst[2] = row[2];
                    dst[3] = 0xff;
                }
            }
        return;
    }
    if (!swap && rgba8 && (info.baseFormat == GL_RGB || info.baseFormat == GL_RGBA)) {
        // Other byte groups into RGBA8: a byte shuffle (8-bit normalized
        // values convert exactly), with an opaque alpha where the group has
        // none or the internal format drops it.
        int8_t map[4];
        if (byteShuffle(format, type, info.storedAlphaIsOne, map) && l.groupBytes == groupBytes(format, type)) {
            for (int z = 0; z < depth; ++z)
                for (int y = 0; y < height; ++y) {
                    const uint8_t* row = rowAt(z, y);
                    for (int x = 0; x < width; ++x, row += l.groupBytes, dst += 4)
                        for (int c = 0; c < 4; ++c) dst[c] = map[c] < 0 ? (map[c] == kShuffleOne ? 0xff : 0) : row[map[c]];
                }
            return;
        }
        // 16-bit packed groups (565, 4444, 5551 and their REV orders): a
        // table of the generic conversion of every value.
        if (const uint32_t* table = packed16Table(format, type, info); table && l.groupBytes == 2) {
            for (int z = 0; z < depth; ++z)
                for (int y = 0; y < height; ++y) {
                    const uint8_t* row = rowAt(z, y);
                    for (int x = 0; x < width; ++x, row += 2, dst += 4) {
                        uint16_t v;
                        std::memcpy(&v, row, 2);
                        std::memcpy(dst, &table[v], 4);
                    }
                }
            return;
        }
    }
    if (!swap && type == GL_FLOAT && info.storage == Storage::RGBA16Float &&
        ((format == GL_RGBA && info.baseFormat == GL_RGBA && !info.storedAlphaIsOne) ||
         (format == GL_RGB && info.baseFormat == GL_RGB)) && l.groupBytes == groupBytes(format, type)) {
        // Floats into half floats, as encodeStorage converts them.
        const int components = format == GL_RGBA ? 4 : 3;
        for (int z = 0; z < depth; ++z)
            for (int y = 0; y < height; ++y) {
                const uint8_t* row = rowAt(z, y);
                for (int x = 0; x < width; ++x, row += l.groupBytes, dst += 8) {
                    float f[4] = {0, 0, 0, 1};
                    std::memcpy(f, row, static_cast<size_t>(components) * 4);
                    if (info.storedAlphaIsOne) f[3] = 1;
                    for (int c = 0; c < 4; ++c) {
                        const uint16_t h = floatToHalf(f[c]);
                        std::memcpy(dst + c * 2, &h, 2);
                    }
                }
            }
        return;
    }
    const bool raw = rawTransfer(format, type, info.storage) && l.groupBytes == texelBytes && !(ps.clampFloatDepth && info.isDepth());
    const size_t rawComponent = type == GL_BYTE ? 1 : type == GL_HALF_FLOAT || type == GL_SHORT ? 2 : 4;
    for (int z = 0; z < depth; ++z)
        for (int y = 0; y < height; ++y) {
            const uint8_t* row = base + static_cast<size_t>(z) * l.imageStride + static_cast<size_t>(y) * l.rowStride;
            for (int x = 0; x < width; ++x) {
                if (raw) {
                    const uint8_t* group = row + static_cast<size_t>(x) * l.groupBytes;
                    for (size_t c = 0; c < texelBytes; c += rawComponent) {
                        const uint64_t v = readWord(group + c, rawComponent, swap);
                        std::memcpy(dst + c, &v, rawComponent);  // little-endian host
                    }
                    dst += texelBytes;
                    continue;
                }
                readGroup(row + static_cast<size_t>(x) * l.groupBytes, format, type, integerDest, swap, t);
                if (format == GL_ALPHA || format == GL_ALPHA_INTEGER) {
                    // An ALPHA group fills only alpha; colour channels read as zero.
                    t.c[0] = t.c[1] = t.c[2] = 0.0;
                }
                if (format == GL_LUMINANCE || format == GL_LUMINANCE_ALPHA) {
                    // Conversion to RGB: L is copied into R, G and B (GL 2.1 section 3.6.5).
                    t.c[1] = t.c[2] = t.c[0];
                }
                if (clampDepth) t.depth = std::clamp(t.depth, 0.0, 1.0);
                if (info.type == ComponentType::UnsignedNormalized && !info.isDepthOrStencil())
                    for (double& c : t.c) c = std::clamp(c, 0.0, 1.0);
                rgbaToStored(info, t);
                encodeStorage(info.storage, t, dst);
                dst += texelBytes;
            }
        }
}

void packPixels(const void* storage, const FormatInfo& info, int width, int height, int depth, GLenum format,
                GLenum type, const PixelStoreParams& ps, bool clampColor, void* out) {
    const TransferLayout l = transferLayout(format, type, width, height, depth, ps);
    const size_t texelBytes = storageBytes(info.storage);
    const bool integerSource = info.isInteger();
    const auto* src = static_cast<const uint8_t*>(storage);
    auto* base = static_cast<uint8_t*>(out) + l.skipBytes;
    const bool swap = ps.swapBytes && typeBytes(type) > 1;
    Texel t;
    // The fast paths of unpackPixels, reversed: rows in the stored layout
    // are copied whole (unsigned normalized and integer texels need no
    // clamping), and RGBA8 texels read as BGRA swap two bytes.
    const GLenum byteType = (type == GL_UNSIGNED_INT_8_8_8_8_REV && (format == GL_RGBA || format == GL_BGRA)) ? GL_UNSIGNED_BYTE
                                                                                                           : type;
    const size_t rowBytes = static_cast<size_t>(width) * texelBytes;
    auto rowAt = [&](int z, int y) { return base + static_cast<size_t>(z) * l.imageStride + static_cast<size_t>(y) * l.rowStride; };
    if (!swap && l.groupBytes == texelBytes && !(ps.clampFloatDepth && info.isDepth()) &&
        (identityTransfer(format, byteType, info) || rawTransfer(format, type, info.storage))) {
        for (int z = 0; z < depth; ++z)
            for (int y = 0; y < height; ++y, src += rowBytes) std::memcpy(rowAt(z, y), src, rowBytes);
        return;
    }
    if (!swap && format == GL_BGRA && byteType == GL_UNSIGNED_BYTE && l.groupBytes == 4 && info.baseFormat == GL_RGBA &&
        (info.storage == Storage::RGBA8Unorm || info.storage == Storage::RGBA8Unorm_sRGB)) {
        for (int z = 0; z < depth; ++z)
            for (int y = 0; y < height; ++y) {
                uint8_t* row = rowAt(z, y);
                for (int x = 0; x < width; ++x, row += 4, src += 4) {
                    uint32_t v;
                    std::memcpy(&v, src, 4);
                    v = (v & 0xff00ff00u) | ((v >> 16) & 0xffu) | ((v & 0xffu) << 16);
                    std::memcpy(row, &v, 4);
                }
            }
        return;
    }
    const bool raw = rawTransfer(format, type, info.storage) && l.groupBytes == texelBytes;
    const size_t rawComponent = type == GL_BYTE ? 1 : type == GL_HALF_FLOAT || type == GL_SHORT ? 2 : 4;
    for (int z = 0; z < depth; ++z)
        for (int y = 0; y < height; ++y) {
            uint8_t* row = base + static_cast<size_t>(z) * l.imageStride + static_cast<size_t>(y) * l.rowStride;
            for (int x = 0; x < width; ++x) {
                if (raw) {
                    uint8_t* group = row + static_cast<size_t>(x) * l.groupBytes;
                    for (size_t c = 0; c < texelBytes; c += rawComponent) {
                        uint64_t v = 0;
                        std::memcpy(&v, src + c, rawComponent);
                        writeWord(group + c, v, rawComponent, swap);
                    }
                    src += texelBytes;
                    continue;
                }
                decodeStorage(info.storage, src, t);
                src += texelBytes;
                storedToRgba(info, t);
                // Read colour clamping (GL 3.3 section 4.3.2) clamps to [0, 1] for every format.
                if (clampColor && !integerSource)
                    for (double& c : t.c) c = std::clamp(c, 0.0, 1.0);
                writeGroup(row + static_cast<size_t>(x) * l.groupBytes, format, type, integerSource, swap, t);
            }
        }
}

// -------------------------------------------------------------- RGTC

bool isRgtcStorage(Storage storage) {
    return storage == Storage::BC4_RUnorm || storage == Storage::BC4_RSnorm || storage == Storage::BC5_RGUnorm ||
           storage == Storage::BC5_RGSnorm;
}

namespace {

// One BC4 channel block: 16 values (row-major) -> 8 bytes.
void encodeBc4(const float values[16], bool snorm, uint8_t* out) {
    int q[16];
    int lo = snorm ? 127 : 255, hi = snorm ? -127 : 0;
    for (int i = 0; i < 16; ++i) {
        const float v = snorm ? std::clamp(values[i], -1.0f, 1.0f) : std::clamp(values[i], 0.0f, 1.0f);
        q[i] = static_cast<int>(std::lround(v * (snorm ? 127.0f : 255.0f)));
        lo = std::min(lo, q[i]);
        hi = std::max(hi, q[i]);
    }
    // r0 > r1 selects the 8-value palette (6 interpolated values).
    const int r0 = hi, r1 = lo;
    out[0] = snorm ? static_cast<uint8_t>(static_cast<int8_t>(r0)) : static_cast<uint8_t>(r0);
    out[1] = snorm ? static_cast<uint8_t>(static_cast<int8_t>(r1)) : static_cast<uint8_t>(r1);
    double palette[8];
    palette[0] = r0;
    palette[1] = r1;
    for (int k = 1; k <= 6; ++k) palette[k + 1] = ((7 - k) * r0 + k * r1) / 7.0;
    uint64_t bits = 0;
    for (int i = 0; i < 16; ++i) {
        int best = 0;
        double bestError = 1e30;
        for (int k = 0; k < (r0 > r1 ? 8 : 1); ++k) {
            const double e = std::fabs(palette[k] - q[i]);
            if (e < bestError) {
                bestError = e;
                best = k;
            }
        }
        bits |= static_cast<uint64_t>(best) << (3 * i);
    }
    for (int b = 0; b < 6; ++b) out[2 + b] = static_cast<uint8_t>(bits >> (8 * b));
}

void decodeBc4(const uint8_t* in, bool snorm, float values[16]) {
    const int r0 = snorm ? static_cast<int8_t>(in[0]) : in[0];
    const int r1 = snorm ? static_cast<int8_t>(in[1]) : in[1];
    const double scale = snorm ? 127.0 : 255.0;
    double palette[8];
    palette[0] = r0;
    palette[1] = r1;
    if (r0 > r1) {
        for (int k = 1; k <= 6; ++k) palette[k + 1] = ((7 - k) * r0 + k * r1) / 7.0;
    } else {
        for (int k = 1; k <= 4; ++k) palette[k + 1] = ((5 - k) * r0 + k * r1) / 5.0;
        palette[6] = snorm ? -127.0 : 0.0;
        palette[7] = scale;
    }
    uint64_t bits = 0;
    for (int b = 0; b < 6; ++b) bits |= static_cast<uint64_t>(in[2 + b]) << (8 * b);
    for (int i = 0; i < 16; ++i) {
        const double v = palette[(bits >> (3 * i)) & 7] / scale;
        values[i] = static_cast<float>(snorm ? std::max(-1.0, v) : v);
    }
}

}  // namespace

void encodeRgtc(Storage storage, const float* rgba, int width, int height, int depth, uint8_t* blocks) {
    const bool snorm = storage == Storage::BC4_RSnorm || storage == Storage::BC5_RGSnorm;
    const int channels = storage == Storage::BC5_RGUnorm || storage == Storage::BC5_RGSnorm ? 2 : 1;
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    for (int z = 0; z < depth; ++z)
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx)
                for (int c = 0; c < channels; ++c) {
                    float values[16];
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x) {
                            const int sx = std::min(bx * 4 + x, width - 1), sy = std::min(by * 4 + y, height - 1);
                            values[y * 4 + x] = rgba[((static_cast<size_t>(z) * height + sy) * width + sx) * 4 + c];
                        }
                    encodeBc4(values, snorm, blocks);
                    blocks += 8;
                }
}

void decodeRgtc(Storage storage, const uint8_t* blocks, int width, int height, int depth, float* rgba) {
    const bool snorm = storage == Storage::BC4_RSnorm || storage == Storage::BC5_RGSnorm;
    const int channels = storage == Storage::BC5_RGUnorm || storage == Storage::BC5_RGSnorm ? 2 : 1;
    const int bw = (width + 3) / 4, bh = (height + 3) / 4;
    for (size_t i = 0; i < static_cast<size_t>(width) * height * depth; ++i) {
        rgba[i * 4 + 0] = rgba[i * 4 + 1] = rgba[i * 4 + 2] = 0.0f;
        rgba[i * 4 + 3] = 1.0f;
    }
    for (int z = 0; z < depth; ++z)
        for (int by = 0; by < bh; ++by)
            for (int bx = 0; bx < bw; ++bx)
                for (int c = 0; c < channels; ++c) {
                    float values[16];
                    decodeBc4(blocks, snorm, values);
                    blocks += 8;
                    for (int y = 0; y < 4; ++y)
                        for (int x = 0; x < 4; ++x) {
                            const int dx = bx * 4 + x, dy = by * 4 + y;
                            if (dx >= width || dy >= height) continue;
                            rgba[((static_cast<size_t>(z) * height + dy) * width + dx) * 4 + c] = values[y * 4 + x];
                        }
                }
}

}  // namespace wmgl
