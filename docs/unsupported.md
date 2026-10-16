# Unsupported

What WineMetalGL does not support, and why. The cases that run, and the few
that fail, are in [Conformance results](conformance.md).

## OpenGL 4.x

Contexts stop at OpenGL 4.3: `wglCreateContextAttribsARB` refuses a request
for 4.4 or newer with `ERROR_INVALID_VERSION_ARB`, as it does for any version
an implementation does not offer. Some features of later versions are
available in 4.3 contexts as the extensions that were promoted into each
version:

<!-- BEGIN GENERATED: gl4 (scripts/docs/generate.py) -->
| Version | Features | Promoted extensions advertised | Not advertised |
|---|---|---|---|
| 4.0 | Tessellation, GLSL 4.00, double precision, indirect draws, per-sample shading | `GL_ARB_draw_buffers_blend`, `GL_ARB_draw_indirect`, `GL_ARB_gpu_shader5`, `GL_ARB_gpu_shader_fp64`, `GL_ARB_sample_shading`, `GL_ARB_shader_subroutine`, `GL_ARB_tessellation_shader`, `GL_ARB_texture_buffer_object_rgb32`, `GL_ARB_texture_cube_map_array`, `GL_ARB_texture_gather`, `GL_ARB_texture_query_lod`, `GL_ARB_transform_feedback2`, `GL_ARB_transform_feedback3` (13 of 13) | none |
| 4.1 | Separate shader objects, program binaries, viewport arrays, 64-bit attributes | `GL_ARB_ES2_compatibility`, `GL_ARB_get_program_binary`, `GL_ARB_separate_shader_objects`, `GL_ARB_shader_precision`, `GL_ARB_vertex_attrib_64bit`, `GL_ARB_viewport_array` (6 of 6) | none |
| 4.2 | Image load/store, atomic counters, immutable texture storage | `GL_ARB_base_instance`, `GL_ARB_compressed_texture_pixel_storage`, `GL_ARB_conservative_depth`, `GL_ARB_internalformat_query`, `GL_ARB_map_buffer_alignment`, `GL_ARB_shader_atomic_counters`, `GL_ARB_shader_image_load_store`, `GL_ARB_shading_language_420pack`, `GL_ARB_shading_language_packing`, `GL_ARB_texture_compression_bptc`, `GL_ARB_texture_storage`, `GL_ARB_transform_feedback_instanced` (12 of 12) | none |
| 4.3 | Compute shaders, shader storage buffers, debug output, texture views | `GL_ARB_arrays_of_arrays`, `GL_ARB_clear_buffer_object`, `GL_ARB_compute_shader`, `GL_ARB_copy_image`, `GL_ARB_ES3_compatibility`, `GL_ARB_explicit_uniform_location`, `GL_ARB_fragment_layer_viewport`, `GL_ARB_framebuffer_no_attachments`, `GL_ARB_internalformat_query2`, `GL_ARB_invalidate_subdata`, `GL_ARB_multi_draw_indirect`, `GL_ARB_program_interface_query`, `GL_ARB_shader_image_size`, `GL_ARB_shader_storage_buffer_object`, `GL_ARB_stencil_texturing`, `GL_ARB_texture_buffer_range`, `GL_ARB_texture_query_levels`, `GL_ARB_texture_storage_multisample`, `GL_ARB_texture_view`, `GL_ARB_vertex_attrib_binding`, `GL_KHR_debug` (21 of 22) | `GL_ARB_robust_buffer_access_behavior` |
| 4.4 | Buffer storage, multi-bind, texture clears, enhanced layouts | `GL_ARB_buffer_storage`, `GL_ARB_clear_texture`, `GL_ARB_enhanced_layouts`, `GL_ARB_multi_bind`, `GL_ARB_query_buffer_object`, `GL_ARB_texture_mirror_clamp_to_edge`, `GL_ARB_texture_stencil8`, `GL_ARB_vertex_type_10f_11f_11f_rev` (8 of 8) | none |
| 4.5 | Direct state access, clip control, robustness, cull distances | `GL_ARB_clip_control`, `GL_ARB_conditional_render_inverted`, `GL_ARB_cull_distance`, `GL_ARB_derivative_control`, `GL_ARB_direct_state_access`, `GL_ARB_ES3_1_compatibility`, `GL_ARB_get_texture_sub_image`, `GL_ARB_shader_texture_image_samples`, `GL_ARB_texture_barrier`, `GL_KHR_context_flush_control`, `GL_KHR_robustness` (11 of 11) | none |
| 4.6 | SPIR-V shaders, anisotropic filtering, indirect draw counts | `GL_ARB_gl_spirv`, `GL_ARB_indirect_parameters`, `GL_ARB_pipeline_statistics_query`, `GL_ARB_polygon_offset_clamp`, `GL_ARB_shader_atomic_counter_ops`, `GL_ARB_shader_draw_parameters`, `GL_ARB_shader_group_vote`, `GL_ARB_spirv_extensions`, `GL_ARB_texture_filter_anisotropic`, `GL_ARB_transform_feedback_overflow_query`, `GL_KHR_no_error` (11 of 11) | none |
<!-- END GENERATED: gl4 -->

### macOS 15

The evidence runs on macOS 26 and newer. On macOS 15:

- Texture LOD bias set on a texture or sampler (`GL_TEXTURE_LOD_BIAS`, and
  `glTexEnv`'s `GL_TEXTURE_FILTER_CONTROL`) cannot go into Metal's sampler:
  its LOD bias is macOS 26 API (macOS 15 has the setter, but its samplers
  ignore the value). Lookups through a biased sampler are filtered
  in the shader instead (the clamp-to-border emulation's path,
  `src/core/shader/border.h`), which applies the bias; they lose anisotropic
  filtering, and cube maps keep the unbiased lookup.

The GPU of macOS virtual machines on macOS 15 ("Apple Paravirtual device",
GitHub's macOS 15 runners) keeps only 16 bits of a draw's Metal vertex start
and base vertex (measured: a vertex start of 65536 and base vertices of
-32769 and 65536 draw from the wrong vertices). There, draws whose offsets
leave -32768 to 32767 take their vertex numbers from indices written on the
CPU instead.

### Double precision

Metal has no 64-bit floating-point type, so doubles (`GL_ARB_gpu_shader_fp64`,
`GL_ARB_vertex_attrib_64bit`) run in software: shaders keep the bit patterns in
64-bit integers and call an IEEE 754 binary64 library
(`src/core/shader/fp64.h`). Addition, subtraction, multiplication, division,
square root and every conversion are correctly rounded (to nearest, ties to
even), with subnormals, infinities and NaNs. Where results can differ from a
GPU with hardware doubles:

- `fma()` rounds twice (a multiply, then an add). GLSL allows this outside
  `precise` expressions; a `precise` fused multiply-add is not exact either.
- A double literal in the subnormal range (below about 2.2e-308) compiles to
  zero, because the shader compiler (glslang) flushes such literals.
  Subnormal values computed at run time or read from buffers, attributes and
  uniforms are kept.
- A NaN result is the first NaN operand made quiet, or the default NaN; other
  payload propagation rules do not apply.

Only shaders that declare doubles pay for this: each double operation is a
library call several times slower than a float operation.

## Compatibility profile (OpenGL 1.0 to 2.1)

WineMetalGL implements OpenGL 1.0 to 2.1 itself, as its compatibility
profile, which contexts get unless they request a core profile (together with
every 3.x feature). Fixed-function processing runs as generated shaders keyed
by the state that changes their code (`src/core/compat/fixed_function.mm`);
the state itself reaches them, and GLSL 1.x shaders that read
`gl_ModelViewMatrix`, `gl_LightSource` and the other built-ins, through one
uniform block. Display lists record every list-compilable command of
GL 1.0 to 2.1 with copied arguments.

ARB assembly programs (`GL_ARB_vertex_program`, `GL_ARB_fragment_program`,
`GL_ARB_fragment_program_shadow`) are translated to GLSL when they draw
(`src/core/compat/arb_program.cpp`), and linked with the generated
fixed-function code for a stage without a program. Generic attributes alias
the conventional ones as on NVIDIA drivers (normal 2, colours 3 and 4, fog
coordinate 5, texture coordinates 8 to 15) when only one of the two arrays is
enabled.

Not implemented. None of these is blocked by Metal: they were left out
because games rarely use them, and the game corpus needs none of them.
Commands without an implementation generate `GL_INVALID_OPERATION` and are
logged once.

| Feature | Version | Behaviour now | Why it is left out | What it would take |
|---|---|---|---|---|
| Vertex blending in ARB vertex programs (`vertex.weight`, `vertex.matrixindex`, `state.matrix.modelview[n]` past 0, `state.matrix.palette`) | Extensions | The program fails to load (`GL_INVALID_OPERATION`, with the error position and string) | `GL_ARB_vertex_blend` and `GL_ARB_matrix_palette` are not advertised, and games blend in their programs instead | The two extensions' state and their bindings in the translator |
| Accumulation buffer (`glAccum`) | 1.0 | `GL_INVALID_OPERATION` | Rare; a few old games use it for motion blur | An RGBA16F texture and blits |
| Selection and feedback (`glRenderMode`) | 1.0 | Nothing is drawn outside `GL_RENDER`, and no records are returned | Mostly CAD programs and old editors' picking | Transforming vertices on the CPU and writing the records |
| Evaluators (`glMap*`, `glEval*`) | 1.0 | `GL_INVALID_OPERATION` | Very rare in games | Generating the vertices on the CPU and drawing them as immediate mode |
| Line stipple | 1.0 | The pattern is stored and ignored | Rare | A distance-along-line value and a fragment discard |
| Wide lines and polygon stipple with application shaders | 1.0 | Drawn by the fixed-function programs only | Needs code added to the application's own shaders | Injecting the expansion and discard into linked programs |
| Face culling in `GL_POINT` polygon mode | 1.0 | The vertices of every triangle are drawn | Metal has no point polygon mode, and drawing vertices as points loses each triangle's facing | Computing facing per triangle before the points are expanded |
| Pixel maps, transfer scale and bias | 1.0 | Only the defaults apply; `glPixelMap*` generates `GL_INVALID_OPERATION` | Rare | A conversion step in the pixel-transfer path |
| `GL_POINT_FADE_THRESHOLD_SIZE` | 1.4 | Stored and not applied | A small gap, not yet done | Fading the alpha of small fixed-function points |
| Imaging subset (colour tables, convolution, histogram, colour matrix) | 1.2 | `GL_INVALID_OPERATION`; `GL_ARB_imaging` is not advertised. The colour matrix stack (`glMatrixMode(GL_COLOR)`, two deep) is kept and reads back, as the Steam overlay saves and restores it unchecked, but it does not transform pixels | Optional in the specification: it exists only when `GL_ARB_imaging` is advertised | Large, for almost no use |
| Colour-index mode | 1.0 | Unreachable | Wine's macOS driver offers only RGBA pixel formats, so no program can create a colour-index context | Nothing worth doing |

## OpenGL ES

OpenGL ES contexts are OpenGL ES 3.2 (`WINEMETALGL_ES_VERSION` lowers the
version they report). What they leave out:

| Feature | Behaviour | Why |
|---|---|---|
| Lines wider than 1 | `ALIASED_LINE_WIDTH_RANGE` is 1 to 1 | Optional in OpenGL ES; Metal draws 1-pixel lines only |
| HDR ASTC (`GL_KHR_texture_compression_astc_hdr`), 3D ASTC | Not advertised | LDR ASTC only |
| `GL_NV_fbo_color_attachments`, `GL_OVR_multiview`, `GL_EXT_multisampled_render_to_texture`, `GL_EXT_shader_framebuffer_fetch`, `GL_EXT_shader_group_vote`, `GL_EXT_sRGB_write_control`, `GL_EXT_fragment_shading_rate`, `GL_KHR_shader_subgroup`, `GL_ANDROID_extension_pack_es31a` | Not advertised | Not implemented yet; `GL_EXT_draw_buffers` covers several color attachments |
| Separate depth and stencil attachments | `FRAMEBUFFER_UNSUPPORTED` | Metal keeps depth and stencil in one texture; OpenGL ES allows the combination to be unsupported |
| More than 4 samples | `MAX_SAMPLES` is 4 | Apple GPUs render with 1, 2 or 4 samples |
| Arrays of matrices as shader stage inputs or outputs (`out mat3x2 m[2]`, in blocks too) | The program does not link | Metal stage inputs and outputs cannot hold them (SPIRV-Cross) |
| Arrays of atomic counters passed to functions | The shader does not compile | Atomic counters are lowered to storage buffer words, which function parameters cannot reference |
| Out-of-bounds reads of uniform block arrays in robust contexts | May return values from memory past the block instead of zero or in-range values | Metal does not bound these reads |

Apple GPUs shade the 2x2 pixel quads along an edge that two triangles of an
indexed draw share together, so derivatives (and the texture level of a
lookup) next to the edge mix both triangles' attributes. OpenGL ES draws whose
vertex attributes are all in client memory are drawn without indices, which
keeps each triangle's quads apart; draws from buffer objects keep the
hardware behaviour, which changes the level of a few pixels along such edges
only where the attributes' derivatives differ across them.

## Conformance notes

<!-- BEGIN GENERATED: totals-notes (scripts/docs/generate.py) -->
**Unsupported %** ([conformance totals](conformance.md#conformance-totals)): cases the CTS reports NotSupported because WineMetalGL does not offer what they test (an extension it does not advertise, or a limit above the GL minimum), over the cases any implementation can run. Contexts are OpenGL 4.6, the newest version; the unsupported cases are optional extensions ([OpenGL 4.x](#opengl-4x)).

Failing cases: `KHR-GL30.api.coverage`, `KHR-GL40.internalformat.renderbuffer.rgb9_e5`, `KHR-GL41.internalformat.renderbuffer.rgb9_e5`, `KHR-GL42.internalformat.renderbuffer.rgb9_e5`, `KHR-GL42.texture_view.view_sampling`, `KHR-GL43.clear_tex_image.negative_gl_clear_tex_and_sub_image_compressed_texture`, `KHR-GL43.internalformat.renderbuffer.rgb9_e5`, `KHR-GL43.texture_view.view_sampling`, `KHR-GL46.clear_tex_image.negative_gl_clear_tex_and_sub_image_compressed_texture`, `KHR-GL46.internalformat.renderbuffer.rgb9_e5`, `KHR-GL46.texture_view.view_sampling`. [Conformance results](conformance.md) gives the reason for each.
<!-- END GENERATED: totals-notes -->

## CTS cases reported NotSupported

<!-- BEGIN GENERATED: notsupported (scripts/docs/generate.py) -->
Every NotSupported case in a committed result matches one entry of `tests/cts/notsupported.json`. `extension` and `version` cases test something WineMetalGL does not offer; `test-design` cases are skipped by the test itself on every implementation.

| Pattern | Kind | KHR-GL30 | KHR-GL31 | KHR-GL32 | KHR-GL33 | KHR-GL40 | KHR-GL41 | KHR-GL42 | KHR-GL43 | KHR-GL46 | Reason |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `KHR-GL4?.sparse_texture_tests.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1089 | 1089 | GL_ARB_sparse_texture is not advertised. |
| `KHR-GL4?.sparse_buffer_tests.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 82 | 82 | GL_ARB_sparse_buffer is not advertised. |
| `KHR-GL4?.blend_equation_advanced.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 88 | 88 | GL_KHR_blend_equation_advanced is not advertised. |
| `KHR-GL4?.shader_ballot_tests.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 4 | 4 | GL_ARB_shader_ballot and GL_ARB_gpu_shader_int64 are not advertised. |
| `KHR-GL4?.clear_tex_image.*` | version | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | The cases need GL 4.4 or GL_ARB_clear_texture, which is not advertised. |
| `KHR-GL4?.indirect_parameters_tests.*` | version | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | The cases need GL 4.6 or GL_ARB_indirect_parameters, which is not advertised. |
| `KHR-GL4?.texture_buffer_size_clamping.*` | version | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | The cases need GL 4.5 or GL_ARB_direct_state_access, which is not advertised. |
| `KHR-GL4?.texture_query_lod.*` | test-design | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 3 | 3 | The cases need a render target of at least 256 x 256 (gl4cTextureQueryLodTests.cpp, TextureQueryLodBaseTest::iterate); the mustpass configurations use 64 x 64 surfaces, so they report NotSupported on every implementation. |
| `KHR-GL4?.fragment_shading_rate.*` | extension | 0 | 0 | 0 | 0 | 1067 | 1067 | 1067 | 1067 | 1067 | GL_EXT_fragment_shading_rate is not advertised. |
| `KHR-GL4?.texture_view.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | GL_ARB_texture_view (GL 4.3) is not advertised. |
| `KHR-GL4?.texture_barrier_ARB.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | GL_ARB_texture_barrier (GL 4.5) is not advertised. |
| `KHR-GL4?.shader_subroutine.arrays_of_arrays_of_uniforms` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | GL_ARB_arrays_of_arrays (GL 4.3) is not advertised. |
| `KHR-GL4?.shader_image_load_store.basic-allFormats-loadStoreComputeStage` | version | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | The case uses compute shaders (GL 4.3, GL_ARB_compute_shader), which are not advertised. |
| `KHR-GL4?.shader_image_load_store.*-ms` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 1 | The case needs GL_MAX_IMAGE_SAMPLES of 4 or more. WineMetalGL reports 0, the GL minimum: Metal shaders cannot write multisample textures. |
| `KHR-GL4?.shader_image_size.*-ms-*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 36 | 36 | The case needs GL_MAX_IMAGE_SAMPLES of 4 or more. WineMetalGL reports 0, the GL minimum: Metal shaders cannot write multisample textures. |
| `KHR-GL4?.shader_image_load_store.advanced-sso-perSample` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | 1 | The case needs GL_MAX_IMAGE_SAMPLES of 4 or more. WineMetalGL reports 0, the GL minimum: Metal shaders cannot write multisample textures. |
| `KHR-GL[34]?.shaders.shader_integer_mix.prototypes` | version | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 1 | 1 | Without `#extension GL_EXT_shader_integer_mix` the case needs GLSL 4.50 (glcShaderIntegerMixTests.cpp, ShaderIntegerMixPrototypesCase). |
| `KHR-GL4?.draw_elements_base_vertex_tests.valid_active_tf` | test-design | 0 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 1 | The case applies only to OpenGL ES contexts and reports NotSupported on every desktop context (esextcDrawElementsBaseVertexTests.cpp, DrawElementsBaseVertexNegativeActiveTransformFeedbackTest). |
| `KHR-GL[34]?.cull_distance.*` | extension | 0 | 0 | 0 | 16 | 16 | 16 | 16 | 16 | 16 | GL_ARB_cull_distance is not advertised. |
| `KHR-GL[34]?.ext_texture_shadow_lod.*` | extension | 15 | 15 | 15 | 0 | 0 | 0 | 0 | 0 | 0 | GL_EXT_texture_shadow_lod is not advertised. |
| `KHR-GL[34]?.transform_feedback_overflow_query_ARB.*` | extension | 0 | 0 | 13 | 0 | 0 | 0 | 0 | 0 | 0 | GL_ARB_transform_feedback_overflow_query is not advertised. |
| `KHR-GL[34]?.pipeline_statistics_query_tests_ARB.*` | extension | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | GL_ARB_pipeline_statistics_query is not advertised. |
| `KHR-GL[34]?.texture_filter_minmax_tests.*` | extension | 0 | 0 | 0 | 5 | 5 | 5 | 5 | 5 | 5 | GL_ARB_texture_filter_minmax is not advertised. |
| `KHR-GL3?.transform_feedback2.*` | extension | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | GL_ARB_transform_feedback2 is not advertised. |
| `KHR-GL3?.transform_feedback3.*` | extension | 3 | 3 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | GL_ARB_transform_feedback3 is not advertised. |
| `KHR-GL3?.gpu_shader5_gl.*` | extension | 0 | 0 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | GL_ARB_gpu_shader5 is not advertised. |
| `KHR-GL3?.shaders.shader_integer_mix.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | GL_EXT_shader_integer_mix is not advertised. |
| `KHR-GL[34]?.pixelstoragemodes.compressedteximage*.rgba_astc_*` | extension | 0 | 0 | 0 | 80 | 80 | 80 | 80 | 80 | 80 | ASTC formats need GL_KHR_texture_compression_astc_ldr, which is not advertised. |
| `KHR-GL[34]?.packed_depth_stencil.stencil_texturing.*` | version | 0 | 0 | 2 | 2 | 2 | 2 | 2 | 0 | 0 | Stencil texturing needs GL 4.3 (GL_ARB_stencil_texturing is not advertised). |
| `KHR-GL[34]?.CommonBugs.CommonBug_PerVertexValidation` | version | 0 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | 0 | The case runs only on OpenGL 4.1 or newer contexts. |
| `KHR-GL[34]?.texture_swizzle.*` | test-design | 0 | 0 | 0 | 263 | 263 | 263 | 249 | 241 | 192 | The case pairs a texture target with an access routine or format that the test marks invalid (gl3cTextureSwizzleTests.cpp isTargetSuppByAccess/isTargetSuppByFormat, or a format whose m_minimum_gl_context is above 3.3), so it reports NotSupported on every implementation. |
| `KHR-GL3[012].frag_coord_conventions.multisample` | extension | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | Below a GL 3.3 context the case needs GL_ARB_sample_shading (glcFragCoordConventionsTests.cpp), which is not advertised; KHR-GL33 runs it. |
| `KHR-GL31.clip_distance.*` | test-design | 0 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | KHR-GL31 creates a forward-compatible 3.1 context, and glu::contextSupports (gluRenderContext.cpp) matches a forward-compatible context only against its exact version, so the case's check for a GL 3.0 context fails (glcClipDistance.cpp); KHR-GL30, KHR-GL32 and KHR-GL33 run it. |
| `KHR-GL31.texture_lod_bias.texture_lod_bias_all` | test-design | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | KHR-GL31 creates a forward-compatible 3.1 context, and glu::contextSupports (gluRenderContext.cpp) matches a forward-compatible context only against its exact version, so the case's check for a GL 3.0 context fails (glcTextureLodBiasTests.cpp); KHR-GL30, KHR-GL32 and KHR-GL33 run it. |
| `KHR-GL4?.sparse_texture2_tests.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1204 | GL_ARB_sparse_texture2 is not advertised. |
| `KHR-GL4?.sparse_texture_clamp_tests.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 575 | GL_ARB_sparse_texture_clamp is not advertised. |
| `KHR-GL4?.negative_texture_lookup_functions_with_bias_tests.sparseTexture*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 27 | The sparseTexture*ARB lookups need GL_ARB_sparse_texture2, which is not advertised. |
| `KHR-GL4?.post_depth_coverage_tests.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | GL_ARB_post_depth_coverage is not advertised: Metal has no post-depth-test sample coverage input. |
| `KHR-GL4?.shader_texture_image_samples_tests.image_functional_test` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | The case needs GL_MAX_IMAGE_SAMPLES above 0. WineMetalGL reports 0, the GL minimum: Metal shaders cannot write multisample textures. |
| `KHR-GL4?.*sample_variables.*samples_8*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 76 | The case needs 8 samples; GL_MAX_SAMPLES is 4, the most every color and depth format takes on the Metal device. |
| `KHR-GL4?.tessellation_shader.tessellation_shader_tessellation.max_in_out_attributes` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | The case needs GL_MAX_VERTEX_OUTPUT_COMPONENTS to cover GL_MAX_TESS_CONTROL_INPUT_COMPONENTS (esextcTessellationShaderTessellation.cpp, TessellationShaderTessellationMaxInOut::initTest). WineMetalGL reports the GL minimums, 64 and 128. |
| `KHR-GL4?.texture_cube_map_array.generate_mip_map_non_filterable_*` | test-design | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | The cases run only in OpenGL ES contexts and report NotSupported on every desktop context (esextcTextureCubeMapArrayGenerateMipMap.cpp). |

OpenGL ES lists (cases NotSupported in any list of the version):

| Pattern | Kind | ES 2.0 | ES 3.0 | ES 3.1 | ES 3.2 | Reason |
|---|---|---:|---:|---:|---:|---|
| `KHR-GLES2.texture_3d.compressed_texture.*` | extension | 30 | 0 | 0 | 0 | The cases need HDR ASTC (GL_KHR_texture_compression_astc_hdr), which OpenGL ES contexts do not advertise (LDR ASTC only). |
| `KHR-GLES2.core.internalformat.renderbuffer.stencil_index[14]_oes` | extension | 2 | 0 | 0 | 0 | GL_OES_stencil1 and GL_OES_stencil4 are not advertised. |
| `KHR-GLES2.core.internalformat.copy_tex_image.rgb10` | test-design | 1 | 0 | 0 | 0 | OpenGL ES copies only into formats whose component sizes match the source's; the test reports NotSupported for a non-renderable format with other sizes on every implementation (glcInternalformatTests.cpp, CopyTexImageCase). |
| `KHR-GLES3.cull_distance.functional_test_item_8_primitive_mode_[lt]*` | test-design | 0 | 16 | 0 | 0 | Item 8 fetches cull distances only for points; the test reports NotSupported for lines and triangles on every implementation (glcCullDistance.cpp, CullDistance::FunctionalTest::iterate). |
| `KHR-GLES3.packed_depth_stencil.stencil_texturing.*` | version | 0 | 2 | 0 | 0 | KHR-GLES3 creates OpenGL ES 3.0 contexts; stencil texturing needs OpenGL ES 3.1. |
| `KHR-GLES3.shaders.shader_integer_mix.prototypes` | test-design | 0 | 1 | 0 | 0 | Without `#extension GL_EXT_shader_integer_mix` the case needs GLSL ES 3.10; KHR-GLES3 compiles it as GLSL ES 3.00 (glcShaderIntegerMixTests.cpp, ShaderIntegerMixPrototypesCase). |
| `dEQP-GLES2.functional.fbo.completeness.attachment_combinations.*` | extension | 54 | 0 | 0 | 0 | dEQP-GLES2 combines several color attachments only with GL_NV_fbo_color_attachments (es2fFboCompletenessTests.cpp), which is not advertised. |
| `dEQP-GLES2.functional.multisample.*` | test-design | 16 | 0 | 0 | 0 | The cases render to the default framebuffer and need it multisampled; configuration 1 (`rgba8888d24s8ms0`) has no samples, so they report NotSupported on every implementation. |
| `dEQP-GLES3.functional.multisample.[cd]*` | test-design | 0 | 16 | 0 | 0 | The cases need a multisampled default framebuffer; configuration 1 (`rgba8888d24s8ms0`) has none, so they report NotSupported on every implementation. |
| `dEQP-GLES*.functional.rasterization.*wide*` | extension | 9 | 20 | 0 | 0 | OpenGL ES contexts support line widths up to 1 (ALIASED_LINE_WIDTH_RANGE); wider lines are optional. |
| `dEQP-GLES*.functional.clipping.line.*wide*` | extension | 8 | 8 | 0 | 0 | OpenGL ES contexts support line widths up to 1 (ALIASED_LINE_WIDTH_RANGE); wider lines are optional. |
| `dEQP-GLES[23].functional.shaders.fragdata.*` | test-design | 2 | 3 | 0 | 0 | Dynamic indexing of gl_FragData is optional in GLSL ES 1.00 (and the 3.00 cases use it through ES 1.00 shaders); the test reports NotSupported unless the implementation's own capability flag is set (glsShaderLibraryCase.cpp). |
| `dEQP-GLES2.functional.multisampled_render_to_texture.*` | extension | 1 | 0 | 0 | 0 | GL_EXT_multisampled_render_to_texture is not advertised. |
| `dEQP-GLES3.functional.transform_feedback.*` | extension | 0 | 333 | 0 | 0 | The cases capture more components than MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS / SEPARATE_COMPONENTS (the OpenGL ES minimums) allow. |
| `dEQP-GLES3.functional.fbo.msaa.8_samples.*` | extension | 0 | 22 | 0 | 0 | MAX_SAMPLES is 4 (Apple GPUs render with 1, 2 or 4 samples). |
| `dEQP-GLES3.functional.multisample.fbo_8_samples.*` | extension | 0 | 16 | 0 | 0 | MAX_SAMPLES is 4 (Apple GPUs render with 1, 2 or 4 samples). |
| `dEQP-GLES3.functional.fbo.blit.depth_stencil.*stencil_index8*` | extension | 0 | 12 | 0 | 0 | Separate depth and stencil attachments are FRAMEBUFFER_UNSUPPORTED (Metal stores depth and stencil together); OpenGL ES allows reporting them unsupported. |
| `dEQP-GLES3.functional.fbo.multiview.*` | extension | 0 | 3 | 0 | 0 | GL_OVR_multiview is not advertised. |
| `dEQP-GLES3.functional.fragment_ops.scissor.clear_depth` | test-design | 0 | 1 | 0 | 0 | The RGB565 configuration has no depth buffer, so the case reports NotSupported on every implementation. |
| `dEQP-GLES3.functional.fragment_ops.scissor.clear_stencil` | test-design | 0 | 1 | 0 | 0 | The RGB565 configuration has no stencil buffer, so the case reports NotSupported on every implementation. |
| `dEQP-GLES3.functional.fragment_ops.scissor.framebuffer_blit_*` | test-design | 0 | 3 | 0 | 0 | In the multisample configuration the default framebuffer is multisampled and OpenGL ES cannot blit into it, so the cases report NotSupported on every implementation (glsScissorTests.cpp). |
| `KHR-GLES31.core.sample_variables.*samples_8*` | extension | 0 | 0 | 38 | 0 | The cases need 8 samples; GL_MAX_SAMPLES and the integer and float formats' sample counts are 4, the most Apple GPUs render with. |
| `KHR-GLES31.core.shader_atomic_counters.advanced-usage-multidim-array-[ml]*` | extension | 0 | 0 | 2 | 0 | The cases need more atomic counters per stage than GL_MAX_*_ATOMIC_COUNTERS allows; the test reports NotSupported below its array size. |
| `KHR-GLES31.core.shader_group_vote.*` | extension | 0 | 0 | 5 | 0 | GL_EXT_shader_group_vote is not advertised in OpenGL ES contexts. |
| `KHR-GLES3[12].core.tessellation_shader.tessellation_shader_tessellation.max_in_out_attributes` | extension | 0 | 0 | 1 | 1 | The case needs GL_MAX_VERTEX_OUTPUT_COMPONENTS to cover GL_MAX_TESS_CONTROL_INPUT_COMPONENTS (esextcTessellationShaderTessellation.cpp); WineMetalGL reports the OpenGL ES minimums. |
| `KHR-GLES3[12].core.tessellation_shader.tessellation_shader_tessellation.TES` | test-design | 0 | 0 | 1 | 1 | OpenGL ES requires a tessellation control shader with an evaluation shader; the case runs only where an evaluation shader may be used alone (esextcTessellationShaderTessellation.cpp). |
| `KHR-GLES*.viewport_array.viewport_index_subroutine` | test-design | 0 | 0 | 1 | 1 | OpenGL ES has no shader subroutines; the case runs only on desktop GL (glcViewportArrayTests.cpp). |
| `KHR-GLESEXT.fragment_shading_rate.*` | extension | 0 | 0 | 0 | 1067 | GL_EXT_fragment_shading_rate is not advertised. |
| `KHR-Single-GLES3[12].subgroups.*` | extension | 0 | 0 | 6053 | 6053 | GL_KHR_shader_subgroup is not advertised. |
| `dEQP-GLES31.functional.android_extension_pack.*` | extension | 0 | 0 | 40 | 0 | GL_ANDROID_extension_pack_es31a is not advertised (it needs GL_KHR_texture_compression_astc_hdr and GL_EXT_shader_framebuffer_fetch, which are not). |
| `dEQP-GLES31.functional.copy_image.*texture3d*` | extension | 0 | 0 | 1176 | 0 | ASTC in 3D textures (GL_KHR_texture_compression_astc_sliced_3d, GL_KHR_texture_compression_astc_hdr or GL_OES_texture_compression_astc) is not advertised; the test reports every 3D target NotSupported without it. |
| `dEQP-GLES31.functional.debug.*framebuffer_fetch*` | extension | 0 | 0 | 12 | 0 | GL_EXT_shader_framebuffer_fetch is not advertised. |
| `dEQP-GLES31.functional.shaders.framebuffer_fetch.*` | extension | 0 | 0 | 44 | 0 | GL_EXT_shader_framebuffer_fetch is not advertised. |
| `dEQP-GLES31.functional.draw_indirect.compute_interop.large.*` | extension | 0 | 0 | 18 | 0 | The cases need a GL_MAX_SHADER_STORAGE_BLOCK_SIZE larger than the 2^27 bytes WineMetalGL reports (the OpenGL ES minimum). |
| `dEQP-GLES31.functional.fbo.srgb_write_control.*` | extension | 0 | 0 | 5 | 0 | GL_EXT_sRGB_write_control is not advertised. |
| `dEQP-GLES31.functional.geometry_shading.basic.output_256` | extension | 0 | 0 | 1 | 0 | The case needs more geometry output components than GL_MAX_GEOMETRY_TOTAL_OUTPUT_COMPONENTS (the OpenGL ES minimum). |
| `dEQP-GLES31.functional.multisample.default_framebuffer.*` | test-design | 0 | 0 | 8 | 0 | The cases need a multisample default framebuffer; mustpass configuration 1 has none. |
| `dEQP-GLES31.functional.sample_shading.min_sample_shading.default_framebuffer_*` | test-design | 0 | 0 | 2 | 0 | The cases need a multisample default framebuffer; mustpass configuration 1 has none. |
| `dEQP-GLES31.functional.primitive_bounding_box.wide_lines.*` | extension | 0 | 0 | 40 | 0 | Lines wider than 1 are optional in OpenGL ES; ALIASED_LINE_WIDTH_RANGE is 1 to 1 (Metal draws 1-pixel lines). |
| `dEQP-GLES31.functional.program_interface_query.*block_array.array.mat3x2` | extension | 0 | 0 | 2 | 0 | Metal stage inputs and outputs cannot hold arrays of matrices (SPIRV-Cross), so the program does not link; the test reports a link failure as a limit NotSupported. |
| `dEQP-GLES31.functional.shaders.opaque_type_indexing.atomic_counter.*tessellation_*` | extension | 0 | 0 | 8 | 0 | GL_MAX_TESS_CONTROL_ATOMIC_COUNTERS and GL_MAX_TESS_EVALUATION_ATOMIC_COUNTERS are 0, the OpenGL ES minimum. |
| `dEQP-GLES31.functional.shaders.*_8*` | extension | 0 | 0 | 72 | 0 | The cases need 8 samples; GL_MAX_SAMPLES is 4, the most Apple GPUs render with. |
| `dEQP-GLES31.functional.shaders.*_16` | extension | 0 | 0 | 52 | 0 | The cases need 16 samples; GL_MAX_SAMPLES is 4, the most Apple GPUs render with. |
| `dEQP-GLES31.functional.sample_shading.*samples_[1-9]*` | extension | 0 | 0 | 8 | 0 | The cases need more than 4 samples; GL_MAX_SAMPLES is 4, the most Apple GPUs render with. |
| `dEQP-GLES31.functional.texture.multisample.samples_*` | extension | 0 | 0 | 84 | 0 | The cases need more than 4 samples (or more GL_MAX_SAMPLE_MASK_WORDS); Apple GPUs render with at most 4. |
| `dEQP-GLES31.functional.primitive_bounding_box.blit_fbo.blit_fbo_to_default` | test-design | 0 | 0 | 1 | 0 | The case needs a single-sample default framebuffer; the multisample list's configuration has a 4-sample one (es31fPrimitiveBoundingBoxTests.cpp). |
<!-- END GENERATED: notsupported -->
