#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The sabotage of the host's chain guards (#104, docs/chain.md): each arm copies the sources into
# build/sabotage-chain-host-<arm>, breaks one guard of src/effects.c or src/main.c there with one sed, builds and runs
# make test-chain-host (or, with PLUGIN_HOSTD set, make test-hostd-chain for the arm only the daemon shows) in that
# copy. The arm holds when the test goes red on the check that names the guard; an arm whose test stays green, goes red
# on something else, or whose sed changed nothing fails the run. Nothing outside build/sabotage-chain-host-* is touched.
#
# CHAIN_SABOTAGE_JOBS: make's parallel jobs in each copy (default 4); PLUGIN_HOSTD: the daemon, for the hostd arm
# (skipped, and said, without it)

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
jobs=${CHAIN_SABOTAGE_JOBS:-4}
failures=0

# arm <name> <file> <sed expression> <the FAIL line's text> [<make target>: test-chain-host, or test-hostd-chain]
arm() {
    local name=$1 file=$2 expr=$3 fail=$4 target=${5:-test-chain-host} dir log
    dir=$root/build/sabotage-chain-host-$name
    log=$dir.log
    rm -rf "$dir"
    mkdir -p "$dir"
    cp -r "$root/src" "$root/lv2" "$root/tests" "$root/data" "$root/Makefile" "$root/VERSION" "$dir/"
    # what the worktree built is not the copy's: everything is built again from the sabotaged sources
    make -s -C "$dir" clean >/dev/null 2>&1
    sed -i -e "$expr" "$dir/$file"
    if cmp -s "$root/$file" "$dir/$file"; then
        echo "FAIL $name: the sed left $file as it was"
        failures=$((failures + 1))
        return
    fi
    if (cd "$dir" && make -j"$jobs" omx-clap-host && LD_LIBRARY_PATH=$dir make "$target" PLUGIN_HOSTD="${PLUGIN_HOSTD:-}") >"$log" 2>&1; then
        echo "FAIL $name: survived, the test stays green with $file broken (log $log)"
        failures=$((failures + 1))
    elif ! grep -q -E "^FAIL .*$fail" "$log"; then
        echo "FAIL $name: red, but not on \"$fail\" (log $log)"
        grep -E "^FAIL" "$log" | head -5 | sed 's/^/     /'
        failures=$((failures + 1))
    else
        echo "ok   $name: red as expected: $(grep -m 1 -E "^FAIL .*$fail" "$log")"
    fi
}

# 1. a member that joins closes its client, so the chain is one node: left open
arm one-node src/effects.c 's/^    jack_client_close(effect->jack_client);$/    (void)0;/' \
    'chain 7 10 11 12: one node'

# 2. the head's cycle runs the chain: the head alone instead
arm chain-run src/effects.c 's/(chain = atomic_load_explicit(&effect->chain_run, memory_order_relaxed)))/(chain = NULL))/' \
    'p1 left: the chain against the series'

# 3. the head's ports report the members' sum: the head's own latency instead
arm latency-sum src/effects.c '/^static void chain_latency_changed/,/^}/s/^    atomic_store_explicit(&chain->head->latency_run, frames, memory_order_relaxed);$/    (void)frames;/' \
    'p1: the latency to the last output'

# 4. a member the line leaves out leaves the walk: kept in it
arm left-out src/effects.c 's/^            chain_leave(chain, member);$/            (void)member;/' \
    'chain 7 10 12: two nodes'

# 5. a member that leaves gets a client of its own back: none
arm client-back src/effects.c '/^static int chain_leave/,/^}/s/^    return client_back(effect);$/    return SUCCESS;/' \
    'chain 7 10 12: two nodes'

# 6. the head's removal gives the other members a client of their own first: its client closed under them
arm head-remove src/effects.c 's/^        if (effect->chain)$/        if (0)/' \
    'remove 12: pass alone in its own client'

# 7. one width end to end, checked before anything changes: not checked. A member of another width is still refused by
# the library's own guard (OMX_CLAP_CHAIN_ERR_WIDTH) when it would join, so the host's check shows on a head of
# another width, which the library refuses only once the chain is being made, answering another code
arm width src/effects.c 's/if (effect->in_ports != head->out_ports || effect->out_ports != head->out_ports)/if (0)/' \
    'chain 8 14 15 -> '

# 8. a note port only on the head: a member with one taken
arm note-port src/effects.c 's/if (i > 0 \&\& (effect->midi_port || effect->midi_out_port))/if (0)/' \
    'chain 8 15 13 -> '

# 9. a member of another chain: taken into this one as well
arm other-chain src/effects.c 's/if (effect->chain \&\& effect->chain != chain)/if (0)/' \
    'chain 8 15 10 -> '

# 10. the cycle allocates, locks or prints nothing: a print in every member's stage
arm rt-print src/effects.c '/^static void chain_stage/,/^}/s/^    effect_t \*effect = ctx;$/    effect_t *effect = ctx;\n    fprintf(stderr, "stage\\n");/' \
    "the host's chain cycle names an allocator"

# 11. behind the daemon the chain is one node because this host answers the line: refused instead
if [ -n "${PLUGIN_HOSTD:-}" ]; then
    arm hostd-line src/main.c 's/            resp = effects_chain(id, instances, n);/            resp = ERR_INVALID_OPERATION;/' \
        'chain 5 is one node' test-hostd-chain
else
    echo "skip hostd-line: no PLUGIN_HOSTD"
fi

if [ "$failures" -ne 0 ]; then
    echo "FAIL chain host sabotage: $failures arm(s) did not hold"
    exit 1
fi
echo "ok   chain host sabotage: every guard seen red"
