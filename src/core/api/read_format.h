// The implementation's color read format (glReadPixels and its queries).
#pragma once

#include "core/gl/types.h"

namespace wmgl {
class Context;
namespace api {

// IMPLEMENTATION_COLOR_READ_FORMAT/TYPE for the read buffer; false when it
// is incomplete or has no color buffer.
bool implementationReadFormat(Context& ctx, GLenum& format, GLenum& type);

}  // namespace api
}  // namespace wmgl
