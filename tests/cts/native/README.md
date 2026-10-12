# Native CTS platform

`tcuWmglPlatform.cpp` is a VK-GL-CTS platform port that runs the OpenGL CTS
on macOS directly against the WineMetalGL core, without Wine. Each GL
context renders to a pbuffer surface created through the driver ABI
(`include/winemetalgl/driver.h`), and entry points come from
`wmgl_get_proc_address`, exactly as the Wine module uses them.

It exists for fast iteration and debugging (lldb, Metal validation, no
Rosetta). Conformance claims still come from the Wine runs
(`scripts/cts/run.py --wine ...`), which exercise the real WGL path.

## Build

```sh
tests/cts/native/build.sh /path/to/VK-GL-CTS
```

This builds the core for the build machine (`-DWMGL_HOST_ONLY=ON`, arm64 on
Apple silicon), installs `targets/wmgl/wmgl.cmake` into the CTS checkout and
builds `glcts` into `<cts>/build-wmgl-native`.

## Run

```sh
scripts/cts/run.py --native \
    --cts /path/to/VK-GL-CTS/build-wmgl-native/external/openglcts/modules/glcts \
    --config 1 --shards 8 --out results/gl33-cfg1-native.txt
```

`run.py` always passes `--deqp-terminate-on-device-lost=disable`: with it
enabled the CTS calls `glGetGraphicsResetStatus` after every case, but only
loads that entry point for GL 4.5 or `GL_KHR_robustness`, so an exact 3.3
context crashes on a null function pointer.
