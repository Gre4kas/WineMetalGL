# Releasing WineMetalGL

## Artifacts

`scripts/stage-release.sh /path/to/wine-build OUTPUT` builds the core from the
checkout (`scripts/build.sh`), ad-hoc signs the Mach-O libraries and stages a
release from a Wine 11.17 build with `patches/wine` applied. `STAGED-FROM.txt`
records the commit, and says so when the tree had uncommitted changes:

```text
OUTPUT/
  artifacts/
    wine-driver/   libwinemetalgl.dylib  the core (x86_64)
                   winemac.so            loads the core from its own directory
                   win32u.so, opengl32.so
    guest/i386/    opengl32.dll
    guest/x86_64/  opengl32.dll
  evidence/        CTS baselines, corpus results and frame times of the commit
  docs/            conformance.md, unsupported.md, testing.md, building.md,
                   performance.md, games.md, api-coverage.json, release.md
  README.md, STAGED-FROM.txt, SHA256SUMS
```

The `wine-driver` files go to `lib/wine/x86_64-unix`, the guest DLLs to
`lib/wine/i386-windows` and `lib/wine/x86_64-windows`. They are ABI-coupled to
the Wine build they were staged from; a different Wine tree applies
`patches/wine` and takes only `libwinemetalgl.dylib`. `WINEMETALGL=0` selects
Apple's OpenGL, and Wine falls back to it when the core cannot be loaded.

`scripts/verify-release.sh OUTPUT` checks the layout, the architectures and
code signatures, that the core links no OpenGL.framework and exports only the
driver ABI, and the SHA256 manifest.

## The release

After the development branch merges, `main` is tagged
`winemetalgl-<version>` (the `project()` version in `CMakeLists.txt`) and
released with two archives:

- `winemetalgl-<version>-x86_64.tar.gz`: the staged release above, with the
  evidence directory. Its Wine modules are staged from MetalSharp's x86_64
  Wine 11.17 (under Rosetta) and are a drop-in for that Wine; any other
  Wine 11.17 is built with `patches/wine` ([Build it yourself](building.md))
  and takes only `libwinemetalgl.dylib`.
- `winemetalgl-<version>-arm64-core.tar.gz`: `scripts/stage-arm64-core.sh
  OUTPUT`, the arm64 core with the driver ABI header and `patches/wine`, for a
  native arm64 Wine built with the patches. Its evidence is the native
  KHR-GL30 to KHR-GL33 runs (the core built for the build machine); it has not
  yet run inside an arm64 Wine.

The two cores are not interchangeable: `winemac.so` loads the core of its own
architecture, and Wine falls back to Apple's OpenGL when it cannot.

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
