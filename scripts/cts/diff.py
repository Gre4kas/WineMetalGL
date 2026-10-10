#!/usr/bin/env python3
"""Compare two scripts/cts result files.

Prints the per-group pass delta, newly passing cases and regressions
(cases that passed in OLD and do not pass in NEW). Exits 1 when there are
regressions, unless --allow-regressions is given.

    scripts/cts/diff.py old.txt new.txt [--depth 2] [--list]
"""

import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import results as cts  # noqa: E402


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("old")
    parser.add_argument("new")
    parser.add_argument("--depth", type=int, default=2)
    parser.add_argument("--list", action="store_true", help="list every changed case")
    parser.add_argument("--allow-regressions", action="store_true")
    args = parser.parse_args()
    _, old = cts.load(args.old)
    _, new = cts.load(args.new)
    passing = lambda status: status in cts.PASSING
    gained = sorted(c for c in new if passing(new[c]) and not passing(old.get(c, "Missing")))
    lost = sorted(c for c in old if passing(old[c]) and not passing(new.get(c, "Missing")))
    old_pass = sum(passing(s) for s in old.values())
    new_pass = sum(passing(s) for s in new.values())
    print("pass: %d -> %d (%+d) of %d cases; %d newly passing, %d regressions" % (
        old_pass, new_pass, new_pass - old_pass, len(new), len(gained), len(lost)))
    delta = collections.defaultdict(lambda: [0, 0])
    for case in gained:
        delta[cts.group_of(case, args.depth)][0] += 1
    for case in lost:
        delta[cts.group_of(case, args.depth)][1] += 1
    for group, (plus, minus) in sorted(delta.items()):
        print("  %-60s +%d -%d" % (group, plus, minus))
    if args.list:
        for case in gained:
            print("+ %s (%s -> %s)" % (case, old.get(case, "Missing"), new[case]))
    for case in lost:
        print("REGRESSION %s (%s -> %s)" % (case, old[case], new.get(case, "Missing")))
    if lost and not args.allow_regressions:
        sys.exit(1)


if __name__ == "__main__":
    main()
