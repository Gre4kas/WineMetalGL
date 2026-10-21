# The Elder Scrolls III: Morrowind (OpenMW): native glue for WineMetalGL

[OpenMW](https://openmw.org) is an open-source engine for *The Elder Scrolls III:
Morrowind*. Its macOS build uses SDL2 and Apple's OpenGL 2.1. The glue in this
folder runs it on WineMetalGL's arm64 core instead, natively, without Wine
(issue #42). OpenMW then gets an OpenGL 4.6 compatibility context, with
post-processing, floating-point reversed-Z depth and shadows.

| File | What it is |
|---|---|
| `glue.m` | `SDL_GL_*` on WineMetalGL: a `CAMetalLayer` on SDL's window, a WineMetalGL surface and contexts, swaps |
| `gen_stubs.py` | Generates an exported arm64 stub for every GL function in the core |
| `install.sh` | Builds both libraries and installs them, with the core, into an `OpenMW.app` |

`install.sh` builds two libraries into the app's `Frameworks`:

- `libwmglhost.dylib` replaces `OpenGL.framework`: each `gl*` symbol jumps to
  the core's function.
- `libSDL2-2.0.0.dylib` is the glue. It re-exports the real SDL, which it keeps
  as `libSDL2-real.dylib`, and overrides `SDL_GL_*`.

`openmw` and OpenSceneGraph's libraries are relinked from `OpenGL.framework`
to `libwmglhost.dylib`, and the app is signed ad hoc.

## Installing

WineMetalGL Settings installs it into an `OpenMW.app` in `/Applications` or
`~/Applications` (Game Fixes → Arm64 Fixes → OpenMW), and takes it out again
when the fix is turned off. By hand:

You need OpenMW's macOS arm64 build: a [release](https://github.com/OpenMW/openmw/releases)
(0.51.0 was tested) or a nightly from OpenMW's GitLab CI (0.52.0 master was
tested). Install it on a copy of the app.

```sh
scripts/stage-arm64-core.sh /tmp/wmgl-arm64        # builds build/arm64-release
"Game Specific Patched Files/The Elder Scrolls III Morrowind (OpenMW)/install.sh" /path/to/OpenMW.app
/path/to/OpenMW.app/Contents/MacOS/openmw
```

Run `install.sh` again after rebuilding the core. To try another core without
reinstalling, set `WMGLHOST_CORE=/path/to/libwinemetalgl.dylib`.

You also need Morrowind's data files (the Game of the Year edition, from Steam
or GOG), set up in `~/Library/Preferences/openmw/openmw.cfg` as usual. The
original `Morrowind.exe` does not need to run. Without the game,
[OpenMW's example suite](https://gitlab.com/OpenMW/example-suite) works too
(fetch it with Git LFS).

## Diagnostics

- `WMGLHOST_TRACE=1` logs the glue's `SDL_GL_*` calls.
- `WMGLHOST_DUMP=dir` saves the back buffer as `dir/frame-N.ppm` every
  `WMGLHOST_DUMP_EVERY` (300) frames.
- The core's own log goes to `~/Library/Logs/WineMetalGL` (see
  [docs/logging.md](../../docs/logging.md)).

## What OpenMW needed in the core

Fixed in the same pull request as this folder:

- `GL_ARB_depth_buffer_float` is advertised. OpenSceneGraph tests for it by
  name, and OpenMW turns off post-processing without it.
- `GL_EXT_gpu_shader4` in GLSL 1.20. OpenMW's shaders require it, and its
  shadow-casting program failed to compile without it.
- `GL_DEPTH_TEXTURE_MODE` and `GL_GENERATE_MIPMAP`, compatibility-profile texture
  parameters that OpenSceneGraph sets.

With these fixes, Morrowind (OpenMW 0.52.0 master, shadows and post-processing
on) renders interiors and exteriors with no GL errors. The issue reports that
the 3D world never reaches the window; with these OpenMW builds and this glue,
that has not reproduced.
