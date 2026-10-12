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
| 4.4 | Buffer storage, multi-bind, texture clears, enhanced layouts | `GL_ARB_buffer_storage` (1 of 8) | `GL_ARB_clear_texture`, `GL_ARB_enhanced_layouts`, `GL_ARB_multi_bind`, `GL_ARB_query_buffer_object`, `GL_ARB_texture_mirror_clamp_to_edge`, `GL_ARB_texture_stencil8`, `GL_ARB_vertex_type_10f_11f_11f_rev` |
| 4.5 | Direct state access, clip control, robustness, cull distances | `GL_ARB_clip_control`, `GL_ARB_texture_barrier`, `GL_KHR_robustness` (3 of 11) | `GL_ARB_conditional_render_inverted`, `GL_ARB_cull_distance`, `GL_ARB_derivative_control`, `GL_ARB_direct_state_access`, `GL_ARB_ES3_1_compatibility`, `GL_ARB_get_texture_sub_image`, `GL_ARB_shader_texture_image_samples`, `GL_KHR_context_flush_control` |
| 4.6 | SPIR-V shaders, anisotropic filtering, indirect draw counts | `GL_ARB_polygon_offset_clamp`, `GL_ARB_texture_filter_anisotropic` (2 of 11) | `GL_ARB_gl_spirv`, `GL_ARB_indirect_parameters`, `GL_ARB_pipeline_statistics_query`, `GL_ARB_shader_atomic_counter_ops`, `GL_ARB_shader_draw_parameters`, `GL_ARB_shader_group_vote`, `GL_ARB_spirv_extensions`, `GL_ARB_transform_feedback_overflow_query`, `GL_KHR_no_error` |
<!-- END GENERATED: gl4 -->

`GL_ARB_compute_shader` is not advertised even though the core runs compute
kernels internally (geometry shader, tessellation control and transform
feedback emulation): compute shaders come with GL 4.3 and its shader storage
buffers, which are the next milestone.

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

Not implemented. None of these is blocked by Metal: they were left out
because games rarely use them, and the game corpus needs none of them.
Commands without an implementation generate `GL_INVALID_OPERATION` and are
logged once.

| Feature | Version | Behaviour now | Why it is left out | What it would take |
|---|---|---|---|---|
| ARB assembly programs (`GL_ARB_vertex_program`, `GL_ARB_fragment_program`) | Extensions | Not advertised | Extensions, not core GL, so a 3.3 compatibility context does not need them; they are the most useful item here for games (KOTOR, Doom 3 and other 2002 to 2005 titles) | A translator from ARB assembly to SPIR-V |
| Accumulation buffer (`glAccum`) | 1.0 | `GL_INVALID_OPERATION` | Rare; a few old games use it for motion blur | An RGBA16F texture and blits |
| Selection and feedback (`glRenderMode`) | 1.0 | Nothing is drawn outside `GL_RENDER`, and no records are returned | Mostly CAD programs and old editors' picking | Transforming vertices on the CPU and writing the records |
| Evaluators (`glMap*`, `glEval*`) | 1.0 | `GL_INVALID_OPERATION` | Very rare in games | Generating the vertices on the CPU and drawing them as immediate mode |
| Line stipple | 1.0 | The pattern is stored and ignored | Rare | A distance-along-line value and a fragment discard |
| Wide lines and polygon stipple with application shaders | 1.0 | Drawn by the fixed-function programs only | Needs code added to the application's own shaders | Injecting the expansion and discard into linked programs |
| Face culling in `GL_POINT` polygon mode | 1.0 | The vertices of every triangle are drawn | Metal has no point polygon mode, and drawing vertices as points loses each triangle's facing | Computing facing per triangle before the points are expanded |
| Pixel maps, transfer scale and bias | 1.0 | Only the defaults apply; `glPixelMap*` generates `GL_INVALID_OPERATION` | Rare | A conversion step in the pixel-transfer path |
| `GL_POINT_FADE_THRESHOLD_SIZE` | 1.4 | Stored and not applied | A small gap, not yet done | Fading the alpha of small fixed-function points |
| Imaging subset (colour tables, convolution, histogram, colour matrix) | 1.2 | `GL_INVALID_OPERATION`; `GL_ARB_imaging` is not advertised | Optional in the specification: it exists only when `GL_ARB_imaging` is advertised | Large, for almost no use |
| Colour-index mode | 1.0 | Unreachable | Wine's macOS driver offers only RGBA pixel formats, so no program can create a colour-index context | Nothing worth doing |

## Conformance notes

<!-- BEGIN GENERATED: totals-notes (scripts/docs/generate.py) -->
**Unsupported %** (README conformance totals): cases the CTS reports NotSupported because WineMetalGL does not offer what they test (an extension it does not advertise or a newer GL version), over the cases any implementation can run. Contexts stop at OpenGL 4.3, so later versions are 100%; some of their features are available as extensions ([OpenGL 4.x](#opengl-4x)).

Failing cases: `KHR-GL30.api.coverage`, `KHR-GL33.transform_feedback2.transform_feedback2_states`, `KHR-GL40.internalformat.renderbuffer.rgb9_e5`, `KHR-GL40.transform_feedback2.transform_feedback2_states`, `KHR-GL41.internalformat.renderbuffer.rgb9_e5`, `KHR-GL41.transform_feedback2.transform_feedback2_states`, `KHR-GL42.internalformat.renderbuffer.rgb9_e5`, `KHR-GL42.map_buffer_alignment.functional`, `KHR-GL42.texture_view.view_sampling`, `KHR-GL42.transform_feedback2.transform_feedback2_states`, `KHR-GL43.internalformat.renderbuffer.rgb9_e5`, `KHR-GL43.map_buffer_alignment.functional`, `KHR-GL43.texture_view.view_sampling`, `KHR-GL43.transform_feedback2.transform_feedback2_states`. [Conformance results](conformance.md) gives the reason for each.
<!-- END GENERATED: totals-notes -->

## CTS cases reported NotSupported

<!-- BEGIN GENERATED: notsupported (scripts/docs/generate.py) -->
Every NotSupported case in a committed result matches one entry of `tests/cts/notsupported.json`. `extension` and `version` cases test something WineMetalGL does not offer; `test-design` cases are skipped by the test itself on every implementation.

| Pattern | Kind | KHR-GL30 | KHR-GL31 | KHR-GL32 | KHR-GL33 | KHR-GL40 | KHR-GL41 | KHR-GL42 | KHR-GL43 | Reason |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| `KHR-GL4?.sparse_texture_tests.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1089 | GL_ARB_sparse_texture is not advertised. |
| `KHR-GL4?.sparse_buffer_tests.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 82 | GL_ARB_sparse_buffer is not advertised. |
| `KHR-GL4?.blend_equation_advanced.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 88 | GL_KHR_blend_equation_advanced is not advertised. |
| `KHR-GL4?.shader_ballot_tests.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 4 | GL_ARB_shader_ballot and GL_ARB_gpu_shader_int64 are not advertised. |
| `KHR-GL4?.clear_tex_image.*` | version | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 78 | The cases need GL 4.4 or GL_ARB_clear_texture, which is not advertised. |
| `KHR-GL4?.indirect_parameters_tests.*` | version | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 3 | The cases need GL 4.6 or GL_ARB_indirect_parameters, which is not advertised. |
| `KHR-GL4?.texture_buffer_size_clamping.*` | version | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 4 | The cases need GL 4.5 or GL_ARB_direct_state_access, which is not advertised. |
| `KHR-GL4?.texture_query_lod.*` | test-design | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 3 | The cases need a render target of at least 256 x 256 (gl4cTextureQueryLodTests.cpp, TextureQueryLodBaseTest::iterate); the mustpass configurations use 64 x 64 surfaces, so they report NotSupported on every implementation. |
| `KHR-GL4?.fragment_shading_rate.*` | extension | 0 | 0 | 0 | 0 | 1067 | 1067 | 1067 | 1067 | GL_EXT_fragment_shading_rate is not advertised. |
| `KHR-GL4?.texture_view.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | GL_ARB_texture_view (GL 4.3) is not advertised. |
| `KHR-GL4?.texture_barrier_ARB.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | GL_ARB_texture_barrier (GL 4.5) is not advertised. |
| `KHR-GL4?.shader_subroutine.arrays_of_arrays_of_uniforms` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | GL_ARB_arrays_of_arrays (GL 4.3) is not advertised. |
| `KHR-GL4?.shader_image_load_store.basic-allFormats-loadStoreComputeStage` | version | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | The case uses compute shaders (GL 4.3, GL_ARB_compute_shader), which are not advertised. |
| `KHR-GL4?.shader_image_load_store.*-ms` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | The case needs GL_MAX_IMAGE_SAMPLES of 4 or more. WineMetalGL reports 0, the GL minimum: Metal shaders cannot write multisample textures. |
| `KHR-GL4?.shader_image_size.*-ms-*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 36 | The case needs GL_MAX_IMAGE_SAMPLES of 4 or more. WineMetalGL reports 0, the GL minimum: Metal shaders cannot write multisample textures. |
| `KHR-GL4?.shader_image_load_store.advanced-sso-perSample` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 1 | The case needs GL_MAX_IMAGE_SAMPLES of 4 or more. WineMetalGL reports 0, the GL minimum: Metal shaders cannot write multisample textures. |
| `KHR-GL[34]?.shaders.shader_integer_mix.prototypes` | version | 0 | 0 | 0 | 1 | 1 | 1 | 1 | 1 | Without `#extension GL_EXT_shader_integer_mix` the case needs GLSL 4.50 (glcShaderIntegerMixTests.cpp, ShaderIntegerMixPrototypesCase). |
| `KHR-GL4?.draw_elements_base_vertex_tests.valid_active_tf` | test-design | 0 | 0 | 0 | 0 | 1 | 1 | 1 | 1 | The case applies only to OpenGL ES contexts and reports NotSupported on every desktop context (esextcDrawElementsBaseVertexTests.cpp, DrawElementsBaseVertexNegativeActiveTransformFeedbackTest). |
| `KHR-GL[34]?.cull_distance.*` | extension | 0 | 0 | 0 | 218 | 218 | 218 | 218 | 218 | GL_ARB_cull_distance is not advertised. |
| `KHR-GL[34]?.ext_texture_shadow_lod.*` | extension | 15 | 15 | 15 | 15 | 15 | 15 | 15 | 15 | GL_EXT_texture_shadow_lod is not advertised. |
| `KHR-GL[34]?.transform_feedback_overflow_query_ARB.*` | extension | 0 | 0 | 13 | 13 | 13 | 13 | 13 | 13 | GL_ARB_transform_feedback_overflow_query is not advertised. |
| `KHR-GL[34]?.pipeline_statistics_query_tests_ARB.*` | extension | 0 | 0 | 0 | 10 | 10 | 10 | 10 | 10 | GL_ARB_pipeline_statistics_query is not advertised. |
| `KHR-GL[34]?.texture_filter_minmax_tests.*` | extension | 0 | 0 | 0 | 5 | 5 | 5 | 5 | 5 | GL_ARB_texture_filter_minmax is not advertised. |
| `KHR-GL3?.transform_feedback2.*` | extension | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | GL_ARB_transform_feedback2 is not advertised. |
| `KHR-GL3?.transform_feedback3.*` | extension | 3 | 3 | 3 | 0 | 0 | 0 | 0 | 0 | GL_ARB_transform_feedback3 is not advertised. |
| `KHR-GL3?.gpu_shader5_gl.*` | extension | 0 | 0 | 3 | 0 | 0 | 0 | 0 | 0 | GL_ARB_gpu_shader5 is not advertised. |
| `KHR-GL3?.shaders.shader_integer_mix.*` | extension | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | GL_EXT_shader_integer_mix is not advertised. |
| `KHR-GL[34]?.pixelstoragemodes.compressedteximage*.rgba_astc_*` | extension | 0 | 0 | 0 | 80 | 80 | 80 | 80 | 80 | ASTC formats need GL_KHR_texture_compression_astc_ldr, which is not advertised. |
| `KHR-GL[34]?.packed_depth_stencil.stencil_texturing.*` | version | 0 | 0 | 2 | 2 | 2 | 2 | 2 | 0 | Stencil texturing needs GL 4.3 (GL_ARB_stencil_texturing is not advertised). |
| `KHR-GL[34]?.CommonBugs.CommonBug_PerVertexValidation` | version | 0 | 1 | 1 | 1 | 1 | 0 | 0 | 0 | The case runs only on OpenGL 4.1 or newer contexts. |
| `KHR-GL[34]?.texture_swizzle.*` | test-design | 0 | 0 | 0 | 263 | 263 | 263 | 249 | 241 | The case pairs a texture target with an access routine or format that the test marks invalid (gl3cTextureSwizzleTests.cpp isTargetSuppByAccess/isTargetSuppByFormat, or a format whose m_minimum_gl_context is above 3.3), so it reports NotSupported on every implementation. |
| `KHR-GL3[012].frag_coord_conventions.multisample` | extension | 1 | 1 | 1 | 0 | 0 | 0 | 0 | 0 | Below a GL 3.3 context the case needs GL_ARB_sample_shading (glcFragCoordConventionsTests.cpp), which is not advertised; KHR-GL33 runs it. |
| `KHR-GL31.clip_distance.*` | test-design | 0 | 3 | 0 | 0 | 0 | 0 | 0 | 0 | KHR-GL31 creates a forward-compatible 3.1 context, and glu::contextSupports (gluRenderContext.cpp) matches a forward-compatible context only against its exact version, so the case's check for a GL 3.0 context fails (glcClipDistance.cpp); KHR-GL30, KHR-GL32 and KHR-GL33 run it. |
| `KHR-GL31.texture_lod_bias.texture_lod_bias_all` | test-design | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | KHR-GL31 creates a forward-compatible 3.1 context, and glu::contextSupports (gluRenderContext.cpp) matches a forward-compatible context only against its exact version, so the case's check for a GL 3.0 context fails (glcTextureLodBiasTests.cpp); KHR-GL30, KHR-GL32 and KHR-GL33 run it. |
<!-- END GENERATED: notsupported -->
