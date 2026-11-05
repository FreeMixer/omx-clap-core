#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# plugin-hostd builds on this repository, so this repository does not build on plugin-hostd. What a pin needs is
# plugin-db's, and the protocol is plugin-hostd-protocol's. The end-to-end tests may still run behind a plugin-hostd at
# test time: a checkout's daemon and clients, or a package a CI step installs. This holds the build:
#   the spec's BuildRequires and debian/control's Build-Depends name no plugin-hostd package,
#   the Makefile asks pkg-config nothing of plugin-hostd and adds no plugin-hostd checkout's headers to a build,
#   no source includes a header only plugin-hostd installs.
#
#   no_hostd_build_dep.py <tree>       0 when the tree is clean
#   no_hostd_build_dep.py --self-test  every kind of return, planted in a scratch tree, must be found
import glob
import os
import re
import shutil
import sys
import tempfile

# a plugin-hostd package, never plugin-hostd-protocol's
PACKAGE = r"(?<![\w.-])plugin-hostd(?:-devel|-dev|-contract|-tests)?(?![\w.-])"
# what plugin-hostd itself installs; plugin-hostd/protocol.h and the headers beside it are plugin-hostd-protocol's
OWN_HEADERS = r'#\s*include\s*[<"]plugin-hostd/(?:pin|plugin_hostd_vocabulary)\.h[>"]'


def uncommented(text):
    return [line for line in text.splitlines() if not line.lstrip().startswith("#")]


def spec_returns(path):
    found = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            m = re.match(r"\s*BuildRequires:\s*(.*)", line)
            if m and re.search(PACKAGE, re.sub(r"pkgconfig\((.*?)\)", r"\1", m.group(1))):
                found.append(line.strip())
    return found


def control_returns(path):
    with open(path, encoding="utf-8") as f:
        source = f.read().split("\n\n")[0]
    found = []
    for m in re.finditer(r"^Build-Depends[\w-]*:(.*(?:\n[ \t].*)*)", source, re.M):
        found += [d.strip() for d in m.group(1).split(",") if re.search(PACKAGE, d)]
    return found


def makefile_returns(path):
    with open(path, encoding="utf-8") as f:
        lines = uncommented(f.read())
    found = []
    for line in lines:
        if re.search(r"PKG_CONFIG\)[^)]*" + PACKAGE, line) or re.search(r"pkg-config[^)|;]*" + PACKAGE, line):
            found.append(line.strip())
        if re.search(r"-I\s*\$\(PLUGIN_HOSTD_DIR\)", line):
            found.append(line.strip())
    return found


def source_returns(root):
    found = []
    for pattern in ("src/**/*.[ch]", "lv2/**/*.[ch]", "tests/**/*.[ch]", "fuzz/**/*.[ch]"):
        for path in glob.glob(os.path.join(root, pattern), recursive=True):
            with open(path, encoding="utf-8", errors="replace") as f:
                for n, line in enumerate(f, 1):
                    if re.search(OWN_HEADERS, line):
                        found.append("%s:%d: %s" % (os.path.relpath(path, root), n, line.strip()))
    return found


def check(root):
    found = []
    for spec in glob.glob(os.path.join(root, "packaging", "*.spec")):
        found += spec_returns(spec)
    found += control_returns(os.path.join(root, "debian", "control"))
    found += makefile_returns(os.path.join(root, "Makefile"))
    found += source_returns(root)
    return found


def self_test():
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    arms = [
        ("packaging/omx-clap-host.spec", "BuildRequires: gcc\n", "BuildRequires: gcc\nBuildRequires: pkgconfig(plugin-hostd) >= 0.5.0\n",
         "a pkgconfig BuildRequires"),
        ("packaging/omx-clap-host.spec", "BuildRequires: gcc\n", "BuildRequires: gcc\nBuildRequires: plugin-hostd-devel\n",
         "a package BuildRequires"),
        ("debian/control", "               pkgconf,\n", "               plugin-hostd-dev (>= 0.5.0~),\n               pkgconf,\n",
         "a Build-Depends"),
        ("Makefile", "PIN_CFLAGS = $(shell $(PKG_CONFIG) --cflags plugin-db)\n",
         "PIN_CFLAGS = $(shell $(PKG_CONFIG) --cflags plugin-hostd)\n", "the Makefile asking pkg-config"),
        ("Makefile", "PIN_CFLAGS = $(shell $(PKG_CONFIG) --cflags plugin-db)\n",
         "PIN_CFLAGS = -I$(PLUGIN_HOSTD_DIR)/include\n", "a plugin-hostd checkout's headers"),
        ("src/layout_pin.h", "#include <plugin-db/pin.h>\n", "#include <plugin-hostd/pin.h>\n", "an include of its pin.h"),
    ]
    failed = 0
    if check(here):
        print("FAIL the control: this tree already holds a build dependency on plugin-hostd", check(here))
        failed = 1
    for rel, before, after, what in arms:
        with tempfile.TemporaryDirectory() as tmp:
            for d in ("packaging", "debian", "src"):
                os.makedirs(os.path.join(tmp, d))
            for f in ("packaging/omx-clap-host.spec", "debian/control", "Makefile", "src/layout_pin.h"):
                shutil.copy(os.path.join(here, f), os.path.join(tmp, f))
            path = os.path.join(tmp, rel)
            with open(path, encoding="utf-8") as f:
                text = f.read()
            if before not in text:
                print("FAIL %s: the arm no longer applies" % what)
                failed = 1
                continue
            with open(path, "w", encoding="utf-8") as f:
                f.write(text.replace(before, after, 1))
            if check(tmp):
                print("ok   %s is found" % what)
            else:
                print("FAIL %s is not found" % what)
                failed = 1
    # the protocol's package and headers are not plugin-hostd's: a check that names them too would be a check nobody can pass
    with tempfile.TemporaryDirectory() as tmp:
        spec = os.path.join(tmp, "s.spec")
        with open(spec, "w", encoding="utf-8") as f:
            f.write("BuildRequires: plugin-hostd-protocol-devel >= 0.2.0\n")
        if spec_returns(spec):
            print("FAIL plugin-hostd-protocol is taken for plugin-hostd")
            failed = 1
    return failed


def main():
    if sys.argv[1:] == ["--self-test"]:
        sys.exit(self_test())
    if len(sys.argv) != 2:
        sys.exit(__doc__ or "usage: no_hostd_build_dep.py <tree> | --self-test")
    found = check(sys.argv[1])
    for line in found:
        print("FAIL a build dependency on plugin-hostd: " + line)
    if found:
        sys.exit(1)
    print("ok   nothing is built from plugin-hostd: the pin is plugin-db's")


if __name__ == "__main__":
    main()
