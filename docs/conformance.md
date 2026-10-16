# WineMetalGL conformance

Khronos CTS mustpass lists (`glNN-main.txt`) in configurations 1 to 4 of `mustpass.xml` (5 and 6 repeat the command lines of 1 and 2; their only difference, `useForFirstEGLConfig`, applies to EGL). `native` runs the core directly through its driver ABI (`tests/cts/native`); `wine` runs `glcts.exe` in a WoW64 prefix: `KHR-GL46` and the rechecks on stock Wine 11.0 with the Wine module, `KHR-GL30` to `KHR-GL43` on MetalSharp's Wine 11.17 with the 2.x patches. `KHR-GL46` runs configuration 1 natively and configurations 1 and 3 under Wine. Applicable cases are those that are not NotSupported.

## Conformance totals

| OpenGL | Conformance % | Features | Unsupported % |
|---|---:|---|---:|
| 3.0 | 99.88% | GLSL 1.30, framebuffer objects, vertex array objects, transform feedback, integer textures | 2.28% |
| 3.1 | 100% | Instancing, texture buffers, uniform buffers, primitive restart | 2.38% |
| 3.2 | 100% | Profiles, geometry shaders, sync objects, multisample textures, base vertex | 0.69% |
| 3.3 | 100% | GLSL 3.30, sampler objects, timer queries, instanced arrays, dual-source blending | 1.11% |
| 4.0 | 99.99% | Tessellation, GLSL 4.00, double precision, indirect draws, per-sample shading | 10.11% |
| 4.1 | 99.99% | Separate shader objects, program binaries, viewport arrays, 64-bit attributes | 10.08% |
| 4.2 | 99.98% | Image load/store, atomic counters, immutable texture storage | 9.74% |
| 4.3 | 99.97% | Compute shaders, shader storage buffers, debug output, texture views | 15.60% |
| 4.4 | covered by KHR-GL46 | Buffer storage, multi-bind, texture clears, enhanced layouts | - |
| 4.5 | covered by KHR-GL46 | Direct state access, clip control, robustness, cull distances | - |
| 4.6 | 99.98% | SPIR-V shaders, anisotropic filtering, indirect draw counts | 22.33% |

| GLES | Conformance % | Features | Unsupported % |
|---|---:|---|---:|
| 2.0 | 100% | GLSL ES 1.00, framebuffer objects, the ES 2.0 extensions (float and depth textures, ...) | 0.60% |
| 3.0 | 100% | GLSL ES 3.00, transform feedback, instancing, integer and sRGB formats, multisample renderbuffers | 0.83% |
| 3.1 | 99.98% | Compute shaders, images, storage buffers, indirect draws, separate programs, multisample textures | 1.24% |
| 3.2 | 99.95% | Geometry and tessellation shaders, advanced blending, ASTC, texture buffers, debug output | 0.64% |

**Conformance %**: passing cases over applicable cases (cases that are not NotSupported) of the version's Khronos CTS mustpass list (`KHR-GL30` to `KHR-GL43`, `KHR-GL46`), in the worst of its runs: 8 for `KHR-GL30` to `KHR-GL43` (configurations 1 to 4 of `mustpass.xml`, each under Wine and natively), 3 for `KHR-GL46` (configuration 1 natively, configurations 1 and 3 under Wine), at commit 14eaa75, 49c5099, 851476e, f4ecccd. OpenGL 4.4 and 4.5 have no list of their own: `KHR-GL46` runs their tests.

The cases that were not Pass in `KHR-GL33`, `KHR-GL40`, `KHR-GL41`, `KHR-GL42`, `KHR-GL43` were run again at commit 49c5099, in the same configurations under Wine and natively (`tests/cts/recheck/`); those results replace the baselines' for those cases.

**GLES**: passing cases over applicable cases of all of the version's OpenGL ES mustpass lists together, Khronos's (`KHR-GLES2` to `KHR-GLES32`, `KHR-GLESEXT`, `KHR-NoContext`; not `KHR-Single`, which tests only subgroup operations) and dEQP's (`dEQP-GLES2` to `dEQP-GLES31`, with their rotation, multisample and RGB565 lists), in configuration 1 under Wine at commit 71b7989, c56a900; Unsupported % likewise. `cts-runner --type=es3` runs the OpenGL ES 2.0 and 3.0 lists in every configuration it selects ([OpenGL ES](#opengl-es)).

**Unsupported %**: cases the CTS reports NotSupported because WineMetalGL does not offer what they test (an extension it does not advertise, or a limit above the minimum), over the cases any implementation can run: [Unsupported](unsupported.md#conformance-notes).

## Features

OpenGL 3.x features are core in every context. The OpenGL 4.x features are the extensions each version promoted to core, supported when the context advertises them. The rows marked No are optional: the CTS cases that test them are NotSupported, which is what Unsupported % counts.

| OpenGL | Feature | Supported | Unsupported |
|---|---|---|---|
| 3.0 | GLSL 1.30 | Yes | - |
| 3.0 | Framebuffer objects | Yes | - |
| 3.0 | Vertex array objects | Yes | - |
| 3.0 | Transform feedback | Yes | - |
| 3.0 | Integer textures | Yes | - |
| 3.0 | Floating-point textures and color buffers | Yes | - |
| 3.0 | Depth float buffers | Yes | - |
| 3.0 | Half-float pixels and vertices | Yes | - |
| 3.0 | Buffer range mapping | Yes | - |
| 3.0 | Red and red-green textures | Yes | - |
| 3.0 | RGTC compressed textures | Yes | - |
| 3.0 | Texture arrays | Yes | - |
| 3.0 | Packed float and shared exponent formats | Yes | - |
| 3.0 | sRGB framebuffers | Yes | - |
| 3.0 | Conditional rendering | Yes | - |
| 3.0 | Per-buffer color masks and blending | Yes | - |
| 3.1 | GLSL 1.40 | Yes | - |
| 3.1 | Instanced draws | Yes | - |
| 3.1 | Texture buffers | Yes | - |
| 3.1 | Uniform buffers | Yes | - |
| 3.1 | Primitive restart | Yes | - |
| 3.1 | Buffer copies | Yes | - |
| 3.1 | Rectangle textures | Yes | - |
| 3.1 | Signed normalized textures | Yes | - |
| 3.2 | GLSL 1.50 | Yes | - |
| 3.2 | Core and compatibility profiles | Yes | - |
| 3.2 | Geometry shaders | Yes | - |
| 3.2 | Sync objects | Yes | - |
| 3.2 | Multisample textures | Yes | - |
| 3.2 | Base vertex draws | Yes | - |
| 3.2 | Provoking vertex control | Yes | - |
| 3.2 | Seamless cube maps | Yes | - |
| 3.2 | Depth clamping | Yes | - |
| 3.2 | Fragment coordinate conventions | Yes | - |
| 3.2 | More than 4 samples | No | `GL_MAX_SAMPLES` is 4, the most every color and depth format takes on Apple GPUs |
| 3.3 | GLSL 3.30 | Yes | - |
| 3.3 | Sampler objects | Yes | - |
| 3.3 | Timer queries | Yes | - |
| 3.3 | Instanced arrays | Yes | - |
| 3.3 | Dual-source blending | Yes | - |
| 3.3 | Explicit attribute locations | Yes | - |
| 3.3 | Boolean occlusion queries | Yes | - |
| 3.3 | Shader bit encoding | Yes | - |
| 3.3 | RGB10_A2UI textures | Yes | - |
| 3.3 | Texture swizzles | Yes | - |
| 3.3 | Packed 2_10_10_10 vertex attributes | Yes | - |
| 4.0 | Per-buffer blend equations (`GL_ARB_draw_buffers_blend`) | Yes | - |
| 4.0 | Indirect draws (`GL_ARB_draw_indirect`) | Yes | - |
| 4.0 | GLSL 4.00 shader features (gpu_shader5) (`GL_ARB_gpu_shader5`) | Yes | - |
| 4.0 | Double precision (`GL_ARB_gpu_shader_fp64`) | Yes | - |
| 4.0 | Per-sample shading (`GL_ARB_sample_shading`) | Yes | - |
| 4.0 | Shader subroutines (`GL_ARB_shader_subroutine`) | Yes | - |
| 4.0 | Tessellation shaders (`GL_ARB_tessellation_shader`) | Yes | - |
| 4.0 | RGB32 texture buffers (`GL_ARB_texture_buffer_object_rgb32`) | Yes | - |
| 4.0 | Cube map arrays (`GL_ARB_texture_cube_map_array`) | Yes | - |
| 4.0 | Texture gather (`GL_ARB_texture_gather`) | Yes | - |
| 4.0 | Texture LOD queries (`GL_ARB_texture_query_lod`) | Yes | - |
| 4.0 | Transform feedback objects (`GL_ARB_transform_feedback2`) | Yes | - |
| 4.0 | Multiple transform feedback streams (`GL_ARB_transform_feedback3`) | Yes | - |
| 4.1 | OpenGL ES 2.0 compatibility (`GL_ARB_ES2_compatibility`) | Yes | - |
| 4.1 | Program binaries (`GL_ARB_get_program_binary`) | Yes | - |
| 4.1 | Separate shader objects (`GL_ARB_separate_shader_objects`) | Yes | - |
| 4.1 | Shader precision rules (`GL_ARB_shader_precision`) | Yes | - |
| 4.1 | 64-bit vertex attributes (`GL_ARB_vertex_attrib_64bit`) | Yes | - |
| 4.1 | Viewport arrays (`GL_ARB_viewport_array`) | Yes | - |
| 4.2 | Base instance draws (`GL_ARB_base_instance`) | Yes | - |
| 4.2 | Compressed texture pixel storage (`GL_ARB_compressed_texture_pixel_storage`) | Yes | - |
| 4.2 | Conservative depth (`GL_ARB_conservative_depth`) | Yes | - |
| 4.2 | Internal format queries (`GL_ARB_internalformat_query`) | Yes | - |
| 4.2 | Mapped buffer alignment (`GL_ARB_map_buffer_alignment`) | Yes | - |
| 4.2 | Atomic counters (`GL_ARB_shader_atomic_counters`) | Yes | - |
| 4.2 | Image load/store (`GL_ARB_shader_image_load_store`) | Yes | - |
| 4.2 | GLSL 4.20 syntax (420pack) (`GL_ARB_shading_language_420pack`) | Yes | - |
| 4.2 | Shader packing functions (`GL_ARB_shading_language_packing`) | Yes | - |
| 4.2 | BPTC compressed textures (`GL_ARB_texture_compression_bptc`) | Yes | - |
| 4.2 | Immutable texture storage (`GL_ARB_texture_storage`) | Yes | - |
| 4.2 | Instanced transform feedback draws (`GL_ARB_transform_feedback_instanced`) | Yes | - |
| 4.2 | Multisample images (`GL_MAX_IMAGE_SAMPLES` above 0) | No | Metal shaders cannot write multisample textures; 0 is the GL minimum |
| 4.3 | Arrays of arrays (`GL_ARB_arrays_of_arrays`) | Yes | - |
| 4.3 | Buffer clears (`GL_ARB_clear_buffer_object`) | Yes | - |
| 4.3 | Compute shaders (`GL_ARB_compute_shader`) | Yes | - |
| 4.3 | Image copies (`GL_ARB_copy_image`) | Yes | - |
| 4.3 | OpenGL ES 3.0 compatibility (`GL_ARB_ES3_compatibility`) | Yes | - |
| 4.3 | Explicit uniform locations (`GL_ARB_explicit_uniform_location`) | Yes | - |
| 4.3 | Layer and viewport index in fragment shaders (`GL_ARB_fragment_layer_viewport`) | Yes | - |
| 4.3 | Framebuffers without attachments (`GL_ARB_framebuffer_no_attachments`) | Yes | - |
| 4.3 | Internal format queries 2 (`GL_ARB_internalformat_query2`) | Yes | - |
| 4.3 | Data invalidation (`GL_ARB_invalidate_subdata`) | Yes | - |
| 4.3 | Multi-draw indirect (`GL_ARB_multi_draw_indirect`) | Yes | - |
| 4.3 | Program interface queries (`GL_ARB_program_interface_query`) | Yes | - |
| 4.3 | Robust buffer access behavior (`GL_ARB_robust_buffer_access_behavior`) | No | Not advertised |
| 4.3 | Image size queries (`GL_ARB_shader_image_size`) | Yes | - |
| 4.3 | Shader storage buffers (`GL_ARB_shader_storage_buffer_object`) | Yes | - |
| 4.3 | Stencil texturing (`GL_ARB_stencil_texturing`) | Yes | - |
| 4.3 | Texture buffer ranges (`GL_ARB_texture_buffer_range`) | Yes | - |
| 4.3 | Texture level queries (`GL_ARB_texture_query_levels`) | Yes | - |
| 4.3 | Immutable multisample textures (`GL_ARB_texture_storage_multisample`) | Yes | - |
| 4.3 | Texture views (`GL_ARB_texture_view`) | Yes | - |
| 4.3 | Vertex attribute bindings (`GL_ARB_vertex_attrib_binding`) | Yes | - |
| 4.3 | Debug output (`GL_KHR_debug`) | Yes | - |
| 4.4 | Buffer storage (`GL_ARB_buffer_storage`) | Yes | - |
| 4.4 | Texture clears (`GL_ARB_clear_texture`) | Yes | - |
| 4.4 | Enhanced layouts (`GL_ARB_enhanced_layouts`) | Yes | - |
| 4.4 | Multi-bind (`GL_ARB_multi_bind`) | Yes | - |
| 4.4 | Query buffer objects (`GL_ARB_query_buffer_object`) | Yes | - |
| 4.4 | Mirror clamp to edge (`GL_ARB_texture_mirror_clamp_to_edge`) | Yes | - |
| 4.4 | Stencil-only textures (`GL_ARB_texture_stencil8`) | Yes | - |
| 4.4 | Packed 10/11-bit float vertex attributes (`GL_ARB_vertex_type_10f_11f_11f_rev`) | Yes | - |
| 4.5 | Clip control (`GL_ARB_clip_control`) | Yes | - |
| 4.5 | Inverted conditional rendering (`GL_ARB_conditional_render_inverted`) | Yes | - |
| 4.5 | Cull distances (`GL_ARB_cull_distance`) | Yes | - |
| 4.5 | Coarse and fine derivatives (`GL_ARB_derivative_control`) | Yes | - |
| 4.5 | Direct state access (`GL_ARB_direct_state_access`) | Yes | - |
| 4.5 | OpenGL ES 3.1 compatibility (`GL_ARB_ES3_1_compatibility`) | Yes | - |
| 4.5 | Texture sub-image reads (`GL_ARB_get_texture_sub_image`) | Yes | - |
| 4.5 | Texture and image sample counts in shaders (`GL_ARB_shader_texture_image_samples`) | Yes | - |
| 4.5 | Texture barriers (`GL_ARB_texture_barrier`) | Yes | - |
| 4.5 | Context flush control (`GL_KHR_context_flush_control`) | Yes | - |
| 4.5 | Robustness (`GL_KHR_robustness`) | Yes | - |
| 4.6 | SPIR-V shaders (`GL_ARB_gl_spirv`) | Yes | - |
| 4.6 | Indirect draw counts (`GL_ARB_indirect_parameters`) | Yes | - |
| 4.6 | Pipeline statistics queries (`GL_ARB_pipeline_statistics_query`) | Yes | - |
| 4.6 | Polygon offset clamp (`GL_ARB_polygon_offset_clamp`) | Yes | - |
| 4.6 | Atomic counter operations (`GL_ARB_shader_atomic_counter_ops`) | Yes | - |
| 4.6 | Draw parameters in shaders (`GL_ARB_shader_draw_parameters`) | Yes | - |
| 4.6 | Shader group votes (`GL_ARB_shader_group_vote`) | Yes | - |
| 4.6 | SPIR-V extensions (`GL_ARB_spirv_extensions`) | Yes | - |
| 4.6 | Anisotropic filtering (`GL_ARB_texture_filter_anisotropic`) | Yes | - |
| 4.6 | Transform feedback overflow queries (`GL_ARB_transform_feedback_overflow_query`) | Yes | - |
| 4.6 | No-error contexts (`GL_KHR_no_error`) | Yes | - |
| 4.6 | Sparse textures and buffers (`GL_ARB_sparse_texture`, `GL_ARB_sparse_texture2`, `GL_ARB_sparse_texture_clamp`, `GL_ARB_sparse_buffer`) | No | Not advertised |
| 4.6 | Advanced blend equations (`GL_KHR_blend_equation_advanced`) | No | Not advertised in OpenGL contexts (OpenGL ES contexts have it) |
| 4.6 | Shader ballot and 64-bit integers (`GL_ARB_shader_ballot`, `GL_ARB_gpu_shader_int64`) | No | Not advertised |
| 4.6 | Min/max texture filtering (`GL_ARB_texture_filter_minmax`) | No | Not advertised |
| 4.6 | Post-depth coverage (`GL_ARB_post_depth_coverage`) | No | Metal has no post-depth-test sample coverage input |
| 4.6 | Fragment shading rate (`GL_EXT_fragment_shading_rate`) | No | Not advertised |
| 4.6 | ASTC compressed textures (`GL_KHR_texture_compression_astc_ldr`) | No | Not advertised in OpenGL contexts (OpenGL ES contexts have it) |

| GLES | Feature | Supported | Unsupported |
|---|---|---|---|
| 2.0 | GLSL ES 1.00 vertex and fragment shaders | Yes | - |
| 2.0 | Framebuffer objects | Yes | - |
| 2.0 | Vertex buffer objects | Yes | - |
| 2.0 | Cube maps and non-power-of-two textures | Yes | - |
| 2.0 | Depth, stencil and packed depth-stencil buffers | Yes | - |
| 2.0 | Float, half-float and depth textures (extensions) | Yes | - |
| 2.0 | 3D textures, vertex array objects, instancing (extensions) | Yes | - |
| 2.0 | Color attachments through `GL_NV_fbo_color_attachments` | No | Not advertised; `GL_EXT_draw_buffers` gives several color attachments |
| 2.0 | Separate depth and stencil attachments | No | `FRAMEBUFFER_UNSUPPORTED`: Metal keeps depth and stencil in one texture (allowed by OpenGL ES) |
| 2.0 | Lines wider than 1 | No | Optional; Metal draws 1-pixel lines only |
| 3.0 | GLSL ES 3.00 | Yes | - |
| 3.0 | Transform feedback | Yes | - |
| 3.0 | Instanced draws | Yes | - |
| 3.0 | Uniform buffers | Yes | - |
| 3.0 | Integer textures and attributes | Yes | - |
| 3.0 | sRGB textures and framebuffers | Yes | - |
| 3.0 | Multisample renderbuffers (up to 4 samples) | Yes | - |
| 3.0 | ETC2 and EAC compressed textures | Yes | - |
| 3.0 | 3D and 2D array textures | Yes | - |
| 3.0 | Sampler objects | Yes | - |
| 3.0 | Occlusion queries | Yes | - |
| 3.0 | Sync objects | Yes | - |
| 3.0 | Multiple render targets | Yes | - |
| 3.0 | Primitive restart | Yes | - |
| 3.0 | Program binaries | Yes | - |
| 3.0 | Texture swizzles | Yes | - |
| 3.0 | Immutable texture storage | Yes | - |
| 3.0 | Pixel buffer objects | Yes | - |
| 3.0 | More than 4 samples | No | Optional; Apple GPUs render with 1, 2 or 4 samples |
| 3.0 | Multiview (`GL_OVR_multiview`) | No | Not advertised |
| 3.0 | Multisampled render to texture (`GL_EXT_multisampled_render_to_texture`) | No | Not advertised |
| 3.1 | GLSL ES 3.10 | Yes | - |
| 3.1 | Compute shaders | Yes | - |
| 3.1 | Image load/store | Yes | - |
| 3.1 | Shader storage buffers | Yes | - |
| 3.1 | Atomic counters | Partly | Arrays of atomic counters passed to functions do not compile |
| 3.1 | Indirect draws and dispatches | Yes | - |
| 3.1 | Separate shader programs | Yes | - |
| 3.1 | Multisample textures | Yes | - |
| 3.1 | Vertex attribute bindings | Yes | - |
| 3.1 | Program interface queries | Yes | - |
| 3.1 | Arrays of matrices as stage inputs and outputs | No | Metal stage inputs and outputs cannot hold them (SPIRV-Cross); such programs do not link |
| 3.1 | Texture gather | Yes | - |
| 3.1 | Stencil texturing | Yes | - |
| 3.1 | Framebuffers without attachments | Yes | - |
| 3.2 | GLSL ES 3.20 | Yes | - |
| 3.2 | Geometry shaders | Yes | - |
| 3.2 | Tessellation shaders | Yes | - |
| 3.2 | Advanced blend equations | Yes | - |
| 3.2 | ASTC LDR compressed textures | Yes | - |
| 3.2 | ASTC 3D textures (`GL_KHR_texture_compression_astc_sliced_3d`) | Yes | - |
| 3.2 | ASTC HDR compressed textures | No | Not advertised; LDR ASTC only |
| 3.2 | Fragment shading rate (`GL_EXT_fragment_shading_rate`, `_primitive`, `_attachment`) | Partly | The 1x1 rate only: Metal has no per-draw, per-primitive or attachment shading rates |
| 3.2 | Texture buffers | Yes | - |
| 3.2 | Cube map arrays | Yes | - |
| 3.2 | Debug output | Yes | - |
| 3.2 | Robustness | Yes | - |
| 3.2 | Per-sample shading | Yes | - |
| 3.2 | Multisample 2D array textures | Yes | - |
| 3.2 | Primitive bounding box | Yes | - |
| 3.2 | Per-buffer blending | Yes | - |
| 3.2 | Image copies | Yes | - |
| 3.2 | Border clamping | Yes | - |
| 3.2 | Base vertex draws | Yes | - |

What OpenGL ES contexts leave out, and why: [Unsupported](unsupported.md#opengl-es).

## KHR-GL30

| Config | Runner | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Time | Commit | File |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1 | native | 858 | 0 | 20 | 0 | 0 | 0 | 0 | 0 | 878 | 100% | 12 s | 14eaa75 | `tests/cts/baseline/gl30-cfg1-native.txt` |
| 1 | wine | 858 | 0 | 20 | 0 | 0 | 0 | 0 | 0 | 878 | 100% | 48 s | 14eaa75 | `tests/cts/baseline/gl30-cfg1-wine.txt` |
| 2 | native | 858 | 0 | 20 | 0 | 0 | 0 | 0 | 0 | 878 | 100% | 13 s | 14eaa75 | `tests/cts/baseline/gl30-cfg2-native.txt` |
| 2 | wine | 858 | 0 | 20 | 0 | 0 | 0 | 0 | 0 | 878 | 100% | 50 s | 14eaa75 | `tests/cts/baseline/gl30-cfg2-wine.txt` |
| 3 | native | 857 | 1 | 20 | 0 | 0 | 0 | 0 | 0 | 878 | 99.88% | 13 s | 14eaa75 | `tests/cts/baseline/gl30-cfg3-native.txt` |
| 3 | wine | 857 | 1 | 20 | 0 | 0 | 0 | 0 | 0 | 878 | 99.88% | 47 s | 14eaa75 | `tests/cts/baseline/gl30-cfg3-wine.txt` |
| 4 | native | 857 | 1 | 20 | 0 | 0 | 0 | 0 | 0 | 878 | 99.88% | 12 s | 14eaa75 | `tests/cts/baseline/gl30-cfg4-native.txt` |
| 4 | wine | 857 | 1 | 20 | 0 | 0 | 0 | 0 | 0 | 878 | 99.88% | 26 s | 14eaa75 | `tests/cts/baseline/gl30-cfg4-wine.txt` |

Command lines: `scripts/cts/run.py --mustpass gl30-main.txt --config N` reads them from `mustpass.xml`. CTS build: dEQP Core 1942832354c96f0de379f491519734256fbd4f9d.

Cases that neither pass nor are NotSupported:

| Case | Runs | Reason |
|---|---|---|
| `KHR-GL30.api.coverage` | Fail natively in configurations 3, 4; Fail under Wine in configurations 3, 4 | In configurations 3 and 4 (`--deqp-surface-type=fbo`) the test saves `GL_READ_BUFFER` while the CTS framebuffer object is bound (`GL_COLOR_ATTACHMENT0`), deletes its own bound framebuffer object, which rebinds framebuffer zero, and restores the saved value there (glcApiCoverageTests.cpp, TestCoverageGLCallReadBuffer). The default framebuffer has no `GL_COLOR_ATTACHMENT0` buffer, and `glReadBuffer` generates `GL_INVALID_OPERATION` for a buffer that does not exist, as the specification requires. |

## KHR-GL31

| Config | Runner | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Time | Commit | File |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1 | native | 863 | 0 | 25 | 0 | 0 | 0 | 0 | 0 | 888 | 100% | 3 s | 14eaa75 | `tests/cts/baseline/gl31-cfg1-native.txt` |
| 1 | wine | 863 | 0 | 25 | 0 | 0 | 0 | 0 | 0 | 888 | 100% | 31 s | 14eaa75 | `tests/cts/baseline/gl31-cfg1-wine.txt` |
| 2 | native | 863 | 0 | 25 | 0 | 0 | 0 | 0 | 0 | 888 | 100% | 4 s | 14eaa75 | `tests/cts/baseline/gl31-cfg2-native.txt` |
| 2 | wine | 863 | 0 | 25 | 0 | 0 | 0 | 0 | 0 | 888 | 100% | 27 s | 14eaa75 | `tests/cts/baseline/gl31-cfg2-wine.txt` |
| 3 | native | 863 | 0 | 25 | 0 | 0 | 0 | 0 | 0 | 888 | 100% | 4 s | 14eaa75 | `tests/cts/baseline/gl31-cfg3-native.txt` |
| 3 | wine | 863 | 0 | 25 | 0 | 0 | 0 | 0 | 0 | 888 | 100% | 33 s | 14eaa75 | `tests/cts/baseline/gl31-cfg3-wine.txt` |
| 4 | native | 863 | 0 | 25 | 0 | 0 | 0 | 0 | 0 | 888 | 100% | 4 s | 14eaa75 | `tests/cts/baseline/gl31-cfg4-native.txt` |
| 4 | wine | 863 | 0 | 25 | 0 | 0 | 0 | 0 | 0 | 888 | 100% | 35 s | 14eaa75 | `tests/cts/baseline/gl31-cfg4-wine.txt` |

Command lines: `scripts/cts/run.py --mustpass gl31-main.txt --config N` reads them from `mustpass.xml`. CTS build: dEQP Core 1942832354c96f0de379f491519734256fbd4f9d.

## KHR-GL32

| Config | Runner | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Time | Commit | File |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1 | native | 5622 | 0 | 39 | 0 | 0 | 0 | 0 | 0 | 5661 | 100% | 2.7 min | 14eaa75 | `tests/cts/baseline/gl32-cfg1-native.txt` |
| 1 | wine | 5622 | 0 | 39 | 0 | 0 | 0 | 0 | 0 | 5661 | 100% | 5.2 min | 14eaa75 | `tests/cts/baseline/gl32-cfg1-wine.txt` |
| 2 | native | 5622 | 0 | 39 | 0 | 0 | 0 | 0 | 0 | 5661 | 100% | 2.4 min | 14eaa75 | `tests/cts/baseline/gl32-cfg2-native.txt` |
| 2 | wine | 5622 | 0 | 39 | 0 | 0 | 0 | 0 | 0 | 5661 | 100% | 4.5 min | 14eaa75 | `tests/cts/baseline/gl32-cfg2-wine.txt` |
| 3 | native | 5622 | 0 | 39 | 0 | 0 | 0 | 0 | 0 | 5661 | 100% | 2.2 min | 14eaa75 | `tests/cts/baseline/gl32-cfg3-native.txt` |
| 3 | wine | 5622 | 0 | 39 | 0 | 0 | 0 | 0 | 0 | 5661 | 100% | 4.3 min | 14eaa75 | `tests/cts/baseline/gl32-cfg3-wine.txt` |
| 4 | native | 5622 | 0 | 39 | 0 | 0 | 0 | 0 | 0 | 5661 | 100% | 2.3 min | 14eaa75 | `tests/cts/baseline/gl32-cfg4-native.txt` |
| 4 | wine | 5622 | 0 | 39 | 0 | 0 | 0 | 0 | 0 | 5661 | 100% | 4.2 min | 14eaa75 | `tests/cts/baseline/gl32-cfg4-wine.txt` |

Command lines: `scripts/cts/run.py --mustpass gl32-main.txt --config N` reads them from `mustpass.xml`. CTS build: dEQP Core 1942832354c96f0de379f491519734256fbd4f9d.

## KHR-GL33

| Config | Runner | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Time | Commit | File |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1 | native | 9517 | 0 | 369 | 0 | 0 | 0 | 0 | 0 | 9886 | 100% | 5.2 min | 851476e | `tests/cts/baseline/gl33-cfg1-native.txt` |
| 1 | wine | 9517 | 0 | 369 | 0 | 0 | 0 | 0 | 0 | 9886 | 100% | 9.0 min | 851476e | `tests/cts/baseline/gl33-cfg1-wine.txt` |
| 2 | native | 9517 | 0 | 369 | 0 | 0 | 0 | 0 | 0 | 9886 | 100% | 5.1 min | 851476e | `tests/cts/baseline/gl33-cfg2-native.txt` |
| 2 | wine | 9517 | 0 | 369 | 0 | 0 | 0 | 0 | 0 | 9886 | 100% | 9.9 min | 851476e | `tests/cts/baseline/gl33-cfg2-wine.txt` |
| 3 | native | 9517 | 0 | 369 | 0 | 0 | 0 | 0 | 0 | 9886 | 100% | 5.2 min | 851476e | `tests/cts/baseline/gl33-cfg3-native.txt` |
| 3 | wine | 9517 | 0 | 369 | 0 | 0 | 0 | 0 | 0 | 9886 | 100% | 10.2 min | 851476e | `tests/cts/baseline/gl33-cfg3-wine.txt` |
| 4 | native | 9517 | 0 | 369 | 0 | 0 | 0 | 0 | 0 | 9886 | 100% | 5.2 min | 851476e | `tests/cts/baseline/gl33-cfg4-native.txt` |
| 4 | wine | 9517 | 0 | 369 | 0 | 0 | 0 | 0 | 0 | 9886 | 100% | 11.0 min | 851476e | `tests/cts/baseline/gl33-cfg4-wine.txt` |

Command lines: `scripts/cts/run.py --mustpass gl33-main.txt --config N` reads them from `mustpass.xml`. CTS build: dEQP Core 1942832354c96f0de379f491519734256fbd4f9d.

Against the legacy implementation: `tests/cts/baseline/gl33-cfg1-oldcore.txt` is the last result of the legacy (removed) implementation, configuration 1: 5783 Pass. The core passes 3734 cases it failed and fails 0 cases it passed.

## KHR-GL40

| Config | Runner | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Time | Commit | File |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1 | native | 10425 | 1 | 1436 | 0 | 0 | 0 | 0 | 0 | 11862 | 99.99% | 4.2 min | 851476e | `tests/cts/baseline/gl40-cfg1-native.txt` |
| 1 | wine | 10425 | 1 | 1436 | 0 | 0 | 0 | 0 | 0 | 11862 | 99.99% | 10.6 min | 851476e | `tests/cts/baseline/gl40-cfg1-wine.txt` |
| 2 | native | 10425 | 1 | 1436 | 0 | 0 | 0 | 0 | 0 | 11862 | 99.99% | 4.2 min | 851476e | `tests/cts/baseline/gl40-cfg2-native.txt` |
| 2 | wine | 10425 | 1 | 1436 | 0 | 0 | 0 | 0 | 0 | 11862 | 99.99% | 9.4 min | 851476e | `tests/cts/baseline/gl40-cfg2-wine.txt` |
| 3 | native | 10425 | 1 | 1436 | 0 | 0 | 0 | 0 | 0 | 11862 | 99.99% | 4.2 min | 851476e | `tests/cts/baseline/gl40-cfg3-native.txt` |
| 3 | wine | 10425 | 1 | 1436 | 0 | 0 | 0 | 0 | 0 | 11862 | 99.99% | 10.8 min | 851476e | `tests/cts/baseline/gl40-cfg3-wine.txt` |
| 4 | native | 10425 | 1 | 1436 | 0 | 0 | 0 | 0 | 0 | 11862 | 99.99% | 4.2 min | 851476e | `tests/cts/baseline/gl40-cfg4-native.txt` |
| 4 | wine | 10425 | 1 | 1436 | 0 | 0 | 0 | 0 | 0 | 11862 | 99.99% | 11.0 min | 851476e | `tests/cts/baseline/gl40-cfg4-wine.txt` |

Command lines: `scripts/cts/run.py --mustpass gl40-main.txt --config N` reads them from `mustpass.xml`. CTS build: dEQP Core 1942832354c96f0de379f491519734256fbd4f9d.

Cases that neither pass nor are NotSupported:

| Case | Runs | Reason |
|---|---|---|
| `KHR-GL40.internalformat.renderbuffer.rgb9_e5` | Fail natively in configurations 1, 2, 3, 4; Fail under Wine in configurations 1, 2, 3, 4 | The case expects `glRenderbufferStorage(GL_RGB9_E5)` to succeed (glcInternalformatTests.cpp, RenderbufferCase::createAndAttachRenderBuffer). GL 4.2 section 4.4.2.1 requires `GL_INVALID_ENUM` for an internal format that is not color-, depth- or stencil-renderable, and table 3.12 does not list `GL_RGB9_E5` as color-renderable. |

## KHR-GL41

| Config | Runner | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Time | Commit | File |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1 | native | 10447 | 1 | 1435 | 0 | 0 | 0 | 0 | 0 | 11883 | 99.99% | 4.2 min | 851476e | `tests/cts/baseline/gl41-cfg1-native.txt` |
| 1 | wine | 10447 | 1 | 1435 | 0 | 0 | 0 | 0 | 0 | 11883 | 99.99% | 8.9 min | 851476e | `tests/cts/baseline/gl41-cfg1-wine.txt` |
| 2 | native | 10447 | 1 | 1435 | 0 | 0 | 0 | 0 | 0 | 11883 | 99.99% | 4.2 min | 851476e | `tests/cts/baseline/gl41-cfg2-native.txt` |
| 2 | wine | 10447 | 1 | 1435 | 0 | 0 | 0 | 0 | 0 | 11883 | 99.99% | 9.3 min | 851476e | `tests/cts/baseline/gl41-cfg2-wine.txt` |
| 3 | native | 10447 | 1 | 1435 | 0 | 0 | 0 | 0 | 0 | 11883 | 99.99% | 4.2 min | 851476e | `tests/cts/baseline/gl41-cfg3-native.txt` |
| 3 | wine | 10447 | 1 | 1435 | 0 | 0 | 0 | 0 | 0 | 11883 | 99.99% | 10.2 min | 851476e | `tests/cts/baseline/gl41-cfg3-wine.txt` |
| 4 | native | 10447 | 1 | 1435 | 0 | 0 | 0 | 0 | 0 | 11883 | 99.99% | 4.2 min | 851476e | `tests/cts/baseline/gl41-cfg4-native.txt` |
| 4 | wine | 10447 | 1 | 1435 | 0 | 0 | 0 | 0 | 0 | 11883 | 99.99% | 10.8 min | 851476e | `tests/cts/baseline/gl41-cfg4-wine.txt` |

Command lines: `scripts/cts/run.py --mustpass gl41-main.txt --config N` reads them from `mustpass.xml`. CTS build: dEQP Core 1942832354c96f0de379f491519734256fbd4f9d.

Cases that neither pass nor are NotSupported:

| Case | Runs | Reason |
|---|---|---|
| `KHR-GL41.internalformat.renderbuffer.rgb9_e5` | Fail natively in configurations 1, 2, 3, 4; Fail under Wine in configurations 1, 2, 3, 4 | The case expects `glRenderbufferStorage(GL_RGB9_E5)` to succeed (glcInternalformatTests.cpp, RenderbufferCase::createAndAttachRenderBuffer). GL 4.2 section 4.4.2.1 requires `GL_INVALID_ENUM` for an internal format that is not color-, depth- or stencil-renderable, and table 3.12 does not list `GL_RGB9_E5` as color-renderable. |

## KHR-GL42

| Config | Runner | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Time | Commit | File |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1 | native | 10880 | 2 | 1423 | 0 | 0 | 0 | 0 | 0 | 12305 | 99.98% | 4.5 min | 851476e | `tests/cts/baseline/gl42-cfg1-native.txt` |
| 1 | wine | 10880 | 2 | 1423 | 0 | 0 | 0 | 0 | 0 | 12305 | 99.98% | 11.1 min | 851476e | `tests/cts/baseline/gl42-cfg1-wine.txt` |
| 2 | native | 10880 | 2 | 1423 | 0 | 0 | 0 | 0 | 0 | 12305 | 99.98% | 4.3 min | 851476e | `tests/cts/baseline/gl42-cfg2-native.txt` |
| 2 | wine | 10880 | 2 | 1423 | 0 | 0 | 0 | 0 | 0 | 12305 | 99.98% | 10.0 min | 851476e | `tests/cts/baseline/gl42-cfg2-wine.txt` |
| 3 | native | 10880 | 2 | 1423 | 0 | 0 | 0 | 0 | 0 | 12305 | 99.98% | 4.3 min | 851476e | `tests/cts/baseline/gl42-cfg3-native.txt` |
| 3 | wine | 10880 | 2 | 1423 | 0 | 0 | 0 | 0 | 0 | 12305 | 99.98% | 11.3 min | 851476e | `tests/cts/baseline/gl42-cfg3-wine.txt` |
| 4 | native | 10880 | 2 | 1423 | 0 | 0 | 0 | 0 | 0 | 12305 | 99.98% | 4.4 min | 851476e | `tests/cts/baseline/gl42-cfg4-native.txt` |
| 4 | wine | 10880 | 2 | 1423 | 0 | 0 | 0 | 0 | 0 | 12305 | 99.98% | 10.7 min | 851476e | `tests/cts/baseline/gl42-cfg4-wine.txt` |

Command lines: `scripts/cts/run.py --mustpass gl42-main.txt --config N` reads them from `mustpass.xml`. CTS build: dEQP Core 1942832354c96f0de379f491519734256fbd4f9d.

Cases that neither pass nor are NotSupported:

| Case | Runs | Reason |
|---|---|---|
| `KHR-GL42.internalformat.renderbuffer.rgb9_e5` | Fail natively in configurations 1, 2, 3, 4; Fail under Wine in configurations 1, 2, 3, 4 | The case expects `glRenderbufferStorage(GL_RGB9_E5)` to succeed (glcInternalformatTests.cpp, RenderbufferCase::createAndAttachRenderBuffer). GL 4.2 section 4.4.2.1 requires `GL_INVALID_ENUM` for an internal format that is not color-, depth- or stencil-renderable, and table 3.12 does not list `GL_RGB9_E5` as color-renderable. |
| `KHR-GL42.texture_view.view_sampling` | Fail natively in configurations 1, 2, 3, 4; Fail under Wine in configurations 1, 2, 3, 4 | The case sets the view's filtering with `glTexParameteri` on the parent's target (gl4cTextureViewTests.cpp, TextureViewTestViewSampling::initTextureObject), so a cube-map or cube-map-array view of a parent with another target keeps `GL_LINEAR` magnification, and the fragment stage samples next to a face edge. It expects non-seamless per-face filtering (`GL_TEXTURE_CUBE_MAP_SEAMLESS` is disabled). Metal always filters cube maps seamlessly, blending the neighbouring face. Every other view, target and stage of the case passes. |

## KHR-GL43

| Config | Runner | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Time | Commit | File |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1 | native | 13367 | 3 | 2715 | 0 | 0 | 0 | 0 | 0 | 16085 | 99.97% | 4.7 min | f4ecccd | `tests/cts/baseline/gl43-cfg1-native.txt` |
| 1 | wine | 13367 | 3 | 2715 | 0 | 0 | 0 | 0 | 0 | 16085 | 99.97% | 11.6 min | f4ecccd | `tests/cts/baseline/gl43-cfg1-wine.txt` |
| 2 | native | 13367 | 3 | 2715 | 0 | 0 | 0 | 0 | 0 | 16085 | 99.97% | 4.9 min | f4ecccd | `tests/cts/baseline/gl43-cfg2-native.txt` |
| 2 | wine | 13367 | 3 | 2715 | 0 | 0 | 0 | 0 | 0 | 16085 | 99.97% | 11.3 min | f4ecccd | `tests/cts/baseline/gl43-cfg2-wine.txt` |
| 3 | native | 13367 | 3 | 2715 | 0 | 0 | 0 | 0 | 0 | 16085 | 99.97% | 5.2 min | f4ecccd | `tests/cts/baseline/gl43-cfg3-native.txt` |
| 3 | wine | 13367 | 3 | 2715 | 0 | 0 | 0 | 0 | 0 | 16085 | 99.97% | 9.6 min | f4ecccd | `tests/cts/baseline/gl43-cfg3-wine.txt` |
| 4 | native | 13367 | 3 | 2715 | 0 | 0 | 0 | 0 | 0 | 16085 | 99.97% | 5.2 min | f4ecccd | `tests/cts/baseline/gl43-cfg4-native.txt` |
| 4 | wine | 13367 | 3 | 2715 | 0 | 0 | 0 | 0 | 0 | 16085 | 99.97% | 10.0 min | f4ecccd | `tests/cts/baseline/gl43-cfg4-wine.txt` |

Command lines: `scripts/cts/run.py --mustpass gl43-main.txt --config N` reads them from `mustpass.xml`. CTS build: dEQP Core 1942832354c96f0de379f491519734256fbd4f9d.

Cases that neither pass nor are NotSupported:

| Case | Runs | Reason |
|---|---|---|
| `KHR-GL43.clear_tex_image.negative_gl_clear_tex_and_sub_image_compressed_texture` | Fail natively in configurations 1, 2, 3, 4; Fail under Wine in configurations 1, 2, 3, 4 | The case defines its texture with the generic GL_COMPRESSED_RGBA and expects glClearTexImage to raise INVALID_OPERATION for a compressed image (gl4cClearTexImageAndSubImageTests.cpp, compressedTextureTestCase). GL 4.6 section 8.6 lets the GL store a generic compressed format uncompressed; WineMetalGL stores it as RGBA8 (GL_TEXTURE_COMPRESSED is GL_FALSE), so the clear is valid. |
| `KHR-GL43.internalformat.renderbuffer.rgb9_e5` | Fail natively in configurations 1, 2, 3, 4; Fail under Wine in configurations 1, 2, 3, 4 | The case expects `glRenderbufferStorage(GL_RGB9_E5)` to succeed (glcInternalformatTests.cpp, RenderbufferCase::createAndAttachRenderBuffer). GL 4.2 section 4.4.2.1 requires `GL_INVALID_ENUM` for an internal format that is not color-, depth- or stencil-renderable, and table 3.12 does not list `GL_RGB9_E5` as color-renderable. |
| `KHR-GL43.texture_view.view_sampling` | Fail natively in configurations 1, 2, 3, 4; Fail under Wine in configurations 1, 2, 3, 4 | The case sets the view's filtering with `glTexParameteri` on the parent's target (gl4cTextureViewTests.cpp, TextureViewTestViewSampling::initTextureObject), so a cube-map or cube-map-array view of a parent with another target keeps `GL_LINEAR` magnification, and the fragment stage samples next to a face edge. It expects non-seamless per-face filtering (`GL_TEXTURE_CUBE_MAP_SEAMLESS` is disabled). Metal always filters cube maps seamlessly, blending the neighbouring face. Every other view, target and stage of the case passes. |

## KHR-GL46

| Config | Runner | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Time | Commit | File |
|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| 1 | native | 15157 | 3 | 4554 | 0 | 0 | 0 | 0 | 0 | 19714 | 99.98% | 4.7 min | 49c5099 | `tests/cts/baseline/gl46-cfg1-native.txt` |
| 1 | wine | 15157 | 3 | 4554 | 0 | 0 | 0 | 0 | 0 | 19714 | 99.98% | 7.4 min | 49c5099 | `tests/cts/baseline/gl46-cfg1-wine.txt` |
| 3 | wine | 15157 | 3 | 4554 | 0 | 0 | 0 | 0 | 0 | 19714 | 99.98% | 7.2 min | 49c5099 | `tests/cts/baseline/gl46-cfg3-wine.txt` |

Command lines: `scripts/cts/run.py --mustpass gl46-main.txt --config N` reads them from `mustpass.xml`. CTS build: dEQP Core 1942832354c96f0de379f491519734256fbd4f9d.

Cases that neither pass nor are NotSupported:

| Case | Runs | Reason |
|---|---|---|
| `KHR-GL46.clear_tex_image.negative_gl_clear_tex_and_sub_image_compressed_texture` | Fail natively in configuration 1; Fail under Wine in configurations 1, 3 | The case defines its texture with the generic GL_COMPRESSED_RGBA and expects glClearTexImage to raise INVALID_OPERATION for a compressed image (gl4cClearTexImageAndSubImageTests.cpp, compressedTextureTestCase). GL 4.6 section 8.6 lets the GL store a generic compressed format uncompressed; WineMetalGL stores it as RGBA8 (GL_TEXTURE_COMPRESSED is GL_FALSE), so the clear is valid. |
| `KHR-GL46.internalformat.renderbuffer.rgb9_e5` | Fail natively in configuration 1; Fail under Wine in configurations 1, 3 | The case expects `glRenderbufferStorage(GL_RGB9_E5)` to succeed (glcInternalformatTests.cpp, RenderbufferCase::createAndAttachRenderBuffer). GL 4.2 section 4.4.2.1 requires `GL_INVALID_ENUM` for an internal format that is not color-, depth- or stencil-renderable, and table 3.12 does not list `GL_RGB9_E5` as color-renderable. |
| `KHR-GL46.texture_view.view_sampling` | Fail natively in configuration 1; Fail under Wine in configurations 1, 3 | The case sets the view's filtering with `glTexParameteri` on the parent's target (gl4cTextureViewTests.cpp, TextureViewTestViewSampling::initTextureObject), so a cube-map or cube-map-array view of a parent with another target keeps `GL_LINEAR` magnification, and the fragment stage samples next to a face edge. It expects non-seamless per-face filtering (`GL_TEXTURE_CUBE_MAP_SEAMLESS` is disabled). Metal always filters cube maps seamlessly, blending the neighbouring face. Every other view, target and stage of the case passes. |

## OpenGL ES

OpenGL ES contexts (`WGL_EXT_create_context_es2_profile`; OpenGL ES 3.2) run the OpenGL ES mustpass lists, Khronos's (`gles*-khr-*`) and dEQP's (`aosp_mustpass`), each in configuration 1 of its `mustpass.xml` under Wine: `scripts/cts/run.py --mustpass LIST.txt --config 1`.

`cts-runner --type=es3`, which runs every OpenGL ES 2.0 and 3.0 list in each configuration it selects (22 WGL window pixel formats: 1 to 10 (no samples, no sRGB), 31 to 40 (sRGB-capable), 61 and 62 (RGB565)), at commit c57a9ed: 48 runs, 219,931 cases, no failing case (Fail, Crash, Timeout, InternalError) in any run. The runs are listed in `tests/cts/es/cts-runner-es3.txt`.

| List | Runs | OpenGL ES | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Time | Commit | File |
|---|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|
| `gles2-khr-main` | KHR-GLES2 | 2.0 | 440 | 0 | 33 | 0 | 0 | 0 | 0 | 0 | 473 | 100% | 7 s | c56a900 | `tests/cts/baseline/gles2-khr-main-cfg1-wine.txt` |
| `gles2-khr-noctx-main` | KHR-NoContext.es2 | 2.0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 1 | 100% | 3 s | c56a900 | `tests/cts/baseline/gles2-khr-noctx-main-cfg1-wine.txt` |
| `gles2-main` | dEQP-GLES2 | 2.0 | 17075 | 0 | 90 | 0 | 0 | 0 | 0 | 0 | 17165 | 100% | 3.5 min | c56a900 | `tests/cts/baseline/gles2-main-cfg1-wine.txt` |
| `gles3-khr-main` | KHR-GLES3 | 3.0 | 6479 | 0 | 19 | 0 | 0 | 0 | 0 | 0 | 6498 | 100% | 49 s | c56a900 | `tests/cts/baseline/gles3-khr-main-cfg1-wine.txt` |
| `gles3-main` | dEQP-GLES3 | 3.0 | 44335 | 0 | 433 | 0 | 0 | 0 | 0 | 0 | 44768 | 100% | 13.1 min | c56a900 | `tests/cts/baseline/gles3-main-cfg1-wine.txt` |
| `gles3-rotate-portrait` | dEQP-GLES3, screen rotation 0 | 3.0 | 963 | 0 | 20 | 0 | 0 | 0 | 0 | 0 | 983 | 100% | 11 s | c56a900 | `tests/cts/baseline/gles3-rotate-portrait-cfg1-wine.txt` |
| `gles3-rotate-landscape` | dEQP-GLES3, screen rotation 90 | 3.0 | 963 | 0 | 20 | 0 | 0 | 0 | 0 | 0 | 983 | 100% | 10 s | c56a900 | `tests/cts/baseline/gles3-rotate-landscape-cfg1-wine.txt` |
| `gles3-rotate-reverse-portrait` | dEQP-GLES3, screen rotation 180 | 3.0 | 963 | 0 | 20 | 0 | 0 | 0 | 0 | 0 | 983 | 100% | 10 s | c56a900 | `tests/cts/baseline/gles3-rotate-reverse-portrait-cfg1-wine.txt` |
| `gles3-rotate-reverse-landscape` | dEQP-GLES3, screen rotation 270 | 3.0 | 963 | 0 | 20 | 0 | 0 | 0 | 0 | 0 | 983 | 100% | 10 s | c56a900 | `tests/cts/baseline/gles3-rotate-reverse-landscape-cfg1-wine.txt` |
| `gles3-multisample` | dEQP-GLES3, 4 samples | 3.0 | 4615 | 0 | 20 | 0 | 0 | 0 | 0 | 0 | 4635 | 100% | 33 s | c56a900 | `tests/cts/baseline/gles3-multisample-cfg1-wine.txt` |
| `gles3-565-no-depth-no-stencil` | dEQP-GLES3, RGB565 without depth and stencil | 3.0 | 3332 | 0 | 8 | 0 | 0 | 0 | 0 | 0 | 3340 | 100% | 35 s | c56a900 | `tests/cts/baseline/gles3-565-no-depth-no-stencil-cfg1-wine.txt` |
| `gles31-khr-main` | KHR-GLES31 | 3.1 | 4052 | 1 | 48 | 0 | 0 | 0 | 0 | 0 | 4101 | 99.97% | 57 s | 71b7989 | `tests/cts/baseline/gles31-khr-main-cfg1-wine.txt` |
| `gles31-khr-single` | KHR-Single-GLES31 | 3.1 | 0 | 0 | 6053 | 0 | 0 | 0 | 0 | 0 | 6053 | 100% | 13 s | 71b7989 | `tests/cts/baseline/gles31-khr-single-cfg1-wine.txt` |
| `gles31-main` | dEQP-GLES31 | 3.1 | 37400 | 6 | 396 | 0 | 0 | 0 | 0 | 0 | 37802 | 99.98% | 14.4 min | 71b7989 | `tests/cts/baseline/gles31-main-cfg1-wine.txt` |
| `gles31-rotate-portrait` | dEQP-GLES31, screen rotation 0 | 3.1 | 114 | 0 | 18 | 0 | 0 | 0 | 0 | 0 | 132 | 100% | 7 s | 71b7989 | `tests/cts/baseline/gles31-rotate-portrait-cfg1-wine.txt` |
| `gles31-rotate-landscape` | dEQP-GLES31, screen rotation 90 | 3.1 | 114 | 0 | 18 | 0 | 0 | 0 | 0 | 0 | 132 | 100% | 7 s | 71b7989 | `tests/cts/baseline/gles31-rotate-landscape-cfg1-wine.txt` |
| `gles31-rotate-reverse-portrait` | dEQP-GLES31, screen rotation 180 | 3.1 | 114 | 0 | 18 | 0 | 0 | 0 | 0 | 0 | 132 | 100% | 7 s | 71b7989 | `tests/cts/baseline/gles31-rotate-reverse-portrait-cfg1-wine.txt` |
| `gles31-rotate-reverse-landscape` | dEQP-GLES31, screen rotation 270 | 3.1 | 114 | 0 | 18 | 0 | 0 | 0 | 0 | 0 | 132 | 100% | 7 s | 71b7989 | `tests/cts/baseline/gles31-rotate-reverse-landscape-cfg1-wine.txt` |
| `gles31-multisample` | dEQP-GLES31, 4 samples | 3.1 | 216 | 0 | 23 | 0 | 0 | 0 | 0 | 0 | 239 | 100% | 9 s | 71b7989 | `tests/cts/baseline/gles31-multisample-cfg1-wine.txt` |
| `gles31-565-no-depth-no-stencil` | dEQP-GLES31, RGB565 without depth and stencil | 3.1 | 21 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 23 | 100% | 6 s | 71b7989 | `tests/cts/baseline/gles31-565-no-depth-no-stencil-cfg1-wine.txt` |
| `gles32-khr-main` | KHR-GLES32 | 3.2 | 1403 | 0 | 2 | 0 | 0 | 0 | 0 | 0 | 1405 | 100% | 28 s | 71b7989 | `tests/cts/baseline/gles32-khr-main-cfg1-wine.txt` |
| `gles32-khr-noctx-main` | KHR-NoContext.es32 | 3.2 | 12 | 1 | 0 | 0 | 0 | 0 | 0 | 0 | 13 | 92.30% | 6 s | 71b7989 | `tests/cts/baseline/gles32-khr-noctx-main-cfg1-wine.txt` |
| `gles32-khr-single` | KHR-Single-GLES32 | 3.2 | 0 | 0 | 6053 | 0 | 0 | 0 | 0 | 0 | 6053 | 100% | 14 s | 71b7989 | `tests/cts/baseline/gles32-khr-single-cfg1-wine.txt` |
| `gles32-khr-glesext` | KHR-GLESEXT | 3.2 | 1081 | 0 | 16 | 0 | 0 | 0 | 0 | 0 | 1097 | 100% | 10 s | 71b7989 | `tests/cts/baseline/gles32-khr-glesext-cfg1-wine.txt` |

CTS build: dEQP Core 1942832354c96f0de379f491519734256fbd4f9d.

### `gles31-khr-main`: cases that neither pass nor are NotSupported

| Case | Result | Reason |
|---|---|---|
| `KHR-GLES31.core.shader_atomic_counters.advanced-usage-multidim-array-small` | Fail | Not a CTS defect: a WineMetalGL limitation. The shader passes arrays of atomic counters to functions (es31cShaderAtomicCountersTests.cpp, AdvancedUsageMultiDimArray); glslang lowers atomic counters to storage buffer words for SPIR-V, which function parameters cannot reference, and the shader does not compile. |

### `gles31-main`: cases that neither pass nor are NotSupported

| Case | Result | Reason |
|---|---|---|
| `dEQP-GLES31.functional.program_interface_query.program_input.type.interface_blocks.in.named_block.array.mat3x2` | Fail | Not a CTS defect: a WineMetalGL limitation. The shaders pass arrays of matrices between stages in an interface block; Metal stage inputs and outputs cannot hold arrays of matrices (SPIRV-Cross reports "MSL cannot emit arrays-of-matrices in input and output variables"), so the program does not link. |
| `dEQP-GLES31.functional.program_interface_query.program_input.type.interface_blocks.in.named_block_explicit_location.array.mat3x2` | Fail | Not a CTS defect: a WineMetalGL limitation. The shaders pass arrays of matrices between stages in an interface block; Metal stage inputs and outputs cannot hold arrays of matrices (SPIRV-Cross reports "MSL cannot emit arrays-of-matrices in input and output variables"), so the program does not link. |
| `dEQP-GLES31.functional.program_interface_query.program_input.type.interface_blocks.in.unnamed_block.array.mat3x2` | Fail | Not a CTS defect: a WineMetalGL limitation. The shaders pass arrays of matrices between stages in an interface block; Metal stage inputs and outputs cannot hold arrays of matrices (SPIRV-Cross reports "MSL cannot emit arrays-of-matrices in input and output variables"), so the program does not link. |
| `dEQP-GLES31.functional.program_interface_query.program_output.type.interface_blocks.out.named_block.array.mat3x2` | Fail | Not a CTS defect: a WineMetalGL limitation. The shaders pass arrays of matrices between stages in an interface block; Metal stage inputs and outputs cannot hold arrays of matrices (SPIRV-Cross reports "MSL cannot emit arrays-of-matrices in input and output variables"), so the program does not link. |
| `dEQP-GLES31.functional.program_interface_query.program_output.type.interface_blocks.out.named_block_explicit_location.array.mat3x2` | Fail | Not a CTS defect: a WineMetalGL limitation. The shaders pass arrays of matrices between stages in an interface block; Metal stage inputs and outputs cannot hold arrays of matrices (SPIRV-Cross reports "MSL cannot emit arrays-of-matrices in input and output variables"), so the program does not link. |
| `dEQP-GLES31.functional.program_interface_query.program_output.type.interface_blocks.out.unnamed_block.array.mat3x2` | Fail | Not a CTS defect: a WineMetalGL limitation. The shaders pass arrays of matrices between stages in an interface block; Metal stage inputs and outputs cannot hold arrays of matrices (SPIRV-Cross reports "MSL cannot emit arrays-of-matrices in input and output variables"), so the program does not link. |

### `gles32-khr-noctx-main`: cases that neither pass nor are NotSupported

| Case | Result | Reason |
|---|---|---|
| `KHR-NoContext.es32.robust_buffer_access_behavior.uniform_buffer` | Fail | Not a CTS defect: a WineMetalGL limitation. The case reads a uniform block array past its end in a robust context and expects zero or a value from the buffer (glcRobustBufferAccessBehaviorTests.cpp, UniformBufferTest); Metal does not bound uniform buffer reads, so the value is whatever memory follows (the case passes or fails from run to run). |

NotSupported cases, and the reason for each: [Unsupported](unsupported.md#cts-cases-reported-notsupported).

## API

GL 4.6 core profile commands in the Khronos registry: 656. Implemented: 656. Missing: none.

Advertised extensions: `GL_ARB_compatibility`, `GL_ARB_multitexture`, `GL_ARB_transpose_matrix`, `GL_ARB_texture_env_add`, `GL_EXT_texture_env_add`, `GL_ARB_texture_env_combine`, `GL_EXT_texture_env_combine`, `GL_ARB_texture_env_dot3`, `GL_ARB_texture_env_crossbar`, `GL_ARB_texture_cube_map`, `GL_EXT_texture_cube_map`, `GL_ARB_texture_border_clamp`, `GL_ARB_texture_compression`, `GL_ARB_texture_mirrored_repeat`, `GL_ARB_depth_texture`, `GL_ARB_shadow`, `GL_ARB_window_pos`, `GL_ARB_point_parameters`, `GL_ARB_point_sprite`, `GL_EXT_secondary_color`, `GL_EXT_fog_coord`, `GL_EXT_blend_func_separate`, `GL_EXT_blend_minmax`, `GL_EXT_blend_subtract`, `GL_EXT_blend_color`, `GL_EXT_stencil_wrap`, `GL_EXT_texture_lod_bias`, `GL_EXT_bgra`, `GL_EXT_draw_range_elements`, `GL_EXT_compiled_vertex_array`, `GL_EXT_rescale_normal`, `GL_EXT_separate_specular_color`, `GL_ARB_vertex_buffer_object`, `GL_ARB_texture_non_power_of_two`, `GL_ARB_occlusion_query`, `GL_ARB_draw_buffers`, `GL_ARB_texture_float`, `GL_ARB_half_float_pixel`, `GL_ARB_pixel_buffer_object`, `GL_EXT_pixel_buffer_object`, `GL_SGIS_generate_mipmap`, `GL_ARB_texture_rectangle`, `GL_NV_texture_rectangle`, `GL_NV_blend_square`, `GL_EXT_packed_pixels`, `GL_SGIS_texture_edge_clamp`, `GL_ARB_multisample`, `GL_EXT_texture3D`, `GL_EXT_blend_equation_separate`, `GL_ARB_shading_language_100`, `GL_ARB_shader_objects`, `GL_ARB_vertex_shader`, `GL_ARB_fragment_shader`, `GL_ARB_vertex_program`, `GL_ARB_fragment_program`, `GL_ARB_fragment_program_shadow`, `GL_EXT_gpu_program_parameters`, `GL_EXT_texture_compression_latc`, `GL_ARB_vertex_type_2_10_10_10_rev`, `GL_ARB_ES2_compatibility`, `GL_ARB_texture_storage`, `GL_EXT_texture_storage`, `GL_ARB_compressed_texture_pixel_storage`, `GL_ARB_texture_storage_multisample`, `GL_EXT_texture_compression_s3tc`, `GL_EXT_texture_filter_anisotropic`, `GL_ARB_texture_filter_anisotropic`, `GL_ARB_framebuffer_object`, `GL_EXT_framebuffer_object`, `GL_EXT_framebuffer_blit`, `GL_EXT_framebuffer_multisample`, `GL_EXT_packed_depth_stencil`, `GL_ARB_vertex_array_object`, `GL_ARB_map_buffer_range`, `GL_ARB_texture_rg`, `GL_ARB_texture_compression_rgtc`, `GL_ARB_instanced_arrays`, `GL_ARB_draw_instanced`, `GL_ARB_draw_elements_base_vertex`, `GL_ARB_uniform_buffer_object`, `GL_ARB_copy_buffer`, `GL_ARB_sync`, `GL_ARB_timer_query`, `GL_EXT_timer_query`, `GL_ARB_occlusion_query2`, `GL_ARB_sampler_objects`, `GL_ARB_texture_swizzle`, `GL_ARB_depth_clamp`, `GL_ARB_seamless_cube_map`, `GL_ARB_provoking_vertex`, `GL_ARB_texture_multisample`, `GL_ARB_blend_func_extended`, `GL_ARB_explicit_attrib_location`, `GL_ARB_fragment_coord_conventions`, `GL_ARB_shader_bit_encoding`, `GL_ARB_texture_rgb10_a2ui`, `GL_ARB_half_float_vertex`, `GL_ARB_texture_buffer_object`, `GL_ARB_framebuffer_sRGB`, `GL_EXT_framebuffer_sRGB`, `GL_EXT_texture_sRGB`, `GL_ARB_map_buffer_alignment`, `GL_ARB_depth_texture`, `GL_ARB_texture_float`, `GL_EXT_texture_shared_exponent`, `GL_EXT_texture_integer`, `GL_ARB_draw_buffers_blend`, `GL_ARB_base_instance`, `GL_ARB_clip_control`, `GL_ARB_get_program_binary`, `GL_ARB_buffer_storage`, `GL_AMD_pinned_memory`, `GL_KHR_debug`, `GL_ARB_debug_output`, `GL_ARB_separate_shader_objects`, `GL_KHR_robustness`, `GL_ARB_robustness`, `GL_ARB_internalformat_query`, `GL_ARB_invalidate_subdata`, `GL_ARB_copy_image`, `GL_NVX_gpu_memory_info`, `GL_ATI_meminfo`, `GL_ARB_draw_indirect`, `GL_ARB_gpu_shader5`, `GL_ARB_gpu_shader_fp64`, `GL_ARB_sample_shading`, `GL_ARB_shader_subroutine`, `GL_ARB_tessellation_shader`, `GL_ARB_texture_buffer_object_rgb32`, `GL_ARB_texture_cube_map_array`, `GL_ARB_texture_gather`, `GL_ARB_texture_query_lod`, `GL_ARB_transform_feedback2`, `GL_ARB_transform_feedback3`, `GL_ARB_shader_precision`, `GL_ARB_vertex_attrib_64bit`, `GL_ARB_viewport_array`, `GL_ARB_conservative_depth`, `GL_ARB_shader_atomic_counters`, `GL_ARB_shader_image_load_store`, `GL_ARB_shading_language_420pack`, `GL_ARB_shading_language_packing`, `GL_ARB_texture_compression_bptc`, `GL_ARB_transform_feedback_instanced`, `GL_ARB_compute_shader`, `GL_ARB_explicit_uniform_location`, `GL_ARB_program_interface_query`, `GL_ARB_shader_storage_buffer_object`, `GL_ARB_texture_buffer_range`, `GL_ARB_texture_view`, `GL_ARB_texture_query_levels`, `GL_ARB_vertex_attrib_binding`, `GL_ARB_arrays_of_arrays`, `GL_ARB_clear_buffer_object`, `GL_ARB_ES3_compatibility`, `GL_ARB_fragment_layer_viewport`, `GL_ARB_framebuffer_no_attachments`, `GL_ARB_internalformat_query2`, `GL_ARB_multi_draw_indirect`, `GL_ARB_shader_image_size`, `GL_ARB_stencil_texturing`, `GL_ARB_texture_barrier`, `GL_ARB_clear_texture`, `GL_ARB_enhanced_layouts`, `GL_ARB_multi_bind`, `GL_ARB_query_buffer_object`, `GL_ARB_texture_mirror_clamp_to_edge`, `GL_ARB_texture_stencil8`, `GL_ARB_vertex_type_10f_11f_11f_rev`, `GL_ARB_conditional_render_inverted`, `GL_ARB_cull_distance`, `GL_ARB_derivative_control`, `GL_ARB_direct_state_access`, `GL_ARB_ES3_1_compatibility`, `GL_ARB_get_texture_sub_image`, `GL_ARB_shader_texture_image_samples`, `GL_KHR_context_flush_control`, `GL_ARB_gl_spirv`, `GL_ARB_spirv_extensions`, `GL_ARB_indirect_parameters`, `GL_ARB_pipeline_statistics_query`, `GL_ARB_shader_atomic_counter_ops`, `GL_ARB_shader_draw_parameters`, `GL_ARB_shader_group_vote`, `GL_ARB_transform_feedback_overflow_query`, `GL_ARB_parallel_shader_compile`, `GL_KHR_parallel_shader_compile`, `GL_KHR_no_error`, `GL_EXT_shader_integer_mix`, `GL_ARB_polygon_offset_clamp`, `GL_EXT_polygon_offset_clamp`, `GL_ARB_shader_viewport_layer_array`, `GL_EXT_texture_sRGB_decode`, `GL_EXT_texture_shadow_lod`.

The machine-readable form is `docs/api-coverage.json`.
