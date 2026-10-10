# Wine integration

The patches in `wine-11.17/` integrate WineMetalGL with Wine 11.17 for x86_64
Unix Wine with x86_64 and i386 guests in one WoW64 prefix. They apply with
`git apply` from the top of a Wine 11.17 source tree (checked against the
`wine-11.17` release), and any Wine 11.17 built with them loads the core.
The results in `docs/` were recorded with MetalSharp's Wine 11.17, which the
x86_64 release's prebuilt Wine modules are built from. Other Wine versions may
need a small rebase when Wine's OpenGL driver interface changes.

- `0001-opengl-wgl-fixes.patch`: three Wine OpenGL fixes.
  `win32u` indexes the parsed extension list correctly for drivers without
  `GL_NUM_EXTENSIONS`, `wglChoosePixelFormatARB` keeps window requests
  within the on-screen pixel formats, and `glDeleteSync(0)` is ignored
  instead of raising `GL_INVALID_VALUE`.
- `0002-winemac-winemetalgl-core.patch`: `winemac.drv` loads
  `libwinemetalgl.dylib` from its own directory and hands it a `CAMetalLayer`
  per window; contexts, surfaces, pbuffers and swaps go through the driver ABI
  in `include/winemetalgl/driver.h`. No CGL context is created.
  `WGL_WINE_query_renderer` reports the core's renderer (vendor Apple, video
  memory, GL versions), which wined3d uses to describe the GPU.

The core is Wine's OpenGL once the patches are applied. `WINEMETALGL=0`
selects Apple's OpenGL (reference frames), and `winemac.drv` falls back to
Apple's OpenGL when the core is missing or refuses to initialise.

`scripts/verify-source.sh` checks that the patches apply when
`WINEMETALGL_WINE_SOURCE` names a Wine checkout.
