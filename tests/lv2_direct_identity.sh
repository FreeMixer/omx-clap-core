#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# Every bundle of LV2_DIRS through lv2/tests/lv2_direct_identity, one bundle per process so a plugin that crashes is one
# line, and when given a second recording the two compared (docs/fast-path.md). Prints; exit 1 on any plugin that differs
# while each side was deterministic.
#
#   tests/lv2_direct_identity.sh record <binary> <out.tsv>
#   tests/lv2_direct_identity.sh compare <before.tsv> <after.tsv>
set -u
case ${1:-} in
record)
    bin=$2 out=$3
    : >"$out"
    for b in ${LV2_DIRS:-/usr/lib64/lv2}/*.lv2; do
        [ -f "$b/manifest.ttl" ] || continue
        env -u DISPLAY -u WAYLAND_DISPLAY LV2_PATH="${LV2_DIRS:-/usr/lib64/lv2}" timeout -k 5 "${DIRECT_TIMEOUT:-120}" "$bin" "$b" 2>/dev/null \
            | grep -a '^[a-z][a-z0-9+.-]*:[^ ]* ' >>"$out" || echo "${b##*/} bundle-failed - 0 0" >>"$out"
    done
    ;;
compare)
    python3 -I - "$2" "$3" <<'PY'
import sys
def load(p):
    r = {}
    for line in open(p, errors="replace"):
        f = line.split()
        if len(f) == 5:
            r[f[0]] = f[1:]
    return r
a, b = load(sys.argv[1]), load(sys.argv[2])
rendered = same = 0
flaky, diff, gone, scribble, stale = [], [], [], [], []
for u in sorted(set(a) | set(b)):
    x, y = a.get(u), b.get(u)
    if not x or not y or x[1] == "-" or y[1] == "-":
        if (x and x[1] != "-") != (y and y[1] != "-"):
            gone.append(u)
        continue
    rendered += 1
    if x[0] != x[1] or y[0] != y[1]:
        flaky.append(u)
    elif x[0] != y[0]:
        diff.append(u)
    else:
        same += 1
    if int(y[2]):
        scribble.append(u)
    if int(y[3]):
        stale.append(u)
print(f"rendered {rendered}: bit-identical {same}, not deterministic {len(flaky)}, DIFFER {len(diff)}, renders on one side only {len(gone)}")
print(f"wrote their input buffer: {len(scribble)}; left an output sample unwritten: {len(stale)}")
for name, l in (("DIFFER", diff), ("ONE-SIDE", gone), ("SCRIBBLE", scribble), ("STALE", stale), ("FLAKY", flaky)):
    for u in l:
        print(name, u)
sys.exit(1 if diff or gone else 0)
PY
    ;;
*) echo "usage: lv2_direct_identity.sh record <binary> <out.tsv> | compare <before> <after>" >&2; exit 2 ;;
esac
