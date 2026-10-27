#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The sabotage of the host's xruns line (#371): each arm copies the sources into build/sabotage-xruns-<arm>, breaks one
# guard of src/effects.c there with one sed, builds and runs make test-jack-xruns in that copy. The arm holds when the
# test goes red on the check that names the guard; an arm whose test stays green, goes red on something else, or whose
# sed changed nothing fails the run. Nothing outside build/sabotage-xruns-* is touched.
#
# XRUNS_SABOTAGE_JOBS: make's parallel jobs in each copy (default 4)

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
jobs=${XRUNS_SABOTAGE_JOBS:-4}
failures=0

# arm <name> <sed expression>... <the FAIL line's text>: the last word is the check the arm must red
arm() {
    local name=$1 fail=$2 dir log
    shift 2
    dir=$root/build/sabotage-xruns-$name
    log=$dir.log
    rm -rf "$dir"
    mkdir -p "$dir"
    cp -r "$root/src" "$root/lv2" "$root/tests" "$root/data" "$root/Makefile" "$root/VERSION" "$dir/"
    make -s -C "$dir" clean >/dev/null 2>&1
    local e
    for e in "$@"; do sed -i -e "$e" "$dir/src/effects.c"; done
    if cmp -s "$root/src/effects.c" "$dir/src/effects.c"; then
        echo "FAIL $name: the sed left src/effects.c as it was"
        failures=$((failures + 1))
        return
    fi
    if (cd "$dir" && make -j"$jobs" omx-clap-host && LD_LIBRARY_PATH=$dir make test-jack-xruns) >"$log" 2>&1; then
        echo "FAIL $name: survived, the test stays green with src/effects.c broken (log $log)"
        failures=$((failures + 1))
    elif ! grep -q -E "^FAIL .*$fail" "$log"; then
        echo "FAIL $name: red, but not on \"$fail\" (log $log)"
        grep -E "^FAIL" "$log" | head -5 | sed 's/^/     /'
        failures=$((failures + 1))
    else
        echo "ok   $name: red as expected: $(grep -m 1 -E "^FAIL .*$fail" "$log")"
    fi
}

arm no-callback "the host registered one xrun callback" \
    's|    jack_set_xrun_callback(g_jack_global_client, xrun_cb, &g_xruns);|    (void)xrun_cb;|'
arm after-activate "on a client that was not yet active" \
    's|    jack_set_xrun_callback(g_jack_global_client, xrun_cb, &g_xruns);|    (void)0;|' \
    's/    if (jack_activate(g_jack_global_client) != 0)/    if (jack_activate(g_jack_global_client) != 0 || jack_set_xrun_callback(g_jack_global_client, xrun_cb, \&g_xruns))/'
arm callback-counts-nothing "the first xrun is written at once" \
    's|    xrun_report_note(arg);|    (void)arg;|'
arm clock-in-microseconds "three more inside the second wait" \
    's|(uint64_t)now.tv_sec \* 1000u|(uint64_t)now.tv_sec * 1000000u|'
arm no-tick "the first xrun is written at once" \
    's|^    xruns_feedback();|    (void)xruns_feedback;|'

echo "$failures failed"
[ "$failures" -eq 0 ]
