#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The chain's members under ThreadSanitizer (#104, make test-chain-host-tsan): tests/chain_host_e2e.sh's stress
# (CHAIN_HOST_STRESS rounds, the quantum changed all along, each change the head's buffer-size callback walking its
# members on JACK's thread while the control thread joins, frees, adds and lets go of members) on the host and the
# core built under ThreadSanitizer. g_chain_members orders every walk against every change, so no data race is
# reported whose accesses run in src/effects.c. Reports that are not this host's (PipeWire's own loop, whose recursive
# mutex reads as a double lock and whose queues it does not see ordered) are counted and left aside; a race this target
# does not observe is suppressed by name in tests/chain_host_tsan.supp.
#
#   chain_host_tsan.sh <the host built under ThreadSanitizer> <report directory>
#
# Exit 0 no race in src/effects.c; 1 one or more, the first ones printed.

set -u

here=$(cd "$(dirname "$0")" && pwd)
host=${1:?usage: chain_host_tsan.sh <host> <report dir>}
dir=${2:?usage: chain_host_tsan.sh <host> <report dir>}

mkdir -p "$dir"
rm -f "$dir"/report.*
TSAN_OPTIONS="halt_on_error=0 log_path=$dir/report suppressions=$here/chain_host_tsan.supp" OMX_CLAP_HOST=$host \
    CHAIN_HOST_STRESS=${CHAIN_HOST_STRESS:-40} "$here/chain_host_e2e.sh"
e2e=$?

# each data race report, its two accesses' stacks alone (up to the location, mutex and thread sections): one of
# src/effects.c's when a frame there is in them
ours=$(cat "$dir"/report.* 2>/dev/null | awk '
    /^WARNING: ThreadSanitizer: data race/ { in_race = 1; access = 1; text = $0 "\n"; mine = 0; next }
    in_race && /^  (Location|Mutex|Thread) / { access = 0 }
    in_race && access { text = text $0 "\n"; if ($0 ~ /src\/effects\.c:/) mine = 1 }
    in_race && /^SUMMARY: ThreadSanitizer/ { if (mine) printf "%s%s\n----\n", text, $0; in_race = 0 }')
races=$(cat "$dir"/report.* 2>/dev/null | grep -c '^SUMMARY: ThreadSanitizer: data race')
other=$(cat "$dir"/report.* 2>/dev/null | grep '^SUMMARY: ThreadSanitizer' | grep -v -c 'data race')
found=$(printf '%s' "$ours" | grep -c '^SUMMARY: ThreadSanitizer')

if [ "$found" -ne 0 ]; then
    printf '%s\n' "$ours" | head -60
    echo "FAIL ThreadSanitizer reports $found data race(s) in src/effects.c: $(printf '%s\n' "$ours" | grep -m 1 '^SUMMARY' | sed 's/^SUMMARY: ThreadSanitizer: //')"
    exit 1
fi
if [ "$e2e" -ne 0 ]; then
    echo "FAIL the stress itself failed under ThreadSanitizer"
    exit 1
fi
echo "ok   ThreadSanitizer reports no data race in src/effects.c ($races data race(s) and $other other report(s) outside it left aside)"
