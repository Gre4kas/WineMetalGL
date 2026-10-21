# SpaceEngine: patched winemac.so

`winemac.so` is Wine's macOS driver from MetalSharp's Wine 11.17, rebuilt with
[`winemac-stale-modifiers.patch`](winemac-stale-modifiers.patch). Without it,
pressing W, A, S or D in SpaceEngine plays the macOS alert sound (issue #35).

## What was wrong

Wine maps the Command key to Alt. Sometimes Command goes down while a Wine
window has the keyboard and comes up in another application, for example:

- Command-Tab, or Command-Space, to switch away;
- Command-Q to quit a Wine program.

When that happens, Wine never sees the key-up. Windows' key state keeps Alt
held, and it stays held for every program in that Wine session.

With Alt held, every letter becomes an Alt shortcut (`WM_SYSCHAR`), and Windows
beeps for an Alt shortcut that no menu takes. Two things kept the stuck key:

- When a window became key again, Wine replayed the last modifier change the
  application had seen, which still had Command down.
- Nothing compared Windows' key state with the keys actually down on the Mac.

The patch fixes both:

- A window that becomes key syncs from the modifiers down now.
- A window that gets focus releases the modifiers that Windows holds down but
  that are up on the Mac. That also clears a key state left behind by a
  program that is gone.

This is a Wine bug, not a WineMetalGL one; any Wine program can hit it.

## Installing

WineMetalGL Settings installs it by itself into MetalSharp's Wine 11.17 when
it finds SpaceEngine in a MetalSharp prefix (Game Fixes, on by default). It
keeps the replaced file in `~/Library/Application Support/WineMetalGL/Backups`.

By hand, into MetalSharp's Wine:

```sh
cp ~/.metalsharp/runtime/wine/lib/wine/x86_64-unix/winemac.so ~/winemac.so.metalsharp
cp winemac.so ~/.metalsharp/runtime/wine/lib/wine/x86_64-unix/winemac.so
```

Copying the saved file back undoes it, and a MetalSharp update replaces it.
The library is signed ad hoc.

It was built with clang from MetalSharp's Wine 11.17 source, with the patch
applied. It exports the same functions and links the same libraries as
MetalSharp's.

## Also needed for this game

The rest of issue #35 is fixed in WineMetalGL's core, in the same pull request
as this folder:

- Orbits: SpaceEngine's orbit shader declares two vertex inputs at one
  location, which Metal refused.
- Atmosphere: SpaceEngine's core-profile shaders call `texture2D`.

## Audio

The reported crackling did not reproduce on the test machine. SpaceEngine plays
sound through SDL2 and DirectSound, not XAudio2, so Wolfenstein's
`xaudio2_7.dll` does not apply to it.

SHA-256 of `winemac.so`:
`1eb49be5618ebbd513ec89de2a9f157ac23a30c0e587de713eca5310d5d2382c`
