// Vertex attribute elements as the vertex fetch converts them (GL 3.3
// section 2.8): the draw path's CPU conversion and display-list compilation
// both decode through here, so the same arrays give the same values.
#pragma once

#include <cstdint>

#include "core/gl/types.h"

namespace wmgl {

// Float attributes: `size` components of `type` (GL_BGRA: four, R and B
// swapped). Signed normalized values are max(c / (2^(b-1) - 1), -1), as
// Metal's normalized vertex formats convert them. Components the element does
// not have read (0, 0, 0, 1).
void decodeVertexFloat(const uint8_t* element, GLenum type, GLint size, bool normalized, float out[4]);

// Integer attributes (glVertexAttribIPointer): sign- or zero-extended per
// `type`; components the element does not have read (0, 0, 0, 1).
void decodeVertexInt(const uint8_t* element, GLenum type, GLint size, int32_t out[4]);

}  // namespace wmgl
