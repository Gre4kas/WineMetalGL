#!/bin/sh
set -eu

project_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$project_root"

git diff --check
python3 - <<'PY'
from pathlib import Path
import subprocess
manifest = set(Path('MANIFEST.txt').read_text().splitlines())
tracked = subprocess.check_output(['git', 'ls-files'], text=True).splitlines()
missing = [p for p in tracked if p not in manifest and p not in ('.gitignore', 'MANIFEST.txt')]
if missing:
    raise SystemExit('manifest missing: ' + ', '.join(missing))
PY
python3 -m json.tool docs/api-coverage.json >/dev/null
python3 -I scripts/docs/generate.py --check --check-provenance
shasum -a 256 -c SHA256SUMS >/dev/null
if grep -i -n 'VKMT' scripts/build.sh scripts/stage-host-libs.sh scripts/stage-release.sh scripts/build-wine-module.sh scripts/install-wine-module.sh README.md CMakeLists.txt || grep -R -i -n 'VKMT' tests; then
    printf '%s\n' 'standalone source contains VKMT-specific naming' >&2
    exit 1
fi

# No source may use Apple's OpenGL (the patches only carry Wine's own context lines).
if git grep -n -I -e 'OpenGL.framework' -e 'OpenGL/OpenGL.h' -e 'OpenGL/gl.h' -e 'CGLContextObj' -- src include tools tests CMakeLists.txt; then
    printf '%s\n' 'source uses Apple OpenGL' >&2
    exit 1
fi

if test -f build/release/libwinemetalgl.dylib; then
    test "$(lipo -archs build/release/libwinemetalgl.dylib)" = x86_64
    test "$(otool -l build/release/libwinemetalgl.dylib | awk '/LC_BUILD_VERSION/{found=1} found && /minos/{print $2; exit}')" = 15.0
    if otool -L build/release/libwinemetalgl.dylib | grep -q OpenGL.framework; then
        printf '%s\n' 'libwinemetalgl.dylib links OpenGL.framework' >&2
        exit 1
    fi
fi

printf '%s\n' 'WINEMETALGL_SOURCE_AUDIT_OK'
