# Wolfenstein: The New Order: patched xaudio2_7.dll

`xaudio2_7.dll` here is Wine's built-in XAudio2 2.7 from MetalSharp's Wine
11.17 (FAudio 26.09), rebuilt with the fixes in
[`faudio-resample-and-callback-fixes.patch`](faudio-resample-and-callback-fixes.patch).
Without them the game crashes or freezes as the player enters the turret in
the first chapter's plane, where the music changes (issue #40).

## What was wrong

FAudio's mixing thread corrupted the heap with decoded audio, so the crash
showed up in unrelated places (a critical section, a game object's vtable,
a buffer pointer):

- **Resampler count underflow** (the writer). After a voice is sent to an
  output of another sample rate, the loops that bridge from the previous
  quantum could emit more samples than the pass makes;
  `toResample - tap_samples` then wrapped around and the SSE2 resampler wrote
  far past `resampled_audio`. The bridging loops now stop at `toResample`.
- **Stale pointers across callbacks.** The decode loop took its output
  pointer into `decoded_audio` and the queued buffer before `start_buffer`,
  which unlocks the voice for `OnBufferStart`; a callback that submits
  buffers or creates voices reallocates both. `end_buffer` wrote through its
  buffer pointer after `OnLoopEnd` the same way. Both are now taken again
  after the callbacks.
- **Stale resample step.** The step was recomputed only when the voice's
  frequency changed, not when its output's sample rate did; rerouting now
  makes the next mix recompute it.
- **Guards** where release builds compiled the asserts out: decoding never
  exceeds the voice's `decodeSamples`, and leftover unaligned bytes that would
  overflow their block-sized stash are dropped.

## Installing

WineMetalGL Settings installs it by itself into MetalSharp's Wine 11.17 when
it finds the game in a MetalSharp prefix (Game Fixes, on by default). It keeps
the replaced file in `~/Library/Application Support/WineMetalGL/Backups`.

By hand, into MetalSharp's Wine (64-bit games):

```sh
cp ~/.metalsharp/runtime/wine/lib/wine/x86_64-windows/xaudio2_7.dll ~/xaudio2_7.dll.metalsharp
cp xaudio2_7.dll ~/.metalsharp/runtime/wine/lib/wine/x86_64-windows/xaudio2_7.dll
```

Copying the saved file back undoes it. A MetalSharp update replaces it.

The DLL was built with llvm-mingw 20260922 (clang) from the objects of
MetalSharp's Wine 11.17 build, with `FAudio_internal.c` patched; it exports
the same functions and is marked as a Wine builtin like the original. Only
`xaudio2_7.dll` is here; MetalSharp's other FAudio DLLs (XAudio2 2.0 to 2.9,
XACT, X3DAudio) share the code and need the patch in MetalSharp's own build.

Also needed for this game: WineMetalGL's occlusion query, timer query,
second GL front end and crash handler fixes (from the same pull request as
this folder), and in MetalSharp's Wine, the ntdll GSBASE switch for programs
that use fibers.

SHA-256 of `xaudio2_7.dll`:
`b9a91417d27a4adffc75869929b9fdcb6175bbc1e7299d386d2a6728a189cbc8`
