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
            if (!same(wmgl_dfrexp_mant(a), bits(m)) || wmgl_dfrexp_exp(a) != e) ++bad;
        }
        const int scale = static_cast<int>(rng() % 2200) - 1100;
        if (!same(wmgl_dldexp(a, scale), bits(std::ldexp(d, scale)))) ++bad;
    }
    CHECK_EQ(bad, 0);
}
