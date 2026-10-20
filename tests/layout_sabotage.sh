#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The sabotage of the audio layouts (#92): each arm copies the sources into
# build/sabotage-<arm>, breaks one rule there with one sed, builds the test that
# guards the rule in that copy and runs it. The arm holds when the test goes red
# on the check that names the rule; an arm whose test stays green, goes red on
# something else, or whose sed changed nothing fails the run. Nothing outside
# build/sabotage-* is touched.
#
# LAYOUT_SABOTAGE_JOBS: make's parallel jobs in each copy (default 4)

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
jobs=${LAYOUT_SABOTAGE_JOBS:-4}
failures=0

# arm <name> <file> <sed expression> <the FAIL line's text> <command run in the copy...>
arm() {
    local name=$1 file=$2 expr=$3 fail=$4 dir log
    shift 4
    dir=$root/build/sabotage-$name
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
    # the copy's libomx-clap-core and nothing else: make test exports LD_LIBRARY_PATH as the worktree, whose library
    # is the one not broken and wins over a test's RUNPATH
    if (cd "$dir" && LD_LIBRARY_PATH=$dir "$@") >"$log" 2>&1; then
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

host_test() {
    make -j"$jobs" tests/clap_host_test tests/fake.clap tests/fake_synth.clap tests/fake_mover.clap \
        && ./tests/clap_host_test - "$PWD/tests/fake.clap" "$PWD/tests/fake_synth.clap" "$PWD/tests/fake_mover.clap"
}

# 1. the main pair may differ in width: the refusal of a main input narrower or wider than the output, put back after
# describe_ports' loop, refuses the mono-in stereo-out fake at open
arm widths src/clap_host.c \
    '0,/^    if (in->main_out.channels == 0)$/s//    if (in->main_in.channels \&\& in->main_in.channels != in->main_out.channels)\n        return CLAP_HOST_CODE_WIDER_THAN_STRIP;\n&/' \
    'opens: one channel in, two out' host_test

# 2. LV2 rule 3: as many ungrouped inputs as outputs make one main port of that width; without the comparison the
# four-four fixture's main ports are the first two of each side
arm nn-main lv2/core/lv2_world.c \
    's/sp\[i\]\.main = plain <= 2 || plain == other || nm < 2;/sp[i].main = plain <= 2 || 0 || nm < 2;/' \
    'layout four-four: one main port of four each side' \
    make -j"$jobs" test-lv2-core

# 3. a bound auxiliary input is read: every channel pointed at the silence, the aux-wide fake reads none of its side chain
arm aux-silence src/clap_stage.h \
    's/= bufs \&\& bufs\[c\] ? bufs\[c\] : a->aux_silence\[0\];/= bufs \&\& bufs[c] \&\& 0 ? bufs[c] : a->aux_silence[0];/' \
    "the plugin read the side chain's fourth channel" host_test

# 4. every dry path silences the auxiliary outputs: without it a bypassed plugin's auxiliary outputs repeat its last block
arm aux-out-silence src/clap_stage.h \
    's/            memset(s->aout\[1 + i\]\.data32\[c\], 0, m \* sizeof(float));/            (void)m;/' \
    'bypassed, the plugin idles and its auxiliary outputs are silent' \
    sh -c "make -j$jobs tests/clap_stage_test && ./tests/clap_stage_test"

if [ "$failures" -eq 0 ]; then
    echo "layout sabotage ok"
    exit 0
fi
echo "layout sabotage FAILED ($failures)"
exit 1
