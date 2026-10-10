"""Shared reader for scripts/cts result files ("<case> <status>" lines)."""

import collections

PASSING = {"Pass", "CompatibilityWarning", "QualityWarning"}


def load(path):
    results = collections.OrderedDict()
    header = []
    with open(path) as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line:
                continue
            if line.startswith("#"):
                header.append(line)
                continue
            case, _, status = line.partition(" ")
            results[case] = status
    return header, results


def group_of(case, depth=2):
    parts = case.split(".")
    return ".".join(parts[: max(1, min(depth + 1, len(parts) - 1))])
