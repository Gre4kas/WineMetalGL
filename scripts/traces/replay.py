#!/usr/bin/env python3
"""Replay WineMetalGL's game corpus and compare frames against references.

The corpus manifest (tests/games/corpus.json) lists entries of two kinds:

* "demo": a deterministic program from tests/games/demos that renders N
  frames and dumps the last one with --dump (a binary PPM).
* "trace": an apitrace trace replayed with glretrace.exe; selected frames
  are snapshotted as PNM on stdout.

Frames are compared with a reference image (by default produced by the
Apple OpenGL path, Wine's own opengl32) using two numbers: the fraction of pixels
whose largest channel difference exceeds --threshold, and PSNR. An entry
passes when the fraction is at most its "tolerance" (default 0.005).

    scripts/traces/replay.py --wine W --corpus-root DIR [--update-references]
                             [--path new|apple] [--only NAME] [--html OUT]

Binaries, traces and reference images live outside git under
--corpus-root (see tests/games/README.md); only the manifest is tracked.
"""

import argparse
import atexit
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read_pnm_stream(data):
    """Yield (width, height, rgb bytes) for concatenated binary P6 images."""
    offset = 0
    while offset < len(data):
        tokens = []
        while len(tokens) < 4:
            while offset < len(data) and data[offset:offset + 1].isspace():
                offset += 1
            if offset >= len(data):
                return
            if data[offset:offset + 1] == b"#":
                offset = data.index(b"\n", offset) + 1
                continue
            end = offset
            while end < len(data) and not data[end:end + 1].isspace():
                end += 1
            tokens.append(data[offset:end])
            offset = end
        offset += 1  # single whitespace after maxval
        magic, width, height, maxval = tokens[0], int(tokens[1]), int(tokens[2]), int(tokens[3])
        if magic != b"P6" or maxval != 255:
            raise ValueError("unsupported PNM %r maxval %d" % (magic, maxval))
        size = width * height * 3
        yield width, height, data[offset:offset + size]
        offset += size


def load_ppm(path):
    with open(path, "rb") as handle:
        images = list(read_pnm_stream(handle.read()))
    if not images:
        raise ValueError("no image in %s" % path)
    return images[0]


def write_ppm(path, image):
    width, height, rgb = image
    with open(path, "wb") as handle:
        handle.write(b"P6\n%d %d\n255\n" % (width, height))
        handle.write(rgb)


def compare(reference, actual, threshold):
    if reference[0] != actual[0] or reference[1] != actual[1]:
        return 1.0, 0.0
    ref, act = reference[2], actual[2]
    pixels = reference[0] * reference[1]
    differing = 0
    squared = 0
    for i in range(0, len(ref), 3):
        d0 = abs(ref[i] - act[i])
        d1 = abs(ref[i + 1] - act[i + 1])
        d2 = abs(ref[i + 2] - act[i + 2])
        if max(d0, d1, d2) > threshold:
            differing += 1
        squared += d0 * d0 + d1 * d1 + d2 * d2
    mse = squared / float(len(ref)) if ref else 0.0
    psnr = 99.0 if mse == 0 else 10.0 * math.log10(255.0 * 255.0 / mse)
    return differing / float(pixels), psnr


def path_env(path):
    # WineMetalGL is the prefix's OpenGL (the Wine module); Wine's own
    # opengl32 is Apple's (WINEMETALGL=0 for a Wine with the 2.x patches).
    return {"WINEDLLOVERRIDES": "opengl32=b", "WINEMETALGL": "0"} if path == "apple" else {}


def run_entry(entry, args, env, workdir):
    root = args.corpus_root
    wine = os.path.join(args.wine, "bin", "wine")
    if entry["kind"] == "demo":
        exe = os.path.join(root, entry["binary"])
        out = os.path.join(workdir, entry["name"] + ".ppm")
        # Apple's OpenGL has no drawable for a hidden window (it reads back
        # black), so the reference path renders on screen.
        hidden = [] if args.path == "apple" else ["--hidden"]
        command = [wine, exe] + entry.get("args", []) + hidden + ["--dump", out]
        started = time.monotonic()
        result = subprocess.run(command, cwd=os.path.dirname(exe), env=env, capture_output=True,
                                timeout=entry.get("timeout", 300))
        elapsed = time.monotonic() - started
        if not os.path.exists(out):
            return None, elapsed, result.stdout.decode("latin1")[-2000:] + result.stderr.decode("latin1")[-2000:]
        return [load_ppm(out)], elapsed, ""
    retrace = os.path.join(root, entry.get("retrace", "tools/apitrace/bin/glretrace.exe"))
    trace = os.path.join(root, entry["trace"])
    command = [wine, retrace, "--headless", "--snapshot-prefix=-", "--snapshot-format=PNM",
               "--snapshot=%s" % entry.get("snapshot", "frame")] + entry.get("retrace_args", []) + [trace]
    started = time.monotonic()
    result = subprocess.run(command, cwd=os.path.dirname(retrace), env=env, capture_output=True,
                            timeout=entry.get("timeout", 600))
    elapsed = time.monotonic() - started
    images = list(read_pnm_stream(result.stdout))
    if not images:
        return None, elapsed, result.stderr.decode("latin1")[-2000:]
    keep = entry.get("frames")
    if keep:
        images = [images[i] for i in keep if i < len(images)]
        if not images:
            return None, elapsed, "none of frames %s was rendered" % keep
    return images, elapsed, ""


def git_head():
    """HEAD's short hash, with "-dirty" when tracked files differ from it."""
    head = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short", "HEAD"], capture_output=True, text=True).stdout.strip()
    dirty = subprocess.run(["git", "-C", ROOT, "diff", "--quiet", "HEAD"]).returncode != 0
    return (head or "unknown") + ("-dirty" if dirty else "")


def cleanup_prefix(wine, prefix, workdir):
    if os.path.isdir(prefix):
        env = dict(os.environ, WINEPREFIX=prefix)
        subprocess.run([os.path.join(wine, "bin", "wineserver"), "-k"], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    shutil.rmtree(workdir, ignore_errors=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--wine", required=True)
    parser.add_argument("--corpus-root", required=True)
    parser.add_argument("--manifest", default=os.path.join(ROOT, "tests", "games", "corpus.json"))
    parser.add_argument("--template", help="template WoW64 prefix to clone")
    parser.add_argument("--path", choices=["new", "apple"], default="new")
    parser.add_argument("--only", action="append")
    parser.add_argument("--threshold", type=int, default=8)
    parser.add_argument("--update-references", action="store_true",
                        help="store this run's frames as the references")
    parser.add_argument("--html", help="write an HTML report with side-by-side images")
    parser.add_argument("--json", help="record the results (tests/games/results.json is the committed evidence)")
    args = parser.parse_args()

    manifest = json.load(open(args.manifest))
    entries = [e for e in manifest["entries"] if not args.only or e["name"] in args.only]
    workdir = tempfile.mkdtemp(prefix="wmgl-traces-")
    prefix = os.path.join(workdir, "prefix")
    # Each prefix is ~2 GB: stop its wineserver and delete it however the run ends.
    atexit.register(cleanup_prefix, args.wine, prefix, workdir)
    if args.template:
        subprocess.run(["cp", "-c", "-R", args.template, prefix], check=True)
    env = dict(os.environ)
    env.update({"WINEPREFIX": prefix, "WINEARCH": "wow64", "WINEDEBUG": env.get("WINEDEBUG", "-all")})
    env.setdefault("DYLD_LIBRARY_PATH", os.path.join(args.wine, "lib"))
    env.pop("WINEMETALGL", None)
    env.update(path_env(args.path))
    if not args.template:
        subprocess.run([os.path.join(args.wine, "bin", "wine"), "wineboot", "-u"], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)

    failures = 0
    rows = []
    for entry in entries:
        ref_dir = os.path.join(args.corpus_root, "references", entry.get("reference", entry["name"]))
        try:
            images, elapsed, log = run_entry(entry, args, env, workdir)
        except subprocess.TimeoutExpired:
            images, elapsed, log = None, 0.0, "timeout"
        if images is None:
            failures += 1
            print("FAIL %-28s no frames (%s)" % (entry["name"], log.strip().splitlines()[-1] if log.strip() else "?"))
            rows.append((entry, "FAIL", [], elapsed))
            continue
        if args.update_references and "reference" not in entry:
            os.makedirs(ref_dir, exist_ok=True)
            for i, image in enumerate(images):
                write_ppm(os.path.join(ref_dir, "%03d.ppm" % i), image)
            print("REF  %-28s %d frame(s) stored (%.1f s)" % (entry["name"], len(images), elapsed))
            continue
        if args.update_references:
            continue
        tolerance = entry.get("tolerance", 0.005)
        results = []
        for i, image in enumerate(images):
            ref_path = os.path.join(ref_dir, "%03d.ppm" % i)
            if not os.path.exists(ref_path):
                results.append((i, 1.0, 0.0, None))
                continue
            fraction, psnr = compare(load_ppm(ref_path), image, args.threshold)
            out_path = os.path.join(workdir, "%s-%03d.ppm" % (entry["name"], i))
            write_ppm(out_path, image)
            results.append((i, fraction, psnr, out_path))
        worst = max(r[1] for r in results)
        status = "PASS" if worst <= tolerance else "FAIL"
        failures += status == "FAIL"
        print("%s %-28s worst %.4f%% differing pixels, min PSNR %.1f dB, %.1f s" % (
            status, entry["name"], worst * 100.0, min(r[2] for r in results), elapsed))
        rows.append((entry, status, results, elapsed))

    if args.html and not args.update_references:
        write_html(args.html, rows, args.corpus_root)
    if args.json and not args.update_references:
        record = {
            "path": args.path,
            "commit": git_head(),
            "date": time.strftime("%Y-%m-%d"),
            "threshold": args.threshold,
            "entries": [{"name": entry["name"], "tier": entry.get("tier", ""), "status": status,
                         "worst_fraction": round(max((r[1] for r in results), default=1.0), 6),
                         "min_psnr_db": round(min((r[2] for r in results), default=0.0), 1)}
                        for entry, status, results, _ in rows],
        }
        with open(args.json, "w") as handle:
            json.dump(record, handle, indent=1)
            handle.write("\n")
    sys.exit(1 if failures else 0)


def write_html(path, rows, corpus_root):
    """Side-by-side report; images are converted to PNG with sips (macOS)."""
    out_dir = os.path.splitext(path)[0] + "_files"
    os.makedirs(out_dir, exist_ok=True)
    lines = ["<!doctype html><meta charset=utf-8><title>WineMetalGL traces</title>",
             "<style>body{font:14px system-ui;background:#111;color:#ddd}img{width:420px;margin:4px}</style>"]
    for entry, status, results, elapsed in rows:
        lines.append("<h2>%s: %s (%.1f s)</h2>" % (entry["name"], status, elapsed))
        for i, fraction, psnr, actual in results:
            ref = os.path.join(corpus_root, "references", entry.get("reference", entry["name"]), "%03d.ppm" % i)
            pair = []
            for label, src in (("ref", ref), ("actual", actual)):
                if src and os.path.exists(src):
                    png = os.path.join(out_dir, "%s-%03d-%s.png" % (entry["name"], i, label))
                    subprocess.run(["sips", "-s", "format", "png", src, "--out", png],
                                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    pair.append('<img src="%s/%s" title="%s">' % (os.path.basename(out_dir), os.path.basename(png), label))
            lines.append("<p>frame %d: %.3f%% differing, %.1f dB</p>%s" % (i, fraction * 100, psnr, "".join(pair)))
    open(path, "w").write("\n".join(lines))


if __name__ == "__main__":
    main()
