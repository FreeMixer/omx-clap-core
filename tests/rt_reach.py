#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# rt_reach.py: the text of every function the roots reach, for the real-time scan of the host's chain cycle (#104,
# make test-chain-host): each function the given C files define (src/effects.c, the core's src/clap_host.c and
# src/clap_chain.c, and the static inline functions of the headers they run on the audio thread) is found by its definition at the start of a line and its body to the closing
# brace at the start of a line; a call is a name followed by "(" in a body. From the roots, every function so defined
# that a reached body calls is reached too, each once; a call of a name a given file #defines as another name alone
# (clap_stage.h's omx_clap_* words for hosted_stage.h's functions) is a call of that one. A function no given file
# defines (JACK's, libc's, omx-dsp's) is named on the "# outside:" line, not followed.
#
#   rt_reach.py <out> <root>[,<root>...] <file.c|file.h> ...
#
# <out> gets the reached functions' text, each headed "/* <name> (<file>) */", the reached names on a "# reached:" line
# and the calls into no given file on a "# outside:" line. Exit 0; 2 usage or a root no file defines.
import re, sys

# a function's head: a type and a name at the start of a line, then its parameters, on that line or the next ones
DEF = re.compile(r'^(?!typedef\b)[A-Za-z_][\w \t\*]*?\b([A-Za-z_]\w*)\s*\(')
CALL = re.compile(r'\b([A-Za-z_]\w*)\s*\(')
# a name that is another's: "#define omx_clap_queue_pending omx_hosted_queue_pending", a call of it is a call of that
ALIAS = re.compile(r'^#\s*define\s+([A-Za-z_]\w*)\s+([A-Za-z_]\w*)\s*(?:$|//|/\*)')
KEYWORDS = {'if', 'for', 'while', 'switch', 'return', 'sizeof', 'case', 'do', 'else', '_Alignof', '__atomic_load_n',
            '__atomic_store_n', '__typeof__', 'defined'}


def functions(path):
    """name -> body text of every function defined at the start of a line in `path`"""
    lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
    found, i = {}, 0
    while i < len(lines):
        m = DEF.match(lines[i])
        # a definition: its head ends the line, or the line after it opens the body; a prototype ends in ';'
        if m:
            name = m.group(1)
            j = i
            while j < len(lines) and '{' not in lines[j] and not lines[j].rstrip().endswith(';'):
                j += 1
            if j < len(lines) and '{' in lines[j] and name not in KEYWORDS:
                k = j
                while k < len(lines) and lines[k] != '}':
                    k += 1
                found.setdefault(name, '\n'.join(lines[i:k + 1]))
                i = k + 1
                continue
        i += 1
    return found


def aliases(path):
    """name -> the name it is defined as, for every one-name #define in `path`"""
    found = {}
    for line in open(path, encoding='utf-8', errors='replace'):
        m = ALIAS.match(line)
        if m:
            found.setdefault(m.group(1), m.group(2))
    return found


def main(argv):
    if len(argv) < 4:
        print(__doc__ or 'usage: rt_reach.py <out> <root>[,<root>...] <file> ...', file=sys.stderr)
        return 2
    out, roots, files = argv[1], argv[2].split(','), argv[3:]
    defined, where, alias = {}, {}, {}
    for f in files:
        for name, body in functions(f).items():
            if name not in defined:
                defined[name], where[name] = body, f
        for name, other in aliases(f).items():
            alias.setdefault(name, other)
    missing = [r for r in roots if r not in defined]
    if missing:
        print('rt_reach.py: no file defines ' + ', '.join(missing), file=sys.stderr)
        return 2
    reached, outside, todo = [], set(), list(roots)
    while todo:
        name = todo.pop(0)
        if name in reached:
            continue
        reached.append(name)
        # the calls of the body, its head left out: the head names the function itself
        body = defined[name].split('{', 1)[1] if '{' in defined[name] else ''
        for call in CALL.findall(body):
            if call in KEYWORDS:
                continue
            call = alias.get(call, call) if call not in defined else call
            if call in defined:
                if call not in reached:
                    todo.append(call)
            else:
                outside.add(call)
    with open(out, 'w', encoding='utf-8') as o:
        for name in reached:
            o.write('/* %s (%s) */\n%s\n\n' % (name, where[name], defined[name]))
        o.write('# reached: %s\n' % ' '.join(reached))
        o.write('# outside: %s\n' % ' '.join(sorted(outside)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
