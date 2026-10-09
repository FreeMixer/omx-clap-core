#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The stage-2 oracle's kinds (openmixer docs/design/specs/2026-10-02-lv2-as-clap-adapter.md §10.2): each kind must be
# represented by at least one fixture bundle, or the run says which is missing. This checks the fixtures only. The
# comparison itself, through mod-host, is not run here: mod-host is not installed by this tree, and the adapter does not
# yet carry the kinds (lv2/core refuses an event port, and state, presets, transport and patch: are stage 2).
#
# Exit 0 only when every kind has a fixture. Not part of `make test-lv2`: until a fixture exists for each kind, that is a gap.

set -u

here=$(cd "$(dirname "$0")" && pwd)
fixtures=$here/fixtures

# kind;the pattern a fixture carries for it;what the kind is (';' separates them: the patterns use '|')
kinds="events;AtomPort;an atom port: MIDI or another event sequence
transport;time#Position\|time:Position;time:Position read from the host's transport
state;state#interface\|state:interface;state:interface, save and restore
worker;worker:interface\|worker#interface\|worker:schedule\|worker/worker.h;worker:schedule, work() on the worker thread
patch;patch#writable\|patch:writable\|patch#Parameter\|patch:Parameter;patch: numeric parameters, the file property"

missing=""
total=0
ok=0

while IFS=';' read -r kind pattern what; do
    total=$((total + 1))
    # any file under the fixtures: the worker fixture is C, so its kind shows in the source, not the bundle
    hits=$(grep -rl -e "$pattern" "$fixtures" 2>/dev/null | sed "s,^$fixtures/,," | sort | tr '\n' ' ')
    if [ -n "$hits" ]; then
        ok=$((ok + 1))
        echo "ok      kind $kind ($what): $hits"
    else
        missing="$missing $kind"
        echo "MISSING kind $kind ($what): no fixture carries it"
    fi
done <<EOF
$kinds
EOF

echo "stage 2 kinds: $ok of $total have a fixture"
if [ -n "$missing" ]; then
    echo "missing:$missing"
    exit 1
fi
