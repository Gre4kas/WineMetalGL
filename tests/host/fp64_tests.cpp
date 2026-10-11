// The soft-float library behind GL_ARB_gpu_shader_fp64 (shader/fp64_soft.h),
// compiled natively and compared bit for bit with the host's IEEE doubles.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>

#include "test.h"

namespace fp64host {

using ulong = uint64_t;
using uint = uint32_t;
static inline int wmgl_clz64(ulong x) { return x ? __builtin_clzll(x) : 64; }
static inline ulong wmgl_mulhi64(ulong a, ulong b) { return static_cast<ulong>((static_cast<unsigned __int128>(a) * b) >> 64); }
static inline uint wmgl_f32_bits(float f) {
    uint u;
    std::memcpy(&u, &f, 4);
    return u;
}
static inline float wmgl_bits_f32(uint u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}
#define thread
#define WMGL_FP64_SOURCE(...) __VA_ARGS__
#include "core/shader/fp64_soft.h"
#undef WMGL_FP64_SOURCE
#undef thread

}  // namespace fp64host

using namespace fp64host;

namespace {

ulong bits(double d) {
    ulong u;
    std::memcpy(&u, &d, 8);
    return u;
}
double value(ulong u) {
    double d;
    std::memcpy(&d, &u, 8);
    return d;
}

// Equal bit patterns, or both NaN (payloads are not specified by GL).
bool same(ulong a, ulong b) { return a == b || (std::isnan(value(a)) && std::isnan(value(b))); }

// Random doubles that cover every class: zeros, subnormals, normals of all
// magnitudes, infinities, NaNs, and operand pairs with nearby exponents
// (cancellation and rounding ties).
struct Source {
    std::mt19937_64 rng{0x5eed1234u};
    ulong any() {
        switch (rng() % 16) {
        case 0: return rng() & 0x800FFFFFFFFFFFFFull;                           // subnormal or zero
        case 1: return (rng() & 0x8000000000000000ull) | 0x7FF0000000000000ull;  // infinity
        case 2: return rng() | 0x7FF0000000000001ull;                           // NaN
        case 3: return (rng() & 0x8000000000000000ull);                         // signed zero
        case 4: return bits(static_cast<double>(static_cast<int32_t>(rng())) / 4.0);  // halves and quarters
        case 5: {  // near 1 with few low bits set
            const ulong low = rng() & 0x3F;
            return (rng() & 0x8000000000000000ull) | 0x3FF0000000000000ull | low;
        }
        default: {  // normal, exponent within +-64 of 1023 most of the time
            const ulong sign = rng() & 0x8000000000000000ull;
            const ulong exp = rng() % 4 == 0 ? (rng() % 2046) + 1 : 1023 - 64 + rng() % 128;
            return sign | (exp << 52) | (rng() & 0x000FFFFFFFFFFFFFull);
        }
        }
    }
    // b close to a in magnitude (exercises cancellation and alignment).
    ulong near(ulong a) {
        const ulong exp = (a >> 52) & 0x7FF;
        if (exp == 0 || exp == 0x7FF) return any();
        const long delta = static_cast<long>(rng() % 120) - 60;
        const long e = std::clamp<long>(static_cast<long>(exp) + delta, 1, 2046);
        return (rng() & 0x8000000000000000ull) | (static_cast<ulong>(e) << 52) | (rng() & 0x000FFFFFFFFFFFFFull);
    }
};

constexpr int kSamples = 200000;

template <class Soft, class Host>
int checkBinary(Soft soft, Host host) {
    Source s;
    int bad = 0;
    for (int i = 0; i < kSamples; ++i) {
        const ulong a = s.any(), b = i % 2 ? s.near(a) : s.any();
        const ulong got = soft(a, b), want = bits(host(value(a), value(b)));
        if (!same(got, want) && bad++ < 4)
            std::fprintf(stderr, "  %016llx, %016llx: got %016llx want %016llx\n", static_cast<unsigned long long>(a),
                         static_cast<unsigned long long>(b), static_cast<unsigned long long>(got),
                         static_cast<unsigned long long>(want));
    }
    return bad;
}

template <class Soft, class Host>
int checkUnary(Soft soft, Host host) {
    Source s;
    int bad = 0;
    for (int i = 0; i < kSamples; ++i) {
        const ulong a = s.any();
        const ulong got = soft(a), want = bits(host(value(a)));
        if (!same(got, want) && bad++ < 4)
            std::fprintf(stderr, "  %016llx: got %016llx want %016llx\n", static_cast<unsigned long long>(a),
                         static_cast<unsigned long long>(got), static_cast<unsigned long long>(want));
    }
    return bad;
}

}  // namespace

TEST(fp64_add_sub_mul_div_are_correctly_rounded) {
    CHECK_EQ(checkBinary(wmgl_dadd, [](double a, double b) { return a + b; }), 0);
    CHECK_EQ(checkBinary(wmgl_dsub, [](double a, double b) { return a - b; }), 0);
    CHECK_EQ(checkBinary(wmgl_dmul, [](double a, double b) { return a * b; }), 0);
    CHECK_EQ(checkBinary(wmgl_ddiv, [](double a, double b) { return a / b; }), 0);
    // Exact ties: 1 + 2^-53 rounds to even (1), 1 + 3 * 2^-53 rounds up.
    CHECK_EQ(wmgl_dadd(bits(1.0), bits(std::ldexp(1.0, -53))), bits(1.0));
    CHECK_EQ(wmgl_dadd(bits(1.0), bits(3 * std::ldexp(1.0, -53))), bits(1.0 + std::ldexp(1.0, -51)));
    CHECK_EQ(wmgl_dsub(bits(1.0), bits(1.0)), bits(0.0));
}

TEST(fp64_sqrt_is_correctly_rounded) {
    CHECK_EQ(checkUnary(wmgl_dsqrt, [](double a) { return std::sqrt(a); }), 0);
    CHECK_EQ(wmgl_dsqrt(bits(-0.0)), bits(-0.0));
    CHECK(std::isnan(value(wmgl_dsqrt(bits(-1.0)))));
}

TEST(fp64_compares_follow_ieee) {
    Source s;
    int bad = 0;
    for (int i = 0; i < kSamples; ++i) {
        const ulong a = s.any(), b = i % 3 ? s.near(a) : (i % 2 ? a : s.any());
        const double x = value(a), y = value(b);
        bad += wmgl_deq(a, b) != (x == y);
        bad += wmgl_dlt(a, b) != (x < y);
        bad += wmgl_dle(a, b) != (x <= y);
        bad += wmgl_dunord(a, b) != (std::isnan(x) || std::isnan(y));
    }
    CHECK_EQ(bad, 0);
}

TEST(fp64_conversions_are_exact_or_correctly_rounded) {
    Source s;
    int bad = 0;
    std::mt19937 rng(7);
    for (int i = 0; i < kSamples; ++i) {
        const ulong a = s.any();
        const float want = static_cast<float>(value(a));
        const float got = wmgl_d_to_f32(a);
        if (!(std::memcmp(&want, &got, 4) == 0 || (std::isnan(want) && std::isnan(got)))) ++bad;
        const uint f = static_cast<uint>(rng());
        const float in = wmgl_bits_f32(f);
        if (!same(wmgl_d_from_f32(in), bits(static_cast<double>(in)))) ++bad;
        const int32_t si = static_cast<int32_t>(rng());
        if (wmgl_d_from_i32(si) != bits(static_cast<double>(si))) ++bad;
        const uint32_t ui = static_cast<uint32_t>(rng());
        if (wmgl_d_from_u32(ui) != bits(static_cast<double>(ui))) ++bad;
        const double d = value(a);
        if (std::isfinite(d) && std::fabs(d) < 2147483647.0 && wmgl_d_to_i32(a) != static_cast<int32_t>(d)) ++bad;
        if (std::isfinite(d) && d >= 0 && d < 4294967295.0 && wmgl_d_to_u32(a) != static_cast<uint32_t>(d)) ++bad;
    }
    CHECK_EQ(bad, 0);
    CHECK_EQ(wmgl_d_from_i32(std::numeric_limits<int32_t>::min()), bits(-2147483648.0));
}

TEST(fp64_rounding_functions_match_libm) {
    CHECK_EQ(checkUnary(wmgl_dfloor, [](double a) { return std::floor(a); }), 0);
    CHECK_EQ(checkUnary(wmgl_dceil, [](double a) { return std::ceil(a); }), 0);
    CHECK_EQ(checkUnary(wmgl_dtrunc, [](double a) { return std::trunc(a); }), 0);
    CHECK_EQ(checkUnary(wmgl_dround, [](double a) { return std::round(a); }), 0);
    CHECK_EQ(checkUnary(wmgl_dround_even, [](double a) { return std::nearbyint(a); }), 0);
    CHECK_EQ(checkUnary(wmgl_dfract, [](double a) { return a - std::floor(a); }), 0);
    CHECK_EQ(checkUnary(wmgl_dmodf_int, [](double a) { double i; std::modf(a, &i); return i; }), 0);
}

TEST(fp64_frexp_ldexp_match_libm) {
    Source s;
    int bad = 0;
    std::mt19937 rng(11);
    for (int i = 0; i < kSamples; ++i) {
        const ulong a = s.any();
        const double d = value(a);
        if (std::isfinite(d)) {
            int e = 0;
            const double m = std::frexp(d, &e);
            if (!same(wmgl_dfrexp_mant(a), bits(m)) || wmgl_dfrexp_exp(a) != e) {
                if (bad++ < 5)
                    std::fprintf(stderr, "  frexp(%016llx): %016llx %d, libm %016llx %d\n", (unsigned long long)a,
                                 (unsigned long long)wmgl_dfrexp_mant(a), wmgl_dfrexp_exp(a), (unsigned long long)bits(m), e);
            }
        }
        const int scale = static_cast<int>(rng() % 2200) - 1100;
        if (!same(wmgl_dldexp(a, scale), bits(std::ldexp(d, scale))) && bad++ < 5)
            std::fprintf(stderr, "  ldexp(%016llx, %d): %016llx, libm %016llx\n", (unsigned long long)a, scale,
                         (unsigned long long)wmgl_dldexp(a, scale), (unsigned long long)bits(std::ldexp(d, scale)));
    }
    CHECK_EQ(bad, 0);
}

// ------------------------------------------------- the lowering in the linker

#include "core/shader/compiler.h"
#include "core/shader/fp64.h"

TEST(fp64_link_lowers_every_double_stage) {
    using namespace wmgl::shader;
    const char* vs = R"(#version 410 core
layout(std140) uniform Block { layout(row_major) dmat3x2 r; double s; } b;
uniform dvec3 u;
layout(location = 0) in dvec4 a;
layout(location = 1) in dmat2x3 m;
flat out dvec3 v;
flat out double w;
void main() {
    v = u * b.s + dvec3(b.r * dvec3(1.0), a.w) + m[1];
    w = determinant(dmat2(a.xy, a.zw));
    gl_Position = vec4(vec3(v), 1.0);
})";
    const char* fs = R"(#version 410 core
flat in dvec3 v;
flat in double w;
out vec4 color;
void main() { color = vec4(dvec4(v, w)); })";
    LinkInput input;
    input.shaders = {{Stage::Vertex, vs}, {Stage::Fragment, fs}};
    const LinkResult r = link(input);
    CHECK(r.ok);
    if (!r.ok) {
        std::fprintf(stderr, "%s\n", r.log.c_str());
        return;
    }
    for (int s : {static_cast<int>(Stage::Vertex), static_cast<int>(Stage::Fragment)}) {
        const std::string& msl = r.stages[s].msl;
        // No double type reaches Metal.
        for (const char* type : {"double ", "double2", "double3", "double4", "double2x"}) CHECK(msl.find(type) == std::string::npos);
        CHECK(msl.find("wmgl_d_round_pack") != std::string::npos);
        // Every stub definition ("... wmgl_s<N>_<lib>(...)\n{") returns a library call.
        int stubs = 0;
        for (size_t at = msl.find(" wmgl_s"); at != std::string::npos; at = msl.find(" wmgl_s", at + 1)) {
            const size_t open = msl.find('(', at), eol = msl.find('\n', at);
            if (open == std::string::npos || eol == std::string::npos || msl.compare(eol, 3, "\n{\n") != 0) continue;
            ++stubs;
            CHECK(msl.compare(eol + 3, 16, "    return wmgl_") == 0);
        }
        CHECK(stubs > 0);
    }
    // dvec4 a takes one location (part 1 on a spare Metal attribute); the
    // dmat2x3 m two (one per column), each split in two.
    int parts = 0, seconds = 0;
    for (const StageOutput::VertexInput& in : r.stages[static_cast<int>(Stage::Vertex)].vertexInputs) {
        CHECK_EQ(in.kind, 'd');
        ++parts;
        if (in.part == 1) {
            ++seconds;
            CHECK(in.location > 2);
        }
    }
    CHECK_EQ(parts, 6);
    CHECK_EQ(seconds, 3);
    // Shaders without doubles are not touched.
    LinkInput plain;
    plain.shaders = {{Stage::Vertex, "#version 330 core\nin vec4 p; void main() { gl_Position = p; }"}};
    const LinkResult q = link(plain);
    CHECK(q.ok && q.stages[0].msl.find("wmgl_d_") == std::string::npos);
}
