# Build it yourself

How to build the core, build a Wine 11.17 that loads it, and check that
programs use it. The test suites are in [Test suite](testing.md); release
packages are in [Releasing](release.md).

## Requirements

- macOS 15 or later on Apple silicon, Xcode 16 or later, CMake 3.24 or later,
  Ninja and Python 3.
- Rosetta 2, to run an x86_64 Wine.
- For Wine and the guest test programs: Homebrew `bison`, `flex` and
  `mingw-w64` (`x86_64-w64-mingw32-gcc`, `i686-w64-mingw32-gcc`).

glslang and SPIRV-Cross are vendored at pinned revisions
([`DEPENDENCIES.md`](../DEPENDENCIES.md)), so the build fetches nothing. The
core links only Apple system frameworks and never links OpenGL.framework.

## The core

```sh
./scripts/build.sh            # build/release/libwinemetalgl.dylib (x86_64, macOS 15)
```

That is the core for an x86_64 Wine under Rosetta. For a native arm64 Wine,
`scripts/stage-arm64-core.sh /path/to/output` builds the arm64 core and stages
it with the driver ABI header and the Wine patches; that combination has not
yet been run inside an arm64 Wine.

For development, the core and its tests also build for the machine itself,
without Rosetta:

```sh
cmake -S . -B build/host -DWMGL_HOST_ONLY=ON
cmake --build build/host
build/host/wmgl_host_tests && build/host/wmgl_gpu_tests
```

## Wine 11.17 with the patches

The patches in [`patches/wine/wine-11.17`](../patches/wine/README.md) add the
core to `winemac.drv` and fix four Wine OpenGL bugs. They apply to the
`wine-11.17` release; other Wine versions may need a small rebase when Wine's
OpenGL driver interface changes. These steps build an x86_64 Wine (run under
Rosetta) with x86_64 and i386 guests, and cover only what WineMetalGL needs;
any other configure options are up to you.

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
- Wine's optional features need x86_64 builds of their libraries, such as
  FreeType for fonts. On Apple silicon, Homebrew installs arm64 libraries,
  which an x86_64 Wine cannot load;
  `WINEMETALGL_HOST_LIB_DIR=/path/to/x86_64/libs scripts/stage-host-libs.sh /path/to/wine`
  copies an x86_64 FreeType closure into a Wine installation.

## Checking that a program uses the core

With the patches applied, the core is Wine's OpenGL. Look at a program's
`GL_RENDERER` string: it ends in `(WineMetalGL)`, for example
`Apple M4 (WineMetalGL)`. With `WINEMETALGL_LOG=2` the core prints
`winemetalgl:info: WineMetalGL core on <GPU>` to stderr when it loads. If the
renderer is Apple's, `winemac.so` did not find or could not load
`libwinemetalgl.dylib` in its own directory, and Wine fell back to Apple's
OpenGL.

## Environment

| Variable | Effect |
|---|---|
| `WINEMETALGL=0` | Use Apple's OpenGL instead of the core (how reference frames are recorded) |
| `WINEMETALGL_LOG=1..3` | Log warnings, information or traces (errors are always logged) |
| `WINEMETALGL_TRACE=1` | Log every GL call |
| `WINEMETALGL_STATS=1` | Log per-frame statistics |
| `WINEMETALGL_VENDOR`, `WINEMETALGL_RENDERER` | Replace the `GL_VENDOR` and `GL_RENDERER` strings, for games that refuse unknown vendors |

Some programs' OpenGL loaders need entry points of GL versions above 4.3,
which Windows drivers return whatever the context's version. Wine returns them
to programs that opt in through the registry of their prefix:

```text
HKCU\Software\Wine\AppDefaults\<app.exe>\OpenGL    LaterEntryPoints = "Y"
```

Calling one of these functions raises `GL_INVALID_OPERATION`, since 4.3
contexts do not offer them. Dead Cells (`deadcells_gl.exe`) needed it with
earlier versions; the GL 4.2 and 4.3 entry points its loader requires are
part of 4.3 contexts.
