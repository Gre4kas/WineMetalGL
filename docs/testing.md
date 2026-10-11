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
an i386 guest in one fresh WoW64 prefix. Each prints markers that the script
checks.

```sh
WINEMETALGL_WINE_RUNTIME=/path/to/wine tests/wine/probe-opengl-wow64.sh
```

The 51 gate fixtures run by default; `--all` adds the 17 fixtures kept for
the later 4.x milestones, which need features of GL 4.3 or later.

## Khronos CTS

The Khronos VK-GL-CTS at commit `1942832`, built for Wine with
`tests/cts/windows/build.sh` (`glcts.exe`) and natively with
`tests/cts/native/build.sh` (a `glcts` that calls the core through its driver
ABI, without Wine). Each mustpass list, `KHR-GL30` to `KHR-GL42`, runs in
configurations 1 to 4 of `mustpass.xml`, under Wine and natively: 8 runs per
list. Configurations 5 and 6 repeat the command lines of 1 and 2 (their only
difference, `useForFirstEGLConfig`, applies to EGL), so they are not run.

```sh
for list in gl30 gl31 gl32 gl33 gl40 gl41 gl42; do
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

`--template` clones an existing WoW64 prefix instead of creating one per
shard, and `--filter` runs a subset of cases. Each result file records the
commit; run from a tree without uncommitted changes, or the source gate
rejects the evidence. Run times from the committed results:

<!-- BEGIN GENERATED: cts-times (scripts/docs/generate.py) -->
| Mustpass list | Cases | Under Wine, per run | Natively, per run | All 8 runs |
|---|---:|---:|---:|---:|
| `KHR-GL30` | 878 | 26 s to 50 s | 12 s to 13 s | 3.7 min |
| `KHR-GL31` | 888 | 27 s to 35 s | 3 s to 4 s | 2.4 min |
| `KHR-GL32` | 5,661 | 4.2 min to 5.2 min | 2.2 min to 2.7 min | 27.9 min |
| `KHR-GL33` | 9,886 | 6.8 min to 12.4 min | 4.2 min to 4.5 min | 50.8 min |
<!-- END GENERATED: cts-times -->

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
the core targets macOS 15 and does not link OpenGL.framework. When
`WINEMETALGL_WINE_SOURCE` names a Wine 11.17 tree, it also checks that the
Wine patches apply. `check-forbidden-identifiers.sh` keeps code that
recognises Khronos CTS shaders out of the source. CI runs both.
