#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The sabotage of the no-copy LV2 adapter (docs/fast-path.md, item 1): each arm copies the sources into
# build/sabotage-direct-<arm>, breaks one guard there with one sed, builds lv2/tests/lv2_clap_test in that copy and runs
# it. The arm holds when the test goes red on the check that names the guard; an arm whose test stays green, goes red on
# something else, or whose sed changed nothing fails the run. Nothing outside build/sabotage-direct-* is touched.
#
# DIRECT_SABOTAGE_JOBS: make's parallel jobs in each copy (default 4)
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
jobs=${DIRECT_SABOTAGE_JOBS:-4}
failures=0

# arm <name> <file> <sed expression> <the FAIL line's text>
arm() {
    local name=$1 file=$2 expr=$3 fail=$4 dir log
    dir=$root/build/sabotage-direct-$name
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
    if (cd "$dir" && make -j"$jobs" lv2/tests/lv2_clap_test build/lv2/omx-direct-fixture.lv2/omx-direct-fixture.so \
            && make -j"$jobs" test-lv2-clap) >"$log" 2>&1; then
        echo "FAIL $name: survived, the test stays green with $file broken (log $log)"
        failures=$((failures + 1))
    elif ! grep -q -E "^FAIL .*$fail" "$log"; then
        echo "FAIL $name: red, but not on \"$fail\" (log $log)"
        tail -5 "$log" | sed 's/^/     /'
        failures=$((failures + 1))
    else
        echo "ok   $name: red as expected: $(grep -m 1 -E "^FAIL .*$fail" "$log")"
    fi
    rm -rf "$dir"
}

clap=lv2/clap/lv2_clap.c
core=lv2/core/lv2_instance.c

# 1. no plugin is connected: the adapter copies again, as it did
arm copies "$core" '/^int lv2_instance_direct/,/^}/s/return .*/return 0;/' 'nothing was copied'

# 2. lv2:inPlaceBroken is not read: an aliased plugin reads the silence it cleared
arm broken-flag "$clap" 's/(l->in_place_broken || q != dst/(q != dst/' 'never sees one buffer as input and output'

# 3. every alias is let through
arm any-alias "$clap" 's/if (blocks_overlap(dst, q, n) \&\& (l->in_place_broken.*$/if (0)/' 'never sees one buffer|another channel.s input'

# 4. a plugin that does not state hardRTCapable is connected from process()
arm hard-rt "$core" 's/return in->plugin->hard_rt \&\& !in->worker;/return !in->worker;/' 'never called from process'

# 5. a plugin with a worker is connected from process()
arm worker "$core" 's/return in->plugin->hard_rt \&\& !in->worker;/return in->plugin->hard_rt;/' 'worker: connect_port is never called'

# 6. connect_port on every block, the port's memory not looked at
arm connect-each "$core" '/^void lv2_instance_port_bind/,/^}/s/if (in->bound\[port\] == at)/if (0)/' 'cost no connect_port'

# 7. an unaligned buffer is connected
arm aligned "$clap" 's/return p \&\& ((uintptr_t)p \& 15u) == 0;/return p != NULL;/' 'not on 16 bytes'

# 8. deactivate leaves the plugin on the host's buffers
arm home "$core" '/^void lv2_instance_deactivate/,/^}/s/^    ports_home(in);$/    (void)0;/' 'finds input 0 on the instance'

echo
[ "$failures" -eq 0 ] && echo "ok   every guard of the no-copy adapter goes red when broken" || echo "FAIL $failures arm(s)"
exit "$((failures > 0))"
