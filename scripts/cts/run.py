#!/usr/bin/env python3
"""Run Khronos OpenGL CTS cases in parallel shards, under Wine or natively.

Under Wine, each shard gets its own WoW64 prefix (an APFS clone of one
template prefix) and its own glcts.exe process. With --native, glcts is the
macOS build from tests/cts/native/build.sh, which runs the WineMetalGL core
directly on pbuffers without Wine. When a process crashes or a case stops making
progress, that case is recorded as Crash or Timeout and the shard resumes
after it, so one bad case never hides the rest of the run.

Results are written as one "<case> <status>" line per case, sorted, with a
header recording the configuration, commit and environment. scripts/cts/diff.py
and scripts/cts/summary.py read that format.

Example (full GL 3.3 mustpass, first configuration, 6 shards):

    scripts/cts/run.py --wine /path/to/wine --cts /path/to/glcts.exe \
        --config 1 --shards 6 --out /path/to/results/gl33-cfg1.txt

    scripts/cts/run.py --native --cts /path/to/build-wmgl-native/.../glcts \
        --config 1 --shards 8 --out /path/to/results/gl33-cfg1-native.txt
"""

import argparse
import collections
import os
import re
import shutil
import signal
import subprocess
import sys
import tempfile
import threading
import time
import xml.etree.ElementTree as ET

MAX_ABORTS = 20  # platform aborts tolerated per shard before cases are marked Crash
MUSTPASS_REL = "external/openglcts/data/gl_cts/data/mustpass/gl/khronos_mustpass/main"
# The single-configuration lists (KHR-Single-GL4x: enhanced layouts, arrays of arrays).
SINGLE_MUSTPASS_REL = "external/openglcts/data/gl_cts/data/mustpass/gl/khronos_mustpass_single/main"


def mustpass_dir(caselist_name):
    return SINGLE_MUSTPASS_REL if caselist_name.endswith("-khr-single.txt") else MUSTPASS_REL


CASE_BEGIN = re.compile(r"^#beginTestCaseResult (\S+)")
CASE_END = re.compile(r"^#(endTestCaseResult|terminateTestCaseResult)")
STATUS = re.compile(r'<Result StatusCode="(\w+)"')


def find_cts_root(cts_exe):
    path = os.path.dirname(os.path.abspath(cts_exe))
    while path != "/":
        if os.path.isdir(os.path.join(path, MUSTPASS_REL)):
            return path
        path = os.path.dirname(path)
    return None


def load_configs(cts_root, caselist_name):
    """Return the mustpass command lines for a case list, in mustpass.xml order."""
    tree = ET.parse(os.path.join(cts_root, mustpass_dir(caselist_name), "mustpass.xml"))
    configs = []
    for node in tree.iter("Configuration"):
        if node.get("caseListFile") == caselist_name:
            configs.append(node.get("commandLine", "").split())
    return configs


def read_cases(args, cts_root):
    if args.caselist:
        path = args.caselist
    else:
        path = os.path.join(cts_root, mustpass_dir(args.mustpass), args.mustpass)
    cases = []
    with open(path) as handle:
        for line in handle:
            line = line.strip()
            if line and not line.startswith("#"):
                cases.append(line)
    if args.filter:
        patterns = [re.compile(fnmatch_to_regex(p)) for p in args.filter]
        cases = [c for c in cases if any(p.match(c) for p in patterns)]
    return cases


def fnmatch_to_regex(pattern):
    return "^" + re.escape(pattern).replace(r"\*", ".*") + "$"


def parse_qpa(path):
    """Return (ordered results dict, case that began but did not finish)."""
    results = collections.OrderedDict()
    current = None
    block = []
    if not os.path.exists(path):
        return results, None
    with open(path, "rb") as handle:
        for raw in handle:
            line = raw.decode("latin1").rstrip("\n")
            begin = CASE_BEGIN.match(line)
            if begin:
                current = begin.group(1)
                block = []
                continue
            if current is None:
                continue
            if CASE_END.match(line):
                text = "\n".join(block)
                status = STATUS.search(text)
                if line.startswith("#terminateTestCaseResult"):
                    reason = line.split(" ", 1)[1] if " " in line else "Crash"
                    results[current] = "Crash" if "Crash" in reason or not status else status.group(1)
                else:
                    results[current] = status.group(1) if status else "InternalError"
                current = None
                continue
            block.append(line)
    return results, current


class Shard:
    def __init__(self, index, cases, args, env, workdir, config_args):
        self.index = index
        self.cases = list(cases)
        self.args = args
        self.env = env
        self.workdir = workdir
        self.config_args = config_args
        self.results = collections.OrderedDict()
        self.prefix = os.path.join(workdir, "prefix-%d" % index)
        self.attempt = 0
        self.aborts = 0

    def run(self):
        if not self.args.native:
            clone_prefix(self.args.template, self.prefix)
        remaining = list(self.cases)
        while remaining:
            self.attempt += 1
            caselist = os.path.join(self.workdir, "shard-%d-%d.txt" % (self.index, self.attempt))
            qpa = os.path.join(self.workdir, "shard-%d-%d.qpa" % (self.index, self.attempt))
            with open(caselist, "w") as handle:
                handle.write("\n".join(remaining) + "\n")
            killed, aborted = self._run_process(caselist, qpa)
            done, unfinished = parse_qpa(qpa)
            self.results.update(done)
            # dEQP's Win32 test window posts WM_QUIT when it is destroyed;
            # when the platform loop sees it the run stops between or inside
            # cases ("Test run was ABORTED!"). That is not a crash of the case
            # in progress: run the remaining cases again (bounded).
            if aborted and not killed and self.aborts < MAX_ABORTS:
                self.aborts += 1
                sys.stderr.write("shard %d: run aborted by the platform (%d), resuming\n" % (self.index, self.aborts))
                remaining = [c for c in remaining if c not in self.results]
                continue
            if unfinished is not None and unfinished not in self.results:
                self.results[unfinished] = "Timeout" if killed else "Crash"
            next_remaining = [c for c in remaining if c not in self.results]
            if len(next_remaining) == len(remaining):
                # The process produced nothing at all; mark the first case so
                # the shard always makes progress.
                self.results[remaining[0]] = "Timeout" if killed else "Crash"
                next_remaining = remaining[1:]
            remaining = next_remaining
            if not self.args.keep_qpa:
                for path in (caselist, qpa):
                    if os.path.exists(path):
                        os.remove(path)
        self._kill_wineserver()
        if not self.args.keep_prefix and not self.args.native:
            shutil.rmtree(self.prefix, ignore_errors=True)

    def _run_process(self, caselist, qpa):
        env = dict(self.env)
        env["WINEPREFIX"] = self.prefix
        launcher = [] if self.args.native else [os.path.join(self.args.wine, "bin", "wine")]
        command = launcher + [
            self.args.cts,
            "--deqp-caselist-file=%s" % caselist,
            "--deqp-log-filename=%s" % qpa,
            "--deqp-log-images=%s" % ("enable" if self.args.log_images else "disable"),
            "--deqp-log-shader-sources=%s" % ("enable" if self.args.log_shaders else "disable"),
            "--deqp-terminate-on-device-lost=disable",
        ] + self.config_args
        log = os.path.join(self.workdir, "shard-%d.log" % self.index)
        log_start = os.path.getsize(log) if os.path.exists(log) else 0
        stdout = open(log, "ab")
        process = subprocess.Popen(command, cwd=os.path.dirname(self.args.cts), env=env,
                                   stdout=stdout, stderr=subprocess.STDOUT, start_new_session=True)
        last_size = -1
        last_change = time.monotonic()
        killed = False
        while process.poll() is None:
            time.sleep(1.0)
            size = os.path.getsize(qpa) if os.path.exists(qpa) else 0
            now = time.monotonic()
            if size != last_size:
                last_size = size
                last_change = now
            elif now - last_change > self.args.case_timeout:
                killed = True
                os.killpg(process.pid, signal.SIGKILL)
                process.wait()
                break
        stdout.close()
        self._kill_wineserver()
        with open(log, "rb") as handle:
            handle.seek(log_start)
            aborted = b"Test run was ABORTED" in handle.read()
        return killed, aborted

    def _kill_wineserver(self):
        if self.args.native:
            return
        env = dict(self.env)
        env["WINEPREFIX"] = self.prefix
        subprocess.run([os.path.join(self.args.wine, "bin", "wineserver"), "-k"], env=env,
                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def clone_prefix(template, destination):
    shutil.rmtree(destination, ignore_errors=True)
    # APFS clone; falls back to a plain copy on other filesystems.
    if subprocess.run(["cp", "-c", "-R", template, destination],
                      stderr=subprocess.DEVNULL).returncode != 0:
        shutil.copytree(template, destination, symlinks=True)


def make_template(args, env):
    if args.template and os.path.isdir(args.template):
        return
    args.template = args.template or os.path.join(args.workdir, "prefix-template")
    env = dict(env)
    env["WINEPREFIX"] = args.template
    subprocess.run([os.path.join(args.wine, "bin", "wine"), "wineboot", "-u"], env=env,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    subprocess.run([os.path.join(args.wine, "bin", "wineserver"), "-w"], env=env)


def git_head():
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    try:
        head = subprocess.check_output(["git", "-C", root, "rev-parse", "--short", "HEAD"], text=True).strip()
        dirty = subprocess.run(["git", "-C", root, "diff", "--quiet", "HEAD"]).returncode != 0
        return head + ("-dirty" if dirty else "")
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--wine", help="Wine installation (contains bin/wine)")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 if any case is not Pass or NotSupported")
    parser.add_argument("--native", action="store_true",
                        help="run a native macOS glcts (tests/cts/native) without Wine")
    parser.add_argument("--cts", required=True, help="glcts.exe from a VK-GL-CTS build (or native glcts)")
    parser.add_argument("--mustpass", default="gl33-main.txt", help="mustpass case list name")
    parser.add_argument("--caselist", help="explicit case list file instead of the mustpass list")
    parser.add_argument("--filter", action="append", help="only cases matching this glob (repeatable)")
    parser.add_argument("--config", type=int, default=1, help="mustpass configuration number (1-based)")
    parser.add_argument("--shards", type=int, default=6)
    parser.add_argument("--case-timeout", type=float, default=120.0, help="seconds without QPA progress")
    parser.add_argument("--out", required=True, help="results file")
    parser.add_argument("--workdir", help="scratch directory (default: temporary)")
    parser.add_argument("--template", help="existing WoW64 template prefix to clone")
    parser.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for Wine")
    parser.add_argument("--reference", action="store_true",
                        help="run the Apple OpenGL reference path (Wine's own opengl32)")
    parser.add_argument("--keep-qpa", action="store_true")
    parser.add_argument("--keep-prefix", action="store_true")
    parser.add_argument("--log-images", action="store_true")
    parser.add_argument("--log-shaders", action="store_true")
    args = parser.parse_args()
    if not args.native and not args.wine:
        parser.error("--wine is required unless --native is given")

    args.wine = os.path.abspath(args.wine) if args.wine else None
    args.cts = os.path.abspath(args.cts)
    cts_root = find_cts_root(args.cts)
    if not cts_root:
        sys.exit("cannot find the CTS checkout above %s" % args.cts)
    configs = load_configs(cts_root, args.mustpass)
    if not 1 <= args.config <= max(1, len(configs)):
        sys.exit("configuration %d does not exist (%d available)" % (args.config, len(configs)))
    config_args = configs[args.config - 1] if configs else []

    cases = read_cases(args, cts_root)
    if not cases:
        sys.exit("no cases selected")

    owns_workdir = args.workdir is None
    args.workdir = args.workdir or tempfile.mkdtemp(prefix="wmgl-cts-")
    os.makedirs(args.workdir, exist_ok=True)

    env = dict(os.environ)
    env.update({"WINEARCH": "wow64", "WINEDEBUG": env.get("WINEDEBUG", "-all")})
    if not args.native:
        env.setdefault("DYLD_LIBRARY_PATH", os.path.join(args.wine, "lib"))
    env.pop("WINEMETALGL", None)
    if args.reference:
        # Wine's own opengl32 instead of the Wine module (WINEMETALGL=0 for a
        # Wine with the 2.x patches).
        env["WINEDLLOVERRIDES"] = "opengl32=b"
        env["WINEMETALGL"] = "0"
    for item in args.env:
        key, _, value = item.partition("=")
        env[key] = value

    if not args.native:
        make_template(args, env)
    shard_count = max(1, min(args.shards, len(cases)))
    # Contiguous slices keep related cases (and their shared GL state
    # patterns) together, which matches how the CTS is usually run.
    size = (len(cases) + shard_count - 1) // shard_count
    shards = [Shard(i, cases[i * size:(i + 1) * size], args, env, args.workdir, config_args)
              for i in range(shard_count)]
    started = time.monotonic()
    threads = [threading.Thread(target=s.run) for s in shards]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join()

    results = {}
    for shard in shards:
        results.update(shard.results)
    # The CTS build that ran (dEQP prints its revision when it starts).
    cts_revision = "unknown"
    for shard in shards:
        log = os.path.join(args.workdir, "shard-%d.log" % shard.index)
        if os.path.exists(log):
            found = re.search(rb"dEQP Core ([0-9a-f]+)", open(log, "rb").read())
            if found:
                cts_revision = found.group(1).decode()
                break
    for case in cases:
        results.setdefault(case, "Missing")
    counts = collections.Counter(results.values())
    elapsed = time.monotonic() - started
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, "w") as handle:
        handle.write("# mustpass %s config %d: %s\n" % (args.mustpass, args.config, " ".join(config_args)))
        handle.write("# commit %s, path %s, %d cases, %.0f s\n" % (
            git_head(), "apple-reference" if args.reference else "winemetalgl-native" if args.native else "winemetalgl",
            len(cases), elapsed))
        handle.write("# cts dEQP Core %s\n" % cts_revision)
        handle.write("# totals %s\n" % " ".join("%s=%d" % kv for kv in sorted(counts.items())))
        for case in sorted(results):
            handle.write("%s %s\n" % (case, results[case]))
    print("%d cases in %.0f s: %s" % (len(cases), elapsed,
                                       ", ".join("%s %d" % kv for kv in sorted(counts.items()))))
    if owns_workdir and not args.keep_qpa and not args.keep_prefix:
        shutil.rmtree(args.workdir, ignore_errors=True)
    if args.strict and any(status not in ("Pass", "NotSupported") for status in results.values()):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
