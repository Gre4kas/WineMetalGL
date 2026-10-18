#!/usr/bin/env python3
# Generates, from the core's GL proc table (build/<preset>/generated/
# gl_entry.gen.cpp), stubs.S (an exported arm64 stub for every GL name,
# jumping through wmglhost_procs), names.inc (the names, in table order) and
# procs.c (the table itself).
#
#   gen_stubs.py path/to/gl_entry.gen.cpp output-directory
import os
import re
import sys

source = open(sys.argv[1]).read()
start = source.index('const ProcEntry kProcTable[] = {')
names = re.findall(r'\{"(gl[A-Za-z0-9_]*)"', source[start:source.index('};', start)])
out = sys.argv[2]
with open(os.path.join(out, 'names.inc'), 'w') as f:
    for n in names:
        f.write('"%s",\n' % n)
with open(os.path.join(out, 'procs.c'), 'w') as f:
    f.write('__attribute__((visibility("default"))) void *wmglhost_procs[%d];\n' % len(names))
with open(os.path.join(out, 'stubs.S'), 'w') as f:
    f.write('.text\n')
    for i, n in enumerate(names):
        f.write('.globl _%s\n.p2align 2\n_%s:\n  adrp x16, _wmglhost_procs@PAGE\n'
                '  add x16, x16, _wmglhost_procs@PAGEOFF\n  ldr x16, [x16, #%d]\n  br x16\n' % (n, n, i * 8))
