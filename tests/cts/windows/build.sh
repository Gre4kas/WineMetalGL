#!/bin/sh
# Builds the Windows glcts.exe that scripts/cts/run.py runs under Wine, with
# MinGW-w64 (x86_64-w64-mingw32-gcc; Homebrew: mingw-w64).
#
#   tests/cts/windows/build.sh <VK-GL-CTS checkout> [build dir]
#
# The baselines in tests/cts/baseline were recorded with VK-GL-CTS commit
# 1942832 ("dEQP Core 1942832..." in their headers).
set -eu
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
cts=${1:?usage: build.sh <VK-GL-CTS checkout> [build dir]}
build=${2:-$cts/build-win64}

# MinGW needs <limits> for std::numeric_limits in the Win32 subprocess executor.
if ! grep -q '#include <limits>' "$cts/framework/common/tcuSubprocessTestExecutorWin.cpp"; then
    git -C "$cts" apply "$here/0001-tcuSubprocessTestExecutorWin-include-limits.patch"
fi
python3 "$cts/external/fetch_sources.py"
cmake -S "$cts" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$here/mingw64-toolchain.cmake" -DSELECTED_BUILD_TARGETS=glcts
cmake --build "$build" --target glcts
printf '%s\n' "glcts.exe: $build/external/openglcts/modules/glcts.exe"
