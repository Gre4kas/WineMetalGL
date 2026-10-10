#include "core/format/vertex_decode.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace wmgl {

namespace {

float component(const uint8_t* p, GLenum type, bool normalized, int index) {
    switch (type) {
    case GL_BYTE: {
        const int8_t v = reinterpret_cast<const int8_t*>(p)[index];
        return normalized ? std::max(-1.0f, v / 127.0f) : static_cast<float>(v);
    }
    case GL_UNSIGNED_BYTE: {
        const uint8_t v = p[index];
        return normalized ? v / 255.0f : static_cast<float>(v);
    }
    case GL_SHORT: {
        int16_t v;
        std::memcpy(&v, p + index * 2, 2);
        return normalized ? std::max(-1.0f, v / 32767.0f) : static_cast<float>(v);
    }
    case GL_UNSIGNED_SHORT: {
        uint16_t v;
        std::memcpy(&v, p + index * 2, 2);
        return normalized ? v / 65535.0f : static_cast<float>(v);
    }
    case GL_INT: {
        int32_t v;
        std::memcpy(&v, p + index * 4, 4);
        return normalized ? std::max(-1.0f, static_cast<float>(v / 2147483647.0)) : static_cast<float>(v);
    }
    case GL_UNSIGNED_INT: {
        uint32_t v;
        std::memcpy(&v, p + index * 4, 4);
        return normalized ? static_cast<float>(v / 4294967295.0) : static_cast<float>(v);
    }
    case GL_FIXED: {
        int32_t v;
        std::memcpy(&v, p + index * 4, 4);
        return v / 65536.0f;
    }
    case GL_DOUBLE: {
        double v;
        std::memcpy(&v, p + index * 8, 8);
        return static_cast<float>(v);
    }
    case GL_FLOAT: {
        float v;
        std::memcpy(&v, p + index * 4, 4);
        return v;
    }
    case GL_HALF_FLOAT: {
        uint16_t h;
        std::memcpy(&h, p + index * 2, 2);
        const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 0x1f, mantissa = h & 0x3ff;
        float v;
        if (exponent == 0) v = std::ldexp(static_cast<float>(mantissa), -24);
        else if (exponent == 31) v = mantissa ? NAN : INFINITY;
        else v = std::ldexp(static_cast<float>(mantissa | 0x400), static_cast<int>(exponent) - 25);
        return sign ? -v : v;
    }
    default: return 0.0f;
    }
}

// Packed 32-bit formats (GL 3.3 section 2.8.2), all four components.
bool packed(const uint8_t* p, GLenum type, bool normalized, float out[4]) {
    if (type != GL_INT_2_10_10_10_REV && type != GL_UNSIGNED_INT_2_10_10_10_REV) return false;
    uint32_t word;
    std::memcpy(&word, p, 4);
    const int bits[4] = {10, 10, 10, 2};
    int shift = 0;
    for (int c = 0; c < 4; ++c) {
        const uint32_t raw = (word >> shift) & ((1u << bits[c]) - 1);
        shift += bits[c];
        if (type == GL_UNSIGNED_INT_2_10_10_10_REV) {
            out[c] = normalized ? raw / static_cast<float>((1u << bits[c]) - 1) : static_cast<float>(raw);
        } else {
            const int32_t value = static_cast<int32_t>(raw << (32 - bits[c])) >> (32 - bits[c]);
            const float max = static_cast<float>((1 << (bits[c] - 1)) - 1);
            out[c] = normalized ? std::max(-1.0f, value / max) : static_cast<float>(value);
        }
    }
    return true;
}

}  // namespace

void decodeVertexFloat(const uint8_t* element, GLenum type, GLint size, bool normalized, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (packed(element, type, normalized, out)) return;
    const int count = size == GL_BGRA ? 4 : std::min<int>(size, 4);
    for (int c = 0; c < count; ++c) out[c] = component(element, type, normalized, c);
    if (size == GL_BGRA) std::swap(out[0], out[2]);
}

void decodeVertexInt(const uint8_t* element, GLenum type, GLint size, int32_t out[4]) {
    out[0] = out[1] = out[2] = 0;
    out[3] = 1;
    for (int c = 0; c < std::min<int>(size, 4); ++c) {
        switch (type) {
        case GL_BYTE: out[c] = reinterpret_cast<const int8_t*>(element)[c]; break;
        case GL_UNSIGNED_BYTE: out[c] = element[c]; break;
        case GL_SHORT: { int16_t x; std::memcpy(&x, element + c * 2, 2); out[c] = x; break; }
        case GL_UNSIGNED_SHORT: { uint16_t x; std::memcpy(&x, element + c * 2, 2); out[c] = x; break; }
        default: std::memcpy(&out[c], element + c * 4, 4); break;
        }
    }
}

}  // namespace wmgl
