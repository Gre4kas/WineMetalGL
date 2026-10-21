# Quake 4: display modes, the second render thread, x87 math

Quake 4 (and other 32-bit idTech 4 games) on a Mac runs into three problems
outside WineMetalGL's GL (issue #58; WineMetalGL's own part, a pipeline cache
that retried a save Metal cannot make every 3 seconds, is fixed in it):

- **It does not start**: "the console crash", a console window that says the
  mode could not be set. Quake 4 asks for an old mode (640x480, 1024x768,
  1920x1080, ...) and Wine's Mac driver lists only the display's native
  modes, so `ChangeDisplaySettings` fails with `DISP_CHANGE_BADMODE`.
- **It is slow while the GPU waits**: low GPU time, low frame rate.
  Its math is x87 floating-point code (the game DLL has about
  147,000 x87 instructions to 7,400 SSE ones), which Rosetta translates into
  slow soft-float.
- **It renders on one thread**: it detects the Mac's CPU as a "1.00 GHz"
  single core and leaves its dual-core renderer (`r_useSMP`) off.

Measured on an M4 with the game's heaviest scenes (frames of more than 300
draws), at 1470x956:

| | median | 95th percentile | frames over 50 ms |
| --- | --- | --- | --- |
| as it starts | 41.9 ms (24 fps) | 99.6 ms | 8.5% |
| `r_useSMP 1` | 30.6 ms (33 fps) | 69.9 ms | 8.4% |
| `r_useSMP 1` and x87 acceleration | 16.0 ms (62 fps) | 18.4 ms | 0.3% |

## Display modes: `legacy-display-modes.reg`

Sets Wine's `EmulateModeset`: the modes a game asks for, up to the screen's
size, become virtual modes that succeed without changing the real one (the
game is shown at the screen's size). It is prefix-wide: Wine builds its mode
list in whichever of its processes refreshes the displays, so a key for
`Quake4.exe` alone has no effect. Every program in that prefix or bottle then
changes modes this way.

```sh
./apply-display-modes.sh ~/.metalsharp/prefix-steam
./apply-display-modes.sh "My Bottle"      # a CrossOver bottle
```

The script imports the file and stops the prefix's wineserver, which the new
mode list needs. Without the file, setting a custom mode the size of the
screen works too (`+set r_mode -1 +set r_customWidth 1470 +set
r_customHeight 956`, with your screen's size in points).

## The second render thread: `autoexec.cfg`

Copy it into the game's `q4base` folder (next to `Quake4Config.cfg`), or add
`+set r_useSMP 1` to the game's launch options. The game then saves the
setting into `Quake4Config.cfg` itself.

## x87 math: WineMetalGL's x87 acceleration

Turn on **x87 Acceleration** in WineMetalGL Settings (or set
`WINEMETALGL_X87=1`). When a 32-bit game loads `opengl32.dll`, WineMetalGL
starts x87sidecar, which makes Rosetta translate x87 code into fast native
code. MetalSharp's Wine does this for every 32-bit program already, and
WineMetalGL then leaves it alone. See `WINEMETALGL_X87` in
[docs/building.md](../../docs/building.md).
