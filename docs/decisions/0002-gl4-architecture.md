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

## Arbitrary sampler border colours

**Question.** Metal's samplers have three border colours: transparent black, opaque black and opaque white. GL takes any colour, and integer textures take integer border colours. This is a GL 1.x feature, but the CTS counts the gap against 4.4: `texture_border_clamp` (integer formats) and the `gpu_shader5` clamp-to-border gathers.

**Decision.** Clamp-to-border is emulated in the shader, only for draws that need it.

1. **Wrapping (`src/core/shader/border.cpp`).** Every stage is translated by `BorderMsl`, a SPIRV-Cross subclass. It wraps each sample and gather of a non-arrayed 1D, 2D, 3D or rectangle texture in a macro named after the texture's Metal index: `wmgl_bcs_N` for samples, `wmgl_bcg_N` for gathers.
2. **Default.** The default definition keeps only the sampled value, so the common path costs nothing.
3. **Per draw.** The draw looks at the bound textures and samplers (`borderEmulation`). If an axis wraps `CLAMP_TO_BORDER` with a border Metal cannot give, the draw uses a function variant: the pipeline key's `borders`, a number from `borderVariantId`.
4. **What the variant does.** Its macros keep the hardware's value while the filter footprint stays inside the image. Otherwise:
   - nearest filtering returns the border;
   - linear filtering filters by hand from texel reads, using the border for texels outside the image and the texture's other wrap modes on the other axes;
   - gathers substitute the border texel by texel;
   - shadow lookups compare against the border's depth.
5. **Inputs to the variant.** The level of detail, the filters, the LOD clamps, the base-format mapping and the swizzle are baked into it.
6. **Coverage.** Vertex functions (including the transform feedback capture) and fragment functions, compute dispatches, and the geometry and tessellation control kernels all take variants. Rectangle textures, whose Metal samplers can only clamp to the edge, always use the emulation when they wrap to the border.

**Cost and limits.**
- One function variant per distinct set of borders, compiled on first use.
- Arrays of samplers, `textureGatherOffsets` and cube maps are not emulated. Cube maps filter seamlessly, so they have no border.
- Hand filtering ignores anisotropy at the image's edge.

## Draw parameters and indirect counts (GL 4.6)

**Question.** `gl_BaseVertex`, `gl_BaseInstance` and `gl_DrawID` have no Metal equivalent that holds GL's values. Metal's `[[base_vertex]]` is not GL's after WineMetalGL rebases or rewrites indices, and Metal draws have no index within a multi-draw.

**Decision.**
- The vertex shader's draw parameters (the GL 4.60 names, and the ARB names with `GL_ARB_shader_draw_parameters`) become hidden default-block uniforms, like `gl_DepthRange` (`lowerBuiltinUniforms`).
- The draw path fills them from the GL command:
  - the base vertex of an elements draw (0 for array draws);
  - the base instance;
  - the index within the multi-draw, set by `DrawState::DrawIdScope` around each draw of `glMultiDraw*`.
- `glMultiDraw*IndirectCount` reads the count from `PARAMETER_BUFFER` on the CPU, after any GPU write to it, as the other indirect draws read their commands. The count is clamped to `maxdrawcount`, and `maxdrawcount` commands must fit in the indirect buffer.

**Cost.** One default-uniform upload per draw, which every draw already makes. A read-back wait when the GPU wrote the parameter buffer.

## Pipeline statistics and transform feedback overflow (GL 4.6)

**Question.** Apple GPUs expose no pipeline statistics counters to Metal.

**Decision.** Count on the CPU from what the draw paths already know, and in the shader only where nothing else can count:
- **Vertices and primitives submitted**: per restart-delimited run of the command.
- **Vertex shader invocations**: one per submitted vertex. Metal can reuse transformed vertices of indexed draws, so the true count can be lower.
- **Tessellation control patches and evaluation invocations**: from the CPU tessellator's domain points.
- **Geometry shader invocations and emitted primitives**: from the emulation's kernel size and its read-back output.
- **Compute invocations**: from the dispatch size, read from the indirect buffer for indirect dispatches.
- **Clipping input**: the primitives reaching the rasterizer (submitted, tessellated, or emitted on stream 0).
- **Clipping output**: counts the clipping input, because Metal does not report what clipping does.
- **Fragment shader invocations**: a fragment function variant (pipeline key `countFragments`) increments a 32-bit counter at buffer 16. The counter is fresh for each command buffer the query spans, and the query sums them.
- **Overflow queries**: set when the capture clamps a stream's primitives to the space left in its buffers.

**Cost.** A pipeline variant only while a `FRAGMENT_SHADER_INVOCATIONS` query is active.

## Vertex-stage atomic counters and side effects with transform feedback

**Question.**
- A vertex function had one free buffer slot for atomic counter buffers, and GL 4.6 tests use two.
- Transform feedback runs the vertex stage in a capture pass before the draw, so a vertex shader that writes memory ran twice.

**Decision.**
- **Counter buffers.** A vertex function's counter buffers are entries of its storage block argument buffer (buffer 28), so `MAX_VERTEX_ATOMIC_COUNTER_BUFFERS` is 8.
- **Discarding draws.** With rasterizer discard, the capture pass is the only vertex-stage run, and the draw is skipped.
- **Rasterizing draws.** When the vertex stage writes memory (storage blocks, counters or images) and rasterizes, the capture pass rasterizes the assembled list of independent primitives itself, with the current state.
- **Fallback.** That capture-while-drawing path is skipped when something else rewrites the list: provoking-vertex rotation for flat inputs, polygon-mode points, wide lines, tessellation. Those draws keep two passes.

**Cost and limits.** Those remaining two-pass draws still run a memory-writing vertex shader twice.

## SPIR-V shaders (GL 4.6, `ARB_gl_spirv`)

**Question.** The compiler and every emulation (geometry, tessellation, transform feedback capture, fp64, border colours) start from GLSL. Should SPIR-V get its own path into Metal?

**Decision.** No. `glSpecializeShader` turns the module into GLSL, and the existing path takes it as application source (`src/core/shader/spirv_input.cpp`). The translation:
1. checks the entry point and specialization constant IDs (`INVALID_VALUE` otherwise);
2. writes the constants into the module;
3. emits GLSL 4.60 with SPIRV-Cross. A subclass declares GL atomic counters (`uniform atomic_uint`, the `atomicCounter*` functions) and `gl_PatchVerticesIn`, which SPIRV-Cross's GLSL lacks.

Naming:
- SPIR-V matches stage interfaces by location; GLSL by name.
- Every inter-stage variable is named after its location (`wmgl_spv_<stage>_l<location>`).
- At link time, a stage's inputs are renamed after the closest earlier stage's outputs.
- Unnamed resources are named after their location or binding.
- Program queries report those names as empty, and every name of a module without debug names (`spirvDisplayName`).

The program binary keeps the translated GLSL, as for GLSL programs.

**Cost and limits.**
- A translation per `glSpecializeShader`.
- Transform feedback varyings of a SPIR-V program report empty names even when the module names them.
- Modules are not validated beyond what the translation and the GLSL compiler reject. `GL_SPIR_V_EXTENSIONS` lists the extensions the translation handles; others are refused.

## Image uniforms past Metal's read_write limit

**Question.** A Metal function takes at most 8 `read_write` textures, and images were all declared `read_write`. Programs (and the CTS) use more images per stage than that, mostly ones they only read or only write. Should images move into an argument buffer?

**Decision.** No. A SPIR-V pass (`inferStorageImageAccess`) decorates every image a stage only reads `NonWritable`, and every image it only writes `NonReadable`, so they become `access::read` and `access::write` textures. Atomics, images passed to functions and any other use keep both accesses. A stage binds up to 16 images (textures 16..31), at most 8 of them `read_write`. Image units are binding points only, so `GL_MAX_IMAGE_UNITS` is 16. The per-stage image uniform limits stay 8, since 8 images that are both read and written are all a stage can be guaranteed.

**Cost and limits.**
- A stage with more than 8 images that are each read and written fails to link.
- An argument buffer of images was tried. Format views of one texture then lost writes (`shader_image_load_store.multiple-uniforms`), so it was dropped.
