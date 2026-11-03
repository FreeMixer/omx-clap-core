#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The host walks the live instances, not the table: effects_idle, the state, remove-all, port-name and carrier walks go
# through LIVE_EACH_INSTANCE, and a slot's client is set in one place (effect_client_set) so the list cannot drift from
# the slots. The idle tick reaches the tool ids because the list holds them.
#
#   live_list_check.sh <src/effects.c>
set -eu
src=${1:?usage: live_list_check.sh <src/effects.c>}
fail=0

hdr=$(dirname "$src")/effects.h; [ -f "$hdr" ] || hdr=src/effects.h
grep -q '^#define MAX_PLUGIN_INSTANCES *9990$' "$hdr" && grep -q '^#define MAX_TOOL_INSTANCES *10$' "$hdr" ||
    { echo "FAIL the instance counts of $hdr are not those tests/live_list_test.c uses (9990 plugins, 10 tools)"; fail=1; }

idle=$(sed -n '/^void effects_idle(void)/,/^}/p' "$src")
printf '%s\n' "$idle" | grep -q 'LIVE_EACH_INSTANCE(' || { echo "FAIL effects_idle does not walk the live list"; fail=1; }
printf '%s\n' "$idle" | grep -q 'MAX_\(PLUGIN_\)\?INSTANCES' && { echo "FAIL effects_idle walks the slot table"; fail=1; }

# the table is walked in two places only: the named-size reconcile (event driven, not per tick) and nowhere else
walks=$(grep -c 'for (.*< MAX_\(PLUGIN_\)\?INSTANCES' "$src" || true)
[ "$walks" -le 1 ] || { echo "FAIL $walks walks of the slot table in $src, at most the one reconcile"; grep -n 'for (.*< MAX_\(PLUGIN_\)\?INSTANCES' "$src"; fail=1; }

# the client of a slot is assigned in effect_client_set alone
assigns=$(grep -c -E '(->|\.)jack_client = ' "$src" || true)
[ "$assigns" -eq 1 ] || { echo "FAIL $assigns direct assignments of jack_client, want the one in effect_client_set"; grep -n -E '(->|\.)jack_client = ' "$src"; fail=1; }

[ "$fail" -eq 0 ] && echo "ok   the host walks the live instances, tool ids included, and sets a client in one place"
exit "$fail"
