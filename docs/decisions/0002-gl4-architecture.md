# 0002: GL 4.4–4.6 architecture

- **Status:** accepted, extended as the 3.0.0 work lands
- **Date:** 2026-10-11
- **Context:** milestone M3 (`feat/gl46`): OpenGL 4.4, 4.5 and 4.6 on Metal, released as 3.0.0

Each section is one decision: what Metal lacks or offers, what WineMetalGL does, and what that costs.

## Direct state access (GL 4.5)

**Question.** Should the 97 GL 4.5 DSA commands get their own implementation per object type, or reuse the bind-to-edit code?

**Decision.** They reuse it. `src/core/api/dsa.mm`:
1. swaps the named object into a binding with `BindingSwap` (the copy-write buffer, the active unit's texture target, the draw/read framebuffer, the renderbuffer, the vertex array, the transform feedback object, the query buffer);
2. calls the existing command;
3. restores the binding.

The errors only the named commands have (the GL 4.5 rules on names, targets and default objects) are checked first, so the older CTS lists keep their behaviour.

**Cost.** Two binding writes per call. No draw-time cost.

## Cull distances (GL 4.5, `ARB_cull_distance`)

**Question.** Metal has clip distances but no cull distances. A primitive must be dropped only when one distance is negative at all of its vertices. A per-fragment test of the interpolated distance would clip instead.

**Options.**
- MSL 4's per-vertex fragment inputs (`vertex_value<T>`) are exact, but need macOS 26; the floor is macOS 15.
- Routing every cull-distance draw through the compute geometry emulation is exact but slow.

**Decision.** An interpolated indicator.
- SPIRV-Cross masks the `gl_CullDistance` output into a local array (`mask_stage_output_by_builtin`).
- `addCullDistances` writes each element as the `[[user(cullN)]]` varying a fragment stage reads.
- It also writes a second varying, `[[user(wmgl_cullN)]]`, which is 0 where the distance is negative and 1e30 elsewhere.
- Interpolated (no perspective), that second varying is exactly 0 only where every vertex of the primitive is negative.
- The fragment variant `fragmentWithCullDistances` (pipeline key `cull`) discards those fragments.
- The geometry and tessellation emulations carry `gl_CullDistance` like `gl_ClipDistance`.
- `#extension GL_ARB_cull_distance` raises a shader to GLSL 4.50 (glslang has the built-in there only); a glslang preamble defines the macro.

**Cost and limits.**
- One extra varying per distance, and early fragment tests are lost for such draws.
- A fragment exactly on an edge or vertex whose vertices all have the distance negative is dropped with the primitive.
- `layout(early_fragment_tests)` shaders still update depth and stencil for culled primitives.
