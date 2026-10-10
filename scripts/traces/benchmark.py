#!/usr/bin/env python3
"""Frame-time benchmark of WineMetalGL's game corpus (W24).

Runs every corpus entry on WineMetalGL ("new") and, where Apple's OpenGL
can run it, on Apple's OpenGL ("apple"), several times each, and writes
tests/games/perf.json. Demos run with --benchmark (vsync off, per-frame wall
times after a warm-up, printed as FRAMETIME); apitrace traces run with
glretrace --benchmark and WINEMETALGL_STATS (WineMetalGL only).

Budgets (WineMetalGL 3.3 roadmap, W24):
  * entries Apple's OpenGL runs (tiers A and C): average frame time at most
    1.2x Apple's;
  * Tier B entries: at least 60 fps (average frame time <= 16.7 ms);
  * every entry: no frame over 50 ms after the warm-up (median over the runs
    of each run's longest frame);
  * no entry slower than the committed baseline (--baseline, by default the
    previous tests/games/perf.json) by more than 10%.

    scripts/traces/benchmark.py --wine /path/to/wine --corpus-root /path/to/corpus \\
        --template /path/to/prefix-template [--runs 3] [--frames 600]
"""

import argparse
import datetime
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FRAMETIME = re.compile(r"FRAMETIME avg=([\d.]+) p95=([\d.]+) max=([\d.]+) frames=(\d+)")
STATS_FRAME = re.compile(r"winemetalgl:stats: frame (\d+) ([\d.]+) ms")

RATIO_BUDGET = 1.2
TIER_B_FRAME_MS = 1000.0 / 60.0
HITCH_MS = 50.0
REGRESSION = 1.10


def impl_env(base, impl):
    env = dict(base)
    env.pop("WINEMETALGL", None)
    if impl == "apple":
        env["WINEMETALGL"] = "0"  # Apple's OpenGL (winemac's fallback path)
    return env


def strip_frames(args):
    out, skip = [], False
    for a in args:
        if skip:
            skip = False
            continue
        if a == "--frames":
            skip = True
            continue
        out.append(a)
    return out


def run_once(entry, impl, args, env):
    """Returns (avg, p95, max) frame times in ms, or None."""
    wine = os.path.join(args.wine, "bin", "wine")
    root = args.corpus_root
    if entry["kind"] == "demo":
        exe = os.path.join(root, entry["binary"])
        # On screen for both paths: a hidden window is paced by the window server.
        command = [wine, exe] + strip_frames(entry.get("args", [])) + ["--frames", str(args.frames), "--benchmark"]
        result = subprocess.run(command, cwd=os.path.dirname(exe), env=impl_env(env, impl), capture_output=True,
                                timeout=entry.get("timeout", 600))
        match = FRAMETIME.search(result.stdout.decode("latin1"))
        return tuple(float(match.group(i)) for i in (1, 2, 3)) if match else None
    # Traces: glretrace's own timing includes window and context setup, so
    # frame times come from WineMetalGL's per-frame statistics (presents
    # after the first three frames). Apple's OpenGL has no such statistics.
    if impl == "apple":
        return None
    retrace = os.path.join(root, entry.get("retrace", "tools/apitrace/bin/glretrace.exe"))
    command = [wine, retrace, "--benchmark"] + entry.get("retrace_args", []) + [os.path.join(root, entry["trace"])]
    stats_env = impl_env(env, impl)
    stats_env["WINEMETALGL_STATS"] = "1"
    result = subprocess.run(command, cwd=os.path.dirname(retrace), env=stats_env, capture_output=True,
                            timeout=entry.get("timeout", 600))
    frames = [float(m.group(2)) for m in STATS_FRAME.finditer(result.stderr.decode("latin1")) if int(m.group(1)) >= 3]
    if not frames:
        return None
    ordered = sorted(frames)
    return (sum(frames) / len(frames), ordered[(len(ordered) - 1) * 95 // 100], ordered[-1])


def measure(entry, impl, args, env):
    runs = [r for r in (run_once(entry, impl, args, env) for _ in range(args.runs)) if r]
    if not runs:
        return None
    result = {"avg_ms": round(statistics.median(r[0] for r in runs), 3), "runs": len(runs)}
    if runs[0][1] is not None:
        result["p95_ms"] = round(statistics.median(r[1] for r in runs), 3)
        # A hitch repeats in every run; a one-off stall of the host in one run is not one.
        result["max_ms"] = round(statistics.median(r[2] for r in runs), 3)
        result["worst_max_ms"] = round(max(r[2] for r in runs), 3)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--wine", required=True)
    parser.add_argument("--corpus-root", required=True)
    parser.add_argument("--manifest", default=os.path.join(ROOT, "tests", "games", "corpus.json"))
    parser.add_argument("--template", help="template WoW64 prefix to clone")
    parser.add_argument("--only", action="append")
    parser.add_argument("--runs", type=int, default=3)
    parser.add_argument("--frames", type=int, default=600)
    parser.add_argument("--out", default=os.path.join(ROOT, "tests", "games", "perf.json"))
    parser.add_argument("--baseline", help="previous perf.json (default: --out before this run)")
    args = parser.parse_args()

    baseline_path = args.baseline or args.out
    baseline = {}
    if os.path.exists(baseline_path):
        for e in json.load(open(baseline_path)).get("entries", []):
            if e.get("new"):
                baseline[e["name"]] = e["new"]["avg_ms"]
    # With --only, the entries not measured keep their previous records.
    previous = []
    if args.only and os.path.exists(args.out):
        previous = [e for e in json.load(open(args.out)).get("entries", []) if e["name"] not in args.only]

    manifest = json.load(open(args.manifest))
    entries = [e for e in manifest["entries"] if not args.only or e["name"] in args.only]
    workdir = tempfile.mkdtemp(prefix="wmgl-bench-")
    prefix = os.path.join(workdir, "prefix")
    env = dict(os.environ)
    if args.template:
        subprocess.run(["cp", "-c", "-R", args.template, prefix], check=True)
    env.update({"WINEPREFIX": prefix, "WINEARCH": "wow64", "WINEDEBUG": env.get("WINEDEBUG", "-all")})
    env.setdefault("DYLD_LIBRARY_PATH", os.path.join(args.wine, "lib"))
    if not args.template:
        subprocess.run([os.path.join(args.wine, "bin", "wine"), "wineboot", "-u"], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)

    records, failures = [], 0
    for entry in entries:
        new = measure(entry, "new", args, env)
        # Apple's OpenGL is timed for demos only: traces are timed with
        # WineMetalGL's frame statistics, which Apple's OpenGL lacks.
        compare = entry["tier"] in ("A", "C") and entry.get("apple", True) and entry["kind"] == "demo"
        apple = measure(entry, "apple", args, env) if compare else None
        problems = []
        if compare and not apple:
            problems.append("no Apple OpenGL frame times")
        if not new:
            problems.append("no frame times")
        else:
            if new.get("max_ms") is not None and new["max_ms"] > HITCH_MS:
                problems.append("hitch %.1f ms" % new["max_ms"])
            if apple and new["avg_ms"] > RATIO_BUDGET * apple["avg_ms"]:
                problems.append("%.2fx Apple's OpenGL" % (new["avg_ms"] / apple["avg_ms"]))
            if entry["tier"] == "B" and new["avg_ms"] > TIER_B_FRAME_MS:
                problems.append("below 60 fps")
            if entry["name"] in baseline and new["avg_ms"] > REGRESSION * baseline[entry["name"]]:
                problems.append("%.0f%% slower than the baseline" % (100.0 * (new["avg_ms"] / baseline[entry["name"]] - 1.0)))
        status = "FAIL" if problems else "PASS"
        failures += status == "FAIL"
        ratio = round(new["avg_ms"] / apple["avg_ms"], 3) if new and apple else None
        records.append({"name": entry["name"], "tier": entry["tier"], "status": status, "new": new, "apple": apple,
                        "ratio": ratio, "problems": problems,
                        "apple_note": None if compare or entry["tier"] == "B" else
                        "not timed: traces have no frame statistics under Apple's OpenGL"})
        print("%s %-22s new %s apple %s%s" % (
            status, entry["name"], "%.3f ms" % new["avg_ms"] if new else "-",
            "%.3f ms" % apple["avg_ms"] if apple else "-", ("  " + "; ".join(problems)) if problems else ""))
        sys.stdout.flush()

    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    order = {e["name"]: i for i, e in enumerate(manifest["entries"])}
    record = {"commit": commit, "date": datetime.date.today().isoformat(), "runs": args.runs, "frames": args.frames,
              "budget": {"ratio": RATIO_BUDGET, "tier_b_frame_ms": round(TIER_B_FRAME_MS, 3), "hitch_ms": HITCH_MS,
                         "regression": REGRESSION},
              "entries": sorted(previous + records, key=lambda e: order.get(e["name"], len(order)))}
    with open(args.out, "w") as handle:
        json.dump(record, handle, indent=1)
        handle.write("\n")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
