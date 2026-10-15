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
  gl_pfn.gen.h      PFN_<command> function pointer types (for tests that load
                    entry points through wmgl_get_proc_address)
  gl_list.gen.cpp/h display list recorders for the list-compilable commands

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
            "lens": [p.get("len") for p in node.findall("param")],
            "alias": alias.get("name") if alias is not None else None,
            "version": None,
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
                entry = commands.get(cmd.get("name"))
                if entry is not None and entry["version"] is None:
                    entry["version"] = number

    # OpenGL ES commands desktop GL has no counterpart for (ES 3.2's
    # glPrimitiveBoundingBox and glBlendBarrier).
    for feature in root.findall("feature"):
        if feature.get("api") != "gles2":
            continue
        number = tuple(int(x) for x in feature.get("number").split("."))
        for require in feature.findall("require"):
            for cmd in require.findall("command"):
                if cmd.get("name") not in selected:
                    selected[cmd.get("name")] = "GL ES %d.%d" % number

    wanted = set(extension_names)
    found = set()
    es_commands = set()  # from OpenGL ES requirements only
    for ext in root.find("extensions").findall("extension"):
        name = ext.get("name")
        supported = ext.get("supported", "").split("|")
        desktop = "gl" in supported or "glcore" in supported
        # OpenGL ES extensions listed in extensions.txt (their commands
        # mostly alias desktop ones: glCopyImageSubDataEXT).
        if not desktop and not (name in wanted and "gles2" in supported):
            continue
        apis = (None, "gl", "gles2") if name in wanted else (None, "gl")
        for require in ext.findall("require"):
            if require.get("api") not in apis:
                continue
            for cmd in require.findall("command"):
                cname = cmd.get("name")
                if name in wanted:
                    if not desktop or require.get("api") == "gles2":
                        es_commands.add(cname)
                    selected.setdefault(cname, name)
                    found.add(name)
                elif commands.get(cname, {}).get("alias") in selected:
                    # alias of a selected command (e.g. glBindBufferARB)
                    selected.setdefault(cname, name)
        if name in wanted and name not in found and not any(r.findall("command") for r in ext.findall("require")):
            found.add(name)  # no commands (enums only)
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
    # OpenGL ES extension commands the registry gives no alias: the desktop
    # command of the same name without the suffix when the two take the same
    # arguments (glGenQueriesEXT), or ES_ALIASES.
    for cname in selected:
        entry = commands.get(cname)
        if entry is None or entry["alias"] or (cname not in es_commands and cname not in ES_ALIASES):
            continue
        if cname in ES_OWN:
            continue
        if cname in ES_ALIASES:
            entry["alias"] = ES_ALIASES[cname]
            continue
        base = re.sub(r"(EXT|OES|KHR)$", "", cname)
        if base != cname and base in selected and base in commands and \
                [t for t, _ in commands[base]["params"]] == [t for t, _ in entry["params"]] and \
                commands[base]["ret"] == entry["ret"]:
            entry["alias"] = base
    return commands, selected, missing


# OpenGL ES commands that take the same arguments as a desktop command,
# by another type of the same size (GLenum internalformat for GLint).
ES_ALIASES = {
    "glTexImage3DOES": "glTexImage3D",
    "glTexSubImage3DOES": "glTexSubImage3D",
    "glCopyTexSubImage3DOES": "glCopyTexSubImage3D",
    "glCompressedTexImage3DOES": "glCompressedTexImage3D",
    "glCompressedTexSubImage3DOES": "glCompressedTexSubImage3D",
    "glFramebufferTexture3DOES": "glFramebufferTexture3D",
}

# OpenGL ES commands with their own implementation although a desktop
# command of their name without the suffix exists (other enums).
ES_OWN = {"glGetObjectLabelEXT"}


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


# Display lists (GL 3.3 compatibility section 5.5.1): commands of GL 1.0-2.1
# are compiled into the list being built, except these, which always run
# immediately. Later commands are never compiled.
NOT_COMPILED = set("""
glIsList glGenLists glDeleteLists glFeedbackBuffer glSelectBuffer glRenderMode glVertexPointer glNormalPointer
glColorPointer glSecondaryColorPointer glIndexPointer glEdgeFlagPointer glFogCoordPointer glTexCoordPointer
glVertexAttribPointer glInterleavedArrays glEnableClientState glDisableClientState glEnableVertexAttribArray
glDisableVertexAttribArray glClientActiveTexture glPushClientAttrib glPopClientAttrib glReadPixels glPixelStoref
glPixelStorei glGenTextures glDeleteTextures glAreTexturesResident glGenQueries glDeleteQueries glBindBuffer
glDeleteBuffers glGenBuffers glBufferData glBufferSubData glMapBuffer glUnmapBuffer glCreateProgram glCreateShader
glDeleteProgram glDeleteShader glAttachShader glBindAttribLocation glCompileShader glDetachShader glLinkProgram
glShaderSource glValidateProgram glFlush glFinish glNewList glEndList glMap1d glMap1f glMap2d glMap2f
""".split())

# Commands whose implementation records itself: current attributes and
# Begin/End build batches, vertex array draws dereference the arrays
# (core/compat/immediate.h).
SELF_RECORDED = re.compile(
    r"^gl(Begin$|End$|EdgeFlagv?$|ArrayElement|DrawArrays|DrawElements|DrawRangeElements|MultiDrawArrays|"
    r"MultiDrawElements|Rect[dfis]v?|Color[34]|SecondaryColor3|Normal3|TexCoord[1-4]|MultiTexCoord[1-4]|"
    r"FogCoord[df]|Vertex[234]|VertexAttrib(I|L|P|4N)?[1-4]|Index[dfisu])")

# Commands allowed between glBegin and glEnd (GL 3.3 compatibility section 2.6.3).
ALLOWED_IN_BEGIN = re.compile(
    r"^gl(Begin$|End$|Vertex[234]|VertexP[234]|Color[34]|ColorP[34]|SecondaryColor3|SecondaryColorP3|Normal3|NormalP3|"
    r"TexCoord[1-4]|TexCoordP[1-4]|MultiTexCoord[1-4]|MultiTexCoordP[1-4]|FogCoord[df]|Index[dfisu]|EdgeFlagv?$|"
    r"VertexAttrib(I|L|P|4N)?[1-4]|EvalCoord|EvalPoint|Material|ArrayElement|CallList)")

PROXY_TEXIMAGE = {"glTexImage1D", "glTexImage2D", "glTexImage3D", "glCompressedTexImage1D", "glCompressedTexImage2D",
                  "glCompressedTexImage3D"}


def base_type(ptype):
    """'const GLfloat *' -> 'GLfloat' (void -> uint8_t)."""
    t = ptype.replace("const", "").replace("*", "").strip()
    return "uint8_t" if t == "void" else t


def list_recorder(cmd, info, api_name):
    """C++ recorder for a compiled command, or None if its pointer sizes are unknown."""
    params, lens = info["params"], info["lens"]
    names = [n for _, n in params]
    pre, captures, args, scope = [], [], [], None
    for (ptype, pname), length in zip(params, lens):
        if "*" not in ptype:
            captures.append(pname)
            args.append(pname)
            continue
        if ptype.count("*") > 1:
            return None
        m = re.match(r"COMPSIZE\((.*)\)$", length or "")
        comp = [x for x in m.group(1).split(",") if x] if m else None
        if comp is not None and len(comp) >= 3 and comp[0] == "format" and comp[1] == "type":
            dims = comp[2:] + ["1"] * (3 - len(comp[2:]))
            pre.append("wmgl::list::Pixels px_%s = wmgl::list::capturePixels(ctx, %s, format, type, %s, %s);" % (
                pname, pname, ", ".join(dims), "true" if len(comp) == 5 else "false"))
            captures.append("px_%s = std::move(px_%s)" % (pname, pname))
            args.append("static_cast<%s>(px_%s.data())" % (ptype, pname))
            scope = "px_" + pname
            continue
        if comp is not None and comp in (["width", "height"], []):
            dims = "width, height" if comp else "32, 32"
            pre.append("wmgl::list::Pixels px_%s = wmgl::list::captureBitmap(ctx, %s, %s);" % (pname, pname, dims))
            captures.append("px_%s = std::move(px_%s)" % (pname, pname))
            args.append("static_cast<%s>(px_%s.data())" % (ptype, pname))
            scope = "px_" + pname
            continue
        if length == "imageSize":  # compressed images: client memory or the unpack buffer
            pre.append("wmgl::list::Pixels px_%s = wmgl::list::captureBytes(ctx, %s, imageSize);" % (pname, pname))
            captures.append("px_%s = std::move(px_%s)" % (pname, pname))
            args.append("static_cast<%s>(px_%s.data())" % (ptype, pname))
            scope = "px_" + pname
            continue
        if comp == ["pname"]:
            count = "wmgl::list::pnameCount(pname)"
        elif comp == ["n", "type"]:
            count = "wmgl::list::callListsBytes(n, type)"
        elif comp is None and length and re.match(r"^[A-Za-z0-9_*]+$", length) and \
                all(tok.isdigit() or tok in names for tok in length.split("*")):
            count = "(%s)" % length  # a negative count captures nothing (the command reports it when run)
        else:
            return None
        t = base_type(ptype)
        pre.append("std::vector<%s> v_%s; if (%s && %s > 0) v_%s.assign(reinterpret_cast<const %s*>(%s), "
                   "reinterpret_cast<const %s*>(%s) + static_cast<size_t>(%s));"
                   % (t, pname, pname, count, pname, t, pname, t, pname, count))
        pre.append("const bool null_%s = %s == nullptr;" % (pname, pname))
        captures.append("v_%s = std::move(v_%s)" % (pname, pname))
        captures.append("null_%s" % pname)
        args.append("null_%s ? nullptr : reinterpret_cast<%s>(v_%s.data())" % (pname, ptype, pname))
    sig = ", ".join(["wmgl::Context& ctx"] + ["%s %s" % p for p in params])
    body = []
    if cmd in PROXY_TEXIMAGE:
        body.append("    if (wmgl::list::isProxyTarget(target)) return true;  // proxy queries run immediately")
    body += ["    " + line for line in pre]
    # (GL parameter names include "c": the closure's context is "wmgl_ctx".)
    call = "wmgl::api::%s(wmgl_ctx%s)" % (api_name, "".join(", " + a for a in args))
    inner = ("wmgl::list::UnpackScope scope(wmgl_ctx, %s.unpack); %s;" % (scope, call)) if scope else call + ";"
    # glCallList(s) inside a compiled glBegin add the called list's vertices there.
    emits = ", true" if cmd in ("glCallList", "glCallLists") else ""
    body.append("    return wmgl::list::record(ctx, [%s](wmgl::Context& wmgl_ctx) mutable { %s }%s);" % (
        ", ".join(captures), inner, emits))
    return "bool record_%s(%s) {\n%s\n}\n" % (api_name, sig, "\n".join(body))


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

    # Display list recorders for the compiled commands.
    recorders = {}
    uncompiled = []
    for impl in impls:
        info = commands[impl]
        version = info["version"]
        if version is None or version > (2, 1) or impl in NOT_COMPILED or SELF_RECORDED.match(impl):
            continue
        if impl.startswith("glGet") or impl.startswith("glIs") or info["ret"] != "void":
            continue
        code = list_recorder(impl, info, api_name(impl))
        if code is None:
            uncompiled.append(impl)
            continue
        recorders[impl] = code
    with open(os.path.join(args.out, "gl_list.gen.cpp"), "w") as out:
        out.write("// Generated by tools/gen/gl_gen.py. Do not edit.\n")
        out.write("// Display list recorders: each copies its arguments into a closure that\n")
        out.write("// calls the implementation when the list executes (core/compat/display_list.h).\n\n")
        out.write('#include <utility>\n#include <vector>\n\n#include "gl_api.gen.h"\n#include "gl_list.gen.h"\n')
        out.write('#include "core/compat/display_list.h"\n#include "core/context.h"\n\nnamespace wmgl::list {\n\n')
        for impl in sorted(recorders):
            out.write(recorders[impl] + "\n")
        out.write("}  // namespace wmgl::list\n")
    with open(os.path.join(args.out, "gl_list.gen.h"), "w") as out:
        out.write("// Generated by tools/gen/gl_gen.py. Do not edit.\n#pragma once\n\n")
        out.write('#include "core/gl/types.h"\n\nnamespace wmgl {\nclass Context;\nnamespace list {\n\n')
        out.write("bool isProxyTarget(GLenum target);\n")
        for impl in sorted(recorders):
            out.write("bool record_%s(%s);\n" % (api_name(impl), ", ".join(
                ["wmgl::Context& ctx"] + ["%s %s" % p for p in commands[impl]["params"]])))
        out.write("\n}  // namespace list\n}  // namespace wmgl\n")

    with open(os.path.join(args.out, "gl_entry.gen.cpp"), "w") as out:
        out.write("// Generated by tools/gen/gl_gen.py. Do not edit.\n\n")
        out.write('#include "gl_api.gen.h"\n#include "gl_list.gen.h"\n#include "core/compat/immediate.h"\n')
        out.write('#include "core/context.h"\n#include "core/proc_table.h"\n#include "core/stats.h"\n\nnamespace {\n\n')
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
            # Objects Metal autoreleases during the call go when it returns
            # (declared first: drained after the lock is released).
            out.write("    wmgl::CallPool callPool;\n")
            # Only this call's errors log its arguments (below).
            out.write("    ctx->logArguments = false;\n")
            # CPU time inside GL calls (WINEMETALGL_STATS, core/stats.h).
            out.write('    wmgl::stats::CallTimer statsTimer("%s");\n' % cmd)
            # Every command runs under its share group's lock: contexts on
            # different threads may use the same shared objects.
            out.write("    wmgl::ShareLock shareLock(*ctx);\n")
            out.write("    statsTimer.locked();\n")
            out.write('    ctx->currentCommand = "%s";\n' % cmd)
            if not ALLOWED_IN_BEGIN.match(impl) and not ALLOWED_IN_BEGIN.match(cmd):
                # Most commands are invalid between glBegin and glEnd.
                out.write("    if (__builtin_expect(ctx->immediate != nullptr, 0) && ctx->immediate->active) {\n")
                out.write("        ctx->setError(GL_INVALID_OPERATION);\n        return%s;\n    }\n" % (
                    (" " + dflt) if dflt else ""))
            if impl in recorders:
                out.write("    if (__builtin_expect(ctx->listCompiler != nullptr, 0) && !wmgl::list::record_%s(%s)) return;\n" % (
                    api_name(impl), call))
            # A logged error shows the call's arguments (Context::logArguments).
            fmt, fargs = [], []
            for t, n in params:
                t0 = t.replace("const ", "").strip()
                if "*" in t or t0 in ("GLsync", "GLDEBUGPROC", "GLDEBUGPROCARB", "GLDEBUGPROCKHR", "GLDEBUGPROCAMD", "GLVULKANPROCNV", "GLeglImageOES", "GLeglClientBufferEXT"):
                    fmt.append(n + " %p"); fargs.append("reinterpret_cast<const void*>(%s)" % n)
                elif t0 in ("GLenum", "GLbitfield"):
                    fmt.append(n + " 0x%x"); fargs.append("static_cast<unsigned>(%s)" % n)
                elif t0 in ("GLfloat", "GLdouble", "GLclampf", "GLclampd"):
                    fmt.append(n + " %g"); fargs.append("static_cast<double>(%s)" % n)
                else:
                    fmt.append(n + " %lld"); fargs.append("static_cast<long long>(%s)" % n)
            log = '        wmgl::logCallArguments(*ctx, "%s(%s)"%s);\n' % (cmd, ", ".join(fmt), "".join(", " + a for a in fargs))
            if dflt:
                out.write("    auto result = wmgl::api::%s(%s);\n" % (api_name(impl), call))
                out.write("    if (__builtin_expect(ctx->logArguments, 0)) {\n" + log + "    }\n    return result;\n}\n")
            else:
                out.write("    wmgl::api::%s(%s);\n" % (api_name(impl), call))
                out.write("    if (__builtin_expect(ctx->logArguments, 0)) {\n" + log + "    }\n}\n")
        out.write("\n}  // namespace\n\nnamespace wmgl {\n\nconst ProcEntry kProcTable[] = {\n")
        for cmd in names:
            out.write('    {"%s", reinterpret_cast<void*>(&e_%s)},\n' % (cmd, cmd))
        out.write("};\nconst unsigned kProcTableSize = %d;\n\n}  // namespace wmgl\n" % len(names))

    with open(os.path.join(args.out, "gl_pfn.gen.h"), "w") as out:
        out.write("// Generated by tools/gen/gl_gen.py. Do not edit.\n#pragma once\n\n")
        out.write('#include "core/gl/types.h"\n\n')
        for cmd in names:
            decl = ", ".join("%s %s" % (t, n) for t, n in commands[cmd]["params"]) or "void"
            out.write("typedef %s (*PFN_%s)(%s);\n" % (commands[cmd]["ret"], cmd, decl))

    with open(os.path.join(args.out, "gl_commands.gen.json"), "w") as out:
        json.dump({cmd: {"source": selected[cmd], "impl": impl_of(cmd)} for cmd in names}, out, indent=1, sort_keys=True)
    print("gl_gen: %d entry points, %d implementations, %d display list recorders" % (
        len(names), len(impls), len(recorders)))
    if uncompiled:
        print("gl_gen: not compiled into display lists (unknown argument sizes): " + " ".join(sorted(uncompiled)))


if __name__ == "__main__":
    main()
