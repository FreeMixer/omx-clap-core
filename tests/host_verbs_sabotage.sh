#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The sabotage of the verbs' guard (tests/host_verbs_e2e.sh): each arm breaks one thing and runs the guard, which must
# go red on the check that names it. Two arms change the declaration and leave the host as it is: a copy of the
# installed contract directory with one sed in its verbs.json (a format, what this host owes a verb). Two change the
# host and leave the declaration: a copy of the sources with one sed in src/main.c (a handler's registration deleted,
# a verb with no declaration registered), built again. An arm whose guard stays green, goes red on something else, or
# whose sed changed nothing fails the run. Nothing outside build/sabotage-host-verbs-* is touched.
#
# OMX_CONTRACT_PATH: the installed declarations the guard reads (make test-host-verbs-sabotage installs the build
# tree's); PHD_PIN_LAYOUT_SCHEME: as tests/host_verbs_e2e.sh; HOST_VERBS_SABOTAGE_JOBS: make's parallel jobs in each
# copy (default 4)

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
jobs=${HOST_VERBS_SABOTAGE_JOBS:-4}
: "${OMX_CONTRACT_PATH:?}" "${PHD_PIN_LAYOUT_SCHEME:?}"
failures=0

# judge <name> <log> <the FAIL line's text>
judge() {
    if [ "$4" -eq 0 ]; then
        echo "FAIL $1: survived, the guard stays green (log $2)"
        failures=$((failures + 1))
    elif ! grep -q -E "^FAIL .*$3" "$2"; then
        echo "FAIL $1: red, but not on \"$3\" (log $2)"
        grep -E "^FAIL" "$2" | head -5 | sed 's/^/     /'
        failures=$((failures + 1))
    else
        echo "ok   $1: red as expected: $(grep -m 1 -E "^FAIL .*$3" "$2")"
    fi
}

# declared <name> <sed expression on omx-clap-core's verbs.json> <the FAIL line's text>
declared() {
    local name=$1 expr=$2 fail=$3 dir=$root/build/sabotage-host-verbs-$1 src status
    src=$(for d in ${OMX_CONTRACT_PATH//:/ }; do [ -f "$d/3/omx-clap-core/verbs.json" ] && { echo "$d"; break; }; done)
    [ -n "$src" ] || { echo "FAIL $name: no omx-clap-core/verbs.json under $OMX_CONTRACT_PATH"; failures=$((failures + 1)); return; }
    rm -rf "$dir" "$dir.log"
    mkdir -p "$dir"
    cp -r "$src/3" "$dir/"
    sed -i -e "$expr" "$dir/3/omx-clap-core/verbs.json"
    if cmp -s "$src/3/omx-clap-core/verbs.json" "$dir/3/omx-clap-core/verbs.json"; then
        echo "FAIL $name: the sed left verbs.json as it was"
        failures=$((failures + 1))
        return
    fi
    OMX_CONTRACT_PATH=$dir "$root/tests/host_verbs_e2e.sh" >"$dir.log" 2>&1
    status=$?
    judge "$name" "$dir.log" "$fail" "$status"
}

# hosted <name> <sed expression on src/main.c> <the FAIL line's text>
hosted() {
    local name=$1 expr=$2 fail=$3 dir=$root/build/sabotage-host-verbs-$1 status
    rm -rf "$dir" "$dir.log"
    mkdir -p "$dir"
    cp -r "$root/src" "$root/lv2" "$root/tests" "$root/data" "$root/Makefile" "$root/VERSION" "$dir/"
    make -s -C "$dir" clean >/dev/null 2>&1
    sed -i -e "$expr" "$dir/src/main.c"
    if cmp -s "$root/src/main.c" "$dir/src/main.c"; then
        echo "FAIL $name: the sed left src/main.c as it was"
        failures=$((failures + 1))
        return
    fi
    (cd "$dir" && make -j"$jobs" omx-clap-host tests/fake.clap tests/fake_synth.clap \
        && LD_LIBRARY_PATH=$dir OMX_CLAP_HOST=$dir/omx-clap-host ./tests/host_verbs_e2e.sh) >"$dir.log" 2>&1
    status=$?
    judge "$name" "$dir.log" "$fail" "$status"
}

declared format 's/"format": "remote_page_get %i %i"/"format": "remote_page_get %i"/' 'remote_page_get 0 \(required\)'
declared owed '/"noteDrops": {/,/"since"/ s/"omx-clap-host": "required"/"omx-clap-host": "unsupported"/' 'note_drops 1 \(unsupported\)'
hosted handler '/protocol_add_command(OMX_VERB_NOTE_DROPS_FMT, note_drops_cb);/d' 'missing \[.note_drops.\]'
hosted undeclared 's/protocol_add_command(OMX_VERB_NOTE_DROPS_FMT, note_drops_cb);/&\n    protocol_add_command("note_peaks %i", note_drops_cb);/' 'registered "note_peaks %i": a verb with no declaration'

# plugin-hostd's package left out of the contract path: its verbs file, which declares chain of this host, is not read
nopkg=$root/build/sabotage-host-verbs-nopkg
rm -rf "$nopkg" "$nopkg.log"
mkdir -p "$nopkg"
src=$(for d in ${OMX_CONTRACT_PATH//:/ }; do [ -f "$d/3/plugin-hostd/verbs.json" ] && { echo "$d"; break; }; done)
if [ -z "$src" ]; then
    echo "FAIL nopkg: no plugin-hostd/verbs.json under $OMX_CONTRACT_PATH"; failures=$((failures + 1))
else
    cp -r "$src/3" "$nopkg/" && rm -rf "$nopkg/3/plugin-hostd"
    OMX_CONTRACT_PATH=$nopkg "$root/tests/host_verbs_e2e.sh" >"$nopkg.log" 2>&1
    judge nopkg "$nopkg.log" "plugin-hostd's package is installed" $?
fi
# a chain line of two members refused, as the report of #361 has it
hosted chain2 's/resp = effects_chain(id, instances, n);/resp = n > 1 ? ERR_INVALID_OPERATION : effects_chain(id, instances, n);/' 'chain 1 0 2 \(required\)'

[ "$failures" -eq 0 ] || { echo "host verbs sabotage: $failures arms did not hold"; exit 1; }
echo "host verbs sabotage: 6 arms, each red on its check"
