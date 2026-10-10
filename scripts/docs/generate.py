#!/usr/bin/env python3
"""Generates WineMetalGL's published claims from committed evidence.

    scripts/docs/generate.py [--check]

Inputs (all tracked):
  tests/cts/baseline/gl33-cfgN-*.txt   KHR-GL33 results (scripts/cts/run.py)
  tests/cts/notsupported.json          reason for every NotSupported case
  tests/games/corpus.json              game corpus manifest
  tests/games/results.json             last corpus run (scripts/traces/replay.py --json)
  third_party/khronos/gl.xml           the GL 3.3 core command list
  src/core                             advertised extensions, implemented commands

Outputs:
  docs/conformance.md, docs/games.md, docs/api-coverage.json, and the
  section of README.md between the GENERATED markers.

--check exits non-zero if any output differs from what is committed.
"""

import argparse
import fnmatch
import glob
import json
import os
import re
import sys
import xml.etree.ElementTree as ET

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, "scripts", "cts"))
import results as cts  # noqa: E402

BEGIN = "<!-- BEGIN GENERATED: status (scripts/docs/generate.py) -->"
END = "<!-- END GENERATED: status -->"
STATUSES = ["Pass", "Fail", "NotSupported", "InternalError", "Crash", "Timeout", "Missing"]


def rel(*parts):
    return os.path.join(ROOT, *parts)


def load_baselines():
    """{(config, path): (header dict, results)} for the new core's baselines."""
    out = {}
    for path in sorted(glob.glob(rel("tests", "cts", "baseline", "gl33-cfg*-*.txt"))):
        name = os.path.basename(path)[:-4]
        m = re.match(r"gl33-cfg(\d)-(.+)$", name)
        if not m or m.group(2) == "oldcore":
            continue
        header, results = cts.load(path)
        meta = {"file": os.path.relpath(path, ROOT), "config": int(m.group(1)), "runner": m.group(2)}
        for line in header:
            if line.startswith("# mustpass"):
                meta["command_line"] = line.split(":", 1)[1].strip()
            elif line.startswith("# commit"):
                meta["commit"] = line.split(",")[0].replace("# commit", "").strip()
        out[(meta["config"], meta["runner"])] = (meta, results)
    return out


def totals(results):
    counts = {s: 0 for s in STATUSES}
    for status in results.values():
        counts["Pass" if status in cts.PASSING else status] = counts.get(status, 0) + 1
    return counts


def classify_not_supported(baselines):
    audit = json.load(open(rel("tests", "cts", "notsupported.json")))
    entries = audit["entries"]
    per_entry = {e["pattern"]: set() for e in entries}
    unexplained = []
    for (config, runner), (_, results) in baselines.items():
        for case, status in results.items():
            if status != "NotSupported":
                continue
            match = next((e for e in entries if fnmatch.fnmatchcase(case, e["pattern"])), None)
            if match:
                per_entry[match["pattern"]].add(case)
            else:
                unexplained.append("%s (config %d, %s)" % (case, config, runner))
    return entries, per_entry, unexplained


def core_commands():
    """GL 3.3 core profile commands from the Khronos registry."""
    tree = ET.parse(rel("third_party", "khronos", "gl.xml"))
    commands, removed = set(), set()
    for feature in tree.getroot().iter("feature"):
        if feature.get("api") != "gl" or float(feature.get("number")) > 3.3:
            continue
        for require in feature.iter("require"):
            if require.get("profile") in (None, "core"):
                commands.update(c.get("name") for c in require.iter("command"))
        for remove in feature.iter("remove"):
            if remove.get("profile") == "core":
                removed.update(c.get("name") for c in remove.iter("command"))
    return sorted(commands - removed)


def implemented_commands():
    """Commands with an implementation in src/core (the generated weak stubs aside)."""
    names = set()
    pattern = re.compile(r"^[A-Za-z_][\w:<>\*\s&]*?\b([A-Z][A-Za-z0-9_]*)\(Context& ctx", re.M)
    # Families defined through macros in api/program.mm.
    uniform_v = re.compile(r"^WMGL_UNIFORM_V\((\w+),", re.M)
    uniform_matrix = re.compile(r"^WMGL_UNIFORM_MATRIX\((\w+),", re.M)
    for path in glob.glob(rel("src", "core", "**", "*.mm"), recursive=True) + glob.glob(
            rel("src", "core", "**", "*.cpp"), recursive=True):
        text = open(path).read()
        names.update("gl" + m.group(1) for m in pattern.finditer(text))
        names.update("glUniform" + m.group(1) for m in uniform_v.finditer(text))
        names.update("glUniformMatrix" + m.group(1) + "fv" for m in uniform_matrix.finditer(text))
    return names


def advertised_extensions():
    text = open(rel("src", "core", "context.mm")).read()
    return re.findall(r'extensions_\.push_back\("([^"]+)"\)', text)


def games():
    manifest = json.load(open(rel("tests", "games", "corpus.json")))
    path = rel("tests", "games", "results.json")
    record = json.load(open(path)) if os.path.exists(path) else {"entries": []}
    by_name = {e["name"]: e for e in record["entries"]}
    return manifest["entries"], by_name, record


def render(baselines):
    entries, per_entry, unexplained = classify_not_supported(baselines)
    if unexplained:
        raise SystemExit("NotSupported cases without a reason in tests/cts/notsupported.json:\n  " +
                         "\n  ".join(sorted(unexplained)[:50]))
    old = cts.load(rel("tests", "cts", "baseline", "gl33-cfg1-oldcore.txt"))[1]
    core = core_commands()
    implemented = implemented_commands()
    missing = [c for c in core if c not in implemented]
    extensions = advertised_extensions()
    game_entries, game_results, game_record = games()

    # ---- conformance.md
    md = ["# WineMetalGL conformance", "",
          "Generated by `scripts/docs/generate.py` from the committed result files; do not edit.", "",
          "## KHR-GL33", "",
          "`KHR-GL33` mustpass (`gl33-main.txt`) under each `mustpass.xml` configuration. "
          "`native` runs the core directly through its driver ABI (`tests/cts/native`); `wine` runs "
          "`glcts.exe` in a WoW64 prefix through Wine's `opengl32`.", "",
          "| Config | Runner | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Cases | Commit | File |",
          "|---|---|---:|---:|---:|---:|---:|---:|---:|---|---|"]
    for (config, runner) in sorted(baselines):
        meta, results = baselines[(config, runner)]
        t = totals(results)
        md.append("| %d | %s | %d | %d | %d | %d | %d | %d | %d | %s | `%s` |" % (
            config, runner, t["Pass"], t["Fail"], t["NotSupported"], t["InternalError"], t["Crash"], t["Timeout"],
            len(results), meta.get("commit", "?"), meta["file"]))
    md += ["", "Command lines: `scripts/cts/run.py --config N` reads them from `mustpass.xml`.", "",
           "### NotSupported", "",
           "Every NotSupported case matches an entry of `tests/cts/notsupported.json`:", "",
           "| Cases | Kind | Pattern | Reason |", "|---:|---|---|---|"]
    for e in entries:
        md.append("| %d | %s | `%s` | %s |" % (len(per_entry[e["pattern"]]), e["kind"], e["pattern"], e["reason"]))
    if (1, "native") in baselines:
        new = baselines[(1, "native")][1]
        gained = sum(1 for c, s in new.items() if s in cts.PASSING and old.get(c) not in cts.PASSING)
        lost = sum(1 for c, s in old.items() if s in cts.PASSING and new.get(c) not in cts.PASSING)
        md += ["", "### Against the legacy implementation", "",
               "`tests/cts/baseline/gl33-cfg1-oldcore.txt` is the last result of the legacy (removed) "
               "implementation, configuration 1: %d Pass. The core passes %d cases it failed and fails %d "
               "cases it passed." % (totals(old)["Pass"], gained, lost)]
    md += ["", "## API", "",
           "GL 3.3 core profile commands in the Khronos registry: %d. Implemented: %d. Missing: %s." % (
               len(core), len(core) - len(missing), ", ".join("`%s`" % m for m in missing) if missing else "none"),
           "", "Advertised extensions: %s." % ", ".join("`%s`" % e for e in extensions), "",
           "The machine-readable form is `docs/api-coverage.json`.", ""]
    conformance = "\n".join(md)

    coverage = json.dumps({
        "project": "WineMetalGL",
        "generated_by": "scripts/docs/generate.py",
        "context": {"versions": "up to 3.3", "profiles": ["core", "compatibility"]},
        "host": {"os": "macOS", "minimum": "15.0", "architecture": "x86_64"},
        "gl33_core_commands": len(core),
        "implemented": len(core) - len(missing),
        "missing": missing,
        "extensions": extensions,
    }, indent=1) + "\n"

    # ---- games.md
    g = ["# Game corpus", "",
         "Generated by `scripts/docs/generate.py` from `tests/games/corpus.json` and the last run recorded in "
         "`tests/games/results.json` (commit %s, %s); do not edit." % (
             game_record.get("commit", "?"), game_record.get("date", "?")), "",
         "Each entry renders frames that are compared with reference frames from Apple's OpenGL "
         "(`scripts/traces/replay.py`). See `tests/games/README.md` for the tiers.", "",
         "| Entry | Tier | Kind | Result | Differing pixels | Min PSNR | Notes |", "|---|---|---|---|---:|---:|---|"]
    for e in game_entries:
        r = game_results.get(e["name"])
        g.append("| %s | %s | %s | %s | %s | %s | %s |" % (
            e["name"], e.get("tier", ""), e["kind"], r["status"] if r else "not run",
            "%.4f%%" % (r["worst_fraction"] * 100) if r else "", "%.1f dB" % r["min_psnr_db"] if r else "",
            e.get("notes", "").replace("|", "/")))
    games_md = "\n".join(g) + "\n"

    # ---- README status
    lines = [BEGIN, "## Status", ""]
    if baselines:
        rows = sorted(baselines)
        worst_fail = max(totals(baselines[k][1])["Fail"] + totals(baselines[k][1])["InternalError"] +
                         totals(baselines[k][1])["Crash"] + totals(baselines[k][1])["Timeout"] for k in rows)
        configs = sorted({c for c, _ in rows})
        lines.append("`KHR-GL33` (9,886 cases) under mustpass configurations %s: %s Fail, InternalError, Crash or "
                     "Timeout in the worst run; NotSupported only for unadvertised extensions, newer GL versions "
                     "and cases the test marks invalid. Per-run numbers: [`docs/conformance.md`](docs/conformance.md)."
                     % (", ".join(str(c) for c in configs), worst_fail))
    passed = [e for e in game_entries if game_results.get(e["name"], {}).get("status") == "PASS"]
    lines += ["", "Game corpus: %d of %d entries match their reference frames ([`docs/games.md`](docs/games.md))."
              % (len(passed), len(game_entries)), "",
              "GL 3.3 core commands implemented: %d of %d." % (len(core) - len(missing), len(core)), END]
    status = "\n".join(lines)
    return {"docs/conformance.md": conformance, "docs/api-coverage.json": coverage, "docs/games.md": games_md}, status


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    outputs, status = render(load_baselines())
    readme = open(rel("README.md")).read()
    if BEGIN not in readme or END not in readme:
        raise SystemExit("README.md has no generated status section")
    outputs["README.md"] = readme[:readme.index(BEGIN)] + status + readme[readme.index(END) + len(END):]
    stale = []
    for path, text in outputs.items():
        full = rel(path)
        current = open(full).read() if os.path.exists(full) else None
        if current == text:
            continue
        if args.check:
            stale.append(path)
        else:
            with open(full, "w") as handle:
                handle.write(text)
            print("wrote %s" % path)
    if stale:
        raise SystemExit("generated files are out of date (run scripts/docs/generate.py): " + ", ".join(stale))


if __name__ == "__main__":
    main()
