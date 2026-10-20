#!/usr/bin/env python3
"""Runs the performance probe (tests/wine/opengl_perf_probe.c) under a Wine
installation and records, per scene, the probe's CPU frame times and the
core's WINEMETALGL_STATS counters. Not part of the gate.

  tests/wine/perf_probe.py run [--label NAME] [--core DYLIB] [--module DIR]
                               [--wine-runtime DIR] [--frames N] [--warmup N]
                               [--scenes a,b,...] [--out FILE.json]
  tests/wine/perf_probe.py compare BEFORE.json AFTER.json

`run` never changes the Wine installation it is given: it clones it (an APFS
copy-on-write clone, made once into the cache directory) and installs the Wine
module (--module, default build/wine-module) and the core (--core, default the
core the given installation already has) into the clone with
scripts/install-wine-module.sh. The prefix is made once in the cache too.
The default installation is MetalSharp's Wine, ~/.metalsharp/runtime/wine.

Each scene runs in its own process. Counters are averaged over the frames
after the warm-up ("steady"); pipeline and link counts are also summed over
every frame ("total"), since compile stutter happens before the steady state.
Runs use their own pipeline archive (WINEMETALGL_PIPELINE_CACHE), emptied
at the start of each run, and the shader cache starts empty for each core
installed (its files are per core file), and a new shader salt, so the probe's shaders
compile cold.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
CACHE = os.path.expanduser("~/Library/Caches/WineMetalGL/perf-probe")
DEFAULT_RUNTIME = os.path.expanduser("~/.metalsharp/runtime/wine")
SOURCE = os.path.join(ROOT, "tests/wine/opengl_perf_probe.c")

# Fields of the "winemetalgl:stats: frame ..." line (src/core/stats.mm endFrame).
FIELDS = [
    (r"frame (\d+) ([\d.]+) ms", ["index", "frame_ms"]),
    (r"\| gl ([\d.]+) ms in (\d+) calls", ["gl_ms", "calls"]),
    (r"\| present ([\d.]+) ms \(drawable ([\d.]+) ms\)", ["present_ms", "drawable_ms"]),
    (r"\| draws (\d+) passes (\d+)", ["draws", "passes"]),
    (r"\| pipelines (\d+) \(([\d.]+) ms\) archive (\d+) waited (\d+)", ["pipelines", "pipeline_ms", "archive_hits", "pipeline_waits"]),
    (r"\| links (\d+) \(([\d.]+) ms\)", ["links", "link_ms"]),
    (r"\| transient (\d+) KiB", ["transient_kib"]),
    (r"\| waits (\d+) \(([\d.]+) ms\)", ["waits", "wait_ms"]),
    (r"\| lock ([\d.]+) ms", ["lock_ms"]),
    (r"\| converted (\d+) vertices, (\d+) reused \(client (\d+) format (\d+)", ["converted", "converted_reused", "converted_client", "converted_format"]),
    (r"\| cpu indices (\d+)", ["cpu_index_draws"]),
    (r"\| uniforms (\d+) KiB", ["uniform_kib"]),
    (r"\| commits (\d+) \(query (\d+)\)", ["commits", "query_commits"]),
    (r"pass ends: target (\d+) blit (\d+) compute (\d+) visibility (\d+) commit (\d+) barrier (\d+) clear (\d+) other (\d+)",
     ["end_target", "end_blit", "end_compute", "end_visibility", "end_commit", "end_barrier", "end_clear", "end_other"]),
    (r"\| reads async (\d+) waiting (\d+)", ["reads_async", "reads_waiting"]),
    (r"\| client waits (\d+) \(signaled (\d+), ([\d.]+) ms\)", ["client_waits", "client_waits_signaled", "client_wait_ms"]),
    (r"\| gpu ([\d.]+) ms busy ([\d.]+) ms", ["gpu_ms", "gpu_busy_ms"]),
    (r"\| metal ([\d.]+) MiB", ["metal_mib"]),
    (r"draw cpu: samplers ([\d.]+) vertex ([\d.]+) prepare ([\d.]+) pipeline ([\d.]+) bind ([\d.]+) encode ([\d.]+) index ([\d.]+) ms",
     ["cpu_samplers_ms", "cpu_vertex_ms", "cpu_prepare_ms", "cpu_pipeline_ms", "cpu_bind_ms", "cpu_encode_ms", "cpu_index_ms"]),
]
FIELDS = [(re.compile(p), names) for p, names in FIELDS]
TOTALS = ["pipelines", "pipeline_ms", "links", "link_ms", "waits", "wait_ms"]

# What each scene's table row shows: probe values ("cpu_mean" ...) and
# steady-state counters. The first counter after the frame times is the one
# its fix targets.
HEADLINES = {
    "fence_poll": ["commits", "end_commit", "passes"],
    "clear_upload": ["end_clear", "passes", "end_blit"],
    "midpass_upload": ["end_blit", "passes"],
    "state_draws": ["gl_ms", "cpu_prepare_ms", "cpu_bind_ms"],
    "state_draws_debug": ["gl_ms", "calls"],
    "fixed_variants": ["total_pipelines", "total_links", "total_link_ms", "gl_ms"],
    "ubo_bind": ["cpu_bind_ms", "gl_ms"],
    "timer_queries": ["query_commits", "commits", "passes"],
    "flush": ["commits", "passes"],
    "buffer_subdata": ["gl_ms", "metal_mib", "transient_kib"],
    "map_invalidate": ["gl_ms", "waits", "metal_mib"],
    "invalidate_fb": ["gpu_busy_ms", "passes"],
    "index_update": ["waits", "wait_ms", "commits"],
    "client_arrays": ["converted_client", "converted", "gl_ms"],
    "display_lists": ["converted", "transient_kib", "gl_ms"],
    "readback_bgra": ["reads_waiting", "wait_ms", "reads_async"],
    "pbo_upload": ["wait_ms", "waits", "gl_ms"],
    "lod_bias": ["total_pipelines", "total_pipeline_ms", "pipelines"],
    "sso": ["pipelines", "total_pipelines", "gl_ms"],
    "auto_mipmap": ["commits", "end_blit", "gl_ms"],
    "texture_formats": ["gl_ms"],
    "fans": ["cpu_index_ms", "cpu_index_draws", "gl_ms"],
    "shader_link": ["link_ms", "binary_ms", "first_draw_ms"],
    "shader_link_warm": ["link_ms", "binary_ms", "first_draw_ms"],
    "msaa": ["passes", "gpu_busy_ms"],
    "engine": ["gl_ms", "gpu_busy_ms", "passes"],
}


def log(message):
    print(message, file=sys.stderr, flush=True)


def run_checked(cmd, env=None, **kw):
    result = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, **kw)
    if result.returncode != 0:
        sys.exit("failed (%d): %s\n%s" % (result.returncode, " ".join(cmd), result.stdout[-4000:]))
    return result.stdout


def wine_env(runtime, prefix, extra=None):
    env = dict(os.environ)
    env.update({"WINEPREFIX": prefix, "WINEDEBUG": "-all", "WINEARCH": "wow64"})
    env.pop("WINEMETALGL", None)
    lib = os.path.join(runtime, "lib")
    if os.path.isdir(lib):
        env["DYLD_LIBRARY_PATH"] = lib + (":" + env["DYLD_LIBRARY_PATH"] if env.get("DYLD_LIBRARY_PATH") else "")
    if extra:
        env.update(extra)
    return env


def prepare(args):
    """The cloned runtime and prefix, with the module and core installed."""
    source = os.path.realpath(args.wine_runtime)
    unix = os.path.join(source, "lib/wine/x86_64-unix")
    if not os.path.exists(os.path.join(source, "bin/wine")):
        sys.exit("not a Wine installation: %s" % source)
    os.makedirs(CACHE, exist_ok=True)
    clone = os.path.join(CACHE, "runtime")
    stamp = os.path.join(CACHE, "runtime.source")
    if not os.path.isdir(clone) or open(stamp).read() != source:
        shutil.rmtree(clone, ignore_errors=True)
        log("cloning %s into %s (copy-on-write)" % (source, clone))
        run_checked(["cp", "-cR", source, clone])
        open(stamp, "w").write(source)
        # The installation's own core, the default --core.
        original = os.path.join(unix, "libwinemetalgl.dylib")
        if os.path.exists(original):
            shutil.copy2(original, os.path.join(CACHE, "original-core.dylib"))
    core = args.core or os.path.join(CACHE, "original-core.dylib")
    module = args.module or os.path.join(ROOT, "build/wine-module")
    for path in (core, os.path.join(module, "x86_64-unix/winemetalgl.so")):
        if not os.path.exists(path):
            sys.exit("missing %s" % path)
    prefix = os.path.join(CACHE, "prefix")
    if not os.path.isdir(os.path.join(prefix, "drive_c")):
        log("creating the prefix %s" % prefix)
        # A stock Wine offers to install Wine Mono and Gecko on first boot.
        run_checked([os.path.join(clone, "bin/wine"), "wineboot", "-u"],
                    env=wine_env(clone, prefix, {"WINEDLLOVERRIDES": "mscoree,mshtml="}), timeout=600)
        run_checked([os.path.join(clone, "bin/wineserver"), "-w"], env=wine_env(clone, prefix), timeout=120)
    env = dict(os.environ, MODULE=module, CORE=os.path.realpath(core))
    run_checked([os.path.join(ROOT, "scripts/install-wine-module.sh"), clone, prefix], env=env)
    return clone, prefix, core, module


def build_probe(runtime, prefix):
    out_dir = os.path.join(prefix, "drive_c/perf")
    os.makedirs(out_dir, exist_ok=True)
    exe = os.path.join(out_dir, "opengl_perf_probe.exe")
    cc = os.environ.get("CC64", "x86_64-w64-mingw32-gcc")
    run_checked([cc, "-O2", "-Wall", "-I" + os.path.join(runtime, "include"), SOURCE, "-o", exe,
                 "-lopengl32", "-luser32", "-lgdi32", "-lm"])
    return exe


def parse_stats(path):
    frames = []
    if not os.path.exists(path):
        return frames
    with open(path, errors="replace") as f:
        for line in f:
            if "winemetalgl:stats: frame" not in line:
                continue
            record = {}
            for pattern, names in FIELDS:
                m = pattern.search(line)
                if m:
                    for name, value in zip(names, m.groups()):
                        record[name] = float(value)
            frames.append(record)
    return frames


def parse_probe(text):
    out = {"extras": {}}
    m = re.search(r"frames (\d+) mean ([\d.]+) median ([\d.]+) p95 ([\d.]+) max ([\d.]+) first ([\d.]+) ms", text)
    if m:
        for name, value in zip(["frames", "cpu_mean", "cpu_median", "cpu_p95", "cpu_max", "cpu_first"], m.groups()):
            out[name] = float(value)
    for name, value in re.findall(r"extra (\w+)=([\d.]+)", text):
        out["extras"][name] = float(value)
    out["glerrors"] = re.findall(r"glerror .*", text)
    out["ok"] = "WMGL_PERF_PROBE_OK" in text
    return out


def run_scene(runtime, prefix, scene, frames, warmup, timeout, salt):
    log_path = os.path.join(CACHE, "logs", scene + ".log")
    os.makedirs(os.path.dirname(log_path), exist_ok=True)
    if os.path.exists(log_path):
        os.remove(log_path)
    probe_scene = "shader_link" if scene == "shader_link_warm" else scene
    env = wine_env(runtime, prefix, {"WINEMETALGL_STATS": "1", "WINEMETALGL_LOG_FILE": log_path,
                                     "WINEMETALGL_PIPELINE_CACHE": os.path.join(CACHE, "pipelines")})
    start = time.time()
    try:
        result = subprocess.run([os.path.join(runtime, "bin/wine"), r"C:\perf\opengl_perf_probe.exe", probe_scene,
                                 str(frames), str(warmup), str(salt)], env=env, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT, text=True, errors="replace", timeout=timeout)
        text = result.stdout
    except subprocess.TimeoutExpired as e:
        text = (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
        text += "\n(timed out)"
        subprocess.run([os.path.join(runtime, "bin/wineserver"), "-k"], env=env)
    probe = parse_probe(text)
    probe["seconds"] = round(time.time() - start, 2)
    # The log, else the program's output (stats also go to stderr).
    stats = parse_stats(log_path)
    if not stats:
        tmp = log_path + ".stdout"
        open(tmp, "w").write(text)
        stats = parse_stats(tmp)
    steady = [f for f in stats if f.get("index", 0) >= warmup]
    summary = {"probe": probe, "frames_logged": len(stats), "steady": {}, "total": {}}
    if steady:
        keys = sorted({k for f in steady for k in f if k != "index"})
        summary["steady"] = {k: round(sum(f.get(k, 0) for f in steady) / len(steady), 4) for k in keys}
    for k in TOTALS:
        summary["total"][k] = round(sum(f.get(k, 0) for f in stats), 4)
    if not probe["ok"]:
        summary["output_tail"] = text[-2000:]
    return summary


def value(scene, name):
    probe = scene["probe"]
    if name.startswith("cpu_") and name in probe:
        return probe[name]
    if name in probe.get("extras", {}):
        return probe["extras"][name]
    if name.startswith("total_"):
        return scene["total"].get(name[len("total_"):])
    return scene["steady"].get(name)


def fmt(v):
    if v is None:
        return "-"
    return ("%.3f" % v).rstrip("0").rstrip(".") if abs(v) < 1000 else "%.0f" % v


def columns(name):
    return ["cpu_mean", "cpu_p95", "cpu_max"] + HEADLINES.get(name, [])


def print_run(data):
    print("label %s core %s" % (data["label"], data["core"]))
    for name, scene in data["scenes"].items():
        status = "ok" if scene["probe"]["ok"] else "FAILED"
        cells = ["%s %s" % (c, fmt(value(scene, c))) for c in columns(name)]
        print("%-18s %-6s %s" % (name, status, " | ".join(cells)))
        for e in scene["probe"]["glerrors"][:3]:
            print("    " + e)


def cmd_run(args):
    runtime, prefix, core, module = prepare(args)
    build_probe(runtime, prefix)
    listing = subprocess.run([os.path.join(runtime, "bin/wine"), r"C:\perf\opengl_perf_probe.exe", "list"],
                             env=wine_env(runtime, prefix), stdout=subprocess.PIPE, text=True).stdout
    scenes = [line.split("\t")[0] for line in listing.splitlines() if "\t" in line]
    if "shader_link" in scenes:
        scenes.insert(scenes.index("shader_link") + 1, "shader_link_warm")
    if args.scenes:
        wanted = args.scenes.split(",")
        scenes = [s for s in scenes if s in wanted]
    data = {"label": args.label, "core": os.path.realpath(core), "module": os.path.realpath(module),
            "runtime": os.path.realpath(args.wine_runtime), "frames": args.frames, "warmup": args.warmup,
            "date": time.strftime("%Y-%m-%d %H:%M:%S"), "scenes": {}}
    # Every run starts with an empty pipeline archive, so compile stutter is
    # measured cold (shader_link_warm then runs with the archive filled).
    # Metal's own compiler cache stays warm between runs.
    shutil.rmtree(os.path.join(CACHE, "pipelines"), ignore_errors=True)
    # A new salt per run keeps the probe's own shaders out of Metal's compiler
    # cache (the fixed-function shaders WineMetalGL generates stay cached).
    salt = int(time.time()) % 1000000 + 1
    for scene in scenes:
        log("scene %s" % scene)
        data["scenes"][scene] = run_scene(runtime, prefix, scene, args.frames, args.warmup, args.timeout, salt)
    subprocess.run([os.path.join(runtime, "bin/wineserver"), "-k"], env=wine_env(runtime, prefix))
    out = args.out or os.path.join(ROOT, "build/perf-probe/%s.json" % args.label)
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w") as f:
        json.dump(data, f, indent=1)
    print_run(data)
    print("wrote %s" % out)
    failed = [s for s, d in data["scenes"].items() if not d["probe"]["ok"]]
    if failed:
        print("failed scenes: %s" % ", ".join(failed))
    return 1 if failed else 0


def cmd_compare(args):
    before, after = (json.load(open(p)) for p in (args.before, args.after))
    print("%s -> %s" % (before["label"], after["label"]))
    for name, b in before["scenes"].items():
        a = after["scenes"].get(name)
        if not a:
            continue
        cells = []
        for c in columns(name):
            vb, va = value(b, c), value(a, c)
            if vb is None and va is None:
                continue
            change = ""
            if vb and va is not None:
                change = " (%+.0f%%)" % ((va - vb) / vb * 100.0)
            cells.append("%s %s -> %s%s" % (c, fmt(vb), fmt(va), change))
        print("%-18s %s" % (name, " | ".join(cells)))
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    run = sub.add_parser("run")
    run.add_argument("--label", default="baseline")
    run.add_argument("--core", help="libwinemetalgl.dylib to test (default: the installation's own)")
    run.add_argument("--module", help="Wine module directory (default: build/wine-module)")
    run.add_argument("--wine-runtime", default=DEFAULT_RUNTIME)
    run.add_argument("--frames", type=int, default=200)
    run.add_argument("--warmup", type=int, default=30)
    run.add_argument("--timeout", type=int, default=180)
    run.add_argument("--scenes", help="comma-separated scene names (default: all)")
    run.add_argument("--out")
    compare = sub.add_parser("compare")
    compare.add_argument("before")
    compare.add_argument("after")
    args = parser.parse_args()
    return cmd_run(args) if args.command == "run" else cmd_compare(args)


if __name__ == "__main__":
    sys.exit(main())
