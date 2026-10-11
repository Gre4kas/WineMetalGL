# Pinned dependencies

The package vendors the source required to build without fetching during
configuration:

| Component | Revision | Purpose |
| --- | --- | --- |
| SPIRV-Cross | `bccaa94db814af33d8ef05c153e7c34d8bd4d685` | SPIR-V to GLSL/MSL translation |
| glslang | `46ef757e048e760b46601e6e77ae0cb72c97bd2f` | GLSL parsing and SPIR-V generation |
| Reference tessellator (Microsoft, MIT; from Mesa) | Mesa `65c2dc5a634b6ee31979546ec5eb5212d6ffe721` | GL tessellation (`vendor/tessellator`) |

Local changes to vendored sources (each marked `WineMetalGL:` in the code):

- glslang `glslang/MachineIndependent/linkValidate.cpp`, `checkStageIO`: the
  Vulkan "every input has a matching output" check skips built-in blocks
  (`gl_PerVertex`) and redeclared built-in inputs (`gl_ClipDistance`), which
  GL provides implicitly, and inputs the stage never reads, which GL does
  not require to be written.
- glslang `glslang/MachineIndependent/linkValidate.cpp`, `isIoResizeArray`:
  per-patch tessellation evaluation inputs (`patch in`) are not arrayed per
  vertex, so `patch` arrays match their control-stage outputs at link time.
- SPIRV-Cross `spirv_glsl.cpp`, `type_to_packed_alignment` and
  `type_to_packed_size`: std140 rounds a matrix column (or row) up to the
  alignment of a float vec4 (16 bytes), as GL 4.6 section 7.6.2.2 rule 4
  says, not of a vector of the matrix's component type; double matrices with
  two rows otherwise fail the layout check when geometry shaders are
  translated to GLSL. Float layouts are unchanged.
- glslang `glslang/MachineIndependent/ParseHelper.cpp`, `fixOffset`: an
  atomic counter whose offset puts it past `gl_MaxAtomicCounterBufferSize` is
  a compile-time error, as GLSL 4.20 requires.
- glslang `glslang/MachineIndependent/Versions.{h,cpp}`: `GL_ARB_shader_subroutine`
  is a known extension (and predefined macro); WineMetalGL lowers subroutines
  before glslang parses a shader (`src/core/shader/subroutines.h`).
- glslang `glslang/MachineIndependent/ParseHelper.cpp`, `redeclareBuiltinBlock`:
  `gl_PerVertex` may be redeclared from GLSL 1.50, as desktop drivers allow,
  instead of only from 4.10 or with `GL_ARB_separate_shader_objects`.
- glslang `glslang/MachineIndependent/ParseHelper.cpp`, `setLayoutQualifier`:
  `layout(binding)` is also accepted with `GL_ARB_shader_atomic_counters`,
  which adds it for atomic counters.
- SPIRV-Cross `spirv_msl.{hpp,cpp}`: an MSL option, `native_texture_atomics`,
  emits image atomics as MSL 3.1 texture atomics instead of emulating them
  through a buffer that aliases the texture.
- SPIRV-Cross `spirv_msl.cpp`, `OpImageWrite`: a write to a `read_write`
  texture is followed by `fence()`, so the invocation's later loads see it.
- SPIRV-Cross `spirv_msl.cpp`, `InterpolateAtOffset`: the GLSL offset is
  biased by 1/2 (upstream uses 7/16 for Intel GPUs), so on Apple GPUs an
  offset of `gl_SamplePosition - 0.5` interpolates exactly at the sample.
- glslang `glslang/MachineIndependent/ParseHelper.cpp`, `invariantCheck`:
  desktop GLSL 4.20 and later accept `invariant` on inputs outside the vertex
  stage, as desktop drivers do; only ESSL 3.00 and later restrict it to
  outputs.
- glslang `glslang/MachineIndependent/ParseHelper.cpp`, `layoutTypeCheck`:
  an image's `layout(binding)` is checked against `gl_MaxImageUnits` (the
  image units it names) instead of `gl_MaxCombinedTextureImageUnits`.
- glslang `glslang/MachineIndependent/linkValidate.cpp`, `finalCheck`: a
  std140/std430 block whose non-last member is an array sized only by its
  largest constant index gets its member offsets recomputed once that size is
  known (they were assigned when the block was declared).
- glslang `glslang/MachineIndependent/ParseHelper.cpp` and
  `localintermediate.h`, `vkRelaxedRemapUniformVariable`: every global
  uniform declared with `layout(location)` is recorded with the number of
  locations it spans (`getExplicitUniformLocations`), for the linker's GL 4.3
  uniform location assignment; the Vulkan rules drop the qualifier.
- glslang `glslang/MachineIndependent/ParseHelper.cpp`,
  `vkRelaxedRemapUniformVariable`: an unsized uniform array takes its size
  from its initializer before the Vulkan rules drop the initializer (the
  values are applied from the default block, `applyUniformInitializers`).
- SPIRV-Cross `spirv_glsl.cpp`, `bitcast_expression`, and `spirv_msl.cpp`,
  `emit_atomic_func_op`: atomic operands are read unpacked, so an element of a
  scalar array in a std140 block (padded to a vector) is passed as a scalar.
- SPIRV-Cross `spirv_msl.cpp`, `get_argument_address_space`: `coherent`
  storage no longer becomes `volatile` (only `volatile` does): Metal has no
  element access on volatile matrices, and MSL's barriers and atomics give
  GL's coherent visibility.
- SPIRV-Cross `spirv_msl.cpp`, `add_composite_variable_to_interface_block`:
  a one-dimensional array of matrices (`in mat2x3 d[2]`) is flattened into one
  input or output per column instead of being rejected.
- SPIRV-Cross `spirv_glsl.cpp`, `to_function_name` and `to_function_args`:
  GLSL has no `textureLod` for rectangle textures and their `texelFetch`
  takes no level, so a level-0 sample (glslang's `texture()` outside fragment
  shaders) is written as `texture()` and a fetch without a level, when
  geometry and tessellation control stages are translated back to GLSL.

The built dylib dynamically links only Apple system frameworks and libraries
(Metal, Foundation, QuartzCore, CoreFoundation, libc++, libSystem and
libobjc); it never links OpenGL.framework (`scripts/verify-source.sh` and CI
check this).

Wine integration requires the Wine patch series in `patches/wine/`, applied to
a Wine 11.17 source tree (`patches/wine/README.md`).

Test tooling that is not vendored: the Khronos VK-GL-CTS (commit `1942832`,
built with `tests/cts/windows/build.sh` and `tests/cts/native/build.sh`),
MinGW-w64 for the Wine fixtures and demos, and apitrace for trace corpus
entries (`tests/games/README.md`).
