# Test suite

What each suite tests, how to run it, and roughly how long it takes. The
times were measured on an Apple M4 (Mac16,12) with macOS 27.2; the Wine
suites run an x86_64 Wine under Rosetta. They grow when other GPU work runs at
the same time: one KHR-GL32 run under Wine took 745 s instead of about 250 s
next to another project's GPU tests.

| Suite | What it tests | Time |
|---|---|---:|
| Host tests | The GLSL upgrade, the shader compiler and the format code, without a GPU | under 1 s |
| GPU tests | The core on Metal through its own API, without Wine | about 2 s |
| Wine fixtures | 51 programs, each as an x86_64 and an i386 guest in one WoW64 prefix, through Wine's `opengl32` | about 1.5 min |
| Khronos CTS | `KHR-GL30` to `KHR-GL42`, 8 runs each (per list below) | about 6 h |
| Game corpus | Rendered frames compared with Apple's OpenGL | about 1 min |
| Frame times | The game corpus timed on both implementations | about 4 min |
| Source gates | The manifest and checksums, generated docs and evidence provenance, no Apple OpenGL, no code that recognises CTS shaders | under 1 s |

[Build it yourself](building.md) covers building the core and a Wine that
loads it.

## Host and GPU tests

```sh
cmake -S . -B build/host -DWMGL_HOST_ONLY=ON && cmake --build build/host
build/host/wmgl_host_tests
build/host/wmgl_gpu_tests            # or one test: build/host/wmgl_gpu_tests gpu_fence_sync
```

## Wine fixtures

Small Windows programs (`tests/wine/fixtures.txt`), each run as an x86_64 and
an i386 guest in one fresh WoW64 prefix, through the Wine module. Each prints
markers that the script checks.

```sh
./scripts/build.sh && ./scripts/build-wine-module.sh /path/to/wine
./scripts/install-wine-module.sh /path/to/wine
WINEMETALGL_WINE_RUNTIME=/path/to/wine tests/wine/probe-opengl-wow64.sh
```

The script puts the module's `opengl32.dll` into its prefix
(`WINEMETALGL_MODULE_DIR`, default `build/wine-module`). The gate runs on
stock Wine 11.0 and on MetalSharp's Wine 11.17.

All 70 fixtures are in the gate (the 10 that need GL 4.4 to 4.6 features
joined it with GL 4.6), each run as an x86_64 and an i386 guest.

## Khronos CTS

The Khronos VK-GL-CTS at commit `1942832`, built for Wine with
`tests/cts/windows/build.sh` (`glcts.exe`) and natively with
`tests/cts/native/build.sh` (a `glcts` that calls the core through its driver
ABI, without Wine). Each mustpass list, `KHR-GL30` to `KHR-GL43`, runs in
configurations 1 to 4 of `mustpass.xml`, under Wine and natively: 8 runs per
list. Configurations 5 and 6 repeat the command lines of 1 and 2 (their only
difference, `useForFirstEGLConfig`, applies to EGL), so they are not run.

```sh
for list in gl30 gl31 gl32 gl33 gl40 gl41 gl42 gl43; do
  for config in 1 2 3 4; do
    python3 scripts/cts/run.py --mustpass $list-main.txt --config $config \
        --wine /path/to/wine --cts /path/to/build-win64/external/openglcts/modules/glcts.exe \
        --shards 6 --out tests/cts/baseline/$list-cfg$config-wine.txt
    python3 scripts/cts/run.py --mustpass $list-main.txt --config $config \
        --native --cts /path/to/build-native/external/openglcts/modules/glcts \
        --shards 8 --out tests/cts/baseline/$list-cfg$config-native.txt
  done
done
python3 scripts/docs/generate.py
```

OpenGL ES lists run in configuration 1 of their `mustpass.xml` under Wine,
Khronos's (`gles2-khr-main` to `gles32-khr-main`, `gles32-khr-glesext`, the
`-noctx-main` and `-khr-single` lists) and dEQP's (`gles2-main`, `gles3-main`,
`gles31-main` and their rotation, multisample and RGB565 lists):

```sh
python3 scripts/cts/run.py --mustpass gles3-main.txt --config 1 \
    --wine /path/to/wine --cts /path/to/build-win64/external/openglcts/modules/glcts.exe \
    --shards 6 --out tests/cts/baseline/gles3-main-cfg1-wine.txt
```

`cts-runner.exe --type=es3`, in the same directory as `glcts.exe`, runs every
OpenGL ES 2.0 and 3.0 list in each pixel format it selects (window formats
without samples or sRGB; about an hour under Wine);
`scripts/cts/runner_summary.py LOGDIR tests/cts/es/cts-runner-es3.txt`
records its result for `generate.py`.

`--template` clones an existing WoW64 prefix instead of creating one per
shard, and `--filter` runs a subset of cases. Each result file records the
commit; run from a tree without uncommitted changes, or the source gate
rejects the evidence.

A list's baselines stay at the release that last ran it in full. When a later
release changes what its non-Pass cases do (a newer context version, a fixed
entry point), those cases are run again: every case that is not Pass in any of
the list's 8 baselines, with `--caselist`, in the same configuration and runner,
into `tests/cts/recheck/<list>-cfgN-<runner>.txt`. `generate.py` replaces the
baseline's results with the recheck's for those cases, refuses a recheck that
leaves out a non-Pass case, and names the recheck commit next to the totals.

Run times from the committed results:

<!-- BEGIN GENERATED: cts-times (scripts/docs/generate.py) -->
| Mustpass list | Cases | Under Wine, per run | Natively, per run | All runs |
|---|---:|---:|---:|---:|
| `KHR-GL30` | 878 | 26 s to 50 s | 12 s to 13 s | 3.7 min |
| `KHR-GL31` | 888 | 27 s to 35 s | 3 s to 4 s | 2.4 min |
| `KHR-GL32` | 5,661 | 4.2 min to 5.2 min | 2.2 min to 2.7 min | 27.9 min |
| `KHR-GL33` | 9,886 | 9.0 min to 11.0 min | 5.1 min to 5.2 min | 60.8 min |
| `KHR-GL40` | 11,862 | 9.4 min to 11.0 min | 4.2 min | 58.7 min |
| `KHR-GL41` | 11,883 | 8.9 min to 10.8 min | 4.2 min | 56.1 min |
| `KHR-GL42` | 12,305 | 10.0 min to 11.3 min | 4.3 min to 4.5 min | 60.6 min |
| `KHR-GL43` | 16,085 | 9.6 min to 11.6 min | 4.7 min to 5.2 min | 62.5 min |
| `KHR-GL46` | 19,714 | 7.2 min to 7.4 min | 4.7 min | 19.2 min |
<!-- END GENERATED: cts-times -->

## Direct3D through wined3d

Wine's Direct3D 10 and 11 (wined3d) run on OpenGL, so they run on WineMetalGL
when the prefix has the module. wined3d then reports feature level 11_1 (9_3
on Apple's OpenGL). Wine's own conformance tests check it. They are built
from a Wine 11.0 tree configured with tests enabled, and run one at a time:

```sh
make dlls/d3d11/tests/x86_64-windows/d3d11_test.exe dlls/d3d10core/tests/x86_64-windows/d3d10core_test.exe
WINETEST_NO_MT_D3D=1 wine dlls/d3d11/tests/x86_64-windows/d3d11_test.exe
```

On stock Wine 11.0 with the module (Apple M4):

| Test | Checks | Failures | Skipped |
|---|---:|---:|---:|
| `d3d11` (x86_64) | 561,394 | 230 | 9 |
| `d3d11` (i386) | 561,394 | 230 | 9 |
| `d3d10core` (x86_64) | 194,651 | 209 | 0 |
| `d3d10core` (i386) | 194,651 | 209 | 0 |

What the failures are:

- 205 in each suite are `test_depth_bias` on D24 formats. Apple GPUs have no
  24-bit depth format, so the depth is stored as `Depth32Float`. Its bias
  units scale with the depth's exponent, which matches the 24-bit unit only
  for depths in [0.5, 1).
- `test_negative_viewports` expects D3D11 hardware's flooring of fractional
  viewport corners. OpenGL viewports keep the fraction.
- Single checks not yet explained:
  - `test_sample_shading`;
  - `test_depth_stencil_sampling`;
  - `test_sm5_bufinfo_instruction`;
  - `test_sample_mask`;
  - `test_layered_rtv_mismatch`.
- 32-bit programs keep their buffer storage below 4 GB: small buffers share
  1 MiB chunks in 16 KiB pages, which leaves room for wined3d's 64 MB
  persistent chunks (`test_deferred_context_map`).
- `dxgi_test` fails on window and fullscreen geometry under Wine's macOS
  driver, which is not OpenGL.

## Game corpus and frame times

Demos and an apitrace trace, rendered on WineMetalGL and compared with
reference frames from Apple's OpenGL. The binaries, traces and references
live outside git (`tests/games/README.md`).

```sh
python3 scripts/traces/replay.py --wine /path/to/wine --corpus-root /path/to/corpus --json tests/games/results.json
python3 scripts/traces/benchmark.py --wine /path/to/wine --corpus-root /path/to/corpus   # tests/games/perf.json
```

Results: [Game corpus](games.md) and [Performance](performance.md).

## Source gates

```sh
./scripts/verify-source.sh
./scripts/check-forbidden-identifiers.sh
```

`verify-source.sh` checks the file manifest and the checksums
(`scripts/update-checksums.sh` refreshes both), that the generated docs match
the committed evidence, that no evidence was recorded from a tree with
uncommitted changes, that nothing uses Apple's OpenGL, and, once built, that
the core targets macOS 15 and does not link OpenGL.framework.
`check-forbidden-identifiers.sh` keeps code that
recognises Khronos CTS shaders out of the source. CI runs both.
