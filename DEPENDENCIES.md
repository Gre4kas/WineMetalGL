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
  GL provides implicitly.
- glslang `glslang/MachineIndependent/linkValidate.cpp`, `isIoResizeArray`:
  per-patch tessellation evaluation inputs (`patch in`) are not arrayed per
  vertex, so `patch` arrays match their control-stage outputs at link time.

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
