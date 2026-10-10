#!/usr/bin/env python3
"""Generate WineMetalGL's OpenGL front end from the Khronos registry.

Reads third_party/khronos/gl.xml and writes, into OUTDIR:

  gl_enums.gen.h    #define for every GL enum (api "gl")
  gl_api.gen.h      wmgl::api::<Name>(Context&, ...) prototypes, one per command
  gl_entry.gen.cpp  unexported entry points (current-context lookup + call)
                    and a sorted name -> function table for get_proc_address
  gl_stubs.gen.cpp  weak default implementations that record
                    GL_INVALID_OPERATION and log the command once
  gl_commands.gen.json  the command list with versions/extensions (for reports)

Commands come from every GL feature up to 4.6 (core and compatibility) plus
the extensions listed in tools/gen/extensions.txt. Extension commands that
alias a selected command reuse that command's implementation.

    gl_gen.py --registry third_party/khronos/gl.xml --extensions tools/gen/extensions.txt --out DIR
"""

import argparse
import json
import os
import re
import xml.etree.ElementTree as ET

MAX_VERSION = (4, 6)


def param_decl(node):
    text = "".join(node.itertext())
    return " ".join(text.split())


def param_type(node):
    name = node.find("name").text
    text = param_decl(node)
    return text[: text.rfind(name)].strip()


def proto_return(proto):
    name = proto.find("name").text
    text = "".join(proto.itertext())
    return " ".join(text[: text.rfind(name)].split())


def default_return(rtype):
    if rtype == "void":
        return None
    if "*" in rtype or rtype in ("GLsync", "GLDEBUGPROC"):
        return "nullptr"
    return "0"


def load(registry, extension_names):
    root = ET.parse(registry).getroot()
    commands = {}
    for node in root.find("commands").findall("command"):
        proto = node.find("proto")
        name = proto.find("name").text
        params = [(param_type(p), p.find("name").text) for p in node.findall("param")]
        alias = node.find("alias")
        commands[name] = {
            "name": name,
            "ret": proto_return(proto),
            "params": params,
            "alias": alias.get("name") if alias is not None else None,
        }

    selected = {}
    for feature in root.findall("feature"):
        if feature.get("api") != "gl":
            continue
        number = tuple(int(x) for x in feature.get("number").split("."))
        if number > MAX_VERSION:
            continue
        for require in feature.findall("require"):
            for cmd in require.findall("command"):
                selected.setdefault(cmd.get("name"), "GL %d.%d" % number)

    wanted = set(extension_names)
    found = set()
    for ext in root.find("extensions").findall("extension"):
        name = ext.get("name")
        supported = ext.get("supported", "").split("|")
        if "gl" not in supported and "glcore" not in supported:
            continue
        for require in ext.findall("require"):
            if require.get("api") not in (None, "gl"):
                continue
            for cmd in require.findall("command"):
                cname = cmd.get("name")
                if name in wanted:
                    selected.setdefault(cname, name)
                    found.add(name)
                elif commands.get(cname, {}).get("alias") in selected:
                    # alias of a selected command (e.g. glBindBufferARB)
                    selected.setdefault(cname, name)
    missing = wanted - found
    # Every other desktop GL command still gets an entry point, so Wine never
    # receives a NULL from get_proc_address: unimplemented commands report
    # GL_INVALID_OPERATION and log their name instead of crashing the caller.
    gl_commands = set()
    for ext in root.find("extensions").findall("extension"):
        supported = ext.get("supported", "").split("|")
        if "gl" not in supported and "glcore" not in supported:
            continue
        for require in ext.findall("require"):
            if require.get("api") not in (None, "gl"):
                continue
            for cmd in require.findall("command"):
                gl_commands.add((cmd.get("name"), ext.get("name")))
    for cname, ext_name in sorted(gl_commands):
        selected.setdefault(cname, ext_name)
    return commands, selected, missing


def enums(registry):
    root = ET.parse(registry).getroot()
    values = {}
    for block in root.findall("enums"):
        for e in block.findall("enum"):
            if e.get("api") not in (None, "gl"):
                continue
            name, value = e.get("name"), e.get("value")
            suffix = e.get("type", "")
            if suffix == "ull" and not value.endswith("ull"):
                value += "ull"
            elif suffix == "u" and not value.endswith("u"):
                value += "u"
            values.setdefault(name, value)
    return values


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--registry", required=True)
    parser.add_argument("--extensions", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    ext_names = [l.split("#")[0].strip() for l in open(args.extensions)]
    ext_names = [l for l in ext_names if l]
    commands, selected, missing = load(args.registry, ext_names)
    if missing:
        raise SystemExit("extensions not in registry: " + ", ".join(sorted(missing)))
    os.makedirs(args.out, exist_ok=True)

    # Resolve each selected command to the implementation it calls.
    def impl_of(name):
        seen = set()
        while commands[name]["alias"] and commands[name]["alias"] in selected and name not in seen:
            seen.add(name)
            name = commands[name]["alias"]
        return name

    names = sorted(selected)
    impls = sorted({impl_of(n) for n in names})

    def api_name(cmd):
        return cmd[2:] if cmd.startswith("gl") else cmd

    def sig(cmd, with_ctx=True, named=True):
        parts = ["Context& ctx"] if with_ctx else []
        for i, (ptype, pname) in enumerate(commands[cmd]["params"]):
            parts.append("%s %s" % (ptype, pname) if named else ptype)
        return ", ".join(parts) if parts else ("void" if not with_ctx else "")

    with open(os.path.join(args.out, "gl_enums.gen.h"), "w") as out:
        out.write("// Generated by tools/gen/gl_gen.py from the Khronos OpenGL registry. Do not edit.\n#pragma once\n\n")
        for name, value in sorted(enums(args.registry).items()):
            if name.startswith("GL_"):
                out.write("#define %s %s\n" % (name, value))

    with open(os.path.join(args.out, "gl_api.gen.h"), "w") as out:
        out.write("// Generated by tools/gen/gl_gen.py. Do not edit.\n#pragma once\n\n")
        out.write('#include "core/gl/types.h"\n\nnamespace wmgl {\nclass Context;\nnamespace api {\n\n')
        for cmd in impls:
            out.write("%s %s(%s);\n" % (commands[cmd]["ret"], api_name(cmd), sig(cmd)))
        out.write("\n}  // namespace api\n}  // namespace wmgl\n")

    with open(os.path.join(args.out, "gl_stubs.gen.cpp"), "w") as out:
        out.write("// Generated by tools/gen/gl_gen.py. Do not edit.\n")
        out.write("// Weak defaults: any real implementation elsewhere in the library wins at link time.\n\n")
        out.write('#include "gl_api.gen.h"\n#include "core/context.h"\n\nnamespace wmgl::api {\n\n')
        for cmd in impls:
            ret = commands[cmd]["ret"]
            params = ["Context& ctx"] + ["%s" % ptype for ptype, _ in commands[cmd]["params"]]
            out.write('__attribute__((weak)) %s %s(%s) {\n    ctx.unimplemented("%s");\n' % (
                ret, api_name(cmd), ", ".join(params), cmd))
            dflt = default_return(ret)
            if dflt:
                out.write("    return %s;\n" % dflt)
            out.write("}\n")
        out.write("\n}  // namespace wmgl::api\n")

    with open(os.path.join(args.out, "gl_entry.gen.cpp"), "w") as out:
        out.write("// Generated by tools/gen/gl_gen.py. Do not edit.\n\n")
        out.write('#include "gl_api.gen.h"\n#include "core/context.h"\n#include "core/proc_table.h"\n\nnamespace {\n\n')
        for cmd in names:
            impl = impl_of(cmd)
            ret = commands[cmd]["ret"]
            params = commands[cmd]["params"]
            decl = ", ".join("%s %s" % (t, n) for t, n in params) or "void"
            call = ", ".join(["*ctx"] + [n for _, n in params])
            out.write("%s e_%s(%s) {\n" % (ret, cmd, decl))
            out.write('    if (__builtin_expect(wmgl::g_traceCalls, 0)) wmgl::traceCall("%s");\n' % cmd)
            out.write("    wmgl::Context* ctx = wmgl::currentContext();\n")
            dflt = default_return(ret)
            out.write('    if (__builtin_expect(!ctx, 0)) { wmgl::noCurrentContext("%s"); return%s; }\n' % (
                cmd, (" " + dflt) if dflt else ""))
            out.write("    %swmgl::api::%s(%s);\n}\n" % ("return " if dflt else "", api_name(impl), call))
        out.write("\n}  // namespace\n\nnamespace wmgl {\n\nconst ProcEntry kProcTable[] = {\n")
        for cmd in names:
            out.write('    {"%s", reinterpret_cast<void*>(&e_%s)},\n' % (cmd, cmd))
        out.write("};\nconst unsigned kProcTableSize = %d;\n\n}  // namespace wmgl\n" % len(names))

    with open(os.path.join(args.out, "gl_commands.gen.json"), "w") as out:
        json.dump({cmd: {"source": selected[cmd], "impl": impl_of(cmd)} for cmd in names}, out, indent=1, sort_keys=True)
    print("gl_gen: %d entry points, %d implementations" % (len(names), len(impls)))


if __name__ == "__main__":
    main()
