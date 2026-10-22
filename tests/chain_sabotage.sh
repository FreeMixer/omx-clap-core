#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The sabotage of the chain's guards (#311, docs/chain.md): each arm copies the
# sources into build/sabotage-chain-<arm>, breaks one guard of src/clap_chain.c
# there with one sed, builds tests/clap_chain_test in that copy and runs it. The
# arm holds when the test goes red on the check that names the guard; an arm
# whose test stays green, goes red on something else, or whose sed changed
# nothing fails the run. Nothing outside build/sabotage-chain-* is touched.
#
# CHAIN_SABOTAGE_JOBS: make's parallel jobs in each copy (default 4)

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
jobs=${CHAIN_SABOTAGE_JOBS:-4}
failures=0
file=src/clap_chain.c

# arm <name> <sed expression> <the FAIL line's text>
arm() {
    local name=$1 expr=$2 fail=$3 dir log
    dir=$root/build/sabotage-chain-$name
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
    # the copy's libomx-clap-core and nothing else: make exports LD_LIBRARY_PATH as the worktree, whose library is the
    # one not broken and wins over a test's RUNPATH
    if (cd "$dir" && make -j"$jobs" tests/clap_chain_test tests/fake.clap tests/fake_compressor.clap \
            && LD_LIBRARY_PATH=$dir ./tests/clap_chain_test "$dir/tests/fake.clap" "$dir/tests/fake_compressor.clap") >"$log" 2>&1; then
        echo "FAIL $name: survived, the test stays green with $file broken (log $log)"
        failures=$((failures + 1))
    elif ! grep -q -E "^FAIL .*$fail" "$log"; then
        echo "FAIL $name: red, but not on \"$fail\" (log $log)"
        tail -5 "$log" | sed 's/^/     /'
        failures=$((failures + 1))
    else
        echo "ok   $name: red as expected: $(grep -m 1 -E "^FAIL .*$fail" "$log")"
    fi
}

# 1. a bypassed member is not in the walk: built into the plan anyway, it is called and its output reaches the chain's
arm bypass-walk '/^static void publish/,/^}/s/if (!chain->slot\[i\]\.bypass)$/if (1)/' \
    'the bypassed members are never called'

# 2. a bypassed member adds no latency: counted anyway
arm bypass-latency '/^static uint32_t latency_sum/,/^}/s/if (!chain->slot\[i\]\.bypass)$/if (1)/' \
    'and add no latency'

# 3. the latency is the members' sum: the last member's alone
arm latency-sum 's/sum += chain->slot\[i\]\.latency;/sum = chain->slot[i].latency;/' \
    'the chain.s latency is the sum'

# 4. a change of the latency is told: never
arm latency-told '/^static int latency_update/,/^}/s/return chain->latency_changed != NULL;/return 0;/' \
    'told'

# 5. a removal closes the chain around the member: the walk is not published again, the removed member still runs
arm remove-close '/^int omx_clap_chain_remove/,/^}/s/^    publish(chain);$/    (void)0;/' \
    'the chain built without it, bit for bit'

# 6. a cycle as long as the blocks runs, a longer one passes: the bound one frame short
arm long-bound 's/if (frames > OMX_CLAP_CHAIN_FRAMES_MAX)/if (frames >= OMX_CLAP_CHAIN_FRAMES_MAX)/' \
    'the blocks. length, runs the members'

# 7. a longer cycle passes dry: its input never reaches its output
arm long-dry '/if (frames > OMX_CLAP_CHAIN_FRAMES_MAX)/,/return OMX_CLAP_CHAIN_PASSED;/s/^        pass_dry(chain, inputs, outputs, frames);$/        (void)0;/' \
    'its input is its output, dry'

# 8. the two blocks alternate: one block for every member, each in place on the block before it
arm blocks-alternate 's/chain->block\[s & 1u\]/chain->block[0]/' \
    'no member is handed its input as its output'

# 9. a structure of another ABI or size is refused: read and written as if it were this header's
arm abi-refusal 's/return abi >= OMX_CLAP_CHAIN_ABI_OLDEST/return 1 || abi >= OMX_CLAP_CHAIN_ABI_OLDEST/' \
    'of another ABI'

if [ "$failures" -ne 0 ]; then
    echo "FAIL chain sabotage: $failures arm(s) did not hold"
    exit 1
fi
echo "ok   chain sabotage: every guard seen red"
