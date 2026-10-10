#include "core/metal/enums.h"

namespace wmgl {

MTLCompareFunction mtlCompare(GLenum func) {
    switch (func) {
    case GL_NEVER: return MTLCompareFunctionNever;
    case GL_LESS: return MTLCompareFunctionLess;
    case GL_EQUAL: return MTLCompareFunctionEqual;
    case GL_LEQUAL: return MTLCompareFunctionLessEqual;
    case GL_GREATER: return MTLCompareFunctionGreater;
    case GL_NOTEQUAL: return MTLCompareFunctionNotEqual;
    case GL_GEQUAL: return MTLCompareFunctionGreaterEqual;
    default: return MTLCompareFunctionAlways;
    }
}

MTLStencilOperation mtlStencilOp(GLenum op) {
    switch (op) {
    case GL_ZERO: return MTLStencilOperationZero;
    case GL_REPLACE: return MTLStencilOperationReplace;
    case GL_INCR: return MTLStencilOperationIncrementClamp;
    case GL_DECR: return MTLStencilOperationDecrementClamp;
    case GL_INVERT: return MTLStencilOperationInvert;
    case GL_INCR_WRAP: return MTLStencilOperationIncrementWrap;
    case GL_DECR_WRAP: return MTLStencilOperationDecrementWrap;
    default: return MTLStencilOperationKeep;
    }
}

MTLBlendFactor mtlBlendFactor(GLenum factor) {
    switch (factor) {
    case GL_ZERO: return MTLBlendFactorZero;
    case GL_ONE: return MTLBlendFactorOne;
    case GL_SRC_COLOR: return MTLBlendFactorSourceColor;
    case GL_ONE_MINUS_SRC_COLOR: return MTLBlendFactorOneMinusSourceColor;
    case GL_DST_COLOR: return MTLBlendFactorDestinationColor;
    case GL_ONE_MINUS_DST_COLOR: return MTLBlendFactorOneMinusDestinationColor;
    case GL_SRC_ALPHA: return MTLBlendFactorSourceAlpha;
    case GL_ONE_MINUS_SRC_ALPHA: return MTLBlendFactorOneMinusSourceAlpha;
    case GL_DST_ALPHA: return MTLBlendFactorDestinationAlpha;
    case GL_ONE_MINUS_DST_ALPHA: return MTLBlendFactorOneMinusDestinationAlpha;
    case GL_CONSTANT_COLOR: return MTLBlendFactorBlendColor;
    case GL_ONE_MINUS_CONSTANT_COLOR: return MTLBlendFactorOneMinusBlendColor;
    case GL_CONSTANT_ALPHA: return MTLBlendFactorBlendAlpha;
    case GL_ONE_MINUS_CONSTANT_ALPHA: return MTLBlendFactorOneMinusBlendAlpha;
    case GL_SRC_ALPHA_SATURATE: return MTLBlendFactorSourceAlphaSaturated;
    case GL_SRC1_COLOR: return MTLBlendFactorSource1Color;
    case GL_ONE_MINUS_SRC1_COLOR: return MTLBlendFactorOneMinusSource1Color;
    case GL_SRC1_ALPHA: return MTLBlendFactorSource1Alpha;
    case GL_ONE_MINUS_SRC1_ALPHA: return MTLBlendFactorOneMinusSource1Alpha;
    default: return MTLBlendFactorOne;
    }
}

MTLBlendOperation mtlBlendOp(GLenum equation) {
    switch (equation) {
    case GL_FUNC_SUBTRACT: return MTLBlendOperationSubtract;
    case GL_FUNC_REVERSE_SUBTRACT: return MTLBlendOperationReverseSubtract;
    case GL_MIN: return MTLBlendOperationMin;
    case GL_MAX: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
    }
}

MTLSamplerAddressMode mtlAddressMode(GLenum wrap) {
    switch (wrap) {
    case GL_CLAMP_TO_EDGE: return MTLSamplerAddressModeClampToEdge;
    case GL_CLAMP: return MTLSamplerAddressModeClampToEdge;  // compatibility; border texels are not supported
    case GL_CLAMP_TO_BORDER: return MTLSamplerAddressModeClampToBorderColor;
    case GL_MIRRORED_REPEAT: return MTLSamplerAddressModeMirrorRepeat;
    case GL_MIRROR_CLAMP_TO_EDGE: return MTLSamplerAddressModeMirrorClampToEdge;
    default: return MTLSamplerAddressModeRepeat;
    }
}

MTLVertexFormat mtlVertexFormat(GLenum type, GLint size, bool normalized, bool integer) {
    if (size == GL_BGRA) return type == GL_UNSIGNED_BYTE && normalized ? MTLVertexFormatUChar4Normalized_BGRA
                                                                      : MTLVertexFormatInvalid;
    const int n = size;
    auto pick = [&](MTLVertexFormat f1, MTLVertexFormat f2, MTLVertexFormat f3, MTLVertexFormat f4) {
        return n == 1 ? f1 : n == 2 ? f2 : n == 3 ? f3 : f4;
    };
    if (integer) {
        switch (type) {
        case GL_BYTE: return pick(MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4);
        case GL_UNSIGNED_BYTE: return pick(MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4);
        case GL_SHORT: return pick(MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4);
        case GL_UNSIGNED_SHORT: return pick(MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4);
        case GL_INT: return pick(MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4);
        case GL_UNSIGNED_INT: return pick(MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4);
        default: return MTLVertexFormatInvalid;
        }
    }
    switch (type) {
    case GL_FLOAT: return pick(MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4);
    case GL_HALF_FLOAT: return pick(MTLVertexFormatHalf, MTLVertexFormatHalf2, MTLVertexFormatHalf3, MTLVertexFormatHalf4);
    case GL_BYTE:
        return normalized ? pick(MTLVertexFormatCharNormalized, MTLVertexFormatChar2Normalized, MTLVertexFormatChar3Normalized, MTLVertexFormatChar4Normalized)
                          : MTLVertexFormatInvalid;  // float conversion of non-normalized bytes: vertex pulling
    case GL_UNSIGNED_BYTE:
        return normalized ? pick(MTLVertexFormatUCharNormalized, MTLVertexFormatUChar2Normalized, MTLVertexFormatUChar3Normalized, MTLVertexFormatUChar4Normalized)
                          : MTLVertexFormatInvalid;
    case GL_SHORT:
        return normalized ? pick(MTLVertexFormatShortNormalized, MTLVertexFormatShort2Normalized, MTLVertexFormatShort3Normalized, MTLVertexFormatShort4Normalized)
                          : MTLVertexFormatInvalid;
    case GL_UNSIGNED_SHORT:
        return normalized ? pick(MTLVertexFormatUShortNormalized, MTLVertexFormatUShort2Normalized, MTLVertexFormatUShort3Normalized, MTLVertexFormatUShort4Normalized)
                          : MTLVertexFormatInvalid;
    case GL_INT_2_10_10_10_REV: return n == 4 && normalized ? MTLVertexFormatInt1010102Normalized : MTLVertexFormatInvalid;
    case GL_UNSIGNED_INT_2_10_10_10_REV: return n == 4 && normalized ? MTLVertexFormatUInt1010102Normalized : MTLVertexFormatInvalid;
    case GL_UNSIGNED_INT_10F_11F_11F_REV: return n == 3 ? MTLVertexFormatFloatRG11B10 : MTLVertexFormatInvalid;
    default: return MTLVertexFormatInvalid;
    }
}

MTLPrimitiveTopologyClass topologyClass(GLenum mode) {
    switch (mode) {
    case GL_POINTS: return MTLPrimitiveTopologyClassPoint;
    case GL_LINES: case GL_LINE_STRIP: case GL_LINE_LOOP: case GL_LINES_ADJACENCY: case GL_LINE_STRIP_ADJACENCY:
        return MTLPrimitiveTopologyClassLine;
    default: return MTLPrimitiveTopologyClassTriangle;
    }
}

}  // namespace wmgl
