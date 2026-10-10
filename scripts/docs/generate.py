#!/usr/bin/env python3
"""Generates WineMetalGL's published claims from committed evidence.

    scripts/docs/generate.py [--check] [--check-provenance]

Inputs (all tracked):
  tests/cts/baseline/gl3M-cfgN-*.txt   KHR-GL30 to KHR-GL33 results (scripts/cts/run.py)
  tests/cts/notsupported.json          reason for every NotSupported case
  tests/cts/known-failures.json        reasons for failing cases the core does not fix
  tests/games/corpus.json              game corpus manifest
  tests/games/results.json             last corpus run (scripts/traces/replay.py --json)
  tests/games/perf.json                frame times (scripts/traces/benchmark.py)
  third_party/khronos/gl.xml           the GL 3.3 core command list
  src/core                             advertised extensions, implemented commands

Outputs:
  docs/conformance.md, docs/games.md, docs/api-coverage.json, and the
  section of README.md between the GENERATED markers.

--check exits non-zero if any output differs from what is committed;
--check-provenance if any evidence was recorded from uncommitted changes.
"""

import argparse
import fnmatch
import glob
import json
import math
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
# Mustpass lists with committed baselines (tests/cts/baseline/<list>-cfgN-<runner>.txt).
SUITES = [("gl30", "KHR-GL30"), ("gl31", "KHR-GL31"), ("gl32", "KHR-GL32"), ("gl33", "KHR-GL33")]
# mustpass.xml configurations that are run. Configurations 5 and 6 repeat the command
# lines of 1 and 2: their only difference, useForFirstEGLConfig, applies to EGL.
CONFIGS = (1, 2, 3, 4)
CONFIGS_NOTE = ("configurations 1 to 4 of `mustpass.xml` (5 and 6 repeat the command lines of 1 and 2; their only "
                "difference, `useForFirstEGLConfig`, applies to EGL)")
# README rows: (version, what the version adds, mustpass list that measures it). Versions
# without a list are either the compatibility profile's (no public Khronos suite) or above
# what contexts offer.
VERSIONS = [
    ("1.0", "Immediate mode, display lists, fixed-function lighting, fog and texturing", None),
    ("1.1", "Vertex arrays, texture objects, polygon offset", None),
    ("1.2", "3D textures, BGRA and packed pixels, glDrawRangeElements", None),
    ("1.3", "Multitexture, cube maps, compressed textures, multisample, texture combiners", None),
    ("1.4", "Depth textures and shadow compare, window position, point parameters, mipmap generation", None),
    ("1.5", "Buffer objects, occlusion queries", None),
    ("2.0", "GLSL 1.10, multiple draw buffers, point sprites, non-power-of-two textures", None),
    ("2.1", "GLSL 1.20, pixel buffer objects, sRGB textures", None),
    ("3.0", "GLSL 1.30, framebuffer objects, vertex array objects, transform feedback, integer textures", "gl30"),
    ("3.1", "Instancing, texture buffers, uniform buffers, primitive restart", "gl31"),
    ("3.2", "Profiles, geometry shaders, sync objects, multisample textures, base vertex", "gl32"),
    ("3.3", "GLSL 3.30, sampler objects, timer queries, instanced arrays, dual-source blending", "gl33"),
    ("4.0", "Tessellation, GLSL 4.00, double precision, indirect draws, per-sample shading", None),
    ("4.1", "Separate shader objects, program binaries, viewport arrays, 64-bit attributes", None),
    ("4.2", "Image load/store, atomic counters, immutable texture storage", None),
    ("4.3", "Compute shaders, shader storage buffers, debug output, texture views", None),
    ("4.4", "Buffer storage, multi-bind, texture clears, enhanced layouts", None),
    ("4.5", "Direct state access, clip control, robustness, cull distances", None),
    ("4.6", "SPIR-V shaders, anisotropic filtering, indirect draw counts", None),
]
# Compatibility-profile versions with features listed as not implemented in README.md.
COMPAT_GAPS = {"1.0", "1.4"}
# Extensions each 4.x version promoted to core (the version's "new features" appendix).
PROMOTED = {
    "4.0": ["GL_ARB_draw_buffers_blend", "GL_ARB_draw_indirect", "GL_ARB_gpu_shader5", "GL_ARB_gpu_shader_fp64",
            "GL_ARB_sample_shading", "GL_ARB_shader_subroutine", "GL_ARB_tessellation_shader",
            "GL_ARB_texture_buffer_object_rgb32", "GL_ARB_texture_cube_map_array", "GL_ARB_texture_gather",
            "GL_ARB_texture_query_lod", "GL_ARB_transform_feedback2", "GL_ARB_transform_feedback3"],
    "4.1": ["GL_ARB_ES2_compatibility", "GL_ARB_get_program_binary", "GL_ARB_separate_shader_objects",
            "GL_ARB_shader_precision", "GL_ARB_vertex_attrib_64bit", "GL_ARB_viewport_array"],
    "4.2": ["GL_ARB_base_instance", "GL_ARB_compressed_texture_pixel_storage", "GL_ARB_conservative_depth",
            "GL_ARB_internalformat_query", "GL_ARB_map_buffer_alignment", "GL_ARB_shader_atomic_counters",
            "GL_ARB_shader_image_load_store", "GL_ARB_shading_language_420pack", "GL_ARB_shading_language_packing",
            "GL_ARB_texture_storage", "GL_ARB_transform_feedback_instanced"],
    "4.3": ["GL_ARB_arrays_of_arrays", "GL_ARB_clear_buffer_object", "GL_ARB_compute_shader", "GL_ARB_copy_image",
            "GL_ARB_ES3_compatibility", "GL_ARB_explicit_uniform_location", "GL_ARB_fragment_layer_viewport",
            "GL_ARB_framebuffer_no_attachments", "GL_ARB_internalformat_query2", "GL_ARB_invalidate_subdata",
            "GL_ARB_multi_draw_indirect", "GL_ARB_program_interface_query", "GL_ARB_robust_buffer_access_behavior",
            "GL_ARB_shader_image_size", "GL_ARB_shader_storage_buffer_object", "GL_ARB_stencil_texturing",
            "GL_ARB_texture_buffer_range", "GL_ARB_texture_query_levels", "GL_ARB_texture_storage_multisample",
            "GL_ARB_texture_view", "GL_ARB_vertex_attrib_binding", "GL_KHR_debug"],
    "4.4": ["GL_ARB_buffer_storage", "GL_ARB_clear_texture", "GL_ARB_enhanced_layouts", "GL_ARB_multi_bind",
            "GL_ARB_query_buffer_object", "GL_ARB_texture_mirror_clamp_to_edge", "GL_ARB_texture_stencil8",
            "GL_ARB_vertex_type_10f_11f_11f_rev"],
    "4.5": ["GL_ARB_clip_control", "GL_ARB_conditional_render_inverted", "GL_ARB_cull_distance",
            "GL_ARB_derivative_control", "GL_ARB_direct_state_access", "GL_ARB_ES3_1_compatibility",
            "GL_ARB_get_texture_sub_image", "GL_ARB_shader_texture_image_samples", "GL_ARB_texture_barrier",
            "GL_KHR_context_flush_control", "GL_KHR_robustness"],
    "4.6": ["GL_ARB_gl_spirv", "GL_ARB_indirect_parameters", "GL_ARB_pipeline_statistics_query",
            "GL_ARB_polygon_offset_clamp", "GL_ARB_shader_atomic_counter_ops", "GL_ARB_shader_draw_parameters",
            "GL_ARB_shader_group_vote", "GL_ARB_spirv_extensions", "GL_ARB_texture_filter_anisotropic",
            "GL_ARB_transform_feedback_overflow_query", "GL_KHR_no_error"],
}


def rel(*parts):
    return os.path.join(ROOT, *parts)


def load_baselines(suite):
    """{(config, runner): (header dict, results)} for one mustpass list's baselines."""
    out = {}
    for path in sorted(glob.glob(rel("tests", "cts", "baseline", "%s-cfg*-*.txt" % suite))):
        name = os.path.basename(path)[:-4]
        m = re.match(re.escape(suite) + r"-cfg(\d)-(.+)$", name)
        if not m or m.group(2) == "oldcore":
            continue
        header, results = cts.load(path)
        meta = {"file": os.path.relpath(path, ROOT), "config": int(m.group(1)), "runner": m.group(2)}
        if meta["config"] not in CONFIGS:
            raise SystemExit("%s: configuration %d is not run (%s)" % (meta["file"], meta["config"], CONFIGS_NOTE))
        for line in header:
            if line.startswith("# mustpass"):
                meta["command_line"] = line.split(":", 1)[1].strip()
            elif line.startswith("# commit"):
                meta["commit"] = line.split(",")[0].replace("# commit", "").strip()
            elif line.startswith("# cts"):
                meta["cts"] = line.replace("# cts", "").strip()
        out[(meta["config"], meta["runner"])] = (meta, results)
    return out


def totals(results):
    counts = {s: 0 for s in STATUSES}
    for status in results.values():
        key = "Pass" if status in cts.PASSING else status
        counts[key] = counts.get(key, 0) + 1
    return counts


def applicable(results):
    """(passing, applicable) for one run: applicable cases are those not NotSupported."""
    t = totals(results)
    return t["Pass"], len(results) - t["NotSupported"]


def percent(passed, total):
    """Rounded down, so a run with one failure never shows as 100%."""
    if passed == total:
        return "100%"
    return "%.2f%%" % (math.floor(10000.0 * passed / total) / 100)


def classify_not_supported(suites):
    audit = json.load(open(rel("tests", "cts", "notsupported.json")))
    entries = audit["entries"]
    per_entry = {e["pattern"]: set() for e in entries}
    unexplained = []
    for suite, baselines in suites.items():
        for (config, runner), (_, results) in baselines.items():
            for case, status in results.items():
                if status != "NotSupported":
                    continue
                match = next((e for e in entries if fnmatch.fnmatchcase(case, e["pattern"])), None)
                if match:
                    per_entry[match["pattern"]].add(case)
                else:
                    unexplained.append("%s (%s config %d, %s)" % (case, suite, config, runner))
    return entries, per_entry, unexplained


def classify_failures(suites):
    """{suite: {case: {"runs": [(config, runner, status)], "reason": str or None}}} for every
    result that neither passes nor is NotSupported, with its tests/cts/known-failures.json reason."""
    known = json.load(open(rel("tests", "cts", "known-failures.json")))["entries"]
    used = set()
    out = {}
    for suite, baselines in suites.items():
        for (config, runner), (_, results) in sorted(baselines.items()):
            for case, status in sorted(results.items()):
                if status in cts.PASSING or status == "NotSupported":
                    continue
                entry = out.setdefault(suite, {}).setdefault(case, {"runs": [], "reason": None})
                entry["runs"].append((config, runner, status))
                match = next((e for e in known if fnmatch.fnmatchcase(case, e["pattern"])), None)
                if match:
                    entry["reason"] = match["reason"]
                    used.add(match["pattern"])
    stale = [e["pattern"] for e in known if e["pattern"] not in used]
    if stale:
        raise SystemExit("tests/cts/known-failures.json entries that match no failing result:\n  " + "\n  ".join(stale))
    return out


def describe_runs(runs):
    """'configurations 3, 4 under Wine and natively' style summary of failing runs."""
    by_runner = {}
    for config, runner, status in runs:
        by_runner.setdefault(runner, []).append((config, status))
    parts = []
    for runner in sorted(by_runner):
        items = by_runner[runner]
        statuses = sorted({s for _, s in items})
        parts.append("%s %s in configuration%s %s" % (
            "/".join(statuses), "under Wine" if runner == "wine" else "natively", "s" if len(items) > 1 else "",
            ", ".join(str(c) for c, _ in items)))
    return "; ".join(parts)


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
    double_v = re.compile(r"^WMGL_UNIFORM_DOUBLE_V\((\w+)\)", re.M)
    double_matrix = re.compile(r"^WMGL_UNIFORM_DOUBLE_MATRIX\((\w+)\)", re.M)
    for path in glob.glob(rel("src", "core", "**", "*.mm"), recursive=True) + glob.glob(
            rel("src", "core", "**", "*.cpp"), recursive=True):
        text = open(path).read()
        names.update("gl" + m.group(1) for m in pattern.finditer(text))
        names.update("glProgramUniform" + m.group(1) for m in double_v.finditer(text))
        names.update("glProgramUniformMatrix" + m.group(1) + "dv" for m in double_matrix.finditer(text))
        for prefix in ("glUniform", "glProgramUniform"):
            names.update(prefix + m.group(1) for m in uniform_v.finditer(text))
            names.update(prefix + "Matrix" + m.group(1) + "fv" for m in uniform_matrix.finditer(text))
    return names


def extension_commands():
    """{extension: [commands]} and {command: alias target} from the Khronos registry."""
    root = ET.parse(rel("third_party", "khronos", "gl.xml")).getroot()
    aliases = {}
    for command in root.iter("command"):
        proto = command.find("proto")
        alias = command.find("alias")
        if proto is not None and alias is not None:
            aliases[proto.findtext("name")] = alias.get("name")
    extensions = {}
    for ext in root.iter("extension"):
        if "gl" not in (ext.get("supported") or "").split("|") and "glcore" not in (ext.get("supported") or "").split("|"):
            continue
        commands = []
        for require in ext.iter("require"):
            if require.get("api") not in (None, "gl"):
                continue
            # Blocks marked "Supported only if GL_X is supported" count only when X is advertised.
            condition = re.search(r"only if (GL_\w+)", require.get("comment") or "")
            commands += [(c.get("name"), condition.group(1) if condition else None) for c in require.iter("command")]
        extensions[ext.get("name")] = commands
    return extensions, aliases


def advertised_extensions():
    text = open(rel("src", "core", "context.mm")).read()
    start = text.index("void Context::initStrings()")
    body = text[start:text.index("\n}\n", start)]
    body = "\n".join(line.split("//")[0] for line in body.splitlines())  # ignore comments
    return re.findall(r'"((?:GL|WGL)_\w+)"', body)


def games():
    manifest = json.load(open(rel("tests", "games", "corpus.json")))
    path = rel("tests", "games", "results.json")
    record = json.load(open(path)) if os.path.exists(path) else {"entries": []}
    by_name = {e["name"]: e for e in record["entries"]}
    return manifest["entries"], by_name, record


def render(suites):
    entries, per_entry, unexplained = classify_not_supported(suites)
    if unexplained:
        raise SystemExit("NotSupported cases without a reason in tests/cts/notsupported.json:\n  " +
                         "\n  ".join(sorted(unexplained)[:50]))
    failures = classify_failures(suites)
    old = cts.load(rel("tests", "cts", "baseline", "gl33-cfg1-oldcore.txt"))[1]
    core = core_commands()
    implemented = implemented_commands()
    missing = [c for c in core if c not in implemented]
    extensions = advertised_extensions()
    ext_commands, aliases = extension_commands()
    incomplete = {}
    for ext in extensions:
        if ext.startswith("WGL_"):
            continue
        absent = [c for c, condition in ext_commands.get(ext, [])
                  if (condition is None or condition in extensions) and c not in implemented and aliases.get(c) not in implemented]
        if ext not in ext_commands:
            absent = ["(not in gl.xml)"]
        if absent:
            incomplete[ext] = absent
    if incomplete:
        raise SystemExit("advertised extensions with unimplemented commands:\n  " +
                         "\n  ".join("%s: %s" % (k, ", ".join(v)) for k, v in sorted(incomplete.items())))
    game_entries, game_results, game_record = games()

    # ---- conformance.md
    md = ["# WineMetalGL conformance", "",
          "Generated by `scripts/docs/generate.py` from the committed result files; do not edit.", "",
          "Khronos CTS mustpass lists (`glNN-main.txt`) in %s. " % CONFIGS_NOTE +
          
          "`native` runs the core directly through its driver ABI (`tests/cts/native`); `wine` runs "
          "`glcts.exe` in a WoW64 prefix through Wine's `opengl32`. Applicable cases are those that are not "
          "NotSupported.", ""]
    for suite, package in SUITES:
        baselines = suites[suite]
        if not baselines:
            continue
        md += ["## %s" % package, "",
               "| Config | Runner | Pass | Fail | NotSupported | InternalError | Crash | Timeout | Missing | Other | Cases | Applicable passing | Commit | File |",
               "|---|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---|---|"]
        for (config, runner) in sorted(baselines):
            meta, results = baselines[(config, runner)]
            t = totals(results)
            other = sum(n for status, n in t.items() if status not in STATUSES)
            md.append("| %d | %s | %d | %d | %d | %d | %d | %d | %d | %d | %d | %s | %s | `%s` |" % (
                config, runner, t["Pass"], t["Fail"], t["NotSupported"], t["InternalError"], t["Crash"], t["Timeout"],
                t["Missing"], other, len(results), percent(*applicable(results)), meta.get("commit", "?"), meta["file"]))
        revisions = sorted({meta.get("cts", "not recorded") for meta, _ in baselines.values()})
        md += ["", "Command lines: `scripts/cts/run.py --mustpass %s-main.txt --config N` reads them from "
               "`mustpass.xml`. CTS build: %s." % (suite, ", ".join(revisions))]
        failing = failures.get(suite, {})
        if failing:
            md += ["", "Cases that neither pass nor are NotSupported:", "", "| Case | Runs | Reason |", "|---|---|---|"]
            for case, f in sorted(failing.items()):
                md.append("| `%s` | %s | %s |" % (case, describe_runs(f["runs"]),
                                                  f["reason"] or "Not yet explained."))
        if suite == "gl33" and (1, "native") in baselines:
            new = baselines[(1, "native")][1]
            gained = sum(1 for c, s in new.items() if s in cts.PASSING and old.get(c) not in cts.PASSING)
            lost = sum(1 for c, s in old.items() if s in cts.PASSING and new.get(c) not in cts.PASSING)
            md += ["", "Against the legacy implementation: `tests/cts/baseline/gl33-cfg1-oldcore.txt` is the last "
                   "result of the legacy (removed) implementation, configuration 1: %d Pass. The core passes %d "
                   "cases it failed and fails %d cases it passed." % (totals(old)["Pass"], gained, lost)]
        md.append("")
    md += ["## NotSupported", "",
           "Every NotSupported case matches an entry of `tests/cts/notsupported.json`:", "",
           "| Cases | Kind | Pattern | Reason |", "|---:|---|---|---|"]
    for e in entries:
        md.append("| %d | %s | `%s` | %s |" % (len(per_entry[e["pattern"]]), e["kind"], e["pattern"], e["reason"]))
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
    # Frame times (W24), from scripts/traces/benchmark.py.
    perf_path = rel("tests", "games", "perf.json")
    perf = json.load(open(perf_path)) if os.path.exists(perf_path) else None
    if perf:
        budget = perf.get("budget", {})
        g += ["", "## Frame times", "",
              "Recorded by `scripts/traces/benchmark.py` in `tests/games/perf.json` (commit %s, %s; median of %s runs "
              "of %s frames, vsync off, after a warm-up). Budgets: at most %.1fx Apple's OpenGL where it runs the "
              "entry, at least 60 fps for Tier B, no frame over %.0f ms, no more than %.0f%% slower than the "
              "previous baseline." % (perf.get("commit", "?"), perf.get("date", "?"), perf.get("runs", "?"),
                                      perf.get("frames", "?"), budget.get("ratio", 1.2), budget.get("hitch_ms", 50),
                                      100 * (budget.get("regression", 1.1) - 1)), "",
              "| Entry | Tier | Budget | WineMetalGL avg / p95 / max (ms) | Apple's OpenGL avg (ms) | Ratio |",
              "|---|---|---|---|---:|---:|"]
        for p in perf.get("entries", []):
            new = p.get("new") or {}
            times = " / ".join("%.3f" % new[k] if new.get(k) is not None else "-" for k in ("avg_ms", "p95_ms", "max_ms"))
            g.append("| %s | %s | %s | %s | %s | %s |" % (
                p["name"], p["tier"], p["status"] + (" (" + "; ".join(p["problems"]) + ")" if p.get("problems") else ""),
                times, "%.3f" % p["apple"]["avg_ms"] if p.get("apple") else "-",
                "%.2fx" % p["ratio"] if p.get("ratio") else "-"))
        untimed = [p for p in perf.get("entries", []) if p.get("apple_note")]
        if untimed:
            g += ["", "Without an Apple's OpenGL time: %s." % "; ".join(
                "`%s` (%s)" % (p["name"], p["apple_note"]) for p in untimed)]
    games_md = "\n".join(g) + "\n"

    # ---- README status
    lines = [BEGIN, "## Status", "",
             "| OpenGL | Adds | Result | Evidence |", "|---|---|---|---|"]
    commits = sorted({meta.get("commit", "?") for b in suites.values() for meta, _ in b.values()})
    for version, adds, suite in VERSIONS:
        baselines = suites.get(suite) if suite else None
        if baselines:
            package = dict(SUITES)[suite]
            runs = sorted(baselines)
            rates = {k: applicable(baselines[k][1]) for k in runs}
            worst = min(runs, key=lambda k: (rates[k][0] / max(rates[k][1], 1), k))
            passed, total = rates[worst]
            cases = len(baselines[worst][1])
            if len(set(rates.values())) == 1:
                evidence = "`%s`: %s of %s applicable cases (%s cases) in each of %d runs" % (
                    package, "{:,}".format(passed), "{:,}".format(total), "{:,}".format(cases), len(runs))
            else:
                evidence = "`%s`: %s of %s applicable cases (%s cases) in the worst of %d runs (configuration %d, %s)" % (
                    package, "{:,}".format(passed), "{:,}".format(total), "{:,}".format(cases), len(runs), worst[0],
                    "Wine" if worst[1] == "wine" else "native")
            result = percent(passed, total)
        elif version < "3.0":
            result = "Compatibility profile" + (", with the gaps listed under "
                                                "[Compatibility profile](#compatibility-profile)" if version in COMPAT_GAPS else "")
            evidence = "No public Khronos CTS; Wine fixtures, GPU tests and the game corpus"
        elif version > "3.3":
            available = [e for e in PROMOTED[version] if e in extensions]
            result = "Not supported"
            evidence = "Contexts stop at 3.3. " + (
                "Of the %d extensions %s promoted, advertised: %s" % (
                    len(PROMOTED[version]), version, ", ".join("`%s`" % e for e in available))
                if available else "None of the %d extensions %s promoted is advertised" % (len(PROMOTED[version]), version))
        else:
            result, evidence = "Not measured", "No committed `%s` baseline" % dict(SUITES).get(suite, suite)
        lines.append("| %s | %s | %s | %s |" % (version, adds, result, evidence))
    lines += ["",
              "Percentages are passing cases over applicable cases (those that are not NotSupported) in the worst run. "
              "Each mustpass list runs in " + CONFIGS_NOTE + ", under Wine (`glcts.exe` through Wine's "
              "`opengl32`) and natively (the core's driver ABI), at commit %s. NotSupported cases are unadvertised "
              "extensions, newer GL versions and cases the test marks invalid; [`docs/conformance.md`]"
              "(docs/conformance.md) lists each with its reason, and the per-run numbers." % ", ".join(commits)]
    failing = [(case, f) for suite, _ in SUITES for case, f in sorted(failures.get(suite, {}).items())]
    if failing:
        lines += ["", "Cases that fail in some run:", ""]
        lines += ["- `%s` (%s): %s" % (case, describe_runs(f["runs"]), f["reason"] or "not yet explained.")
                  for case, f in failing]
    passed = [e for e in game_entries if game_results.get(e["name"], {}).get("status") == "PASS"]
    lines += ["", "Game corpus: %d of %d entries match their reference frames ([`docs/games.md`](docs/games.md))."
              % (len(passed), len(game_entries))]
    if perf:
        within = [p for p in perf.get("entries", []) if p["status"] == "PASS"]
        lines.append("Frame-time budgets met by %d of %d entries." % (len(within), len(perf.get("entries", []))))
    lines += ["",
              "GL 3.3 core commands with an implementation: %d of %d (their behaviour is what the CTS results "
              "above measure)." % (len(core) - len(missing), len(core)), END]
    status = "\n".join(lines)
    return {"docs/conformance.md": conformance, "docs/api-coverage.json": coverage, "docs/games.md": games_md}, status


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--check-provenance", action="store_true",
                        help="fail if any evidence file was recorded from a tree with uncommitted changes")
    args = parser.parse_args()
    suites = {suite: load_baselines(suite) for suite, _ in SUITES}
    if args.check_provenance:
        # Evidence recorded from a tree with uncommitted changes does not describe a commit.
        recorded = [(meta["file"], meta.get("commit", "")) for b in suites.values() for meta, _ in b.values()]
        for name in ("results.json", "perf.json"):
            path = rel("tests", "games", name)
            if os.path.exists(path):
                record = json.load(open(path))
                recorded.append((os.path.relpath(path, ROOT), record.get("commit", "")))
                recorded += [("%s: %s" % (os.path.relpath(path, ROOT), e["name"]), e["commit"])
                             for e in record.get("entries", []) if "commit" in e]
        dirty = ["%s (%s)" % (f, c) for f, c in recorded if c.endswith("-dirty") or "uncommitted" in c]
        if dirty:
            raise SystemExit("evidence recorded from uncommitted trees:\n  " + "\n  ".join(dirty))
    outputs, status = render(suites)
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
