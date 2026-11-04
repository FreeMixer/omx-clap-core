#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The LV2 parity run on the desk: tests/lv2_parity.py's steps in order, under nice, with the memory floor checked
# before each. Every LV2 plugin that mod-host and omx-clap-host both add, through both from the same cycles, compared.
#
#   tests/lv2_parity.sh [--step probe|run|classify|report|all] [--out DIR] [--rates 48000,96000]
#                       [--sample N | --full] [--coverage TSV | --uris FILE|LIST] [-- <more lv2_parity.py run options>]
#
# --out:      the results (default build/lv2-parity/<today> in this tree)
# --rates:    the rates of the run step (default 48000,96000; the floor set is 44100,48000,96000,192000)
# --sample:   a stratified sample of N plugins (default 60); --full: every plugin both hosts add
# --coverage: the coverage table of the probe step: one row per URI with a bucket column (both = both hosts add it) and
#             the layout columns (audio_in, audio_out, cv, midi_in, instrument, ext_data, req_features); or --uris
# MIN_FREE_GIB: the memory floor, MemAvailable in GiB (default 3); below it the script stops before the step
# The run step takes the hosts from OMX_CLAP_HOST (default ./omx-clap-host of this tree) and MOD_HOST (default
# /usr/bin/mod-host), and needs tests/jack_parity (make tests/jack_parity).
set -uo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
step=all
out=$root/build/lv2-parity/$(date +%F)
rates=48000,96000
pick=(--sample 60)
source_args=()
extra=()
while [ $# -gt 0 ]; do
    case $1 in
        --step) step=$2; shift 2 ;;
        --out) out=$2; shift 2 ;;
        --rates) rates=$2; shift 2 ;;
        --sample) pick=(--sample "$2"); shift 2 ;;
        --full) pick=(--full); shift ;;
        --coverage) source_args=(--coverage "$2"); shift 2 ;;
        --uris) source_args=(--uris "$2"); shift 2 ;;
        --) shift; extra=("$@"); break ;;
        *) echo "lv2_parity.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
min=${MIN_FREE_GIB:-3}
py=(nice -n 10 python3 "$here/lv2_parity.py")

floor() {
    local avail
    avail=$(awk '/^MemAvailable:/ {printf "%.1f", $2 / 1048576}' /proc/meminfo)
    if awk -v a="$avail" -v m="$min" 'BEGIN { exit !(a < m) }'; then
        echo "STOPPED: $avail GiB available, the floor is $min GiB" >&2
        exit 3
    fi
    echo "memory: $avail GiB available (floor $min GiB)"
}

full_count() {
    # the size of the full set, for the report's estimate: what probe --full would choose
    local d
    d=$(mktemp -d "$out/.full.XXXXXX") || return
    "${py[@]}" probe "${source_args[@]}" --out "$d" --full >/dev/null && echo $(( $(wc -l <"$d/plugins.tsv") - 1 ))
    rm -rf "$d"
}

case " $step " in
    " all "|" probe "|" report ") [ ${#source_args[@]} -gt 0 ] || { echo "lv2_parity.sh: the $step step needs --coverage or --uris" >&2; exit 2; } ;;
esac
mkdir -p "$out"
for s in probe run classify report; do
    [ "$step" = all ] || [ "$step" = "$s" ] || continue
    floor
    case $s in
        probe) "${py[@]}" probe "${source_args[@]}" --out "$out" "${pick[@]}" || exit $? ;;
        run) "${py[@]}" run --out "$out" --rates "$rates" --omx-clap-host "${OMX_CLAP_HOST:-$root/omx-clap-host}" \
                 --mod-host "${MOD_HOST:-/usr/bin/mod-host}" "${extra[@]}" || exit $? ;;
        classify) "${py[@]}" classify --out "$out" || exit $? ;;
        report) cov=(); [ "${source_args[0]}" = --coverage ] && cov=("${source_args[@]}")
                "${py[@]}" report --out "$out" --full-count "$(full_count)" "${cov[@]}" || exit $? ;;
    esac
done
