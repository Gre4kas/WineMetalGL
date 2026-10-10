# Game corpus

The corpus checks real rendering, not just API behaviour. `corpus.json` is
the tracked manifest. Binaries, apitrace traces and reference frames live
outside git under a **corpus root**:

```text
<corpus-root>/
  demos/            built by tests/games/build-demos.sh
  traces/           apitrace .trace files
  tools/apitrace/   apitrace for Windows (bin/glretrace.exe, lib/wrappers/opengl32.dll)
  references/<entry>/NNN.ppm
```

## Tiers

- **A (core 3.3):** core-profile programs that the Apple OpenGL path can also
  run. Their references come from that path (`--path apple
  --update-references`).
- **B (beyond Apple):** legacy or compatibility-profile contexts using 3.x
  features, which Apple's OpenGL cannot provide. Where possible they reuse an
  equivalent Tier A reference (for example `scene33-compat` must render the
  same frame as `scene33-core`).
- **C (old compatibility games):** regression checks only.

## Running

```sh
tests/games/build-demos.sh /Volumes/AverySSD/wmgl-test/corpus
scripts/traces/replay.py --wine /path/to/wine --corpus-root /path/to/corpus \
    --template /path/to/prefix-template --path new --html /tmp/corpus.html
```

`--path` selects the implementation: `new` (WineMetalGL, Wine's default) or
`apple` (`WINEMETALGL=0`, Apple's OpenGL, which records the references).
`--json tests/games/results.json` records the run that `docs/games.md` reports.

## Capturing a trace

Copy `tools/apitrace/lib/wrappers/opengl32.dll` next to the program, then run
it with `WINEDLLOVERRIDES=opengl32=n,b` and `TRACE_FILE=<windows path>`. Add
the trace to `corpus.json` with the frames to compare.

## Demos

- `demos/scene33.c`: a deterministic GL 3.3 "mini engine" frame (UBO camera,
  instanced draws, mipmapped texture, RGBA16F + depth FBO, post-process pass,
  point sprites, orphaned stream buffer). `--profile core|compat`,
  `--no-forward`, `--frames`, `--dump`.
- `demos/legacy.c`: a deterministic fixed-function frame in the style of GL
  1.x/2.x games (lighting, display lists, immediate mode, client arrays with
  quads, multitexture, sphere-map texgen, fog, alpha test, clip plane, point
  sprites, glBitmap text, glDrawPixels, glPushAttrib). `--profile legacy`
  (wglCreateContext; Apple's OpenGL records the reference) or `compat33`
  (a 3.3 compatibility-profile context, Tier B).
