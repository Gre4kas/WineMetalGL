#!/usr/bin/env python3
"""Per-group summary of one or more scripts/cts result files.

    scripts/cts/summary.py results.txt [--depth 2] [--markdown]
"""

import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import results as cts  # noqa: E402

STATUSES = ["Pass", "Fail", "NotSupported", "InternalError", "Crash", "Timeout", "Missing"]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("files", nargs="+")
    parser.add_argument("--depth", type=int, default=2, help="group depth below the package name")
    parser.add_argument("--markdown", action="store_true")
    args = parser.parse_args()
    for path in args.files:
        header, results = cts.load(path)
        groups = collections.OrderedDict()
        for case, status in results.items():
            key = cts.group_of(case, args.depth)
            groups.setdefault(key, collections.Counter())[
                "Pass" if status in cts.PASSING else status] += 1
        total = collections.Counter()
        for counter in groups.values():
            total.update(counter)
        columns = [s for s in STATUSES if total[s]] + sorted(set(total) - set(STATUSES))
        if args.markdown:
            print("\n".join(h.lstrip("# ") for h in header[:2]))
            print()
            print("| Group | Cases | " + " | ".join(columns) + " |")
            print("|---|---:|" + "---:|" * len(columns))
            for key, counter in sorted(groups.items()):
                print("| `%s` | %d | " % (key, sum(counter.values())) +
                      " | ".join(str(counter[c]) for c in columns) + " |")
            print("| **total** | %d | " % sum(total.values()) + " | ".join(str(total[c]) for c in columns) + " |")
        else:
            print(path)
            for line in header:
                print("  " + line)
            width = max(len(k) for k in groups) + 2
            print("  %-*s %7s " % (width, "group", "cases") + " ".join("%13s" % c for c in columns))
            for key, counter in sorted(groups.items()):
                print("  %-*s %7d " % (width, key, sum(counter.values())) +
                      " ".join("%13d" % counter[c] for c in columns))
            print("  %-*s %7d " % (width, "total", sum(total.values())) +
                  " ".join("%13d" % total[c] for c in columns))


if __name__ == "__main__":
    main()
