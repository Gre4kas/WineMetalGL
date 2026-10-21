# Minecraft: Java Edition: native glue for WineMetalGL

Minecraft creates its window and OpenGL context through LWJGL, with SDL3 in
26.x and with GLFW in 1.21.x and earlier, and on macOS gets Apple's OpenGL
4.1. The glue in this folder runs it on WineMetalGL's arm64 core instead,
natively, without Wine. Minecraft then gets an OpenGL 4.6 core context.
Shader mods (Iris) and renderers (Sodium) load as usual.

| File | What it is |
|---|---|
| `glue.m` | `SDL_GL_*` (SDL3, Minecraft 26.x) on WineMetalGL: a `CAMetalLayer` on SDL's window, a WineMetalGL surface and contexts, swaps |
| `glue_glfw.m` | GLFW's context functions (Minecraft 1.21.x) on WineMetalGL: windows created without a context (`GLFW_NO_API`), each given a `CAMetalLayer`, surface and context from the OpenGL hints |
| `install.sh` | Builds the glue and the GL stubs, and installs them with the core into one folder |

The GL stubs come from the OpenMW folder's `gen_stubs.py`. LWJGL is pointed
at the libraries with two JVM arguments, so the game, the launcher and the
Java runtime are not modified:

- 26.x: `-Dorg.lwjgl.sdl.libname=…/libSDL3.dylib` loads the glue. It re-exports
  LWJGL's own SDL3, which it keeps as `libSDL3-real.dylib`, and overrides
  `SDL_GL_*`.
- 1.21.x: `-Dorg.lwjgl.glfw.libname=…/libglfw.dylib` loads the GLFW glue
  instead. It re-exports LWJGL's own GLFW (`libglfw-real.dylib`) and
  overrides window creation, the context calls and swaps.
- `-Dorg.lwjgl.opengl.libname=…/libwmglhost.dylib` replaces
  `OpenGL.framework`: each `gl*` symbol jumps to the core's function.

## Installing

```sh
brew install sdl3 glfw                              # headers only
scripts/stage-arm64-core.sh /tmp/wmgl-arm64        # builds build/arm64-release
"Game Specific Patched Files/Minecraft (Java Edition)/install.sh"
```

`install.sh` takes LWJGL's SDL3 and GLFW from the official launcher's or
Prism Launcher's libraries (each glue is built when its library and headers
are there), installs into `~/.local/share/winemetalgl/minecraft` (or the
folder given as its first argument), and prints the JVM arguments for each
Minecraft generation. The default path has no spaces, so the arguments need no quoting.
Run it again after rebuilding the core.

The Java runtime must load ad-hoc-signed libraries. Mojang's runtimes have
the `disable-library-validation` entitlement and work: Java 25
(`java-runtime-epsilon`, 26.x) and Java 21 (`java-runtime-delta`, 1.21.x),
which Prism Launcher downloads too.

### Prism Launcher

In the instance's **Settings → Java**, set the Java installation to Mojang's
runtime and add the two arguments to **JVM arguments**. Remove them to run
the same instance on Apple's OpenGL for comparison.

### Official launcher

In **Installations → (installation) → More options**, set **Java executable**
and add the two arguments to **JVM arguments**.

## Diagnostics

- `WMGLHOST_TRACE=1` logs the glue's `SDL_GL_*` calls.
- `WMGLHOST_DUMP=dir` saves the back buffer as `dir/frame-N.ppm` every
  `WMGLHOST_DUMP_EVERY` (300) frames.
- `WMGLHOST_FRAMES=file` writes each swap's time, in seconds, one line per
  frame: frame rates and frame-time percentiles for benchmarks.
- `WMGLHOST_SPIN=pixels` (GLFW glue) moves the cursor that many pixels right
  every frame: the camera turns at a steady rate, to reproduce artifacts that
  appear only in motion.
- `WMGLHOST_CORE=/path/to/libwinemetalgl.dylib` tries another core without
  reinstalling.
- The core's own log goes to `~/Library/Logs/WineMetalGL` (see
  [docs/logging.md](../../docs/logging.md)).
- Minecraft's log names the context: `Using graphics backend OpenGL, using
  drivers: 4.6.0 WineMetalGL Core Profile`.

## Performance

Minecraft 26.3 with Sodium 0.9.2 on an M4, 1708×960, one standing view,
vsync off and frame rate uncapped, the last 45 seconds after a warm-up
(`WMGLHOST_FRAMES`; Apple's OpenGL timed the same way by a pass-through
`SDL_GL_SwapWindow`):

| | Apple's OpenGL | WineMetalGL |
|---|---|---|
| Sodium | 153.5 fps | 388.2 fps |
| Sodium and Iris with BSL | 60.0 fps (held at 60), 1% low 49.9 | 82.1–84.4 fps, 1% low 47.7–53.9 |
