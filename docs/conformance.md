# WineMetalGL conformance

This document reports **measured** results only. Every number here comes from a
committed result file under `tests/cts/baseline/`, produced by
`scripts/cts/run.py` and summarised by `scripts/cts/summary.py`.

## Why the old numbers were withdrawn

Releases up to 1.9.17 published per-version coverage percentages and targeted
Khronos CTS results such as `KHR-GL30.shaders30.*: 651/651` and a 108/108 "GL 3.3
regression ledger". Those results depended on code in the experimental Metal
path that recognised CTS shaders (by source text, uniform names and geometry)
and produced the expected output instead of rendering it. That code was removed
in `b4a1356`, and `scripts/check-forbidden-identifiers.sh` keeps it out.

## Current baseline (legacy path)

`tests/cts/baseline/gl33-cfg1-oldcore.txt`: `KHR-GL33` mustpass (`gl33-main.txt`,
9,886 cases), mustpass configuration 1, Apple M4, macOS 27.2, Wine 11.17:

| Result | Cases |
|---|---:|
| Pass | 5,783 |
| Fail | 3,393 |
| NotSupported | 702 |
| InternalError | 8 |

Per-group results: `scripts/cts/summary.py tests/cts/baseline/gl33-cfg1-oldcore.txt --depth 1`.

This is the comparison point for the new core being built on `feat/gl33-core`.
The new core becomes the default only when it passes at least as many cases in
every group.

## Reproducing

```sh
scripts/cts/run.py --wine /path/to/wine --cts /path/to/glcts.exe \
    --template /path/to/wow64-prefix --config 1 --shards 7 --out results.txt
scripts/cts/summary.py results.txt --depth 1
scripts/cts/diff.py tests/cts/baseline/gl33-cfg1-oldcore.txt results.txt
```

`mustpass.xml` runs `gl33-main.txt` under six configurations (`--config 1..6`).
A conformance claim requires all six. NotSupported is acceptable only for
extension groups the implementation does not advertise.

Formal Khronos conformance additionally requires the members-only GTF tests
(`gl33-gtf-main.txt`) and the Adopter process; neither is part of these results.
