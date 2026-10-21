#include "core/shader/border.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <set>

namespace wmgl::shader {
namespace {

// `call` (a sample_compare or gather_compare) with its reference argument,
// the one that holds `reference` (divided by q for a projective lookup),
// clamped to [0, 1]; empty if it is not found.
std::string clampedReference(const std::string& call, const std::string& reference) {
    size_t open = call.find("sample_compare(");
    if (open == std::string::npos) open = call.find("gather_compare(");
    if (open == std::string::npos) return {};
    open = call.find('(', open);
    // The top-level arguments: sampler, coordinates, [layer,] reference, ...
    std::vector<std::pair<size_t, size_t>> args;
    int depth = 0;
    size_t start = open + 1;
    for (size_t i = start; i < call.size(); ++i) {
        const char c = call[i];
        if (c == '(' || c == '[') ++depth;
        else if ((c == ')' || c == ']') && depth > 0) --depth;
        else if ((c == ',' || c == ')') && depth == 0) {
            args.push_back({start, i});
            if (c == ')') break;
            start = i + 1;
        }
    }
    for (size_t k = 2; k < args.size(); ++k) {
        const auto [from, to] = args[k];
        if (std::string(call, from, to - from).find(reference) == std::string::npos) continue;
        std::string out = call;
        out.insert(to, ")");
        out.insert(from, "saturate(");
        return out;
    }
    return {};
}

// The emulation's MSL helpers (one copy per variant function).
//
// A macro's arguments: r, the sampled or gathered value; t, s, the texture
// and sampler; c, the coordinates as GL gives them (an array's layer last);
// o, the texel offset (0: none); for samples lk (0: implicit level of
// detail, 1: explicit la, 2: implicit plus bias la, 3: gradients gx, gy)
// and d, the depth reference; for gathers k, the component.
//
// The value stays the hardware's while the filter footprint is inside the
// image along every CLAMP_TO_BORDER axis (unless filter has 32: then every
// lookup is filtered here, with the LOD bias). Otherwise nearest filtering gives
// the border colour, and linear filtering is done here from texel reads,
// with the border colour for texels outside the image (and the texture's
// other wrap modes); a gather takes the border colour for its texels outside
// the image.
constexpr const char* kLibrary = R"MSL(
// ---- WineMetalGL: clamp-to-border emulation (core/shader/border.h)
struct wmgl_bp {
    uint axes;     // bits 0-2: S, T, R wrap CLAMP_TO_BORDER
    uint filter;   // 1: magnification linear, 2: minification linear, 4: mipmaps nearest, 8: linear, 16: rectangle,
                   // 32: every lookup (LOD bias), not only those at the border
    uint wrap;     // 4 bits per axis: 0 border, 1 repeat, 2 mirrored repeat, 3 clamp to edge, 4 mirror clamp to edge
    uint cmp;      // compare function (GL_NEVER + n), 8: none
    float minLod, maxLod, lodBias;
};
inline int3 wmgl_off(int o) { return int3(o, 0, 0); }
inline int3 wmgl_off(int2 o) { return int3(o, 0); }
inline int3 wmgl_off(int3 o) { return o; }
inline float3 wmgl_f3(int) { return float3(0.0); }
inline float3 wmgl_f3(float v) { return float3(v, 0.0, 0.0); }
inline float3 wmgl_f3(float2 v) { return float3(v, 0.0); }
inline float3 wmgl_f3(float3 v) { return v; }
// Spatial coordinates, their count (w) and the layer.
template<typename T> inline float4 wmgl_uvw(texture2d<T>, float c) { return float4(c, 0.0, 0.0, 1.0); }
template<typename T> inline float4 wmgl_uvw(texture2d<T>, float2 c) { return float4(c, 0.0, 2.0); }
template<typename T> inline float4 wmgl_uvw(texture2d_array<T>, float2 c) { return float4(c.x, 0.0, 0.0, 1.0); }
template<typename T> inline float4 wmgl_uvw(texture2d_array<T>, float3 c) { return float4(c.xy, 0.0, 2.0); }
template<typename T> inline float4 wmgl_uvw(texture3d<T>, float3 c) { return float4(c, 3.0); }
inline float4 wmgl_uvw(depth2d<float>, float c) { return float4(c, 0.0, 0.0, 1.0); }
inline float4 wmgl_uvw(depth2d<float>, float2 c) { return float4(c, 0.0, 2.0); }
inline float4 wmgl_uvw(depth2d_array<float>, float2 c) { return float4(c.x, 0.0, 0.0, 1.0); }
inline float4 wmgl_uvw(depth2d_array<float>, float3 c) { return float4(c.xy, 0.0, 2.0); }
inline float wmgl_layer(float) { return 0.0; }
inline float wmgl_layer(float2 c) { return c.y; }
inline float wmgl_layer(float3 c) { return c.z; }
template<typename T> inline uint wmgl_layers(texture2d<T>) { return 1u; }
template<typename T> inline uint wmgl_layers(texture3d<T>) { return 1u; }
inline uint wmgl_layers(depth2d<float>) { return 1u; }
template<typename T> inline uint wmgl_layers(texture2d_array<T> t) { return t.get_array_size(); }
inline uint wmgl_layers(depth2d_array<float> t) { return t.get_array_size(); }
template<typename T, typename C> inline uint wmgl_layer_of(T t, C c) {
    return wmgl_layers(t) > 1u ? uint(clamp(floor(wmgl_layer(c) + 0.5), 0.0, float(wmgl_layers(t) - 1u))) : 0u;
}
template<typename T> inline float3 wmgl_size(texture2d<T> t, uint l) { return float3(t.get_width(l), t.get_height(l), 1.0); }
template<typename T> inline float3 wmgl_size(texture2d_array<T> t, uint l) { return float3(t.get_width(l), t.get_height(l), 1.0); }
template<typename T> inline float3 wmgl_size(texture3d<T> t, uint l) {
    return float3(t.get_width(l), t.get_height(l), t.get_depth(l));
}
inline float3 wmgl_size(depth2d<float> t, uint l) { return float3(t.get_width(l), t.get_height(l), 1.0); }
inline float3 wmgl_size(depth2d_array<float> t, uint l) { return float3(t.get_width(l), t.get_height(l), 1.0); }
template<typename T> inline vec<T, 4> wmgl_read(texture2d<T> t, int3 i, uint, uint l) { return t.read(uint2(i.xy), l); }
template<typename T> inline vec<T, 4> wmgl_read(texture2d_array<T> t, int3 i, uint a, uint l) { return t.read(uint2(i.xy), a, l); }
template<typename T> inline vec<T, 4> wmgl_read(texture3d<T> t, int3 i, uint, uint l) { return t.read(uint3(i), l); }
inline float wmgl_read(depth2d<float> t, int3 i, uint, uint l) { return t.read(uint2(i.xy), l); }
inline float wmgl_read(depth2d_array<float> t, int3 i, uint a, uint l) { return t.read(uint2(i.xy), a, l); }
#if WMGL_BORDER_FRAGMENT
template<typename T> inline float wmgl_auto_lod(texture2d<T> t, sampler s, float4 c) { return t.calculate_unclamped_lod(s, c.xy); }
template<typename T> inline float wmgl_auto_lod(texture2d_array<T> t, sampler s, float4 c) { return t.calculate_unclamped_lod(s, c.xy); }
template<typename T> inline float wmgl_auto_lod(texture3d<T> t, sampler s, float4 c) { return t.calculate_unclamped_lod(s, c.xyz); }
inline float wmgl_auto_lod(depth2d<float> t, sampler s, float4 c) { return t.calculate_unclamped_lod(s, c.xy); }
inline float wmgl_auto_lod(depth2d_array<float> t, sampler s, float4 c) { return t.calculate_unclamped_lod(s, c.xy); }
#else
template<typename T> inline float wmgl_auto_lod(T, sampler, float4) { return 0.0; }
#endif
inline bool wmgl_integer(int) { return true; }
inline bool wmgl_integer(uint) { return true; }
inline bool wmgl_integer(float) { return false; }
inline float wmgl_compare(uint f, float ref, float texel) {
    switch (f) {
    case 0u: return 0.0;
    case 1u: return ref < texel ? 1.0 : 0.0;
    case 2u: return ref == texel ? 1.0 : 0.0;
    case 3u: return ref <= texel ? 1.0 : 0.0;
    case 4u: return ref > texel ? 1.0 : 0.0;
    case 5u: return ref != texel ? 1.0 : 0.0;
    case 6u: return ref >= texel ? 1.0 : 0.0;
    default: return 1.0;
    }
}
// A texel index after the axis' wrap mode; -1 outside a border axis.
inline int wmgl_wrap(int i, int size, uint mode) {
    switch (mode) {
    case 1u: return ((i % size) + size) % size;
    case 2u: { const int m = ((i % (2 * size)) + 2 * size) % (2 * size); return m < size ? m : 2 * size - 1 - m; }
    case 3u: return clamp(i, 0, size - 1);
    case 4u: return clamp(i < 0 ? -1 - i : i, 0, size - 1);
    default: return i >= 0 && i < size ? i : -1;
    }
}
// The fraction of the filter footprint inside the image along the border axes.
inline float wmgl_inside(float4 c, int3 o, float3 size, uint axes, bool linear, bool rect) {
    float w = 1.0;
    for (uint a = 0u; a < uint(c.w); ++a) {
        if (!(axes & (1u << a))) continue;
        float x = (rect ? c[a] : c[a] * size[a]) + float(o[a]);
        if (linear) {
            x -= 0.5;
            const float i0 = floor(x), f = x - i0;
            w *= (i0 >= 0.0 && i0 < size[a] ? 1.0 - f : 0.0) + (i0 + 1.0 >= 0.0 && i0 + 1.0 < size[a] ? f : 0.0);
        } else {
            const float i = floor(x);
            w *= i >= 0.0 && i < size[a] ? 1.0 : 0.0;
        }
    }
    return w;
}
// Linear filtering at one level from texel reads (taps outside a border axis
// take `border`). Compare: each texel compared with ref first.
template<typename T, typename V> inline V wmgl_linear(T t, float4 c, int3 o, uint layer, uint l, constant wmgl_bp& p, V border,
                                                    float ref) {
    const bool rect = (p.filter & 16u) != 0u;
    const float3 size = wmgl_size(t, l);
    const uint dims = uint(c.w);
    float3 x = (rect ? c.xyz : c.xyz * size) + float3(o) - 0.5;
    const float3 base = floor(x), a = x - base;
    V sum = V(0);
    for (uint corner = 0u; corner < (1u << dims); ++corner) {
        int3 i = int3(base);
        float weight = 1.0;
        bool inside = true;
        for (uint axis = 0u; axis < dims; ++axis) {
            const bool high = (corner >> axis) & 1u;
            i[axis] += high ? 1 : 0;
            weight *= high ? a[axis] : 1.0 - a[axis];
            const int wrapped = wmgl_wrap(i[axis], int(size[axis]), (p.wrap >> (4u * axis)) & 15u);
            inside = inside && wrapped >= 0;
            i[axis] = max(wrapped, 0);
        }
        if (dims < 2u) i.y = 0;
        if (dims < 3u) i.z = 0;
        V texel = border;
        if (inside) texel = V(wmgl_read(t, i, layer, l));
        sum += weight * texel;
    }
    return sum;
}
template<typename T> inline float wmgl_linear_compare(T t, float4 c, int3 o, uint layer, uint l, constant wmgl_bp& p,
                                                      float border, float ref) {
    const bool rect = (p.filter & 16u) != 0u;
    const float3 size = wmgl_size(t, l);
    const uint dims = uint(c.w);
    float3 x = (rect ? c.xyz : c.xyz * size) + float3(o) - 0.5;
    const float3 base = floor(x), a = x - base;
    float sum = 0.0;
    for (uint corner = 0u; corner < (1u << dims); ++corner) {
        int3 i = int3(base);
        float weight = 1.0;
        bool inside = true;
        for (uint axis = 0u; axis < dims; ++axis) {
            const bool high = (corner >> axis) & 1u;
            i[axis] += high ? 1 : 0;
            weight *= high ? a[axis] : 1.0 - a[axis];
            const int wrapped = wmgl_wrap(i[axis], int(size[axis]), (p.wrap >> (4u * axis)) & 15u);
            inside = inside && wrapped >= 0;
            i[axis] = max(wrapped, 0);
        }
        if (dims < 2u) i.y = 0;
        sum += weight * wmgl_compare(p.cmp, ref, inside ? float(wmgl_read(t, i, layer, l)) : border);
    }
    return sum;
}
// Nearest filtering at one level from a texel read (outside a border axis:
// `border`).
template<typename T, typename V> inline V wmgl_nearest(T t, float4 c, int3 o, uint layer, uint l, constant wmgl_bp& p,
                                                     V border) {
    const bool rect = (p.filter & 16u) != 0u;
    const float3 size = wmgl_size(t, l);
    const uint dims = uint(c.w);
    int3 i = int3(floor((rect ? c.xyz : c.xyz * size) + float3(o)));
    for (uint axis = 0u; axis < dims; ++axis) {
        const int wrapped = wmgl_wrap(i[axis], int(size[axis]), (p.wrap >> (4u * axis)) & 15u);
        if (wrapped < 0) return border;
        i[axis] = wrapped;
    }
    if (dims < 2u) i.y = 0;
    if (dims < 3u) i.z = 0;
    return V(wmgl_read(t, i, layer, l));
}
// Levels and filter (GL 4.6 section 8.14).
struct wmgl_lod_choice { uint l0, l1; float f; bool linear; };
template<typename T> inline wmgl_lod_choice wmgl_choose(T t, sampler s, float4 c, int lk, float la, float3 gx, float3 gy,
                                                      constant wmgl_bp& p) {
    const bool rect = (p.filter & 16u) != 0u;
    float lod = la;
    if (lk == 3) {
        const float3 size = rect ? float3(1.0) : wmgl_size(t, 0u);
        lod = log2(max(length(gx * size), length(gy * size)));
    } else if (lk != 1) {
        lod = wmgl_auto_lod(t, s, c) + (lk == 2 ? la : 0.0);
    }
    lod = clamp(lod + p.lodBias, p.minLod, p.maxLod);
    const uint top = rect ? 0u : t.get_num_mip_levels() - 1u;
    wmgl_lod_choice r = {0u, 0u, 0.0, false};
    if (lod <= 0.0) {
        r.linear = (p.filter & 1u) != 0u;
    } else {
        r.linear = (p.filter & 2u) != 0u;
        if (p.filter & 4u) {
            r.l0 = r.l1 = lod <= 0.5 ? 0u : min(uint(ceil(lod + 0.5)) - 1u, top);
        } else if (p.filter & 8u) {
            r.l0 = min(uint(floor(lod)), top);
            r.l1 = min(r.l0 + 1u, top);
            r.f = r.l0 == r.l1 ? 0.0 : fract(lod);
        }
    }
    return r;
}
template<typename T, typename C, typename B>
inline vec<B, 4> wmgl_border_sample(vec<B, 4> r, T t, sampler s, C cc, int3 o, int lk, float la, float3 gx, float3 gy, float,
                                    constant wmgl_bp& p, vec<B, 4> border) {
    const float4 c = wmgl_uvw(t, cc);
    const bool rect = (p.filter & 16u) != 0u;
    const wmgl_lod_choice q = wmgl_choose(t, s, c, lk, la, gx, gy, p);
    float w = wmgl_inside(c, o, wmgl_size(t, q.l0), p.axes, q.linear, rect);
    if (q.f > 0.0) w = mix(w, wmgl_inside(c, o, wmgl_size(t, q.l1), p.axes, q.linear, rect), q.f);
    const bool always = (p.filter & 32u) != 0u;
    if (w >= 1.0 && !always) return r;
    const uint layer = wmgl_layer_of(t, cc);
    if (always && (wmgl_integer(B(0)) || (!q.linear && q.f == 0.0)))  // one level, nearest
        return wmgl_nearest(t, c, o, layer, q.l0, p, border);
    if (!q.linear && q.f == 0.0) return border;
    if (wmgl_integer(B(0))) return w > 0.5 ? r : border;  // integer textures are never filtered
    if (!q.linear)  // NEAREST_MIPMAP_LINEAR: the nearest texels of two levels, blended
        return vec<B, 4>(mix(wmgl_nearest(t, c, o, layer, q.l0, p, float4(border)),
                             wmgl_nearest(t, c, o, layer, q.l1, p, float4(border)), q.f));
    float4 v = float4(wmgl_linear(t, c, o, layer, q.l0, p, float4(border), 0.0));
    if (q.f > 0.0) v = mix(v, float4(wmgl_linear(t, c, o, layer, q.l1, p, float4(border), 0.0)), q.f);
    return vec<B, 4>(v);
}
// Shadow lookups: the comparison of the border's depth.
template<typename T, typename C>
inline float wmgl_border_sample(float r, T t, sampler s, C cc, int3 o, int lk, float la, float3 gx, float3 gy, float ref,
                                constant wmgl_bp& p, float4 border) {
    const float4 c = wmgl_uvw(t, cc);
    const bool rect = (p.filter & 16u) != 0u;
    const wmgl_lod_choice q = wmgl_choose(t, s, c, lk, la, gx, gy, p);
    float w = wmgl_inside(c, o, wmgl_size(t, q.l0), p.axes, q.linear, rect);
    if (q.f > 0.0) w = mix(w, wmgl_inside(c, o, wmgl_size(t, q.l1), p.axes, q.linear, rect), q.f);
    const bool always = (p.filter & 32u) != 0u;
    if (w >= 1.0 && !always) return r;
    const uint layer = wmgl_layer_of(t, cc);
    if (always && !q.linear && q.f == 0.0) return wmgl_compare(p.cmp, ref, wmgl_nearest(t, c, o, layer, q.l0, p, border.x));
    if (!q.linear && q.f == 0.0) return wmgl_compare(p.cmp, ref, border.x);
    if (!q.linear)
        return mix(wmgl_compare(p.cmp, ref, wmgl_nearest(t, c, o, layer, q.l0, p, border.x)),
                   wmgl_compare(p.cmp, ref, wmgl_nearest(t, c, o, layer, q.l1, p, border.x)), q.f);
    float v = wmgl_linear_compare(t, c, o, layer, q.l0, p, border.x, ref);
    if (q.f > 0.0) v = mix(v, wmgl_linear_compare(t, c, o, layer, q.l1, p, border.x, ref), q.f);
    return v;
}
// Gathers (level 0): the four texels i0j1, i1j1, i1j0, i0j0.
inline bool4 wmgl_gather_outside(float4 c, int3 o, float3 size, uint axes, bool rect) {
    const float2 x = (rect ? c.xy : c.xy * size.xy) + float2(o.xy) - 0.5;
    const float2 i0 = floor(x), i1 = i0 + 1.0;
    const bool ox0 = (axes & 1u) && (i0.x < 0.0 || i0.x >= size.x), ox1 = (axes & 1u) && (i1.x < 0.0 || i1.x >= size.x);
    const bool oy0 = (axes & 2u) && (i0.y < 0.0 || i0.y >= size.y), oy1 = (axes & 2u) && (i1.y < 0.0 || i1.y >= size.y);
    return bool4(ox0 || oy1, ox1 || oy1, ox1 || oy0, ox0 || oy0);
}
template<typename T, typename C, typename B>
inline vec<B, 4> wmgl_border_gather(vec<B, 4> r, T t, C cc, int3 o, int k, float, constant wmgl_bp& p, vec<B, 4> border) {
    const bool4 out = wmgl_gather_outside(wmgl_uvw(t, cc), o, wmgl_size(t, 0u), p.axes, (p.filter & 16u) != 0u);
    return select(r, vec<B, 4>(border[clamp(k, 0, 3)]), out);
}
template<typename T, typename C>
inline float4 wmgl_border_gather(float4 r, T t, C cc, int3 o, int, float ref, constant wmgl_bp& p, float4 border, bool) {
    const bool4 out = wmgl_gather_outside(wmgl_uvw(t, cc), o, wmgl_size(t, 0u), p.axes, (p.filter & 16u) != 0u);
    return select(r, float4(wmgl_compare(p.cmp, ref, border.x)), out);
}
// ---- end of clamp-to-border emulation
)MSL";

const char* kSampleArgs = "(r, t, s, c, o, lk, la, gx, gy, d)";

std::string hexBits(uint32_t bits) {
    char text[16];
    std::snprintf(text, sizeof(text), "0x%08xu", bits);
    return text;
}

}  // namespace

void BorderMsl::renameMetalFunctionObjects() {
    // metal_functional's class templates.
    static const char* const kNames[] = {"plus", "minus", "multiplies", "divides", "modulus", "negate", "equal_to",
                                         "not_equal_to", "greater", "less", "greater_equal", "less_equal", "logical_and",
                                         "logical_or", "logical_not", "bit_and", "bit_or", "bit_xor", "bit_not"};
    auto rename = [](std::string& alias) {
        // glslang names functions with their signature ("plus(").
        const std::string base = alias.substr(0, alias.find('('));
        for (const char* n : kNames)
            if (base == n) {
                alias = base + "0";
                return;
            }
    };
    ir.for_each_typed_id<spirv_cross::SPIRFunction>([&](uint32_t self, spirv_cross::SPIRFunction&) {
        if (spirv_cross::Meta* meta = ir.find_meta(self)) rename(meta->decoration.alias);
    });
    ir.for_each_typed_id<spirv_cross::SPIRType>([&](uint32_t self, spirv_cross::SPIRType&) {
        if (spirv_cross::Meta* meta = ir.find_meta(self)) rename(meta->decoration.alias);
    });
}

std::string BorderMsl::to_texture_op(const spirv_cross::Instruction& i, bool sparse, bool* forward,
                                     spirv_cross::SmallVector<uint32_t>& inherited_expressions) {
    std::string expr = CompilerMSL::to_texture_op(i, sparse, forward, inherited_expressions);
    const uint32_t* ops = stream(i);
    // Apple GPUs take gradientcube's gradients wrongly on some faces (+Z with
    // an x gradient samples level 0): textureGrad on cube maps samples the
    // level of the gradients projected onto the face instead (GL 4.6 section
    // 8.13, isotropically).
    if (const size_t g = expr.find("gradientcube(");
        g != std::string::npos && static_cast<spv::Op>(i.op) == spv::OpImageSampleExplicitLod && i.length >= 7) {
        const uint32_t mask = ops[4];
        if ((mask & spv::ImageOperandsGradMask) && !(mask & (spv::ImageOperandsBiasMask | spv::ImageOperandsLodMask))) {
            const std::string args = to_expression(ops[5]) + ", " + to_expression(ops[6]) + ")";
            if (expr.compare(g + 13, args.size(), args) == 0) {
                const std::string p = to_enclosed_expression(ops[3]) + ".xyz";
                expr.replace(g, 13 + args.size(),
                             "level(wmgl_cube_lod(" + p + ", " + args.substr(0, args.size() - 1) + ", float(" +
                                 to_expression(ops[2]) + ".get_width())))");
            }
        }
    }
    if (sparse) return expr;
    bool gather = false, dref = false, proj = false;
    switch (static_cast<spv::Op>(i.op)) {
    case spv::OpImageSampleImplicitLod: case spv::OpImageSampleExplicitLod: break;
    case spv::OpImageSampleDrefImplicitLod: case spv::OpImageSampleDrefExplicitLod: dref = true; break;
    case spv::OpImageSampleProjImplicitLod: case spv::OpImageSampleProjExplicitLod: proj = true; break;
    case spv::OpImageSampleProjDrefImplicitLod: case spv::OpImageSampleProjDrefExplicitLod: proj = dref = true; break;
    case spv::OpImageGather: gather = true; break;
    case spv::OpImageDrefGather: gather = dref = true; break;
    default: return expr;
    }
    const uint32_t img = ops[2], coord = ops[3];
    const spirv_cross::SPIRVariable* var = maybe_get_backing_variable(img);
    if (!var) return expr;
    auto slot = textureSlots.find(var->self);
    if (slot == textureSlots.end() && var->basevariable) slot = textureSlots.find(var->basevariable);
    if (slot == textureSlots.end()) return expr;
    const spirv_cross::SPIRType& type = expression_type(img);
    const spv::Dim dim = type.image.dim;
    if (type.image.ms || (dim != spv::Dim1D && dim != spv::Dim2D && dim != spv::Dim3D && dim != spv::DimRect)) return expr;
    uint32_t at = 4, drefId = 0, componentId = 0;
    if (dref) drefId = ops[at++];
    else if (gather) componentId = ops[at++];
    const uint32_t mask = at < i.length ? ops[at++] : 0;
    uint32_t bias = 0, lod = 0, gradX = 0, gradY = 0, offset = 0;
    if (mask & spv::ImageOperandsBiasMask) bias = ops[at++];
    if (mask & spv::ImageOperandsLodMask) lod = ops[at++];
    if (mask & spv::ImageOperandsGradMask) {
        gradX = ops[at++];
        gradY = ops[at++];
    }
    if (mask & spv::ImageOperandsConstOffsetMask) offset = ops[at++];
    if (mask & spv::ImageOperandsOffsetMask) offset = ops[at++];
    if (mask & spv::ImageOperandsConstOffsetsMask) return expr;  // textureGatherOffsets: not emulated
    // The coordinates the image takes (an array's layer last): a depth
    // comparison's coordinates may carry the reference too.
    static const char* const kComponents[] = {"x", "y", "z", "w"};
    static const char* const kPrefix[] = {"", ".x", ".xy", ".xyz", ".xyzw"};
    const uint32_t size = expression_type(coord).vecsize;
    const uint32_t n = (dim == spv::Dim1D ? 1 : dim == spv::Dim3D ? 3 : 2) + (type.image.arrayed ? 1 : 0);
    std::string c = to_enclosed_expression(coord), q;
    if (proj) {
        if (size <= n || type.image.arrayed) return expr;
        q = c + "." + kComponents[size - 1];
        c = "(" + c + kPrefix[n] + " / " + q + ")";
    } else if (size > n) {
        c += kPrefix[n];
    }
    const std::string slotText = std::to_string(slot->second);
    if (drefId)
        if (const std::string clamped = clampedReference(expr, to_expression(drefId)); !clamped.empty())
            expr = "wmgl_bcr_" + slotText + "(" + expr + ", " + clamped + ")";
    const std::string tex = to_expression(img), sampler = to_sampler_expression(img);
    const std::string off = offset ? to_expression(offset) : "0";
    // A projective comparison divides the reference by q too.
    const std::string ref = !drefId ? "0.0" : proj ? "(" + to_enclosed_expression(drefId) + " / " + q + ")" : to_expression(drefId);
    if (gather)
        return "wmgl_bcg_" + slotText + "(" + expr + ", " + tex + ", " + sampler + ", " + c + ", " + off + ", " +
               (componentId ? to_expression(componentId) : "0") + ", " + ref + ")";
    const char* kind = lod ? "1" : bias ? "2" : gradX ? "3" : "0";
    const std::string la = lod ? to_expression(lod) : bias ? to_expression(bias) : "0.0";
    const std::string gx = gradX ? to_expression(gradX) : "0", gy = gradY ? to_expression(gradY) : "0";
    return "wmgl_bcs_" + slotText + "(" + expr + ", " + tex + ", " + sampler + ", " + c + ", " + off + ", " + kind + ", " + la +
           ", " + gx + ", " + gy + ", " + ref + ")";
}

void addDefaultBorderMacros(std::string& msl) {
    if (msl.find("wmgl_cube_lod(") != std::string::npos) {
        // The level of cube-map gradients (BorderMsl::to_texture_op): the
        // derivatives of the face coordinates (sc / |ma|, tc / |ma|).
        static const char* const kCubeLod =
            "\nstatic inline float wmgl_cube_lod(float3 p, float3 dx, float3 dy, float size) {\n"
            "    const float3 a = abs(p);\n"
            "    const bool xm = a.x >= a.y && a.x >= a.z, ym = !xm && a.y >= a.z;\n"
            "    const float3 q = xm ? p.zyx : ym ? p.xzy : p;\n"
            "    const float3 qx = xm ? dx.zyx : ym ? dx.xzy : dx, qy = xm ? dy.zyx : ym ? dy.xzy : dy;\n"
            "    const float2 ux = (qx.xy * q.z - q.xy * qx.z) / (q.z * q.z), uy = (qy.xy * q.z - q.xy * qy.z) / (q.z * q.z);\n"
            "    return log2(max(length(ux), length(uy)) * 0.5 * size);\n"
            "}\n";
        const std::string anchor = "using namespace metal;\n";
        const size_t at = msl.find(anchor);
        msl.insert(at == std::string::npos ? 0 : at + anchor.size(), kCubeLod);
    }
    std::set<std::string> names;
    for (const char* prefix : {"wmgl_bcs_", "wmgl_bcg_", "wmgl_bcr_"})
        for (size_t at = msl.find(prefix); at != std::string::npos; at = msl.find(prefix, at + 1)) {
            size_t end = at + std::strlen(prefix);
            while (end < msl.size() && msl[end] >= '0' && msl[end] <= '9') ++end;
            if (end < msl.size() && msl[end] == '(') names.insert(msl.substr(at, end - at));
        }
    if (names.empty()) return;
    std::string defines = "\n// Clamp-to-border emulation off (core/shader/border.h).\n";
    for (const std::string& name : names) defines += "#define " + name + "(r, ...) (r)\n";
    const std::string anchor = "using namespace metal;\n";
    const size_t at = msl.find(anchor);
    if (at == std::string::npos) msl.insert(0, defines);
    else msl.insert(at + anchor.size(), defines);
}

std::string borderDefinitions(const std::vector<BorderSlot>& slots, bool fragment) {
    std::string out = fragment ? "#define WMGL_BORDER_FRAGMENT 1\n" : "#define WMGL_BORDER_FRAGMENT 0\n";
    for (const BorderSlot& s : slots) {
        const std::string n = std::to_string(s.texture);
        const uint32_t filter = (s.magLinear ? 1u : 0u) | (s.minLinear ? 2u : 0u) | (s.mip == 1 ? 4u : s.mip == 2 ? 8u : 0u) |
                                (s.rect ? 16u : 0u) | (s.always ? 32u : 0u);
        char params[192];
        std::snprintf(params, sizeof(params), "{%uu, %uu, %uu, %uu, %.9g, %.9g, %.9g}", s.axes, filter, s.wrap,
                      s.compare ? s.compare - 0x0200u : 8u, s.minLod, s.maxLod, s.lodBias);
        const std::string p(params);
        const std::string type = s.kind == 'i' ? "int4" : s.kind == 'u' ? "uint4" : "float4";
        const std::string element = s.kind == 'i' ? "int" : s.kind == 'u' ? "uint" : "float";
        std::string border = type + "(";
        for (int k = 0; k < 4; ++k)
            border += (k ? ", " : "") + std::string("as_type<") + element + ">(" + hexBits(s.border[k]) + ")";
        border += ")";
        if (s.clampReference) out += "#define wmgl_bcr_" + n + "(r, rc) (rc)\n";
        if (!s.emulate) continue;
        const std::string d = s.clampReference ? "saturate(d)" : "d";
        out += "constant wmgl_bp wmgl_bp_" + n + " = " + p + ";\n";
        out += "#define wmgl_bcs_" + n + kSampleArgs + " wmgl_border_sample(r, t, s, c, wmgl_off(o), lk, la, wmgl_f3(gx), "
               "wmgl_f3(gy), " + d + ", wmgl_bp_" + n + ", " + border + ")\n";
        out += "#define wmgl_bcg_" + n + "(r, t, s, c, o, k, d) wmgl_border_gather(r, t, c, wmgl_off(o), k, " + d + ", wmgl_bp_" +
               n + ", " + border + (s.compare ? ", true" : "") + ")\n";
    }
    return out;
}

std::string mslWithBorders(const std::string& msl, const std::string& definitions) {
    std::string out = msl;
    // Drop the default definitions of the macros the variant defines.
    for (size_t at = definitions.find("#define wmgl_bc"); at != std::string::npos;
         at = definitions.find("#define wmgl_bc", at + 1)) {
        const size_t open = definitions.find('(', at);
        const std::string name = definitions.substr(at + 8, open - at - 8);
        const std::string line = "#define " + name + "(r, ...) (r)\n";
        if (const size_t found = out.find(line); found != std::string::npos) out.erase(found, line.size());
    }
    const std::string anchor = "using namespace metal;\n";
    const size_t at = out.find(anchor);
    const std::string text = std::string(kLibrary).insert(0, definitions.substr(0, definitions.find('\n') + 1)) +
                             definitions.substr(definitions.find('\n') + 1);
    if (at == std::string::npos) out.insert(0, text);
    else out.insert(at + anchor.size(), text);
    return out;
}

namespace {
std::mutex variantsMutex;
std::map<std::string, uint16_t> variantIds;
std::vector<std::string> variants;
}  // namespace

uint16_t borderVariantId(const std::string& definitions) {
    std::lock_guard<std::mutex> lock(variantsMutex);
    if (auto it = variantIds.find(definitions); it != variantIds.end()) return it->second;
    if (variants.size() >= 0xfffe) return 0;  // past 65534 distinct borders: emulation off
    variants.push_back(definitions);
    const uint16_t id = static_cast<uint16_t>(variants.size());
    variantIds.emplace(definitions, id);
    return id;
}

std::string borderVariantDefinitions(uint16_t id) {
    std::lock_guard<std::mutex> lock(variantsMutex);
    return id && id <= variants.size() ? variants[id - 1] : std::string();
}

}  // namespace wmgl::shader
