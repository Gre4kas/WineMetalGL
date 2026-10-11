# Pinned dependencies

The package vendors the source required to build without fetching during
configuration:

| Component | Revision | Purpose |
| --- | --- | --- |
| SPIRV-Cross | `bccaa94db814af33d8ef05c153e7c34d8bd4d685` | SPIR-V to GLSL/MSL translation |
| glslang | `46ef757e048e760b46601e6e77ae0cb72c97bd2f` | GLSL parsing and SPIR-V generation |

Local changes to vendored sources (each marked `WineMetalGL:` in the code):

- glslang `glslang/MachineIndependent/linkValidate.cpp`, `checkStageIO`: the
  Vulkan "every input has a matching output" check skips built-in blocks
  (`gl_PerVertex`) and redeclared built-in inputs (`gl_ClipDistance`), which
  GL provides implicitly.
- SPIRV-Cross `spirv_glsl.cpp`, `type_to_packed_alignment` and
  `type_to_packed_size`: std140 rounds a matrix column (or row) up to the
  alignment of a float vec4 (16 bytes), as GL 4.6 section 7.6.2.2 rule 4
  says, not of a vector of the matrix's component type; double matrices with
  two rows otherwise fail the layout check when geometry shaders are
  translated to GLSL. Float layouts are unchanged.
- glslang `glslang/MachineIndependent/ParseHelper.cpp`, `fixOffset`: an
  atomic counter whose offset puts it past `gl_MaxAtomicCounterBufferSize` is
  a compile-time error, as GLSL 4.20 requires.

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
