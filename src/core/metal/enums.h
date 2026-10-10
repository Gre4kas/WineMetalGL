// GL -> Metal enum translation.
#pragma once

#import <Metal/Metal.h>

#include "core/gl/types.h"

namespace wmgl {

MTLCompareFunction mtlCompare(GLenum func);
MTLStencilOperation mtlStencilOp(GLenum op);
MTLBlendFactor mtlBlendFactor(GLenum factor);
MTLBlendOperation mtlBlendOp(GLenum equation);
MTLSamplerAddressMode mtlAddressMode(GLenum wrap);
// Vertex format for an attribute array; MTLVertexFormatInvalid if Metal has none.
MTLVertexFormat mtlVertexFormat(GLenum type, GLint size, bool normalized, bool integer);
MTLPrimitiveTopologyClass topologyClass(GLenum mode);

}  // namespace wmgl
