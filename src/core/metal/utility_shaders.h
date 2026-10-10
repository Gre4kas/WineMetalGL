// Internal Metal shaders used by the core itself (present blit, clears,
// framebuffer blits). Compiled once per process from source.
#pragma once

namespace wmgl {

inline constexpr const char* kUtilityShaderSource = R"MSL(
#include <metal_stdlib>
using namespace metal;

struct FullscreenOut {
    float4 position [[position]];
    float2 uv;
};

// Fullscreen triangle. flip_y selects whether texture row 0 ends up at the
// top (present: GL bottom-up images onto a top-down drawable) or the bottom.
vertex FullscreenOut wmgl_fullscreen_vs(uint vid [[vertex_id]], constant float4& params [[buffer(0)]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    FullscreenOut out;
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    float2 uv = p;
    // Metal NDC +y is the top row. Row 0 of a GL image is its bottom row.
    out.uv = float2(uv.x, params.x > 0.5 ? 1.0 - uv.y : uv.y);
    // Scale/offset the source rectangle (params.zw = scale, unused here).
    return out;
}

fragment float4 wmgl_copy_fs(FullscreenOut in [[stage_in]], texture2d<float> source [[texture(0)]],
                             sampler s [[sampler(0)]]) {
    return source.sample(s, in.uv);
}

// Clear one colour attachment to a value (used for clears that cannot be a load action).
struct ClearOut {
    float4 position [[position]];
};
vertex ClearOut wmgl_clear_vs(uint vid [[vertex_id]], constant float4& depth [[buffer(0)]]) {
    float2 p = float2((vid << 1) & 2, vid & 2);
    ClearOut out;
    out.position = float4(p * 2.0 - 1.0, depth.x, 1.0);
    return out;
}
struct ClearColors {
    float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; float4 c3 [[color(3)]];
    float4 c4 [[color(4)]]; float4 c5 [[color(5)]]; float4 c6 [[color(6)]]; float4 c7 [[color(7)]];
};
fragment ClearColors wmgl_clear_float_fs(constant float4* colors [[buffer(0)]]) {
    return ClearColors{colors[0], colors[1], colors[2], colors[3], colors[4], colors[5], colors[6], colors[7]};
}
struct ClearIntColors {
    int4 c0 [[color(0)]]; int4 c1 [[color(1)]]; int4 c2 [[color(2)]]; int4 c3 [[color(3)]];
    int4 c4 [[color(4)]]; int4 c5 [[color(5)]]; int4 c6 [[color(6)]]; int4 c7 [[color(7)]];
};
fragment ClearIntColors wmgl_clear_int_fs(constant int4* colors [[buffer(0)]]) {
    return ClearIntColors{colors[0], colors[1], colors[2], colors[3], colors[4], colors[5], colors[6], colors[7]};
}
struct ClearUintColors {
    uint4 c0 [[color(0)]]; uint4 c1 [[color(1)]]; uint4 c2 [[color(2)]]; uint4 c3 [[color(3)]];
    uint4 c4 [[color(4)]]; uint4 c5 [[color(5)]]; uint4 c6 [[color(6)]]; uint4 c7 [[color(7)]];
};
fragment ClearUintColors wmgl_clear_uint_fs(constant uint4* colors [[buffer(0)]]) {
    return ClearUintColors{colors[0], colors[1], colors[2], colors[3], colors[4], colors[5], colors[6], colors[7]};
}
fragment void wmgl_clear_depth_only_fs() {}
)MSL";

}  // namespace wmgl
