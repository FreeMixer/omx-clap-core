#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The sabotage of patch_set and patch_get (jack_lv2_e2e.sh, the atom fixture's section): each arm copies the sources into
# build/sabotage-patch-<arm>, breaks the routing there with one sed, builds the host and runs test-jack-lv2. The arm
# holds when the e2e goes red on the check that names what it broke; an arm whose e2e stays green, goes red on
# something else, or whose sed changed nothing fails the run. The tree's own test-jack-lv2 is the control: it runs first
# and must be green. Nothing outside build/sabotage-patch-* and the tree's own build is touched.
#
# PATCH_SABOTAGE_JOBS: make's parallel jobs (default 4)
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
jobs=${PATCH_SABOTAGE_JOBS:-4}
failures=0
# the protocol clients every copy's e2e sources, resolved once from the tree as the Makefile does: the copies sit deeper
for lib in "${PLUGIN_HOSTD_TESTS_LIB:-}" "$root/../plugin-hostd/tests/lib" /usr/libexec/plugin-hostd/tests/lib; do
    [ -n "$lib" ] && [ -d "$lib" ] && { PLUGIN_HOSTD_TESTS_LIB=$(cd "$lib" && pwd); export PLUGIN_HOSTD_TESTS_LIB; break; }
done

mkdir -p "$root/build"
if (cd "$root" && make -j"$jobs" test-jack-lv2) >"$root/build/sabotage-patch-base.log" 2>&1; then
    echo "ok   control: the tree's own test-jack-lv2 is green"
else
    echo "FAIL control: the tree's own test-jack-lv2 is not green (log $root/build/sabotage-patch-base.log)"
    exit 1
fi

# arm <name> <file> <sed expression> <the FAIL line's text>
arm() {
    local name=$1 file=$2 expr=$3 fail=$4 dir log
    dir=$root/build/sabotage-patch-$name
    log=$dir.log
    rm -rf "$dir"
    mkdir -p "$dir"
    cp -a "$root/src" "$root/lv2" "$root/tests" "$root/data" "$root/Makefile" "$root/VERSION" "$dir/"
    sed -i -e "$expr" "$dir/$file"
    if cmp -s "$root/$file" "$dir/$file"; then
        echo "FAIL $name: the sed left $file as it was"
        failures=$((failures + 1))
        return
    fi
    if (cd "$dir" && make -j"$jobs" test-jack-lv2) >"$log" 2>&1; then
        echo "FAIL $name: survived, test-jack-lv2 stays green with $file broken (log $log)"
        failures=$((failures + 1))
    elif grep -q -E "^FAIL $fail" "$log"; then
        echo "ok   $name: red on $(grep -m1 -E "^FAIL $fail" "$log" | cut -c6-120)"
        rm -rf "$dir" "$log"
    else
        echo "FAIL $name: red, but not on '$fail' (log $log)"
        failures=$((failures + 1))
    fi
}

# the verbs not registered: the unsupported list answers them -902 again
arm no-route src/main.c '/protocol_add_command(EFFECT_PATCH_\(SET\|GET\), patch_\(set\|get\)_cb);/d' \
    'patch_set 6 urn:openmixer:test:atom-fixture#file .* -> resp -902'
# a number no longer takes the parameter path: it reaches the text path, which lists no such property
arm no-param-path src/effects.c 's/if (omx_clap_lv2_patch_param_id(plugin, uri, &id))$/if (0 \&\& omx_clap_lv2_patch_param_id(plugin, uri, \&id))/' \
    'patch_set 6 urn:openmixer:test:atom-fixture#gain 1.5 -> resp -103'

[ "$failures" -eq 0 ] && { echo "patch verbs sabotage ok"; exit 0; }
echo "patch verbs sabotage FAILED ($failures)"
exit 1
