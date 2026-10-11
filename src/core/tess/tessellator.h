// The fixed-function tessellator (GL 4.0 section 11.2.2) for the
// tessellation emulation (shader/tessellation.h): Microsoft's D3D11
// reference tessellator as carried in Mesa (vendor/tessellator), whose
// domains, edge order and level rules match GL's.
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "core/gl/types.h"

namespace wmgl::tess {

struct Mode {
    GLenum domain = GL_TRIANGLES;  // GL_TRIANGLES, GL_QUADS or GL_ISOLINES
    GLenum spacing = GL_EQUAL;     // GL_EQUAL, GL_FRACTIONAL_EVEN or GL_FRACTIONAL_ODD
    bool cw = false;               // vertex order (GL_CW)
    bool pointMode = false;
};

// One tessellated patch: domain points (u, v; w = 1 - u - v for triangles)
// and the primitive list (triangles, line segments, or one entry per point
// in point mode). Empty when the patch is discarded (a relevant outer level
// is zero, negative or NaN).
struct Patch {
    std::vector<float> u, v;
    std::vector<uint32_t> indices;
};

class Tessellator {
public:
    Tessellator();
    ~Tessellator();
    void tessellate(const Mode& mode, const float outer[4], const float inner[2], Patch& out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wmgl::tess
