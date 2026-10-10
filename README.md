# WineMetalGL

WineMetalGL is an OpenGL implementation on Metal for Wine on macOS 15 or newer.
Wine's macOS driver (`winemac.drv`) loads it instead of Apple's OpenGL, so
Windows programs get OpenGL 3.3 contexts with geometry shaders and transform
feedback, core or compatibility profile. Contexts created without a
core-profile request (which Apple's OpenGL caps at 2.1) get the full 3.3
compatibility profile: fixed-function vertex and fragment processing,
immediate mode, display lists, client arrays, legacy GLSL built-ins and the
legacy pixel paths, together with every 3.x feature. It runs x86_64 and i386
Windows programs together in a WoW64 prefix.

The library, `libwinemetalgl.dylib`, has no dependency on Apple's OpenGL. It
compiles GLSL with glslang and SPIRV-Cross to the Metal Shading Language and
implements the GL state machine, resources and draws on Metal. Wine reaches
it through the small driver ABI in `include/winemetalgl/driver.h`.

<!-- BEGIN GENERATED: status (scripts/docs/generate.py) -->
## Status

`KHR-GL33` (9,886 cases) under mustpass configurations 1, 2, 3, 4, 5, 6: 0 Fail, InternalError, Crash or Timeout in the worst run; NotSupported only for unadvertised extensions, newer GL versions and cases the test marks invalid. Per-run numbers: [`docs/conformance.md`](docs/conformance.md).

Game corpus: 8 of 8 entries match their reference frames ([`docs/games.md`](docs/games.md)).

GL 3.3 core commands implemented: 344 of 344.
<!-- END GENERATED: status -->

Earlier releases published conformance numbers that depended on code which
recognised Khronos CTS shaders and produced their expected output. That code
and the implementation built around it have been removed; every number above
is generated from a committed result file (see `docs/decisions/` and
`scripts/check-forbidden-identifiers.sh`).

## Build

```sh
./scripts/build.sh            # build/release/libwinemetalgl.dylib (x86_64)
```

For development on Apple silicon, the core and its tests also build for the
machine itself, without Rosetta:

```sh
cmake -S . -B build/host -DWMGL_HOST_ONLY=ON
cmake --build build/host
build/host/wmgl_host_tests && build/host/wmgl_gpu_tests
```

## Wine

The patches in `patches/wine/wine-11.17` add the core to `winemac.drv` and fix
two Wine OpenGL bugs it relies on. With them applied, the core is Wine's
OpenGL; `WINEMETALGL=0` selects Apple's OpenGL instead (used to record
reference frames), and Wine falls back to Apple's OpenGL if the core cannot be
loaded.

```sh
./scripts/stage-wine.sh /path/to/wine-build      # libwinemetalgl.dylib next to winemac.so
./scripts/stage-host-libs.sh /path/to/wine-runtime
./scripts/stage-release.sh /path/to/wine-build /path/to/output
```

`WINEMETALGL_LOG=1..3` raises the core's log level; `WINEMETALGL_TRACE=1` logs
every GL call. `WINEMETALGL_VENDOR` and `WINEMETALGL_RENDERER` replace the
`GL_VENDOR` / `GL_RENDERER` strings for games that refuse unknown vendors.

## Compatibility profile

Fixed-function processing runs as generated shaders keyed by the state that
changes their code (`src/core/compat/fixed_function.mm`); the state itself
reaches them, and GLSL 1.x shaders that read `gl_ModelViewMatrix`,
`gl_LightSource` and the other built-ins, through one uniform block. Display
lists record every list-compilable command of GL 1.0-2.1 with copied
arguments. Not implemented: evaluators (`glMap*`/`glEval*`), the accumulation
buffer, selection and feedback records (`glRenderMode` draws nothing outside
`GL_RENDER` and returns no records), line stipple, colour-index mode, the
imaging subset, pixel maps and transfer scale/bias beyond their defaults,
wide lines and polygon stipple with application shaders (the fixed-function
programs draw both), and `GL_ARB_vertex_program`/`GL_ARB_fragment_program`.

## Testing

- `tests/host`, `tests/gpu`: the compiler and the core on Metal, no Wine.
- `tests/cts`: Khronos CTS (`KHR-GL33`) natively and under Wine
  (`scripts/cts/run.py`), with the committed baselines.
- `tests/wine`: Wine fixtures (`tests/wine/probe-opengl-wow64.sh`).
- `tests/games`: rendered-frame comparisons (`scripts/traces/replay.py`).

## License

The standalone source is MIT licensed. Wine integration is LGPL-2.1;
third-party shader compiler licenses are in `LICENSES/`.
