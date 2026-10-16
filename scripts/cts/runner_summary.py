#!/usr/bin/env python3
"""Records a cts-runner run as committed evidence.

    scripts/cts/runner_summary.py LOGDIR OUT [--configs TEXT] [--commit COMMIT]

LOGDIR is cts-runner's --logdir (it holds cts-run-summary.xml and one .qpa
per run). OUT gets a header (`# key value` lines: commit, cts revision, run
type, whether cts-runner called it conformant, the configurations) and one
line per run: its log file and the counts cts-runner recorded (`Passed=N
Failed=N NotSupported=N Warnings=N Waived=N DeviceLost=N Executed=N`).
cts-runner counts every result that is not Pass, NotSupported or a warning
as Failed. scripts/docs/generate.py reads OUT (ES_RUNNER_SUMMARY).
"""

import argparse
import os
import re
import subprocess
import sys
import xml.etree.ElementTree as ET

KEYS = ("Passed", "Failed", "NotSupported", "Warnings", "Waived", "DeviceLost", "Executed")


def git_head():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    head = subprocess.check_output(["git", "-C", root, "rev-parse", "--short", "HEAD"], text=True).strip()
    dirty = subprocess.run(["git", "-C", root, "diff", "--quiet", "HEAD"]).returncode != 0
    return head + ("-dirty" if dirty else "")


def cts_revision(logdir, runs):
    """The dEQP release and revision a run's log records."""
    for name, _ in runs:
        path = os.path.join(logdir, name)
        if not os.path.exists(path):
            continue
        with open(path, errors="replace") as handle:
            for line in handle:
                m = re.match(r"#sessionInfo releaseName (.+)", line)
                if m:
                    return m.group(1).strip()
    return "not recorded"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("logdir")
    parser.add_argument("out")
    parser.add_argument("--configs", default="", help="the configurations cts-runner selected, as text")
    parser.add_argument("--commit", help="the commit the tested build came from (default: the tree's HEAD)")
    args = parser.parse_args()
    root = ET.parse(os.path.join(args.logdir, "cts-run-summary.xml")).getroot()
    runs = []
    for run in root.iter("TestRun"):
        result = run.find("TestResult")
        counts = {k: int(result.get(k, 0)) for k in KEYS} if result is not None else None
        runs.append((run.get("FileName"), counts))
    missing = [name for name, counts in runs if counts is None]
    if missing:
        sys.exit("runs without results (the run stopped early): " + ", ".join(missing))
    lines = ["# commit %s" % (args.commit or git_head()),
             "# cts %s" % cts_revision(args.logdir, runs),
             "# type %s" % root.get("Type", "?"),
             "# conformant %s" % root.get("Conformant", "?"),
             "# configs %s" % args.configs]
    lines += ["%s %s" % (name, " ".join("%s=%d" % (k, counts[k]) for k in KEYS)) for name, counts in runs]
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as handle:
        handle.write("\n".join(lines) + "\n")
    failed = sum(counts["Failed"] for _, counts in runs)
    print("%d runs, %d cases executed, %d failed" % (len(runs), sum(c["Executed"] for _, c in runs), failed))


if __name__ == "__main__":
    main()
