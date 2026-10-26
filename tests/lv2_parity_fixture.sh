#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# tests/lv2_parity.py end to end on this tree's fixture bundles (build/lv2): the host fixture (a latency it reports
# after its first activate), the worker-gain fixture (a worker) and the midi-echo synth (MIDI in, no audio in),
# through mod-host and through this tree's omx-clap-host at 48 and 96 kHz. Every one must come out IDENTICAL. Then the
# host fixture at 96 kHz LV2_PARITY_REPEATS times (default 10), IDENTICAL every time. Last, the first run again with
# LV2_PARITY_SABOTAGE=offset (the adapter's recordings one sample late, in a copy) must come out with no IDENTICAL at
# all. Exit 0 only when all three held. Needs mod-host (MOD_HOST, default mod-host on PATH).
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
mod_host=${MOD_HOST:-$(command -v mod-host)}
uris=urn:openmixer:test:host-fixture,urn:openmixer:test:worker-gain,urn:openmixer:test:midi-echo#synth
out=$(mktemp -d "${TMPDIR:-/tmp}/omx-lv2-parity.XXXXXX")
trap 'rm -rf "$out"' EXIT
fails=0
t() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; fails=$((fails + 1)); fi; }

for f in "$root/omx-clap-host" "$here/jack_parity" "$root/build/lv2/omx-host-fixture.lv2/manifest.ttl" "$mod_host"; do
    [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
done
pass() { # <dir> [sabotage]: probe, run, classify
    LV2_PATH=$root/build/lv2 python3 "$here/lv2_parity.py" probe --uris "$uris" --out "$1" >/dev/null || return 1
    LV2_PARITY_SABOTAGE=${2:-} python3 "$here/lv2_parity.py" run --out "$1" --rates 48000,96000 --lv2-path "$root/build/lv2" \
        --omx-clap-host "$root/omx-clap-host" --mod-host "$mod_host" --keep none >"$1/run.log" 2>&1 || return 1
    python3 "$here/lv2_parity.py" classify --out "$1" >/dev/null
}
classes() { cut -f3 "$1/parity.tsv" | tail -n +2 | sort | uniq -c | tr -s ' ' | tr '\n' ';'; }

pass "$out/real"
t "three fixtures at two rates, six measurements" "[ \$(tail -n +2 '$out/real/parity.tsv' | wc -l) = 6 ]"
t "every measurement IDENTICAL ($(classes "$out/real"))" "! tail -n +2 '$out/real/parity.tsv' | cut -f3 | grep -vqx IDENTICAL"
t "none of them silent" "! tail -n +2 '$out/real/parity.tsv' | cut -f4 | grep -qx silent"
# the host fixture at 96 kHz again and again, each run a fresh pair of hosts: IDENTICAL every time. A client cycle that
# passes dry before its instance's audio role is named (#104: 7 runs of 20 on fecd7d7, its two instances 0.33 apart)
# differs from mod-host's only on the runs where JACK's threads collide, so one run alone is no proof
repeats=${LV2_PARITY_REPEATS:-10}
repeat_bad=0
for i in $(seq 1 "$repeats"); do
    d=$out/repeat.$i
    LV2_PATH=$root/build/lv2 python3 "$here/lv2_parity.py" probe --uris urn:openmixer:test:host-fixture --out "$d" >/dev/null \
        && python3 "$here/lv2_parity.py" run --out "$d" --rates 96000 --lv2-path "$root/build/lv2" \
               --omx-clap-host "$root/omx-clap-host" --mod-host "$mod_host" --keep none >"$d/run.log" 2>&1 \
        && python3 "$here/lv2_parity.py" classify --out "$d" >/dev/null
    c=$(tail -n +2 "$d/parity.tsv" 2>/dev/null | cut -f3,5)
    [ "${c%%	*}" = IDENTICAL ] || { repeat_bad=$((repeat_bad + 1)); echo "     repeat $i: ${c:-no measurement}"; }
done
t "the host fixture at 96000, $repeats runs: every one IDENTICAL ($repeat_bad not)" "[ $repeat_bad = 0 ]"
pass "$out/sabotage" offset
t "sabotage (the adapter one sample late): no IDENTICAL ($(classes "$out/sabotage"))" \
  "[ \$(tail -n +2 '$out/sabotage/parity.tsv' | wc -l) = 6 ] && ! tail -n +2 '$out/sabotage/parity.tsv' | cut -f3 | grep -qx IDENTICAL"
[ "$fails" = 0 ] || { echo "run log:"; sed 's/^/     /' "$out/real/run.log"; }
[ "$fails" = 0 ] && echo "lv2 parity fixture ok" || echo "lv2 parity fixture FAILED ($fails)"
[ "$fails" = 0 ]
