# WineMetalGL

[![Metal](https://github.com/metalsharp/WineMetalGL/actions/workflows/metal.yml/badge.svg)](https://github.com/metalsharp/WineMetalGL/actions/workflows/metal.yml)
[![Build](https://github.com/metalsharp/WineMetalGL/actions/workflows/build.yml/badge.svg)](https://github.com/metalsharp/WineMetalGL/actions/workflows/build.yml)
[![CodeQL](https://github.com/metalsharp/WineMetalGL/actions/workflows/codeql.yml/badge.svg)](https://github.com/metalsharp/WineMetalGL/actions/workflows/codeql.yml)

WineMetalGL is an OpenGL implementation on Metal for Wine on macOS 15 or newer.
It installs into an unmodified Wine 11.x as a Wine module of its own, and
gives Windows programs OpenGL 4.6 contexts, core or compatibility profile,
instead of Apple's OpenGL: geometry and tessellation shaders, double
precision, transform feedback with vertex streams, separate shader objects,
viewport arrays, image load/store, atomic counters, compute shaders, shader
storage buffers, texture views, buffer storage, multi-bind, direct state
access, clip control, cull distances, SPIR-V shaders, indirect draw counts
and pipeline statistics queries. Contexts created without a core-profile
request (which Apple's OpenGL caps at 2.1) get the full 4.6 compatibility
profile: fixed-function vertex and fragment processing, immediate mode,
display lists, client arrays, legacy GLSL built-ins and the legacy pixel
paths, together with every 3.x and 4.x feature. It runs x86_64 and i386
Windows programs together in a WoW64 prefix. Wine's Direct3D 10 and 11
(wined3d), which draw through OpenGL, get feature level 11_1 on it
([Test suite](docs/testing.md#direct3d-through-wined3d)).

The core, `libwinemetalgl.dylib`, has no dependency on Apple's OpenGL. It
compiles GLSL with glslang and SPIRV-Cross to the Metal Shading Language and
implements the GL state machine, resources and draws on Metal. The Wine
module (`winemetalgl.dll` and `winemetalgl.so`, with a forwarding
`opengl32.dll` in the prefix) carries WGL and every GL call to it through the
driver ABI in `include/winemetalgl/driver.h`; Wine itself is not patched or
rebuilt ([Build it yourself](docs/building.md#the-wine-module)).

## Proof Of Concept

Dead Cells (Requires OpenGL 3.2+) running under MetalSharp Wine-11.17 Using WineMetalGL;

<img width="1920" height="1082" alt="image" src="https://github.com/user-attachments/assets/dcf456b7-3b7f-48c6-b57e-cfd68794ada2" />

<!-- BEGIN GENERATED: totals (scripts/docs/generate.py) -->
## Conformance totals

| OpenGL | Conformance % | Features | Unsupported % |
|---|---:|---|---:|
| 3.0 | 99.88% | GLSL 1.30, framebuffer objects, vertex array objects, transform feedback, integer textures | 2.28% |
| 3.1 | 100% | Instancing, texture buffers, uniform buffers, primitive restart | 2.38% |
| 3.2 | 100% | Profiles, geometry shaders, sync objects, multisample textures, base vertex | 0.69% |
| 3.3 | 100% | GLSL 3.30, sampler objects, timer queries, instanced arrays, dual-source blending | 1.11% |
| 4.0 | 99.99% | Tessellation, GLSL 4.00, double precision, indirect draws, per-sample shading | 10.11% |
| 4.1 | 99.99% | Separate shader objects, program binaries, viewport arrays, 64-bit attributes | 10.08% |
| 4.2 | 99.98% | Image load/store, atomic counters, immutable texture storage | 9.74% |
| 4.3 | 99.97% | Compute shaders, shader storage buffers, debug output, texture views | 15.60% |
| 4.4 | covered by KHR-GL46 | Buffer storage, multi-bind, texture clears, enhanced layouts | - |
| 4.5 | covered by KHR-GL46 | Direct state access, clip control, robustness, cull distances | - |
| 4.6 | 99.98% | SPIR-V shaders, anisotropic filtering, indirect draw counts | 22.33% |

**Conformance %**: passing cases over applicable cases (cases that are not NotSupported) of the version's Khronos CTS mustpass list (`KHR-GL30` to `KHR-GL43`, `KHR-GL46`), in the worst of its runs: 8 for `KHR-GL30` to `KHR-GL43` (configurations 1 to 4 of `mustpass.xml`, each under Wine and natively), 3 for `KHR-GL46` (configuration 1 natively, configurations 1 and 3 under Wine), at commit 14eaa75, 49c5099, 851476e, f4ecccd. OpenGL 4.4 and 4.5 have no list of their own: `KHR-GL46` runs their tests.

The cases that were not Pass in `KHR-GL33`, `KHR-GL40`, `KHR-GL41`, `KHR-GL42`, `KHR-GL43` were run again at commit 49c5099, in the same configurations under Wine and natively (`tests/cts/recheck/`); those results replace the baselines' for those cases.

What Unsupported % counts, and why each failing case fails: [Unsupported](docs/unsupported.md#conformance-notes).

OpenGL 1.0 to 2.1 are implemented by WineMetalGL itself, as its compatibility profile: the fixed-function pipeline, immediate mode, display lists and legacy GLSL, alongside every 3.x feature. No public Khronos suite measures them; the Wine fixtures, GPU tests and game corpus exercise them, and [Unsupported](docs/unsupported.md#compatibility-profile-opengl-10-to-21) lists what is not implemented.
<!-- END GENERATED: totals -->

## Docs

- [Conformance results](docs/conformance.md): every Khronos CTS run per
  OpenGL version, configuration and runner, with the reason for each failing
  case.
- [Unsupported](docs/unsupported.md): what WineMetalGL does not support and
  why: the gaps in the compatibility profile, what the
  Unsupported % counts, the failing CTS cases, and every case the CTS reports
  NotSupported.
- [Test suite](docs/testing.md): each test suite, how to run it, and roughly
  how long it takes.
- [Build it yourself](docs/building.md): building the core and the Wine
  module, installing them into a Wine 11.x, and checking that a program uses
  them.
- [Performance](docs/performance.md): frame times against Apple's OpenGL.

Also: the [game corpus](docs/games.md) results, [releasing](docs/release.md),
and the [design decisions](docs/decisions/).

Contributing: [CONTRIBUTING.md](CONTRIBUTING.md). Security reports:
[SECURITY.md](SECURITY.md). Code of conduct: [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## License

Copyright (c) 2026 MetalSharp. WineMetalGL is licensed under the GNU Lesser
General Public License, version 3 or (at your option) any later version
(`LGPL-3.0-or-later`): [`LICENSE`](LICENSE), which adds to the GNU General
Public License version 3 in [`LICENSES/GPL-3.0.txt`](LICENSES/GPL-3.0.txt).
Releases up to 2.1.0 were published under the MIT License.

The files of the Wine module adapted from Wine (`src/wine/pe/wgl_formats.c`,
`src/wine/pe/wgl_fonts.c`) are LGPL-2.1-or-later, like Wine
(`LICENSES/Wine-LGPL-2.1.txt`). The vendored glslang and SPIRV-Cross keep
their own licenses (`LICENSES/`).
