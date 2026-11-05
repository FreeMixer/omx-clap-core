#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The protocol is plugin-hostd-protocol's, and this tree keeps no copy of it: none of its tables (protocol/*.json) or
# its headers (protocol.h, search_paths.h, the host verb and vocabulary renders) is here under any name, no file of
# this tree declares what protocol.h declares (its guard, the macros of its tables, the search lists), and the one
# rendered verb table is src/omx_clap_core_host_verbs.h, the render of data/verbs.json, which tests/contract-dir-check.sh
# holds to its declaration. A copy that comes back drifts from the protocol the host is built against.
#
#   python3 tests/no_protocol_copy.py [<tree>]     red on any copy in the tree (default: this one)
#   python3 tests/no_protocol_copy.py --self-test  each kind of copy, planted in a scratch copy of the tree, must be red
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# plugin-hostd-protocol's tables, by the paths they have there
MOVED = [
    "protocol/plugin-hostd.json", "protocol/mod-host.json", "protocol/plugin-hostd.schema.json",
    "protocol/plugin-hostd-protocol.json",
]
# plugin-hostd-protocol's headers, by name, anywhere under the scanned directories
NAMED = {
    "protocol.h", "search_paths.h", "plugin_hostd_protocol_host_verbs.h", "plugin_hostd_protocol_vocabulary.h",
    "plugin_hostd_host_verbs.h",
}
# what only a copy of a declaration holds, in a file of any name; the one file each own render is allowed in
MARKS = [
    (re.compile(r"^#\s*define\s+PLUGIN_HOSTD_PROTOCOL_H\b", re.M), "protocol.h's include guard", None),
    (re.compile(r"^#\s*define\s+PHD_(VERBS|EVENTS|ERRORS|HOST_VERBS|INSTANCE_VERBS|CONF_STRINGS)\(X\)", re.M),
     "a table of protocol.h", None),
    (re.compile(r"^#\s*define\s+PHD_DEFAULT_(LV2|CLAP)_PATH\b", re.M), "a search list of search_paths.h", None),
    (re.compile(r"^#\s*define\s+OMX_VERB_[A-Z0-9_]+_FMT\b", re.M), "a rendered verb table",
     "src/omx_clap_core_host_verbs.h"),
    (re.compile(r'"kind":\s*"verbs"'), "a verbs item of the contract", "data/verbs.json"),
    (re.compile(r'^\s*"protocol":\s*"(plugin-hostd|mod-host|plugin-hostd-protocol)",?\s*$', re.M),
     "a protocol JSON", None),
]
# where the copies are looked for: the sources, the headers, the tables, the tools, the tests and the fuzz harnesses
SCANNED = ("src", "include", "lv2", "tools", "tests", "data", "protocol", "fuzz")
SUFFIXES = (".c", ".h", ".json", ".py", ".in", ".sh", ".mjs")
SKIP = {os.path.join("tests", "no_protocol_copy.py")}


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
    files = sorted(tracked(tree))
    for rel in MOVED:
        if os.path.exists(os.path.join(tree, rel)):
            found.append("%s: plugin-hostd-protocol's table, here" % rel)
    for rel in files:
        if rel in SKIP or not rel.startswith(SCANNED):
            continue
        if os.path.basename(rel) in NAMED:
            found.append("%s: plugin-hostd-protocol's header, here by its name" % rel)
        if not rel.endswith(SUFFIXES):
            continue
        try:
            with open(os.path.join(tree, rel), encoding="utf-8", errors="replace") as f:
                text = f.read()
        except OSError:
            continue
        for pattern, what, own in MARKS:
            if rel != own and pattern.search(text):
                found.append("%s: holds %s" % (rel, what))
    return found


def self_test():
    plants = [
        ("protocol/plugin-hostd.json", '{\n  "protocol": "plugin-hostd",\n  "version": 1\n}\n'),
        ("include/plugin-hostd/protocol.h", "#ifndef PLUGIN_HOSTD_PROTOCOL_H\n#define PLUGIN_HOSTD_PROTOCOL_H\n#endif\n"),
        ("src/verbs_copy.h", "#define PHD_VERBS(X) \\\n    X(A, \"a\", \"\", \"\", \"\")\n"),
        ("lv2/clap/old_verbs.h", '#define OMX_VERB_CHAIN_FMT "chain %i %i ..."\n'),
        ("data/more.json", '{\n  "V": {\n    "kind": "verbs",\n    "verbs": {}\n  }\n}\n'),
        ("tools/paths.h", '#define PHD_DEFAULT_LV2_PATH "/usr/lib/lv2"\n'),
        ("tests/search_paths.h", "/* empty: the name alone is the copy */\n"),
        ("src/mod-host.json", '{\n  "protocol": "mod-host"\n}\n'),
    ]
    failed = 0
    work = tempfile.mkdtemp(prefix="no-protocol-copy.")
    try:
        tree = os.path.join(work, "t")
        for d in SCANNED:
            if os.path.isdir(os.path.join(ROOT, d)):
                shutil.copytree(os.path.join(ROOT, d), os.path.join(tree, d),
                                ignore=shutil.ignore_patterns("*.o", "*.d", "*.so*", "corpus", "__pycache__"))
        before = copies(tree)
        if before:
            print("FAIL self-test: the copy of the tree as it is is red: %s" % before)
            return 1
        for rel, text in plants:
            path = os.path.join(tree, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w") as f:
                f.write(text)
            if any(c.startswith(rel + ":") for c in copies(tree)):
                print("ok   self-test: a copy planted as %s is red" % rel)
            else:
                print("FAIL self-test: a copy planted as %s is not seen" % rel)
                failed += 1
            os.unlink(path)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    if not failed:
        print("no_protocol_copy self-test: %d kinds of copy planted, each red" % len(plants))
    return 1 if failed else 0


def main():
    if sys.argv[1:] == ["--self-test"]:
        return self_test()
    tree = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else ROOT
    found = copies(tree)
    for c in found:
        print("FAIL no protocol copy: " + c)
    if found:
        return 1
    print("ok   no copy of plugin-hostd-protocol's declarations: %d tables and %d header names absent, %d files scanned"
          % (len(MOVED), len(NAMED),
             len([f for f in tracked(tree) if f.startswith(SCANNED) and f.endswith(SUFFIXES)])))
    return 0


sys.exit(main())
