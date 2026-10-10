# Unsupported

What WineMetalGL does not support, and why. The cases that run, and the few
that fail, are in [Conformance results](conformance.md).

## OpenGL 4.x

Contexts stop at OpenGL 3.3: `wglCreateContextAttribsARB` refuses a request
for 4.0 or newer with `ERROR_INVALID_VERSION_ARB`, as it does for any version
an implementation does not offer. Some 4.x features are available in 3.3
contexts as the extensions that were promoted into each version:

<!-- BEGIN GENERATED: gl4 (scripts/docs/generate.py) -->
| Version | Features | Promoted extensions advertised | Not advertised |
|---|---|---|---|
| 4.0 | Tessellation, GLSL 4.00, double precision, indirect draws, per-sample shading | `GL_ARB_draw_buffers_blend` (1 of 13) | `GL_ARB_draw_indirect`, `GL_ARB_gpu_shader5`, `GL_ARB_gpu_shader_fp64`, `GL_ARB_sample_shading`, `GL_ARB_shader_subroutine`, `GL_ARB_tessellation_shader`, `GL_ARB_texture_buffer_object_rgb32`, `GL_ARB_texture_cube_map_array`, `GL_ARB_texture_gather`, `GL_ARB_texture_query_lod`, `GL_ARB_transform_feedback2`, `GL_ARB_transform_feedback3` |
| 4.1 | Separate shader objects, program binaries, viewport arrays, 64-bit attributes | `GL_ARB_ES2_compatibility`, `GL_ARB_get_program_binary`, `GL_ARB_separate_shader_objects` (3 of 6) | `GL_ARB_shader_precision`, `GL_ARB_vertex_attrib_64bit`, `GL_ARB_viewport_array` |
| 4.2 | Image load/store, atomic counters, immutable texture storage | `GL_ARB_base_instance`, `GL_ARB_compressed_texture_pixel_storage`, `GL_ARB_internalformat_query`, `GL_ARB_map_buffer_alignment`, `GL_ARB_texture_storage` (5 of 11) | `GL_ARB_conservative_depth`, `GL_ARB_shader_atomic_counters`, `GL_ARB_shader_image_load_store`, `GL_ARB_shading_language_420pack`, `GL_ARB_shading_language_packing`, `GL_ARB_transform_feedback_instanced` |
| 4.3 | Compute shaders, shader storage buffers, debug output, texture views | `GL_ARB_copy_image`, `GL_ARB_invalidate_subdata`, `GL_ARB_texture_storage_multisample`, `GL_KHR_debug` (4 of 22) | `GL_ARB_arrays_of_arrays`, `GL_ARB_clear_buffer_object`, `GL_ARB_compute_shader`, `GL_ARB_ES3_compatibility`, `GL_ARB_explicit_uniform_location`, `GL_ARB_fragment_layer_viewport`, `GL_ARB_framebuffer_no_attachments`, `GL_ARB_internalformat_query2`, `GL_ARB_multi_draw_indirect`, `GL_ARB_program_interface_query`, `GL_ARB_robust_buffer_access_behavior`, `GL_ARB_shader_image_size`, `GL_ARB_shader_storage_buffer_object`, `GL_ARB_stencil_texturing`, `GL_ARB_texture_buffer_range`, `GL_ARB_texture_query_levels`, `GL_ARB_texture_view`, `GL_ARB_vertex_attrib_binding` |
| 4.4 | Buffer storage, multi-bind, texture clears, enhanced layouts | `GL_ARB_buffer_storage` (1 of 8) | `GL_ARB_clear_texture`, `GL_ARB_enhanced_layouts`, `GL_ARB_multi_bind`, `GL_ARB_query_buffer_object`, `GL_ARB_texture_mirror_clamp_to_edge`, `GL_ARB_texture_stencil8`, `GL_ARB_vertex_type_10f_11f_11f_rev` |
| 4.5 | Direct state access, clip control, robustness, cull distances | `GL_ARB_clip_control`, `GL_KHR_robustness` (2 of 11) | `GL_ARB_conditional_render_inverted`, `GL_ARB_cull_distance`, `GL_ARB_derivative_control`, `GL_ARB_direct_state_access`, `GL_ARB_ES3_1_compatibility`, `GL_ARB_get_texture_sub_image`, `GL_ARB_shader_texture_image_samples`, `GL_ARB_texture_barrier`, `GL_KHR_context_flush_control` |
| 4.6 | SPIR-V shaders, anisotropic filtering, indirect draw counts | `GL_ARB_texture_filter_anisotropic` (1 of 11) | `GL_ARB_gl_spirv`, `GL_ARB_indirect_parameters`, `GL_ARB_pipeline_statistics_query`, `GL_ARB_polygon_offset_clamp`, `GL_ARB_shader_atomic_counter_ops`, `GL_ARB_shader_draw_parameters`, `GL_ARB_shader_group_vote`, `GL_ARB_spirv_extensions`, `GL_ARB_transform_feedback_overflow_query`, `GL_KHR_no_error` |
<!-- END GENERATED: gl4 -->

`GL_ARB_compute_shader` is not advertised even though the core runs compute
kernels internally (geometry shader and transform feedback emulation):
without storage buffers, images or atomic counters a compute shader has no
way to write output, and engines that detect the extension would skip their
working fallback.

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

## CTS cases reported NotSupported

<!-- BEGIN GENERATED: notsupported (scripts/docs/generate.py) -->
Every NotSupported case in a committed result matches one entry of `tests/cts/notsupported.json`. `extension` and `version` cases test something WineMetalGL does not offer; `test-design` cases are skipped by the test itself on every implementation.

| Pattern | Kind | KHR-GL30 | KHR-GL31 | KHR-GL32 | KHR-GL33 | Reason |
|---|---|---:|---:|---:|---:|---|
| `KHR-GL3?.cull_distance.*` | extension | 0 | 0 | 0 | 218 | GL_ARB_cull_distance is not advertised. |
| `KHR-GL3?.ext_texture_shadow_lod.*` | extension | 15 | 15 | 15 | 15 | GL_EXT_texture_shadow_lod is not advertised. |
| `KHR-GL3?.transform_feedback_overflow_query_ARB.*` | extension | 0 | 0 | 13 | 13 | GL_ARB_transform_feedback_overflow_query is not advertised. |
| `KHR-GL3?.pipeline_statistics_query_tests_ARB.*` | extension | 0 | 0 | 0 | 10 | GL_ARB_pipeline_statistics_query is not advertised. |
| `KHR-GL3?.texture_filter_minmax_tests.*` | extension | 0 | 0 | 0 | 5 | GL_ARB_texture_filter_minmax is not advertised. |
| `KHR-GL3?.transform_feedback2.*` | extension | 1 | 1 | 1 | 1 | GL_ARB_transform_feedback2 is not advertised. |
| `KHR-GL3?.transform_feedback3.*` | extension | 3 | 3 | 3 | 3 | GL_ARB_transform_feedback3 is not advertised. |
| `KHR-GL3?.gpu_shader5_gl.*` | extension | 0 | 0 | 3 | 3 | GL_ARB_gpu_shader5 is not advertised. |
| `KHR-GL3?.shaders.shader_integer_mix.*` | extension | 0 | 0 | 0 | 6 | GL_EXT_shader_integer_mix is not advertised. |
| `KHR-GL3?.pixelstoragemodes.compressedteximage*.rgba_astc_*` | extension | 0 | 0 | 0 | 80 | ASTC formats need GL_KHR_texture_compression_astc_ldr, which is not advertised. |
| `KHR-GL3?.packed_depth_stencil.stencil_texturing.*` | version | 0 | 0 | 2 | 2 | Stencil texturing needs GL 4.3 (GL_ARB_stencil_texturing is not advertised). |
| `KHR-GL3?.CommonBugs.CommonBug_PerVertexValidation` | version | 0 | 1 | 1 | 1 | The case runs only on OpenGL 4.1 or newer contexts. |
| `KHR-GL33.texture_swizzle.*` | test-design | 0 | 0 | 0 | 263 | The case pairs a texture target with an access routine or format that the test marks invalid (gl3cTextureSwizzleTests.cpp isTargetSuppByAccess/isTargetSuppByFormat, or a format whose m_minimum_gl_context is above 3.3), so it reports NotSupported on every implementation. |
| `KHR-GL3[012].frag_coord_conventions.multisample` | extension | 1 | 1 | 1 | 0 | Below a GL 3.3 context the case needs GL_ARB_sample_shading (glcFragCoordConventionsTests.cpp), which is not advertised; KHR-GL33 runs it. |
| `KHR-GL31.clip_distance.*` | test-design | 0 | 3 | 0 | 0 | KHR-GL31 creates a forward-compatible 3.1 context, and glu::contextSupports (gluRenderContext.cpp) matches a forward-compatible context only against its exact version, so the case's check for a GL 3.0 context fails (glcClipDistance.cpp); KHR-GL30, KHR-GL32 and KHR-GL33 run it. |
| `KHR-GL31.texture_lod_bias.texture_lod_bias_all` | test-design | 0 | 1 | 0 | 0 | KHR-GL31 creates a forward-compatible 3.1 context, and glu::contextSupports (gluRenderContext.cpp) matches a forward-compatible context only against its exact version, so the case's check for a GL 3.0 context fails (glcTextureLodBiasTests.cpp); KHR-GL30, KHR-GL32 and KHR-GL33 run it. |
<!-- END GENERATED: notsupported -->
