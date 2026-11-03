#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# tests/chain_host_e2e.sh against a chain host that stops answering mid-run: the host is stopped (CHAIN_HOST_STALL) just
# before the line `chain 9 15 16 17`, as a host that deadlocks, or crashes with a core dump that is slow to finish, would
# be. The run must end on its own, red, and say which line the host stopped answering at; a run that waits out every
# later line's reply is a hang, and so is one that waits out even one: a host that is stopped, a zombie, gone or dumping
# its core answers no line, so the run says so as soon as it sees that, not after a reply's 30 s. The bound
# (CHAIN_STALL_SECONDS, default 90) is the green run's time (about 20 s) with room for a loaded runner, under one
# reply timeout more than that.
#
#   chain_host_stall.sh            (from the tree: make test-chain-host-stall)

set -u
here=$(cd "$(dirname "$0")" && pwd)
limit=${CHAIN_STALL_SECONDS:-90}
log=$(mktemp /tmp/omx-chain-stall.XXXXXX)
trap 'rm -f "$log"' EXIT
start=$(date +%s)
CHAIN_HOST_STALL='chain 9 15 16 17' timeout --kill-after=10 "$limit" "$here/chain_host_e2e.sh" >"$log" 2>&1
rc=$?
took=$(($(date +%s) - start))
failures=0
if [ "$rc" -eq 124 ] || [ "$rc" -eq 137 ]; then
    echo "FAIL a chain host that stops answering hangs the run: stopped after ${limit}s"
    tail -5 "$log" | sed 's/^/     /'
    failures=$((failures + 1))
else
    echo "ok   a chain host that stops answering ends the run on its own, in ${took}s (bound ${limit}s)"
    if [ "$rc" -eq 0 ]; then
        echo "FAIL and the run is green though its host stopped answering"
        failures=$((failures + 1))
    else
        echo "ok   and the run is red (exit $rc)"
    fi
fi
if grep -q -E "^FAIL the chain host stopped answering at 'chain 9 15 16 17'" "$log"; then
    echo "ok   and it names the line: $(grep -m 1 -E "^FAIL the chain host stopped answering" "$log")"
else
    echo "FAIL and it does not say which line the chain host stopped answering at"
    failures=$((failures + 1))
fi
[ "$failures" -eq 0 ]
