#include "core/tess/tessellator.h"

#include "tessellator.hpp"

namespace wmgl::tess {

struct Tessellator::Impl {
    CHWTessellator hw;
};

Tessellator::Tessellator() : impl_(std::make_unique<Impl>()) {}
Tessellator::~Tessellator() = default;

void Tessellator::tessellate(const Mode& mode, const float outer[4], const float inner[2], Patch& out) {
    out.u.clear();
    out.v.clear();
    out.indices.clear();
    const PIPE_TESSELLATOR_PARTITIONING partitioning = mode.spacing == GL_FRACTIONAL_ODD    ? PIPE_TESSELLATOR_PARTITIONING_FRACTIONAL_ODD
                                                       : mode.spacing == GL_FRACTIONAL_EVEN ? PIPE_TESSELLATOR_PARTITIONING_FRACTIONAL_EVEN
                                                                                            : PIPE_TESSELLATOR_PARTITIONING_INTEGER;
    // GL's domain is D3D's mirrored, so GL's clockwise is D3D's counter-clockwise
    // (Mesa's draw module passes GL's ccw flag as the D3D cw flag too).
    const PIPE_TESSELLATOR_OUTPUT_PRIMITIVE primitive = mode.pointMode                  ? PIPE_TESSELLATOR_OUTPUT_POINT
                                                        : mode.domain == GL_ISOLINES    ? PIPE_TESSELLATOR_OUTPUT_LINE
                                                        : mode.cw                       ? PIPE_TESSELLATOR_OUTPUT_TRIANGLE_CCW
                                                                                        : PIPE_TESSELLATOR_OUTPUT_TRIANGLE_CW;
    CHWTessellator& hw = impl_->hw;
    hw.Init(partitioning, primitive);
    switch (mode.domain) {
    case GL_QUADS: hw.TessellateQuadDomain(outer[0], outer[1], outer[2], outer[3], inner[0], inner[1]); break;
    case GL_ISOLINES: hw.TessellateIsoLineDomain(outer[0], outer[1]); break;
    default: hw.TessellateTriDomain(outer[0], outer[1], outer[2], inner[0]); break;
    }
    const int points = hw.GetPointCount();
    if (points <= 0) return;
    const DOMAIN_POINT* p = hw.GetPoints();
    out.u.resize(static_cast<size_t>(points));
    out.v.resize(static_cast<size_t>(points));
    for (int i = 0; i < points; ++i) {
        out.u[static_cast<size_t>(i)] = p[i].u;
        out.v[static_cast<size_t>(i)] = p[i].v;
    }
    if (mode.pointMode) {
        out.indices.resize(static_cast<size_t>(points));
        for (int i = 0; i < points; ++i) out.indices[static_cast<size_t>(i)] = static_cast<uint32_t>(i);
        return;
    }
    const int count = hw.GetIndexCount();
    const int* indices = hw.GetIndices();
    out.indices.assign(indices, indices + count);
}

}  // namespace wmgl::tess
