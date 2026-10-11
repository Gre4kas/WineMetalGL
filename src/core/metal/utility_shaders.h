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

// Fullscreen triangle. With params.x = 0, uv.y grows towards the top of the
// target, so texture row 0 lands at the bottom: a GL image (row 0 = bottom)
// keeps its orientation on a drawable. params.x = 1 mirrors it vertically.
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

// Framebuffer blits. The fragment's window position is the destination pixel
// in GL coordinates (GL row 0 = Metal row 0); it is mapped into the source
// rectangle. dst/src are (x0, y0, x1, y1); srcSize.z = 1 for linear filtering.
struct BlitParams {
    float4 dst;
    float4 src;
    float4 srcSize;
};
static float2 wmgl_blit_source(float2 p, constant BlitParams& b) {
    float2 t = (p - b.dst.xy) / (b.dst.zw - b.dst.xy);
    return b.src.xy + t * (b.src.zw - b.src.xy);
}
template <typename T, typename Tex>
static vec<T, 4> wmgl_blit_read(Tex source, float2 s, constant BlitParams& b) {
    int2 c = int2(floor(s));
    if (any(c < 0) || any(c >= int2(b.srcSize.xy))) discard_fragment();
    return source.read(uint2(c));
}
fragment float4 wmgl_blit_float_fs(ClearOut in [[stage_in]], texture2d<float> source [[texture(0)]],
                                   sampler linearSampler [[sampler(0)]], constant BlitParams& b [[buffer(0)]]) {
    float2 s = wmgl_blit_source(in.position.xy, b);
    if (b.srcSize.z > 0.5) return source.sample(linearSampler, s / b.srcSize.xy);
    return wmgl_blit_read<float>(source, s, b);
}
fragment float4 wmgl_blit_float_ms_fs(ClearOut in [[stage_in]], texture2d_ms<float> source [[texture(0)]],
                                      constant BlitParams& b [[buffer(0)]]) {
    int2 c = int2(floor(wmgl_blit_source(in.position.xy, b)));
    if (any(c < 0) || any(c >= int2(b.srcSize.xy))) discard_fragment();
    float4 sum = 0;
    uint n = source.get_num_samples();
    for (uint i = 0; i < n; ++i) sum += source.read(uint2(c), i);
    return sum / float(n);
}
fragment int4 wmgl_blit_int_fs(ClearOut in [[stage_in]], texture2d<int> source [[texture(0)]],
                               constant BlitParams& b [[buffer(0)]]) {
    return wmgl_blit_read<int>(source, wmgl_blit_source(in.position.xy, b), b);
}
fragment int4 wmgl_blit_int_ms_fs(ClearOut in [[stage_in]], texture2d_ms<int> source [[texture(0)]],
                                  constant BlitParams& b [[buffer(0)]]) {
    int2 c = int2(floor(wmgl_blit_source(in.position.xy, b)));
    if (any(c < 0) || any(c >= int2(b.srcSize.xy))) discard_fragment();
    return source.read(uint2(c), 0);
}
fragment uint4 wmgl_blit_uint_fs(ClearOut in [[stage_in]], texture2d<uint> source [[texture(0)]],
                                 constant BlitParams& b [[buffer(0)]]) {
    return wmgl_blit_read<uint>(source, wmgl_blit_source(in.position.xy, b), b);
}
fragment uint4 wmgl_blit_uint_ms_fs(ClearOut in [[stage_in]], texture2d_ms<uint> source [[texture(0)]],
                                    constant BlitParams& b [[buffer(0)]]) {
    int2 c = int2(floor(wmgl_blit_source(in.position.xy, b)));
    if (any(c < 0) || any(c >= int2(b.srcSize.xy))) discard_fragment();
    return source.read(uint2(c), 0);
}
struct BlitDepthOut {
    float depth [[depth(any)]];
};
fragment BlitDepthOut wmgl_blit_depth_fs(ClearOut in [[stage_in]], depth2d<float> source [[texture(0)]],
                                         constant BlitParams& b [[buffer(0)]]) {
    int2 c = int2(floor(wmgl_blit_source(in.position.xy, b)));
    if (any(c < 0) || any(c >= int2(b.srcSize.xy))) discard_fragment();
    return BlitDepthOut{source.read(uint2(c))};
}
// Stencil into multisampled targets: values (R16Uint) holds the new stencil
// per pixel from p.xy, values above 0xff keep the target. p.z == 0 passes
// every written pixel (the zeroing pass), else only those with bit p.z set.
fragment void wmgl_blit_stencil_bit_fs(ClearOut in [[stage_in]], texture2d<uint> values [[texture(0)]],
                                       constant uint4& p [[buffer(0)]]) {
    uint v = values.read(uint2(int2(in.position.xy) - int2(p.xy))).r;
    if (v > 0xffu || (p.z != 0u && (v & p.z) == 0u)) discard_fragment();
}
fragment BlitDepthOut wmgl_blit_depth_ms_fs(ClearOut in [[stage_in]], depth2d_ms<float> source [[texture(0)]],
                                            constant BlitParams& b [[buffer(0)]]) {
    int2 c = int2(floor(wmgl_blit_source(in.position.xy, b)));
    if (any(c < 0) || any(c >= int2(b.srcSize.xy))) discard_fragment();
    return BlitDepthOut{source.read(uint2(c), 0)};
}

// The texels of one 2D slice of a compressed image as float4 (glGetTexImage
// of S3TC/BPTC/ETC2/EAC data): the hardware decode at each texel centre.
kernel void wmgl_decode_texels(texture2d<float> source [[texture(0)]], device float4* out [[buffer(0)]],
                               constant uint2& size [[buffer(1)]], uint2 id [[thread_position_in_grid]]) {
    if (id.x >= size.x || id.y >= size.y) return;
    constexpr sampler nearest(coord::normalized, filter::nearest, address::clamp_to_edge);
    out[id.y * size.x + id.x] = source.sample(nearest, (float2(id) + 0.5) / float2(size), level(0));
}

// GL_RGB32F/I/UI buffer texels (12 bytes) expanded to RGBA32 with alpha
// `one` (1.0f or 1, as bits): params.x = texel count, params.y = one.
kernel void wmgl_expand_rgb32(device const uint* in [[buffer(0)]], device uint4* out [[buffer(1)]],
                              constant uint2& params [[buffer(2)]], uint t [[thread_position_in_grid]]) {
    if (t >= params.x) return;
    out[t] = uint4(in[3 * t], in[3 * t + 1], in[3 * t + 2], params.y);
}

// Geometry-shader strips as lists listing GL's last-vertex provoking vertex
// first (Metal flat-shades from a primitive's first vertex); each triangle is
// rotated, so its winding is unchanged. One thread per input primitive:
// params.x = index region per thread (slot indices and 0xFFFFFFFF restarts),
// params.y = output indices per thread, params.z = 1 for triangles and 0 for
// lines, params.w = thread count. Unused output entries repeat one vertex.
kernel void wmgl_strips_provoking_last(device const uint* in [[buffer(0)]], device uint* out [[buffer(1)]],
                                       constant uint4& params [[buffer(2)]], uint t [[thread_position_in_grid]]) {
    if (t >= params.w) return;
    device const uint* r = in + t * params.x;
    device uint* o = out + t * params.y;
    uint n = 0, run = 0, a = 0, b = 0, fill = t * (params.x / 2u);
    bool any = false;
    for (uint i = 0; i < params.x; ++i) {
        uint v = r[i];
        if (v == 0xFFFFFFFFu) { run = 0; continue; }
        if (!any) { fill = v; any = true; }
        if (params.z != 0u) {
            if (run >= 2u) {
                o[n++] = v;
                o[n++] = (run & 1u) != 0u ? b : a;
                o[n++] = (run & 1u) != 0u ? a : b;
            }
        } else if (run >= 1u) {
            o[n++] = v;
            o[n++] = b;
        }
        a = b;
        b = v;
        ++run;
    }
    for (; n < params.y; ++n) o[n] = fill;
}
)MSL";

}  // namespace wmgl
