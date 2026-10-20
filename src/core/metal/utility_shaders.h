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
    if (b.srcSize.z > 0.5) {
        // Destination pixels whose source is outside the read buffer stay as
        // they are, as with GL_NEAREST (sampling at the edges clamps).
        if (any(s < 0.0) || any(s >= b.srcSize.xy)) discard_fragment();
        return source.sample(linearSampler, s / b.srcSize.xy);
    }
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

// glReadPixels of RGBA8 (unsigned normalized) texels as GL_BGRA bytes into a
// pack buffer, in command order: params.xy = the region's origin, params.zw
// = its size; rowWords = the destination's row stride in 4-byte words.
kernel void wmgl_pack_bgra8(texture2d<float, access::read> source [[texture(0)]], device uint* out [[buffer(0)]],
                            constant uint4& params [[buffer(1)]], constant uint& rowWords [[buffer(2)]],
                            uint2 id [[thread_position_in_grid]]) {
    if (id.x >= params.z || id.y >= params.w) return;
    const float4 c = source.read(uint2(params.x + id.x, params.y + id.y));
    out[id.y * rowWords + id.x] = pack_float_to_unorm4x8(c.bgra);
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

// Metal tessellation factors (MTLTriangle/QuadTessellationFactorsHalf) from
// the levels in the tessellation control kernel's patch records: GL's outer
// levels are D3D's edges in the same order. params.x = record words,
// params.y = word of the outer levels in a record, params.z = patch count,
// params.w = bit 0 for quads, bit 1 for integer spacing, whose levels are
// rounded up first (to an integer, which half represents exactly).
kernel void wmgl_tess_factors(device const float* records [[buffer(0)]], device half* factors [[buffer(1)]],
                              constant uint4& params [[buffer(2)]], uint p [[thread_position_in_grid]]) {
    if (p >= params.z) return;
    device const float* l = records + p * params.x + params.y;
    const bool quads = (params.w & 1u) != 0u, integer = (params.w & 2u) != 0u;
    float level[6] = {l[0], l[1], l[2], l[3], l[4], l[5]};
    if (integer)
        for (uint i = 0; i < 6; ++i) level[i] = ceil(level[i]);
    if (quads) {
        device half* f = factors + p * 6u;
        for (uint i = 0; i < 6; ++i) f[i] = half(level[i]);
    } else {
        device half* f = factors + p * 4u;
        f[0] = half(level[0]);
        f[1] = half(level[1]);
        f[2] = half(level[2]);
        f[3] = half(level[4]);
    }
}
// GPU-driven multi-draws of patches (DrawState::drawPatchesIndirectGpu): the
// commands a culling dispatch wrote are expanded on the GPU into the vertex
// list a CPU read would build (drawPatchCommands), in command order, with
// each command's vertex count rounded down to whole patches.
struct MdiPatchParams {
    uint drawCount, strideWords, elementCount, patchVertices;
    uint capacity;   // list positions the buffers hold (a multiple of patchVertices)
    uint indexSize;  // 2 or 4
    uint groups;
    uint reportSlot;  // scan: the word of `report` that gets the unclamped total
    uint controlGroupPatches;  // patches per control kernel threadgroup
    uint pad[3];
};
// A vertex attribute group (a buffer, stride and divisor whose attributes
// fit one element window): its window of each listed vertex (or instance)
// is copied to the gathered stream the capture draw fetches per vertex.
struct MdiGroup {
    uint srcBase, srcStride, divisor, windowWords;
    uint dstWords, srcLength, pad0, pad1;
};

static uint wmgl_sat_add(uint a, uint b) { uint r = a + b; return r < a ? 0xffffffffu : r; }

static uint wmgl_mdi_vertices(device const uint* c, constant MdiPatchParams& p) {
    const uint count = c[0], instances = c[1], first = c[2];
    if (count == 0u || instances == 0u || first > p.elementCount || count > p.elementCount - first) return 0u;
    const ulong n = ulong(count / p.patchVertices * p.patchVertices) * ulong(instances);
    return n > 0xffffffffu ? 0xffffffffu : uint(n);
}

// One threadgroup. starts[c]: the first list position of command c
// (starts[drawCount] = the total). args (words): 0 capture draw
// (MTLDrawPrimitivesIndirectArguments), 4 control dispatch and 8 factor
// dispatch (MTLDispatchThreadgroupsIndirectArguments), 12 expand dispatch,
// 16 patch draw (MTLDrawPatchIndirectArguments), 20 the total before the
// capacity clamp (read back to grow the capacity).
kernel void wmgl_mdi_patch_scan(device const uint* commands [[buffer(0)]], device uint* starts [[buffer(1)]],
                                device uint* args [[buffer(2)]], device uint* capture [[buffer(3)]],
                                device uint* control [[buffer(4)]], device uint* factors [[buffer(5)]],
                                constant MdiPatchParams& p [[buffer(6)]], device uint* report [[buffer(7)]],
                                uint tid [[thread_index_in_threadgroup]],
                                uint threads [[threads_per_threadgroup]], uint lane [[thread_index_in_simdgroup]],
                                uint simd [[simdgroup_index_in_threadgroup]], uint simds [[simdgroups_per_threadgroup]]) {
    threadgroup uint simdBase[32];
    threadgroup uint grandTotal;
    const uint n = p.drawCount;
    const uint per = (n + threads - 1u) / threads;
    const uint begin = min(n, tid * per), end = min(n, begin + per);
    uint local = 0u;
    for (uint c = begin; c < end; ++c) local = wmgl_sat_add(local, wmgl_mdi_vertices(commands + c * p.strideWords, p));
    const uint prefix = simd_prefix_exclusive_sum(local);
    const uint sum = simd_sum(local);
    if (lane == 0u) simdBase[simd] = sum;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    if (simd == 0u) {
        const uint v = lane < simds ? simdBase[lane] : 0u;
        const uint e = simd_prefix_exclusive_sum(v);
        const uint t = simd_sum(v);
        if (lane < simds) simdBase[lane] = e;
        if (lane == 0u) grandTotal = t;
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint running = simdBase[simd] + prefix;
    for (uint c = begin; c < end; ++c) {
        starts[c] = running;
        running = wmgl_sat_add(running, wmgl_mdi_vertices(commands + c * p.strideWords, p));
    }
    if (tid == 0u) {
        const uint total = grandTotal;
        starts[n] = total;
        const uint kept = min(total, p.capacity) / p.patchVertices * p.patchVertices;
        const uint patches = kept / p.patchVertices;
        args[0] = kept; args[1] = 1u; args[2] = 0u; args[3] = 0u;
        args[4] = (patches + p.controlGroupPatches - 1u) / p.controlGroupPatches; args[5] = 1u; args[6] = 1u;
        args[8] = (patches + 63u) / 64u; args[9] = 1u; args[10] = 1u;
        args[12] = (kept + 63u) / 64u; args[13] = 1u; args[14] = 1u;
        args[16] = patches; args[17] = 1u; args[18] = 0u; args[19] = 0u;
        args[20] = total;
        report[p.reportSlot] = total;
        capture[1] = kept;  // the vertex capture's vertex count and slot limit
        capture[2] = kept;
        control[1] = patches;
        factors[2] = patches;
    }
}

// One thread per list position: its command, vertex index (written to the
// capture's vertex id list, word 12 on) and the windows of its attribute
// groups (per-instance groups: the command's base instance on).
kernel void wmgl_mdi_patch_expand(device const uint* commands [[buffer(0)]], device const uint* starts [[buffer(1)]],
                                  device const uchar* elements [[buffer(2)]], device uint* capture [[buffer(3)]],
                                  device uint* gathered [[buffer(4)]], constant MdiPatchParams& p [[buffer(5)]],
                                  constant MdiGroup* groups [[buffer(6)]], device const uchar* s0 [[buffer(7)]],
                                  device const uchar* s1 [[buffer(8)]], device const uchar* s2 [[buffer(9)]],
                                  device const uchar* s3 [[buffer(10)]], device const uchar* s4 [[buffer(11)]],
                                  device const uchar* s5 [[buffer(12)]], device const uchar* s6 [[buffer(13)]],
                                  device const uchar* s7 [[buffer(14)]], uint k [[thread_position_in_grid]]) {
    if (k >= capture[1]) return;
    uint lo = 0u, hi = p.drawCount;  // starts[lo] <= k < starts[hi]
    while (hi - lo > 1u) {
        const uint mid = (lo + hi) / 2u;
        if (starts[mid] <= k) lo = mid;
        else hi = mid;
    }
    device const uint* c = commands + lo * p.strideWords;
    const uint perInstance = c[0] / p.patchVertices * p.patchVertices;
    const uint local = k - starts[lo];
    const uint instance = local / perInstance, i = c[2] + local % perInstance;
    const uint raw = p.indexSize == 2u ? uint(((device const ushort*)elements)[i]) : ((device const uint*)elements)[i];
    const uint index = uint(int(raw) + int(c[3]));
    capture[12u + k] = index;
    for (uint g = 0u; g < p.groups; ++g) {
        constant MdiGroup& group = groups[g];
        device const uchar* s = g == 0u ? s0 : g == 1u ? s1 : g == 2u ? s2 : g == 3u ? s3 : g == 4u ? s4 : g == 5u ? s5 : g == 6u ? s6 : s7;
        const uint element = group.divisor ? c[4] + instance / group.divisor : index;
        const ulong at = ulong(group.srcBase) + ulong(element) * ulong(group.srcStride);
        device uint* d = gathered + group.dstWords + k * group.windowWords;
        for (uint w = 0u; w < group.windowWords; ++w) {
            const ulong b = at + ulong(w) * 4ul;
            d[w] = b + 4ul <= ulong(group.srcLength) ? *((device const uint*)(s + b)) : 0u;
        }
    }
}
)MSL";

}  // namespace wmgl
