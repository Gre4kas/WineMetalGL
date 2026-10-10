#!/bin/sh
# Regenerate MANIFEST.txt (every tracked file) and SHA256SUMS (every tracked
# file outside vendor/, except SHA256SUMS itself) so scripts/verify-source.sh
# stays green. Run after `git add` and before committing.
set -eu
root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$root"
{ git ls-files; printf '%s\n' MANIFEST.txt SHA256SUMS; } | sort -u > MANIFEST.txt
git ls-files | grep -v -E '^(vendor/|SHA256SUMS$|MANIFEST.txt$)' | sort -u > "${TMPDIR:-/tmp}/wmgl-sums.$$"
printf '%s\n' MANIFEST.txt >> "${TMPDIR:-/tmp}/wmgl-sums.$$"
sort -u "${TMPDIR:-/tmp}/wmgl-sums.$$" | while IFS= read -r f; do test -f "$f" && shasum -a 256 "$f"; done > SHA256SUMS
rm -f "${TMPDIR:-/tmp}/wmgl-sums.$$"
printf 'MANIFEST.txt: %s files, SHA256SUMS: %s files\n' "$(wc -l < MANIFEST.txt | tr -d ' ')" "$(wc -l < SHA256SUMS | tr -d ' ')"
