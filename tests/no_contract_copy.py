#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The parameter units and the JSON string writer are omx-contract's, and this tree keeps no copy of either: the units
# are <omxcontract/omx_param_units.h>'s OMX_PARAM_UNITS(X) (omx_clap_ext.h names that table and spells no row), and a
# JSON string is written by <omxcontract/omx_json_write.h> (omx_json_escape_byte, omx_json_write_string,
# omx_json_string), never by an escape loop of a file's own. A copy that comes back drifts from the sheet the other
# consumers read, or spells an escape the one writer does not.
#
#   python3 tests/no_contract_copy.py [<tree>]     red on any copy under src/, lv2/ and tools/ (default: this tree)
#   python3 tests/no_contract_copy.py --self-test  each kind of copy, planted in a scratch copy of the tree, must be red
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# what only a copy holds, in a file of any name
MARKS = [
    # a units table spelled out: a _UNITS(X) define whose body opens with a row, not a name (the alias
    # `#define OMX_CLAP_PARAM_UNITS(X) OMX_PARAM_UNITS(X)` opens with a name)
    (re.compile(r"^#\s*define\s+\w*_UNITS\(X\)(?:[ \t]*\\\n|[ \t])*X\(", re.M), "a units table of its own"),
    # one row of the sheet, by the words only the units sheet holds
    (re.compile(r'\bX\(\s*(?:0x[0-9A-Fa-f]+|\d+)[uU]?\s*,\s*"(?:db|hz|semitone12TET|midiNote|dbfs|lufs)"\s*\)'),
     "a row of the units sheet"),
    # the \u00XX escape of a control byte, formatted by hand
    (re.compile(r"\\\\u%0\d*[xX]"), "a \\u escape loop of its own"),
    # a quote or a backslash escaped by hand, through a format
    (re.compile(r'"\\\\%c"'), "a quote or backslash escaped by hand"),
]
# where the marks are looked for: the sources and the headers; the fuzz harnesses render the writer's format on
# purpose, as its oracle, and the tests plant copies on purpose
SCANNED = ("src", "lv2", "tools")
SUFFIXES = (".c", ".h")


def tracked(tree):
    p = subprocess.run(["git", "-C", tree, "ls-files"], capture_output=True, text=True)
    if p.returncode == 0 and p.stdout:
        return p.stdout.splitlines()
    out = []
    for base, _, files in os.walk(tree):
        out += [os.path.relpath(os.path.join(base, f), tree) for f in files]
    return out


def copies(tree):
    found = []
    for rel in sorted(tracked(tree)):
        if not rel.startswith(SCANNED) or not rel.endswith(SUFFIXES):
            continue
        try:
            with open(os.path.join(tree, rel), encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        for pattern, what in MARKS:
            m = pattern.search(text)
            if m:
                found.append("%s:%d: holds %s" % (rel, text.count("\n", 0, m.start()) + 1, what))
    return found


def self_test():
    plants = [
        ("src/units_copy.h", "#define OLD_UNITS(X) \\\n    X(0, \"none\") X(1, \"s\") X(2, \"ms\")\n",
         "a units table of its own"),
        ("lv2/clap/one_row.h", "static const struct { unsigned c; const char *w; } u[] = { X(13u, \"db\") };\n",
         "a row of the units sheet"),
        ("src/json_copy.c", "static void esc(char *o, unsigned c) { snprintf(o, 8, \"\\\\u%04x\", c); }\n",
         "a \\u escape loop of its own"),
        ("tools/json_copy.c", "static void esc(FILE *f, int c) { fprintf(f, \"\\\\%c\", c); }\n",
         "a quote or backslash escaped by hand"),
    ]
    failed = 0
    work = tempfile.mkdtemp(prefix="no-contract-copy.")
    try:
        tree = os.path.join(work, "t")
        for d in SCANNED:
            if os.path.isdir(os.path.join(ROOT, d)):
                shutil.copytree(os.path.join(ROOT, d), os.path.join(tree, d),
                                ignore=shutil.ignore_patterns("*.o", "*.d", "*.so*", "__pycache__"))
        before = copies(tree)
        if before:
            print("FAIL self-test: the copy of the tree as it is is red: %s" % before)
            return 1
        for rel, text, what in plants:
            path = os.path.join(tree, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(text)
            hits = [c for c in copies(tree) if c.startswith(rel + ":")]
            if any(c.endswith(what) for c in hits):
                print("ok   self-test: %s planted as %s is red" % (what, rel))
            else:
                print("FAIL self-test: %s planted as %s is not seen (%s)" % (what, rel, hits))
                failed += 1
            os.unlink(path)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if not failed:
        print("no_contract_copy self-test: %d kinds of copy planted, each red" % len(plants))
    return 1 if failed else 0


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    tree = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else ROOT
    found = copies(tree)
    for c in found:
        print("FAIL no contract copy: " + c)
    if found:
        return 1
    print("ok   no copy of omx-contract's units table or JSON writer: %d files scanned"
          % len([f for f in tracked(tree) if f.startswith(SCANNED) and f.endswith(SUFFIXES)]))
    return 0


sys.exit(main())
