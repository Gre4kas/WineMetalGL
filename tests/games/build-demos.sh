#!/bin/sh
# Build the deterministic corpus demos into a corpus root.
#   tests/games/build-demos.sh /path/to/corpus-root
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
out=${1:?usage: $0 /path/to/corpus-root}
mkdir -p "$out/demos"
for source in "$root"/demos/*.c; do
    name=$(basename "$source" .c)
    x86_64-w64-mingw32-gcc -O2 -Wall "$source" -o "$out/demos/$name.exe" -lopengl32 -lgdi32 -luser32
    i686-w64-mingw32-gcc -O2 -Wall "$source" -o "$out/demos/${name}32.exe" -lopengl32 -lgdi32 -luser32
done
printf 'built demos into %s/demos\n' "$out"
