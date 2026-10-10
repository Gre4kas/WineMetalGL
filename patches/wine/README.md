# Wine integration

The patches in `wine-11.17/` integrate WineMetalGL with Wine 11.17 for x86_64
Unix Wine with x86_64 and i386 guests in one WoW64 prefix. They apply with
`git apply` from the top of a Wine 11.17 source tree (checked against the
`wine-11.17` release), and any Wine 11.17 built with them loads the core.
The results in `docs/` were recorded with MetalSharp's Wine 11.17, which the
x86_64 release's prebuilt Wine modules are built from. Other Wine versions may
need a small rebase when Wine's OpenGL driver interface changes.

- `0001-opengl-wgl-fixes.patch`: three Wine OpenGL fixes.
  `win32u` indexes the parsed extension list correctly for drivers without
  `GL_NUM_EXTENSIONS`, `wglChoosePixelFormatARB` keeps window requests
  within the on-screen pixel formats, and `glDeleteSync(0)` is ignored
  instead of raising `GL_INVALID_VALUE`.
- `0002-winemac-winemetalgl-core.patch`: `winemac.drv` loads
  `libwinemetalgl.dylib` from its own directory and hands it a `CAMetalLayer`
  per window; contexts, surfaces, pbuffers and swaps go through the driver ABI
  in `include/winemetalgl/driver.h`. No CGL context is created.
  `WGL_WINE_query_renderer` reports the core's renderer (vendor Apple, video
  memory, GL versions), which wined3d uses to describe the GPU.

The core is Wine's OpenGL once the patches are applied. `WINEMETALGL=0`
selects Apple's OpenGL (reference frames), and `winemac.drv` falls back to
Apple's OpenGL when the core is missing or refuses to initialise.

`scripts/verify-source.sh` checks that the patches apply when
`WINEMETALGL_WINE_SOURCE` names a Wine checkout.

## Building Wine with the patches

These are the steps for an x86_64 Wine (run under Rosetta) with x86_64 and
i386 guests. They cover only what WineMetalGL needs; any other configure
options are up to you.

Prerequisites:

- Xcode 16 or later, and Rosetta 2.
- Homebrew `bison` and `flex`: macOS's bison is too old for Wine.
- Homebrew `mingw-w64`, which provides the PE cross compilers that build
  `opengl32.dll` and the other guest DLLs.
- Optionally, x86_64 builds of the libraries for Wine's optional features,
  such as FreeType for fonts. On Apple silicon, Homebrew installs arm64
  libraries, which an x86_64 Wine cannot load; `scripts/stage-host-libs.sh`
  copies an x86_64 FreeType closure into a Wine runtime.

```sh
# 1. Wine 11.17 source, with the patches applied from the top of the tree
tar -xf wine-11.17.tar.xz
cd wine-11.17
git apply /path/to/WineMetalGL/patches/wine/wine-11.17/*.patch
cd ..

# 2. Configure an x86_64 macOS 15 build with i386 and x86_64 guests
export PATH="/opt/homebrew/opt/bison/bin:/opt/homebrew/opt/flex/bin:$PATH"
mkdir wine-build && cd wine-build
../wine-11.17/configure \
    --build=x86_64-apple-darwin --host=x86_64-apple-darwin \
    --enable-archs=i386,x86_64 --with-mingw \
    CC='clang -arch x86_64 -mmacosx-version-min=15.0' \
    --prefix=/path/to/wine

# 3. Build and install Wine
make -j"$(sysctl -n hw.ncpu)"
make install

# 4. Build the x86_64 core and install it next to winemac.so
cd /path/to/WineMetalGL
./scripts/build.sh
./scripts/stage-wine.sh /path/to/wine/lib/wine/x86_64-unix
```

What matters for the core:

- `--host=x86_64-apple-darwin` with `-arch x86_64` builds the x86_64 Wine
  that the x86_64 core is built for. `winemac.so` only loads a core of its own
  architecture and otherwise falls back to Apple's OpenGL.
- `--enable-archs=i386,x86_64` gives one WoW64 prefix for 32- and 64-bit
  programs.
- `-mmacosx-version-min=15.0` matches the core's minimum macOS.
- Both patches must be applied. They change `opengl32` (the DLL and its Unix
  side), `win32u` and `winemac.drv`, so use the modules from this build, not
  modules from another Wine.

To check that a program is using the core, look at its `GL_RENDERER` string:
it ends in `(WineMetalGL)`, for example `Apple M4 (WineMetalGL)`, unless
`WINEMETALGL_RENDERER` replaces it. With `WINEMETALGL_LOG=2` the core prints
`winemetalgl:info: WineMetalGL core on <GPU>` to stderr when it loads. If the renderer is Apple's, `winemac.so` did not find or could not
load `libwinemetalgl.dylib` in its own directory.

A native arm64 Wine works the same way, with an arm64 build of Wine and the
arm64 core from `scripts/stage-arm64-core.sh`. That combination has not yet
been run.
