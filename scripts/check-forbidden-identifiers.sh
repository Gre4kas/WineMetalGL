#!/bin/sh
# Fail if driver sources mention identifiers that only exist in conformance
# test shaders. See tests/forbidden-identifiers.txt.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
list="$root/tests/forbidden-identifiers.txt"
status=0
grep -v -E '^[[:space:]]*(#|$)' "$list" > "${TMPDIR:-/tmp}/wmgl-forbidden.$$"
trap 'rm -f "${TMPDIR:-/tmp}/wmgl-forbidden.$$"' EXIT
dirs=""
for d in src include; do test -d "$root/$d" && dirs="$dirs $root/$d"; done
if grep -rn -E -f "${TMPDIR:-/tmp}/wmgl-forbidden.$$" $dirs; then
    printf '%s\n' 'error: driver sources reference conformance-test identifiers (tests/forbidden-identifiers.txt)' >&2
    status=1
else
    printf '%s\n' 'forbidden-identifier gate: clean'
fi
exit "$status"
