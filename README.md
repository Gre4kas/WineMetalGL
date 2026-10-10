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

<!-- BEGIN GENERATED: totals (scripts/docs/generate.py) -->
## Conformance totals

| OpenGL | Conformance % | Features | Unsupported % |
|---|---:|---|---:|
| 3.0 | 99.88% | GLSL 1.30, framebuffer objects, vertex array objects, transform feedback, integer textures | 2.28% |
| 3.1 | 100% | Instancing, texture buffers, uniform buffers, primitive restart | 2.38% |
| 3.2 | 100% | Profiles, geometry shaders, sync objects, multisample textures, base vertex | 0.69% |
| 3.3 | 99.98% | GLSL 3.30, sampler objects, timer queries, instanced arrays, dual-source blending | 3.71% |
| 4.0 | - | Tessellation, GLSL 4.00, double precision, indirect draws, per-sample shading | 100% |
| 4.1 | - | Separate shader objects, program binaries, viewport arrays, 64-bit attributes | 100% |
| 4.2 | - | Image load/store, atomic counters, immutable texture storage | 100% |
| 4.3 | - | Compute shaders, shader storage buffers, debug output, texture views | 100% |
| 4.4 | - | Buffer storage, multi-bind, texture clears, enhanced layouts | 100% |
| 4.5 | - | Direct state access, clip control, robustness, cull distances | 100% |
| 4.6 | - | SPIR-V shaders, anisotropic filtering, indirect draw counts | 100% |

**Conformance %**: passing cases over applicable cases (cases that are not NotSupported) of the version's Khronos CTS mustpass list (`KHR-GL30` to `KHR-GL33`), in the worst of its 8 runs (configurations 1 to 4 of `mustpass.xml`, each under Wine and natively) at commit 14eaa75.

**Unsupported %**: cases the CTS reports NotSupported because WineMetalGL does not offer what they test (an extension it does not advertise or a newer GL version), over the cases any implementation can run. Contexts stop at OpenGL 3.3, so 4.x is 100%; some 4.x features are available as extensions ([Unsupported](docs/unsupported.md#opengl-4x)).

Failing cases: `KHR-GL30.api.coverage`, `KHR-GL33.shaders.negative.non_precision_qualifiers_in_struct_members`. [Conformance results](docs/conformance.md) gives the reason for each.

OpenGL 1.0 to 2.1 are implemented by WineMetalGL itself, as its compatibility profile: the fixed-function pipeline, immediate mode, display lists and legacy GLSL, alongside every 3.x feature. No public Khronos suite measures them; the Wine fixtures, GPU tests and game corpus exercise them, and [Unsupported](docs/unsupported.md#compatibility-profile-opengl-10-to-21) lists what is not implemented.
<!-- END GENERATED: totals -->

Earlier releases published conformance numbers that depended on code which
recognised Khronos CTS shaders and produced their expected output. That code
and the implementation built around it have been removed, a gate keeps it out
(`scripts/check-forbidden-identifiers.sh`), and every number above is
generated from a committed result file.

## Docs

- [Conformance results](docs/conformance.md): every Khronos CTS run per
  OpenGL version, configuration and runner, with the reason for each failing
  case.
- [Unsupported](docs/unsupported.md): what WineMetalGL does not support and
  why: OpenGL 4.x, the gaps in the compatibility profile, and every case the
  CTS reports NotSupported.
- [Test suite](docs/testing.md): each test suite, how to run it, and roughly
  how long it takes.
- [Build it yourself](docs/building.md): building the core and a Wine 11.17
  that loads it, and checking that a program uses it.
- [Performance](docs/performance.md): frame times against Apple's OpenGL.

Also: the [game corpus](docs/games.md) results, [releasing](docs/release.md),
and the [design decisions](docs/decisions/).

## License

Copyright (c) 2026 MetalSharp. WineMetalGL is licensed under the GNU Lesser
General Public License, version 3 or (at your option) any later version
(`LGPL-3.0-or-later`): [`LICENSE`](LICENSE), which adds to the GNU General
Public License version 3 in [`LICENSES/GPL-3.0.txt`](LICENSES/GPL-3.0.txt).
Releases up to 2.1.0 were published under the MIT License.

The Wine patches in `patches/wine` are LGPL-2.1-or-later, like Wine
(`LICENSES/Wine-LGPL-2.1.txt`). The vendored glslang and SPIRV-Cross keep
their own licenses (`LICENSES/`).
