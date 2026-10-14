# Releasing WineMetalGL

## Artifacts

`scripts/stage-release.sh /path/to/wine-headers OUTPUT` builds the core
(`scripts/build.sh`) and the Wine module (`scripts/build-wine-module.sh`, with
the headers of a Wine 11.x installation or source tree) from the checkout,
signs the Mach-O libraries and stages a release. `STAGED-FROM.txt`
records the commit, and says so when the tree had uncommitted changes:

```text
OUTPUT/
  install-wine-module.sh
  module/
    x86_64-unix/     winemetalgl.so, libwinemetalgl.dylib (the core)
    x86_64-windows/  winemetalgl.dll, opengl32.dll
    i386-windows/    winemetalgl.dll, opengl32.dll
  evidence/        CTS baselines and their rechecks (cts/recheck), corpus results
                   and frame times of the commit
  docs/            conformance.md, unsupported.md, testing.md, building.md,
                   performance.md, games.md, api-coverage.json, release.md
  LICENSE, LICENSES/  LGPL-3.0-or-later, the GPL-3.0 it builds on, third-party licenses
  README.md, STAGED-FROM.txt, SHA256SUMS
```

`./install-wine-module.sh /path/to/wine [/path/to/prefix]` installs the module
into any x86_64 Wine 11.x and, given a prefix, the forwarding `opengl32.dll`
(and a copy of `winemetalgl.dll`) with the `opengl32=native` override ([Build it yourself](building.md#the-wine-module)).
No Wine file is replaced, and the module does not depend on the Wine build it
was staged with.

`scripts/verify-release.sh OUTPUT` checks the layout, the architectures and
code signatures, that the core links no OpenGL.framework and exports only the
driver ABI, that `winemetalgl.dll` is marked as a Wine builtin and
`winemetalgl.so` has its WoW64 call table, and the SHA256 manifest.

## The release

After the development branch merges, `main` is tagged
`winemetalgl-<version>` (the `project()` version in `CMakeLists.txt`) and
released with two archives and `SHA256SUMS.txt`:

- `winemetalgl-<version>-x86_64.tar.gz`: the staged release above, with the
  evidence directory, for x86_64 Wine 11.x (under Rosetta). Its Wine evidence
  ran on stock Wine 11.0 with the module installed.
- `winemetalgl-<version>-arm64-core.tar.gz`: `scripts/stage-arm64-core.sh
  OUTPUT`, the arm64 core with the driver ABI header. Its evidence is the
  native runs (the core built for the build machine); the Wine module is
  built for x86_64 Wine only, so it has not yet run inside an arm64 Wine.

The two cores are not interchangeable: a library of the other architecture
does not load.

## Signing and notarization

Both staging scripts sign ad hoc unless `WINEMETALGL_SIGN_IDENTITY` names a
Developer ID Application identity; releases set it, which signs the Mach-O
libraries (`libwinemetalgl.dylib`, `winemetalgl.so`) with the hardened
runtime and a secure timestamp, and `verify-release.sh` then checks that
authority. The libraries are notarized as a zip of the staged Mach-O files
(`xcrun notarytool submit --wait`); loose libraries cannot be stapled, so
Gatekeeper checks the ticket online. The Windows DLLs are PE files and carry
no Apple signature. `install-wine-module.sh` keeps a valid signature on the
copies it installs and signs ad hoc only those without one.

## MetalSharp

MetalSharp consumes releases as `VKMT/graphics/winemetalgl-<version>/` and
installs them into its Wine runtime. Checked against the 0.5.0 consumer
(MetalSharp `3626d394`):

| 0.5.0 layout | 2.0.0 layout | MetalSharp change |
|---|---|---|
| `artifacts/host-x86_64/metalsharp-opengl.dylib` (sidecar, built by `tools/bundles/build-winemetalgl-x86.sh` from v0.1.0) | `artifacts/wine-driver/libwinemetalgl.dylib` | install as `lib/wine/x86_64-unix/libwinemetalgl.dylib`; replace the sidecar build script |
| `artifacts/wine-driver/winemac.so`, `win32u.so` | same names, plus `opengl32.so` | MetalSharp's runtime is its own Wine 11.17 tree: apply `patches/wine` there instead of copying these |
| `artifacts/guest/{i386,x86_64}/opengl32.dll` | unchanged | rebuilt from the patched tree |
| `app/src-c/runtime/setup.c` requires `x86_64-unix/metalsharp-opengl.dylib` | | require `x86_64-unix/libwinemetalgl.dylib` |
| `tools/bundles/wine-runtime-hashes.tsv` | | regenerate |
| `steam_actions.c` sets `WINEMETALGL=0` | still honoured (Apple's OpenGL) | remove to enable the core |
| `VKMT_OPENGL_METAL_EXPERIMENTAL` | no longer read | drop |

From 3.0.0 the release is the Wine module, which MetalSharp's Wine 11.17
loads without the 2.x patches:

| 2.x layout | 3.0.0 layout | MetalSharp change |
|---|---|---|
| `artifacts/wine-driver/libwinemetalgl.dylib` | `module/x86_64-unix/libwinemetalgl.dylib` | same destination, `lib/wine/x86_64-unix`; the driver ABI is unchanged, so a patched `winemac.so` still loads it |
| `artifacts/wine-driver/winemac.so`, `win32u.so`, `opengl32.so`; patches applied to the runtime | none | the runtime can return to unpatched `winemac.drv`, `win32u` and `opengl32` |
| | `module/x86_64-unix/winemetalgl.so`, `module/{x86_64,i386}-windows/winemetalgl.dll` | install into `lib/wine/x86_64-unix`, `lib/wine/{x86_64,i386}-windows` |
| `artifacts/guest/{i386,x86_64}/opengl32.dll` (Wine's, patched) | `module/{x86_64,i386}-windows/opengl32.dll` (a forwarder) | install into each prefix's `system32` and `syswow64` with `opengl32=native` (`install-wine-module.sh`), instead of into `lib/wine` |
| `WINEMETALGL=0` | `WINEDLLOVERRIDES=opengl32=b` | Wine's own OpenGL for one program |
