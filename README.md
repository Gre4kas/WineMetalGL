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

| OpenGL | Adds | Result | Evidence |
|---|---|---|---|
| 1.0 | Immediate mode, display lists, fixed-function lighting, fog and texturing | Compatibility profile, with the gaps listed under [Compatibility profile](#compatibility-profile) | No public Khronos CTS; Wine fixtures, GPU tests and the game corpus |
| 1.1 | Vertex arrays, texture objects, polygon offset | Compatibility profile | No public Khronos CTS; Wine fixtures, GPU tests and the game corpus |
| 1.2 | 3D textures, BGRA and packed pixels, glDrawRangeElements | Compatibility profile | No public Khronos CTS; Wine fixtures, GPU tests and the game corpus |
| 1.3 | Multitexture, cube maps, compressed textures, multisample, texture combiners | Compatibility profile | No public Khronos CTS; Wine fixtures, GPU tests and the game corpus |
| 1.4 | Depth textures and shadow compare, window position, point parameters, mipmap generation | Compatibility profile, with the gaps listed under [Compatibility profile](#compatibility-profile) | No public Khronos CTS; Wine fixtures, GPU tests and the game corpus |
| 1.5 | Buffer objects, occlusion queries | Compatibility profile | No public Khronos CTS; Wine fixtures, GPU tests and the game corpus |
| 2.0 | GLSL 1.10, multiple draw buffers, point sprites, non-power-of-two textures | Compatibility profile | No public Khronos CTS; Wine fixtures, GPU tests and the game corpus |
| 2.1 | GLSL 1.20, pixel buffer objects, sRGB textures | Compatibility profile | No public Khronos CTS; Wine fixtures, GPU tests and the game corpus |
| 3.0 | GLSL 1.30, framebuffer objects, vertex array objects, transform feedback, integer textures | Not measured | No committed `KHR-GL30` baseline |
| 3.1 | Instancing, texture buffers, uniform buffers, primitive restart | Not measured | No committed `KHR-GL31` baseline |
| 3.2 | Profiles, geometry shaders, sync objects, multisample textures, base vertex | Not measured | No committed `KHR-GL32` baseline |
| 3.3 | GLSL 3.30, sampler objects, timer queries, instanced arrays, dual-source blending | 100% | `KHR-GL33`: 9,266 of 9,266 applicable cases (9,886 cases) in each of 8 runs |
| 4.0 | Tessellation, GLSL 4.00, double precision, indirect draws, per-sample shading | Not supported | Contexts stop at 3.3. Of the 13 extensions 4.0 promoted, advertised: `GL_ARB_draw_buffers_blend` |
| 4.1 | Separate shader objects, program binaries, viewport arrays, 64-bit attributes | Not supported | Contexts stop at 3.3. Of the 6 extensions 4.1 promoted, advertised: `GL_ARB_ES2_compatibility`, `GL_ARB_get_program_binary`, `GL_ARB_separate_shader_objects` |
| 4.2 | Image load/store, atomic counters, immutable texture storage | Not supported | Contexts stop at 3.3. Of the 11 extensions 4.2 promoted, advertised: `GL_ARB_base_instance`, `GL_ARB_compressed_texture_pixel_storage`, `GL_ARB_internalformat_query`, `GL_ARB_map_buffer_alignment`, `GL_ARB_texture_storage` |
| 4.3 | Compute shaders, shader storage buffers, debug output, texture views | Not supported | Contexts stop at 3.3. Of the 22 extensions 4.3 promoted, advertised: `GL_ARB_copy_image`, `GL_ARB_invalidate_subdata`, `GL_ARB_texture_storage_multisample`, `GL_KHR_debug` |
| 4.4 | Buffer storage, multi-bind, texture clears, enhanced layouts | Not supported | Contexts stop at 3.3. Of the 8 extensions 4.4 promoted, advertised: `GL_ARB_buffer_storage` |
| 4.5 | Direct state access, clip control, robustness, cull distances | Not supported | Contexts stop at 3.3. Of the 11 extensions 4.5 promoted, advertised: `GL_ARB_clip_control`, `GL_KHR_robustness` |
| 4.6 | SPIR-V shaders, anisotropic filtering, indirect draw counts | Not supported | Contexts stop at 3.3. Of the 11 extensions 4.6 promoted, advertised: `GL_ARB_texture_filter_anisotropic` |

Percentages are passing cases over applicable cases (those that are not NotSupported) in the worst run. Each mustpass list runs in configurations 1 to 4 of `mustpass.xml` (5 and 6 repeat the command lines of 1 and 2; their only difference, `useForFirstEGLConfig`, applies to EGL), under Wine (`glcts.exe` through Wine's `opengl32`) and natively (the core's driver ABI), at commit efa7879. NotSupported cases are unadvertised extensions, newer GL versions and cases the test marks invalid; [`docs/conformance.md`](docs/conformance.md) lists each with its reason, and the per-run numbers.

Game corpus: 8 of 8 entries match their reference frames ([`docs/games.md`](docs/games.md)).
Frame-time budgets met by 8 of 8 entries.

GL 3.3 core commands with an implementation: 344 of 344 (their behaviour is what the CTS results above measure).
<!-- END GENERATED: status -->

Earlier releases published conformance numbers that depended on code which
recognised Khronos CTS shaders and produced their expected output. That code
and the implementation built around it have been removed; every number above
is generated from a committed result file (see `docs/decisions/` and
`scripts/check-forbidden-identifiers.sh`).

## Build

Prerequisites: macOS 15 or later on Apple silicon, Xcode 16 or later, CMake
3.24 or later, Ninja and Python 3. The Wine fixtures and the game demos also
need MinGW-w64 (`x86_64-w64-mingw32-gcc` and `i686-w64-mingw32-gcc`; Homebrew
`mingw-w64`), and running anything under Wine needs a Wine 11.17 build with
`patches/wine` applied (see `patches/wine/README.md`). glslang and SPIRV-Cross
are vendored (`DEPENDENCIES.md`).

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
three Wine OpenGL bugs. With them applied, the core is Wine's
OpenGL; `WINEMETALGL=0` selects Apple's OpenGL instead (used to record
reference frames), and Wine falls back to Apple's OpenGL if the core cannot be
loaded.

Building Wine 11.17 with the patches, and checking that a program uses the
core: [`patches/wine/README.md`](patches/wine/README.md#building-wine-with-the-patches).

```sh
./scripts/stage-wine.sh /path/to/wine-build      # libwinemetalgl.dylib next to winemac.so
WINEMETALGL_HOST_LIB_DIR=/path/to/x86_64/libs ./scripts/stage-host-libs.sh /path/to/wine-runtime
./scripts/stage-release.sh /path/to/wine-build /path/to/output   # x86_64 Wine (Rosetta)
./scripts/stage-arm64-core.sh /path/to/output                      # arm64 core for a native arm64 Wine
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
`GL_POINT_FADE_THRESHOLD_SIZE`, face culling in `GL_POINT` polygon mode (the
vertices of every triangle are drawn),
wide lines and polygon stipple with application shaders (the fixed-function
programs draw both), and `GL_ARB_vertex_program`/`GL_ARB_fragment_program`.

## Testing

- `tests/host`, `tests/gpu`: the compiler and the core on Metal, no Wine.
- `tests/cts`: Khronos CTS (`KHR-GL30` to `KHR-GL33`) natively and under
  Wine (`scripts/cts/run.py`), with the committed baselines. `glcts.exe` for Wine
  is built with `tests/cts/windows/build.sh`, the native `glcts` with
  `tests/cts/native/build.sh`.
- `tests/wine`: Wine fixtures (`tests/wine/probe-opengl-wow64.sh`).
- `tests/games`: rendered-frame comparisons (`scripts/traces/replay.py`).

## License

The standalone source is MIT licensed. Wine integration is LGPL-2.1;
third-party shader compiler licenses are in `LICENSES/`.
