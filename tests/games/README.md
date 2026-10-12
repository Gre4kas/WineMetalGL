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

- **A (core):** core-profile programs (3.3 and 4.1) that the Apple OpenGL path
  can also run. Their references come from that path (`--path apple
  --update-references`).
- **B (beyond Apple):** legacy or compatibility-profile contexts using 3.x
  features, and GL 4.2 and 4.3 features, which Apple's OpenGL cannot provide. Where possible they reuse an
  equivalent Tier A reference (for example `scene33-compat` must render the
  same frame as `scene33-core`).
- **C (old compatibility games):** regression checks only.

## Running

```sh
tests/games/build-demos.sh /path/to/corpus
scripts/traces/replay.py --wine /path/to/wine --corpus-root /path/to/corpus \
    --template /path/to/prefix-template --path new --html /tmp/corpus.html
```

The template prefix has the Wine module installed
(`scripts/install-wine-module.sh /path/to/wine /path/to/prefix-template`).
`--path` selects the implementation: `new` (WineMetalGL, the prefix's
OpenGL) or `apple` (Wine's own `opengl32`, Apple's OpenGL, which records the
references; it needs a Wine built with OpenGL).
`--json tests/games/results.json` records the run that `docs/games.md` reports.

The corpus directory is not tracked: `build-demos.sh` builds the demos into
it, `replay.py --path apple --update-references` records the reference frames
from Apple's OpenGL, and trace entries need apitrace in `tools/apitrace` and a
trace captured as below (`scene33.trace` is a capture of `scene33-core.exe`).

## Capturing a trace

Copy `tools/apitrace/lib/wrappers/opengl32.dll` next to the program, then run
it with `WINEDLLOVERRIDES=opengl32=n,b` and `TRACE_FILE=<windows path>`. Add
the trace to `corpus.json` with the frames to compare.

## Demos

- `demos/scene33.c`: a deterministic GL 3.3 "mini engine" frame (UBO camera,
  instanced draws, mipmapped texture, RGBA16F + depth FBO, post-process pass,
  point sprites, orphaned stream buffer). `--profile core|compat`,
  `--no-forward`, `--frames`, `--dump`.
- `demos/scene4x.c`: a deterministic GL 4.x frame: a tessellated terrain
  (control and evaluation shaders) from an indirect draw with a
  double-precision wave height, a geometry shader with two invocations
  writing `gl_ViewportIndex` into a two-viewport array, subroutine shading,
  and a tonemap pass. `--profile core41` (a 4.1 core context; Apple's OpenGL
  records the reference), `core42` (the tonemap stores the frame to an
  image and counts its fragments with an atomic counter, Tier B) or `core43`
  (a multi-draw indirect terrain and a compute-shader tonemap that counts its
  pixels in a shader storage block, Tier B); each must render the same frame.
- `demos/legacy.c`: a deterministic fixed-function frame in the style of GL
  1.x/2.x games (lighting, display lists, immediate mode, client arrays with
  quads, multitexture, sphere-map texgen, fog, alpha test, clip plane, point
  sprites, glBitmap text, glDrawPixels, glPushAttrib). `--profile legacy`
  (wglCreateContext; Apple's OpenGL records the reference) or `compat33`
  (a 3.3 compatibility-profile context, Tier B).
