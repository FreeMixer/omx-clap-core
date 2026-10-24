#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The sabotage of the resource hints' guards (#323): each arm copies the sources
# into build/sabotage-hints-<arm>, breaks one guard there with one sed, builds the
# test that holds it in that copy and runs it. The arm holds when the test goes
# red on the check that names the guard; an arm whose test stays green, goes red
# on something else, or whose sed changed nothing fails the run. Nothing outside
# build/sabotage-hints-* is touched.
#
# HINTS_SABOTAGE_JOBS: make's parallel jobs in each copy (default 4)

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
jobs=${HINTS_SABOTAGE_JOBS:-4}
failures=0

# arm <name> <file> <sed expression> <the FAIL line's text> <command run in the copy...>
arm() {
    local name=$1 file=$2 expr=$3 fail=$4 dir log
    shift 4
    dir=$root/build/sabotage-hints-$name
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
    # the copy's libomx-clap-core and nothing else, never the worktree's that make exports
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

hints_test() {
    make -j"$jobs" tests/resource_hints_test tests/fake_ext.clap && ./tests/resource_hints_test "$PWD/tests/fake_ext.clap"
}

# 1. a declared flag is read as its byte: compared with 9 rather than 1, a flag of 2 passes as declared
arm flag-byte src/clap_host.c 's/flag_byte(&h\.streams_from_disk) > 1u/flag_byte(\&h.streams_from_disk) > 9u/' \
    'a flag of 2' hints_test

# 2. a bit RESOURCE_HINTS has no hint for refuses the declaration
arm unknown-bit src/clap_host.c 's/    if (d & ~RESOURCE_HINTS_ALL)/    if (0)/' 'a bit past the seventh' hints_test

# 3. an unknown hint's member is never read: the plugin's struct copied whole, its garbage reaches the reader
arm unknown-member src/clap_host.c 's/^    out->declared = d;$/    *out = h;/' "an unknown hint's member is 0" hints_test

if [ "$failures" -eq 0 ]; then
    echo "resource hints sabotage ok"
    exit 0
fi
echo "resource hints sabotage FAILED ($failures)"
exit 1
