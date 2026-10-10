# 0001: OpenGL implementation strategy

- **Status:** accepted
- **Date:** 2026-10-10
- **Context:** checkpoint C1 of the GL 3.3 work (`feat/gl33-core`)

## Question

Should WineMetalGL implement OpenGL on Metal itself (a native core), or ship
Mesa's Zink (OpenGL on Vulkan) on top of a Metal-backed Vulkan driver?

## What was measured

All measurements were taken on an Apple M4 running macOS 27.2.

**Zink in the guest.** The `opengl32.dll` and `libgallium_wgl.dll` from mesa-dist-win 26.2.4 (MSVC) were used with `GALLIUM_DRIVER=zink`. They ran through Wine 11.17's `winevulkan`, with MoltenVK as the host `libvulkan.dylib`.
- `glcts.exe` cannot create the context for `KHR-GL30`, `KHR-GL31`, `KHR-GL32` or `KHR-GL33`: "Failed to create WGL context".
- With `MESA_GL_VERSION_OVERRIDE=3.3`, context creation succeeds, but the first query fails with `GL_OUT_OF_MEMORY`.
- Zink also reports that the device lacks its base requirements `logicOp` and `EXT_custom_border_color`.

**Vulkan feature probe** (`vkGetPhysicalDeviceFeatures` plus device extensions):

| Feature (needed by Zink for) | KosmicKrisp (Mesa 26.1.4) | MoltenVK (MetalSharp bundle) |
|---|---|---|
| `geometryShader` (GL 3.2) | **no** | **no** |
| `VK_EXT_transform_feedback` (GL 3.0) | **no** | yes |
| `fillModeNonSolid` (`glPolygonMode`) | no | yes |
| `logicOp` | yes | no |
| `VK_EXT_custom_border_color` | no | no |
| `VK_EXT_provoking_vertex` | no | no |
| `tessellationShader` | no | yes |

Neither driver exposes geometry shaders, so Zink cannot offer GL 3.2 or 3.3 on either.

## Decision

**Native core.** WineMetalGL implements GL 3.3 directly on Metal in `src/core/`,
following the workstreams W05–W26. Metal's missing stages (geometry shaders,
transform feedback) are emulated in WineMetalGL itself.

## Revisit

At each checkpoint (C2, C3, C4), re-run the feature probe against the newest KosmicKrisp. If it gains `geometryShader` and `VK_EXT_transform_feedback`, compare Zink + KosmicKrisp against the native core on the KHR-GL33 baseline. A hybrid (Zink for games it handles, native elsewhere) is only worth it if it doesn't split the work.

## References surveyed

- **ANGLE's Metal backend** (BSD-3-Clause): GLES 3.0 on Metal, shipping in Safari and Chrome. Useful for:
  - index conversion (line loops, triangle fans, `GL_UNSIGNED_BYTE`, primitive restart)
  - provoking-vertex emulation
  - the format table
  - transform-feedback emulation in the vertex stage
  - clear/blit utilities

  Code may be reused with its license notice.
- **MGL** (openglonmetal/MGL, Apache-2.0, last push 2026-01): a GL 4.6 core-profile prototype on Metal using glslang and SPIRV-Cross. Useful as a reference for its state model. Code may be reused with an Apache-2.0 NOTICE.
- **Mesa** (MIT): fixed-function program generation (`ffvertex_prog`, texenv) and ARB assembly parsing, for the compatibility profile.
