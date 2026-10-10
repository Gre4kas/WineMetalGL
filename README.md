# WineMetalGL

WineMetalGL is a general x86_64 Wine OpenGL conformance adapter. This
reference backend translates Wine OpenGL calls to Metal on macOS 15 or newer
and can be integrated with any compatible Wine build. It supports x86_64 and i386
Windows programs together in a WoW64 prefix.

## Conformance status

Earlier releases published per-version percentages and Khronos CTS pass counts
that depended on code which recognised CTS shaders and produced their expected
results. That code has been removed (see `docs/decisions/` and the
`feat/gl33-core` work), and only measured results are published now.

The current measured baseline for the shipped (legacy) path is
[`tests/cts/baseline/gl33-cfg1-oldcore.txt`](tests/cts/baseline/gl33-cfg1-oldcore.txt):
`KHR-GL33` mustpass, first configuration, 5,783 Pass / 3,393 Fail /
702 NotSupported / 8 InternalError of 9,886 cases. `docs/conformance.md`
explains how to reproduce it and how results are reported.

## Build

```sh
cmake --preset release
cmake --build --preset release -j8
```

The output is `build/release/metalsharp-opengl.dylib`. The historical filename is the sidecar ABI name; it does not require a
specific Wine distribution or application runtime. It must contain only one architecture: `x86_64`.

## Wine

```sh
./scripts/stage-wine.sh /path/to/wine-build
./scripts/stage-host-libs.sh /path/to/wine-runtime
./scripts/stage-release.sh /path/to/wine-build /path/to/output
```

The Wine integration bundles `artifacts/wine-driver/winemetalgl.conf` next to
`winemac.so`. It enables the experimental WineMetalGL path by default and caps
reported/requested OpenGL support at 3.3:

```ini
winemetalgl.enabled = 1
winemetalgl.experimental = 1
winemetalgl.maxFeatureLevel = 3.3
```

Set `WINEMETALGL_CONFIG_FILE` to override the bundled configuration. Explicit
`WINEMETALGL`, `WINEMETALGL_EXPERIMENTAL`, and
`WINEMETALGL_MAX_FEATURE_LEVEL` environment variables take precedence. For the
bounded GL 4.0 API-coverage run, the runner explicitly raises the max level to
4.0; the default remains 3.3.

The WoW64 probe is run against any compatible Wine installation:

```sh
WINEMETALGL_WINE_RUNTIME=/path/to/wine-install \
  ./tests/wine/probe-opengl-wow64.sh
```

## CI

GitHub Actions runs the same reproducible build on macOS 15 or newer, checks
the x86_64 artifact, runs the native translation probe, and uploads the built
sidecar. The workflow is `.github/workflows/build.yml`.

## License

The standalone source is MIT licensed. Wine integration is LGPL-2.1;
third-party shader compiler licenses are in `LICENSES/`.
