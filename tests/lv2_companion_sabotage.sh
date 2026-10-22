#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The sabotage of a bundle of presets only (#320): DISTRHO-Ports ships TAL NoiseMaker's and Vitalium's presets as bundles
# with no plugin beside the plugin's own. Each arm copies the sources into build/sabotage-<arm>, breaks the rule there
# with one sed, builds the test that guards it in that copy and runs it. The arm holds when the test goes red on the
# check that names the rule; an arm whose test stays green, goes red on something else, or whose sed changed nothing
# fails the run. Nothing outside build/sabotage-* is touched.
#
# COMPANION_SABOTAGE_JOBS: make's parallel jobs in each copy (default 4); the make variables of the run (OMX_DSP_DIR,
# OMX_CONTRACT_DIR, ...) are passed on as MAKEFLAGS are

set -u

root=$(cd "$(dirname "$0")/.." && pwd)
jobs=${COMPANION_SABOTAGE_JOBS:-4}
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
    make -s -C "$dir" clean >/dev/null 2>&1
    sed -i -e "$expr" "$dir/$file"
    if cmp -s "$root/$file" "$dir/$file"; then
        echo "FAIL $name: the sed left $file as it was"
        failures=$((failures + 1))
        return
    fi
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

# 1. a bundle that loads presets and no plugin is a bundle: refused as one of neither, the companion's presets are not
# the fixture's
arm presets-only lv2/core/lv2_world.c \
    's/return world_presets() > presets_before ? 0 : -1;/return -1;/' \
    'companion first: a bundle of presets only is an entry of no plugin, no refusal' \
    make -j"$jobs" test-lv2-clap

if [ "$failures" -eq 0 ]; then
    echo "companion sabotage ok"
    exit 0
fi
echo "companion sabotage FAILED ($failures)"
exit 1
