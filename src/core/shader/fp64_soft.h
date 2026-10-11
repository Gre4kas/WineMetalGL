// IEEE 754 binary64 arithmetic on 64-bit integers (the bit patterns of
// doubles), for GL_ARB_gpu_shader_fp64 on Metal, which has no double type.
//
// The same text is compiled twice: into the core as the MSL library that
// lowered shaders call (shader/fp64.cpp, as a string), and natively by the
// host tests, which compare every operation bit for bit with the host's
// doubles. The includer defines WMGL_FP64_SOURCE(...) to either expand or
// stringify its argument, and provides, for its target:
//
//   ulong, uint, wmgl_clz64(ulong), wmgl_mulhi64(ulong, ulong),
//   wmgl_f32_bits(float) -> uint, wmgl_bits_f32(uint) -> float.
//
// Results are correctly rounded (round to nearest, ties to even) for add,
// subtract, multiply, divide, square root and every conversion, with
// subnormals, infinities and NaNs as IEEE 754 specifies; a NaN result is the
// first NaN operand made quiet, or the default NaN 0x7FF8000000000000. fma is
// a multiply then an add (two roundings), which GLSL allows outside precise
// expressions. The algorithms follow Berkeley SoftFloat 3's structure
// (round-and-pack with 10 guard bits, magnitude add/subtract).
//
// No preprocessor directives may appear between the parentheses.
WMGL_FP64_SOURCE(

static inline ulong wmgl_d_sign(ulong a) { return a >> 63; }
static inline int wmgl_d_exp(ulong a) { return int((a >> 52) & 0x7FFul); }
static inline ulong wmgl_d_frac(ulong a) { return a & 0x000FFFFFFFFFFFFFul; }
static inline ulong wmgl_d_pack(ulong sign, int exp, ulong sig) { return (sign << 63) + (ulong(uint(exp)) << 52) + sig; }
static inline bool wmgl_d_isnan(ulong a) { return (a & 0x7FFFFFFFFFFFFFFFul) > 0x7FF0000000000000ul; }
static inline bool wmgl_d_isinf(ulong a) { return (a & 0x7FFFFFFFFFFFFFFFul) == 0x7FF0000000000000ul; }
static inline ulong wmgl_d_default_nan() { return 0x7FF8000000000000ul; }

static inline ulong wmgl_d_propagate_nan(ulong a, ulong b) {
    return wmgl_d_isnan(a) ? (a | 0x0008000000000000ul) : (b | 0x0008000000000000ul);
}

static inline ulong wmgl_shift_right_jam64(ulong a, uint dist) {
    return dist < 63u ? ((a >> dist) | ulong((a << ((0u - dist) & 63u)) != 0ul)) : ulong(a != 0ul);
}

// sig: leading one at bit 62 (or below for subnormal results), 10 rounding
// bits; the value is sig / 2^62 * 2^(exp - 1022).
static inline ulong wmgl_d_round_pack(ulong sign, int exp, ulong sig) {
    const ulong roundIncrement = 0x200ul;
    ulong roundBits = sig & 0x3FFul;
    if (uint(exp) >= 0x7FDu) {
        if (exp < 0) {
            sig = wmgl_shift_right_jam64(sig, uint(-exp));
            exp = 0;
            roundBits = sig & 0x3FFul;
        } else if (exp > 0x7FD || sig + roundIncrement >= 0x8000000000000000ul) {
            return wmgl_d_pack(sign, 0x7FF, 0ul);
        }
    }
    sig = (sig + roundIncrement) >> 10;
    if (roundBits == 0x200ul) sig &= ~1ul;
    if (sig == 0ul) exp = 0;
    return wmgl_d_pack(sign, exp, sig);
}

static inline ulong wmgl_d_norm_round_pack(ulong sign, int exp, ulong sig) {
    const int shiftDist = wmgl_clz64(sig) - 1;
    exp -= shiftDist;
    if (shiftDist >= 10 && uint(exp) < 0x7FDu) return wmgl_d_pack(sign, sig != 0ul ? exp : 0, sig << uint(shiftDist - 10));
    return wmgl_d_round_pack(sign, exp, sig << uint(shiftDist));
}

static inline ulong wmgl_d_add_mags(ulong a, ulong b, ulong signZ) {
    int expA = wmgl_d_exp(a), expB = wmgl_d_exp(b);
    ulong sigA = wmgl_d_frac(a), sigB = wmgl_d_frac(b);
    const int expDiff = expA - expB;
    int expZ;
    ulong sigZ;
    if (expDiff == 0) {
        if (expA == 0) return a + sigB;
        if (expA == 0x7FF) return (sigA | sigB) != 0ul ? wmgl_d_propagate_nan(a, b) : a;
        expZ = expA;
        sigZ = (0x0020000000000000ul + sigA + sigB) << 9;
    } else {
        sigA <<= 9;
        sigB <<= 9;
        if (expDiff < 0) {
            if (expB == 0x7FF) return sigB != 0ul ? wmgl_d_propagate_nan(a, b) : wmgl_d_pack(signZ, 0x7FF, 0ul);
            expZ = expB;
            if (expA != 0) sigA += 0x2000000000000000ul;
            else sigA <<= 1;
            sigA = wmgl_shift_right_jam64(sigA, uint(-expDiff));
        } else {
            if (expA == 0x7FF) return sigA != 0ul ? wmgl_d_propagate_nan(a, b) : a;
            expZ = expA;
            if (expB != 0) sigB += 0x2000000000000000ul;
            else sigB <<= 1;
            sigB = wmgl_shift_right_jam64(sigB, uint(expDiff));
        }
        sigZ = 0x2000000000000000ul + sigA + sigB;
        if (sigZ < 0x4000000000000000ul) {
            --expZ;
            sigZ <<= 1;
        }
    }
    return wmgl_d_round_pack(signZ, expZ, sigZ);
}

static inline ulong wmgl_d_sub_mags(ulong a, ulong b, ulong signZ) {
    int expA = wmgl_d_exp(a), expB = wmgl_d_exp(b);
    ulong sigA = wmgl_d_frac(a), sigB = wmgl_d_frac(b);
    const int expDiff = expA - expB;
    if (expDiff == 0) {
        if (expA == 0x7FF) return (sigA | sigB) != 0ul ? wmgl_d_propagate_nan(a, b) : wmgl_d_default_nan();
        ulong sigDiff = sigA - sigB;
        if (sigDiff == 0ul) return 0ul;  // +0 when rounding to nearest
        if (expA != 0) --expA;
        if ((sigDiff >> 63) != 0ul) {
            signZ ^= 1ul;
            sigDiff = 0ul - sigDiff;
        }
        int shiftDist = wmgl_clz64(sigDiff) - 11;
        int expZ = expA - shiftDist;
        if (expZ < 0) {
            shiftDist = expA;
            expZ = 0;
        }
        return wmgl_d_pack(signZ, expZ, sigDiff << uint(shiftDist));
    }
    sigA <<= 10;
    sigB <<= 10;
    int expZ;
    ulong sigZ;
    if (expDiff < 0) {
        signZ ^= 1ul;
        if (expB == 0x7FF) return sigB != 0ul ? wmgl_d_propagate_nan(a, b) : wmgl_d_pack(signZ, 0x7FF, 0ul);
        sigA += expA != 0 ? 0x4000000000000000ul : sigA;
        sigA = wmgl_shift_right_jam64(sigA, uint(-expDiff));
        sigB |= 0x4000000000000000ul;
        expZ = expB;
        sigZ = sigB - sigA;
    } else {
        if (expA == 0x7FF) return sigA != 0ul ? wmgl_d_propagate_nan(a, b) : a;
        sigB += expB != 0 ? 0x4000000000000000ul : sigB;
        sigB = wmgl_shift_right_jam64(sigB, uint(expDiff));
        sigA |= 0x4000000000000000ul;
        expZ = expA;
        sigZ = sigA - sigB;
    }
    return wmgl_d_norm_round_pack(signZ, expZ - 1, sigZ);
}

static inline ulong wmgl_dadd(ulong a, ulong b) {
    return wmgl_d_sign(a) == wmgl_d_sign(b) ? wmgl_d_add_mags(a, b, wmgl_d_sign(a)) : wmgl_d_sub_mags(a, b, wmgl_d_sign(a));
}

static inline ulong wmgl_dsub(ulong a, ulong b) {
    return wmgl_d_sign(a) == wmgl_d_sign(b) ? wmgl_d_sub_mags(a, b, wmgl_d_sign(a)) : wmgl_d_add_mags(a, b, wmgl_d_sign(a));
}

static inline ulong wmgl_dneg(ulong a) { return a ^ 0x8000000000000000ul; }
static inline ulong wmgl_dabs(ulong a) { return a & 0x7FFFFFFFFFFFFFFFul; }

// Normalizes a subnormal significand (nonzero): returns the exponent the
// number has once its leading one is at bit 52, and shifts sig there.
static inline int wmgl_d_norm_subnormal(thread ulong& sig) {
    const int shiftDist = wmgl_clz64(sig) - 11;
    sig <<= uint(shiftDist);
    return 1 - shiftDist;
}

static inline ulong wmgl_dmul(ulong a, ulong b) {
    int expA = wmgl_d_exp(a), expB = wmgl_d_exp(b);
    ulong sigA = wmgl_d_frac(a), sigB = wmgl_d_frac(b);
    const ulong signZ = wmgl_d_sign(a) ^ wmgl_d_sign(b);
    if (expA == 0x7FF) {
        if (sigA != 0ul || (expB == 0x7FF && sigB != 0ul)) return wmgl_d_propagate_nan(a, b);
        return (ulong(expB) | sigB) == 0ul ? wmgl_d_default_nan() : wmgl_d_pack(signZ, 0x7FF, 0ul);
    }
    if (expB == 0x7FF) {
        if (sigB != 0ul) return wmgl_d_propagate_nan(a, b);
        return (ulong(expA) | sigA) == 0ul ? wmgl_d_default_nan() : wmgl_d_pack(signZ, 0x7FF, 0ul);
    }
    if (expA == 0) {
        if (sigA == 0ul) return wmgl_d_pack(signZ, 0, 0ul);
        expA = wmgl_d_norm_subnormal(sigA);
    }
    if (expB == 0) {
        if (sigB == 0ul) return wmgl_d_pack(signZ, 0, 0ul);
        expB = wmgl_d_norm_subnormal(sigB);
    }
    int expZ = expA + expB - 0x3FF;
    sigA = (sigA | 0x0010000000000000ul) << 10;
    sigB = (sigB | 0x0010000000000000ul) << 11;
    const ulong hi = wmgl_mulhi64(sigA, sigB), lo = sigA * sigB;
    ulong sigZ = hi | ulong(lo != 0ul);
    if (sigZ < 0x4000000000000000ul) {
        --expZ;
        sigZ <<= 1;
    }
    return wmgl_d_round_pack(signZ, expZ, sigZ);
}

static inline ulong wmgl_ddiv(ulong a, ulong b) {
    int expA = wmgl_d_exp(a), expB = wmgl_d_exp(b);
    ulong sigA = wmgl_d_frac(a), sigB = wmgl_d_frac(b);
    const ulong signZ = wmgl_d_sign(a) ^ wmgl_d_sign(b);
    if (expA == 0x7FF) {
        if (sigA != 0ul) return wmgl_d_propagate_nan(a, b);
        if (expB == 0x7FF) return sigB != 0ul ? wmgl_d_propagate_nan(a, b) : wmgl_d_default_nan();
        return wmgl_d_pack(signZ, 0x7FF, 0ul);
    }
    if (expB == 0x7FF) return sigB != 0ul ? wmgl_d_propagate_nan(a, b) : wmgl_d_pack(signZ, 0, 0ul);
    if (expB == 0) {
        if (sigB == 0ul) return (ulong(expA) | sigA) == 0ul ? wmgl_d_default_nan() : wmgl_d_pack(signZ, 0x7FF, 0ul);
        expB = wmgl_d_norm_subnormal(sigB);
    }
    if (expA == 0) {
        if (sigA == 0ul) return wmgl_d_pack(signZ, 0, 0ul);
        expA = wmgl_d_norm_subnormal(sigA);
    }
    int expZ = expA - expB + 0x3FE;
    sigA |= 0x0010000000000000ul;
    sigB |= 0x0010000000000000ul;
    if (sigA < sigB) {
        --expZ;
        sigA <<= 1;
    }
    // Restoring division: 63 quotient bits, the leading one at bit 62.
    ulong q = 0ul, r = sigA;
    for (int i = 62; i >= 0; --i) {
        if (r >= sigB) {
            r -= sigB;
            q |= 1ul << uint(i);
        }
        r <<= 1;
    }
    return wmgl_d_round_pack(signZ, expZ, q | ulong(r != 0ul));
}

static inline ulong wmgl_dsqrt(ulong a) {
    int expA = wmgl_d_exp(a);
    ulong sigA = wmgl_d_frac(a);
    const ulong signA = wmgl_d_sign(a);
    if (expA == 0x7FF) {
        if (sigA != 0ul) return wmgl_d_propagate_nan(a, 0ul);
        return signA != 0ul ? wmgl_d_default_nan() : a;
    }
    if (signA != 0ul) return (ulong(expA) | sigA) == 0ul ? a : wmgl_d_default_nan();
    if (expA == 0) {
        if (sigA == 0ul) return a;
        expA = wmgl_d_norm_subnormal(sigA);
    }
    // a = m / 2^52 * 2^e with e even after doubling m for odd e; the root of
    // N = m * 2^56 has 55 bits.
    ulong m = sigA | 0x0010000000000000ul;
    int e = expA - 0x3FF;
    if ((e & 1) != 0) {
        m <<= 1;
        e -= 1;
    }
    const ulong nHi = m >> 8, nLo = m << 56;
    ulong root = 0ul;
    for (int bit = 54; bit >= 0; --bit) {
        const ulong t = root | (1ul << uint(bit));
        const ulong sqHi = wmgl_mulhi64(t, t), sqLo = t * t;
        if (sqHi < nHi || (sqHi == nHi && sqLo <= nLo)) root = t;
    }
    const ulong exact = ulong(wmgl_mulhi64(root, root) == nHi && root * root == nLo);
    return wmgl_d_round_pack(0ul, e / 2 + 0x3FE, (root << 8) | (exact ^ 1ul));
}

static inline ulong wmgl_dfma(ulong a, ulong b, ulong c) { return wmgl_dadd(wmgl_dmul(a, b), c); }

// ---------------------------------------------------------------- compares

static inline bool wmgl_deq(ulong a, ulong b) {
    if (wmgl_d_isnan(a) || wmgl_d_isnan(b)) return false;
    return a == b || ((a | b) << 1) == 0ul;
}

static inline bool wmgl_dlt(ulong a, ulong b) {
    if (wmgl_d_isnan(a) || wmgl_d_isnan(b)) return false;
    const ulong signA = wmgl_d_sign(a), signB = wmgl_d_sign(b);
    if (signA != signB) return signA != 0ul && ((a | b) << 1) != 0ul;
    return a != b && ((signA != 0ul) != (a < b));
}

static inline bool wmgl_dle(ulong a, ulong b) {
    if (wmgl_d_isnan(a) || wmgl_d_isnan(b)) return false;
    const ulong signA = wmgl_d_sign(a), signB = wmgl_d_sign(b);
    if (signA != signB) return signA != 0ul || ((a | b) << 1) == 0ul;
    return a == b || ((signA != 0ul) != (a < b));
}

static inline bool wmgl_dunord(ulong a, ulong b) { return wmgl_d_isnan(a) || wmgl_d_isnan(b); }

// ------------------------------------------------------------- conversions

static inline ulong wmgl_d_from_f32(float f) {
    const uint bits = wmgl_f32_bits(f);
    const ulong sign = ulong(bits >> 31);
    int exp = int((bits >> 23) & 0xFFu);
    uint frac = bits & 0x007FFFFFu;
    if (exp == 0xFF) return frac != 0u ? (wmgl_d_pack(sign, 0x7FF, ulong(frac) << 29) | 0x0008000000000000ul) : wmgl_d_pack(sign, 0x7FF, 0ul);
    if (exp == 0) {
        if (frac == 0u) return wmgl_d_pack(sign, 0, 0ul);
        const int shiftDist = wmgl_clz64(ulong(frac)) - 64 + 32 - 8;
        frac <<= uint(shiftDist);
        exp = 1 - shiftDist;
        frac &= 0x007FFFFFu;
    }
    return wmgl_d_pack(sign, exp + 0x380, ulong(frac) << 29);
}

static inline uint wmgl_f32_round_pack(uint sign, int exp, uint sig) {
    const uint roundIncrement = 0x40u;
    uint roundBits = sig & 0x7Fu;
    if (uint(exp) >= 0xFDu) {
        if (exp < 0) {
            const uint dist = uint(-exp);
            sig = dist < 31u ? ((sig >> dist) | uint((sig << ((0u - dist) & 31u)) != 0u)) : uint(sig != 0u);
            exp = 0;
            roundBits = sig & 0x7Fu;
        } else if (exp > 0xFD || sig + roundIncrement >= 0x80000000u) {
            return (sign << 31) | 0x7F800000u;
        }
    }
    sig = (sig + roundIncrement) >> 7;
    if (roundBits == 0x40u) sig &= ~1u;
    if (sig == 0u) exp = 0;
    return (sign << 31) + (uint(exp) << 23) + sig;
}

static inline float wmgl_d_to_f32(ulong a) {
    const uint sign = uint(a >> 63);
    const int exp = wmgl_d_exp(a);
    const ulong frac = wmgl_d_frac(a);
    if (exp == 0x7FF) {
        if (frac != 0ul) return wmgl_bits_f32((sign << 31) | 0x7FC00000u | uint(frac >> 29));
        return wmgl_bits_f32((sign << 31) | 0x7F800000u);
    }
    const uint frac32 = uint(frac >> 22) | uint((frac & 0x3FFFFFul) != 0ul);
    if ((uint(exp) | frac32) == 0u) return wmgl_bits_f32(sign << 31);
    return wmgl_bits_f32(wmgl_f32_round_pack(sign, exp - 0x381, frac32 | 0x40000000u));
}

static inline ulong wmgl_d_from_u64(ulong v, ulong sign) {
    if (v == 0ul) return wmgl_d_pack(sign, 0, 0ul);
    const int shiftDist = wmgl_clz64(v) - 11;
    if (shiftDist >= 0) return wmgl_d_pack(sign, 0x432 - shiftDist, v << uint(shiftDist));
    return wmgl_d_norm_round_pack(sign, 0x43C, v);
}

static inline ulong wmgl_d_from_i32(int i) {
    const ulong sign = ulong(i < 0);
    const ulong mag = i < 0 ? 0ul - ulong(long(i)) : ulong(i);
    return wmgl_d_from_u64(mag, sign);
}

static inline ulong wmgl_d_from_u32(uint u) { return wmgl_d_from_u64(ulong(u), 0ul); }

// Truncation toward zero; out-of-range values saturate (GL leaves them undefined).
static inline int wmgl_d_to_i32(ulong a) {
    const int exp = wmgl_d_exp(a);
    const bool neg = wmgl_d_sign(a) != 0ul;
    if (exp == 0x7FF && wmgl_d_frac(a) != 0ul) return 0;
    if (exp < 0x3FF) return 0;
    if (exp > 0x3FF + 30) return neg ? int(0x80000000u) : 0x7FFFFFFF;
    const ulong sig = wmgl_d_frac(a) | 0x0010000000000000ul;
    const uint mag = uint(sig >> uint(0x433 - exp));
    return neg ? int(0u - mag) : int(mag);
}

static inline uint wmgl_d_to_u32(ulong a) {
    const int exp = wmgl_d_exp(a);
    if (wmgl_d_sign(a) != 0ul || (exp == 0x7FF && wmgl_d_frac(a) != 0ul)) return 0u;
    if (exp < 0x3FF) return 0u;
    if (exp > 0x3FF + 31) return 0xFFFFFFFFu;
    const ulong sig = wmgl_d_frac(a) | 0x0010000000000000ul;
    return uint(sig >> uint(0x433 - exp));
}

// --------------------------------------------------------------- rounding

// mode: 0 nearest even, 1 nearest away from zero, 2 toward -inf, 3 toward +inf, 4 toward zero.
static inline ulong wmgl_d_round_to_int(ulong a, int mode) {
    const int exp = wmgl_d_exp(a);
    const ulong sign = wmgl_d_sign(a);
    if (exp <= 0x3FE) {
        if ((a << 1) == 0ul) return a;
        if (mode == 0) return (exp == 0x3FE && wmgl_d_frac(a) != 0ul) ? wmgl_d_pack(sign, 0x3FF, 0ul) : wmgl_d_pack(sign, 0, 0ul);
        if (mode == 1) return exp == 0x3FE ? wmgl_d_pack(sign, 0x3FF, 0ul) : wmgl_d_pack(sign, 0, 0ul);
        if (mode == 2) return sign != 0ul ? wmgl_d_pack(1ul, 0x3FF, 0ul) : 0ul;
        if (mode == 3) return sign != 0ul ? wmgl_d_pack(1ul, 0, 0ul) : wmgl_d_pack(0ul, 0x3FF, 0ul);
        return wmgl_d_pack(sign, 0, 0ul);
    }
    if (exp >= 0x433) return exp == 0x7FF && wmgl_d_frac(a) != 0ul ? (a | 0x0008000000000000ul) : a;
    const ulong lastBitMask = 1ul << uint(0x433 - exp);
    const ulong roundBitsMask = lastBitMask - 1ul;
    ulong z = a;
    if (mode == 1) {
        z += lastBitMask >> 1;
    } else if (mode == 0) {
        z += lastBitMask >> 1;
        if ((z & roundBitsMask) == 0ul) z &= ~lastBitMask;
    } else if ((mode == 2 && sign != 0ul) || (mode == 3 && sign == 0ul)) {
        z += roundBitsMask;
    }
    return z & ~roundBitsMask;
}

static inline ulong wmgl_dround_even(ulong a) { return wmgl_d_round_to_int(a, 0); }
static inline ulong wmgl_dround(ulong a) { return wmgl_d_round_to_int(a, 1); }
static inline ulong wmgl_dfloor(ulong a) { return wmgl_d_round_to_int(a, 2); }
static inline ulong wmgl_dceil(ulong a) { return wmgl_d_round_to_int(a, 3); }
static inline ulong wmgl_dtrunc(ulong a) { return wmgl_d_round_to_int(a, 4); }

static inline ulong wmgl_dfract(ulong a) { return wmgl_dsub(a, wmgl_dfloor(a)); }

// GLSL mod: x - y * floor(x / y).
static inline ulong wmgl_dmod(ulong x, ulong y) { return wmgl_dsub(x, wmgl_dmul(y, wmgl_dfloor(wmgl_ddiv(x, y)))); }

// C fmod-style remainder (SPIR-V OpFRem): x - y * trunc(x / y).
static inline ulong wmgl_drem(ulong x, ulong y) { return wmgl_dsub(x, wmgl_dmul(y, wmgl_dtrunc(wmgl_ddiv(x, y)))); }

static inline ulong wmgl_dmodf_int(ulong a) { return wmgl_dtrunc(a); }
static inline ulong wmgl_dmodf_frac(ulong a) {
    if (wmgl_d_isinf(a)) return wmgl_d_pack(wmgl_d_sign(a), 0, 0ul);
    const ulong f = wmgl_dsub(a, wmgl_dtrunc(a));
    return (f << 1) == 0ul ? wmgl_d_pack(wmgl_d_sign(a), 0, 0ul) : f;
}

static inline ulong wmgl_dfrexp_mant(ulong a) {
    int exp = wmgl_d_exp(a);
    ulong sig = wmgl_d_frac(a);
    if (exp == 0x7FF || (a << 1) == 0ul) return a;
    if (exp == 0) exp = wmgl_d_norm_subnormal(sig);
    return wmgl_d_pack(wmgl_d_sign(a), 0x3FE, sig & 0x000FFFFFFFFFFFFFul);
}

static inline int wmgl_dfrexp_exp(ulong a) {
    int exp = wmgl_d_exp(a);
    ulong sig = wmgl_d_frac(a);
    if (exp == 0x7FF || (a << 1) == 0ul) return 0;
    if (exp == 0) exp = wmgl_d_norm_subnormal(sig);
    return exp - 0x3FE;
}

static inline ulong wmgl_dldexp(ulong a, int e) {
    int exp = wmgl_d_exp(a);
    ulong sig = wmgl_d_frac(a);
    if (exp == 0x7FF || (a << 1) == 0ul) return a;
    if (exp == 0) exp = wmgl_d_norm_subnormal(sig);
    e = e > 4000 ? 4000 : e < -4000 ? -4000 : e;
    return wmgl_d_round_pack(wmgl_d_sign(a), exp + e - 1, (sig | 0x0010000000000000ul) << 10);
}

// ------------------------------------------------------- simple built-ins

static inline ulong wmgl_d_one() { return 0x3FF0000000000000ul; }

static inline ulong wmgl_dsign(ulong a) {
    if (wmgl_d_isnan(a) || (a << 1) == 0ul) return a;
    return wmgl_d_pack(wmgl_d_sign(a), 0x3FF, 0ul);
}

static inline ulong wmgl_dmin(ulong a, ulong b) { return wmgl_dlt(b, a) ? b : a; }
static inline ulong wmgl_dmax(ulong a, ulong b) { return wmgl_dlt(a, b) ? b : a; }
static inline ulong wmgl_dclamp(ulong x, ulong lo, ulong hi) { return wmgl_dmin(wmgl_dmax(x, lo), hi); }
static inline ulong wmgl_dmix(ulong x, ulong y, ulong a) {
    return wmgl_dadd(wmgl_dmul(x, wmgl_dsub(wmgl_d_one(), a)), wmgl_dmul(y, a));
}
static inline ulong wmgl_dstep(ulong edge, ulong x) { return wmgl_dlt(x, edge) ? 0ul : wmgl_d_one(); }
static inline ulong wmgl_dsmoothstep(ulong e0, ulong e1, ulong x) {
    const ulong t = wmgl_dclamp(wmgl_ddiv(wmgl_dsub(x, e0), wmgl_dsub(e1, e0)), 0ul, wmgl_d_one());
    const ulong two = 0x4000000000000000ul, three = 0x4008000000000000ul;
    return wmgl_dmul(wmgl_dmul(t, t), wmgl_dsub(three, wmgl_dmul(two, t)));
}
static inline ulong wmgl_dinversesqrt(ulong a) { return wmgl_ddiv(wmgl_d_one(), wmgl_dsqrt(a)); }

)
