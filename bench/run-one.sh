#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One run of the hosting bench: one path, one rate, one quantum, in a PipeWire of its own (a private user, network,
# pid and mount namespace, the scaffolding of tests/clap_lv2_identity.sh). Every plugin of bench/plugins.tsv the
# path can host is racked in ONE host, each fed the probe's noise and recorded; one more instance, a pad, carries the
# probe's impulse; a direct link carries it as the reference. Then the profiler runs for the duration.
#
#   bench/run-one.sh <path> <rate> <quantum> <seconds> <rep> <out dir>
#
# path: modhost (LV2 through mod-host), clap (CLAP through omx-clap-host), adapter (LV2 through omx-clap-host and
# libomx-clap-lv2), inproc-clap / inproc-lv2 (the plugin in a PipeWire filter's own process callback through
# libomx-clap-core, the LV2 one through libomx-clap-lv2: the engine's in-process tier), hostd (LV2 through
# plugin-hostd, its worker omx-clap-host with the adapter, one pool worker for the set), jalv (LV2, one jalv process
# per plugin, as Zynthian runs its layers; RSS and CPU are the sum over them, RSS empty is 0: no host before an add).
# BENCH_IDLE_HOLD: the CPU idle hold of the run (bench/lib.sh), recorded with the device's aggregate in runs.csv.
#
# Writes <out dir>/plugins.csv and <out dir>/runs.csv rows (appended, no header) and keeps the raw profiler log,
# pw-top snapshots and host logs under <out dir>/raw/<path>-<rate>-<quantum>-<rep>/.
#
# MOD_HOST, OMX_CLAP_HOST: the hosts (default: mod-host on PATH, ./omx-clap-host); PLUGINS: the plugin table.

set -u
set -o pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")

if [ $# -ne 6 ]; then
    echo "usage: $0 <path> <rate> <quantum> <seconds> <rep> <out dir>" >&2
    exit 2
fi
path=$1 rate=$2 quantum=$3 seconds=$4 rep=$5 out=$6

if [ -z "${BENCH_INSIDE:-}" ]; then
    mkdir -p "$out"
    BENCH_INSIDE=1 exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$path" "$rate" "$quantum" \
        "$seconds" "$rep" "$(readlink -f "$out")"
fi

settle=${SETTLE:-3}
tag=$path-$rate-$quantum-$rep
[ "${BENCH_IDLE_HOLD:-default}" = default ] || tag=$tag-hold-$BENCH_IDLE_HOLD
raw=$out/raw/$tag
rm -rf "$raw"
mkdir -p "$raw"
die() {
    echo "run $tag: $*" >&2
    echo "$path,$rate,$quantum,$rep,FAILED,\"$*\"" >>"$out/failures.csv"
    exit 1
}
. "$here/lib.sh"

graph_start
plugin_set
n_fx=${#names[@]}
# the probe: noise to every plugin, the impulse to the pad and to the reference
probe_start "$n_fx" 2
host_start
sleep 0.5
rss_empty=$(host_rss_kb)

declare -A t_add add_ms link_ms refused
n_refused=0
for k in $(seq 0 $((n_fx - 1))); do
    t0=$(now_ns)
    reply=$(send "${adds[$k]} $k")
    t1=$(now_ns)
    if ! added "$reply" "$k"; then
        # a plugin this path cannot host is a result, not a failed run: it is recorded and the run goes on
        refused[$k]=1
        n_refused=$((n_refused + 1))
        echo "$path,$rate,$quantum,$rep,${names[$k]},refused,\"${adds[$k]} -> $reply\"" >>"$out/refusals.csv"
        continue
    fi
    link_fx "effect_$k" bench-src:src_l bench-src:src_r "bench-rec:rec_$k" || die "${names[$k]}: ports"
    t2=$(now_ns)
    t_add[$k]=$t0
    add_ms[$k]=$(ms "$t0" "$t1")
    link_ms[$k]=$(ms "$t1" "$t2")
done
reply=$(send "$pad $n_fx")
added "$reply" "$n_fx" || die "pad: $pad -> $reply"
link_fx "effect_$n_fx" bench-src:imp bench-src:imp bench-rec:lat_1 || die "pad: ports"

sleep "$settle"
rss_loaded=$(host_rss_kb)
python3 -I "$here/stats.py" nodes >"$raw/nodes.txt"
host_threads >"$raw/threads.txt"
# the hold as the device answers it with every plugin active: the kernel's limit across every requester
cpu_latency=$(device_reads)

# ---- the measured window ----
pw-top -b -n 2 >"$raw/top-before.txt" 2>&1
probe_reset
case $path in inproc-*) send reset >/dev/null ;; esac
host_cpu0=$(host_cpu_ticks) pw_cpu0=$(cpu_ticks "$pw_pid")
date +%s%N >"$raw/window.txt"
(cd "$raw" && timeout "$seconds" pw-profiler -o prof.log >/dev/null 2>&1)
date +%s%N >>"$raw/window.txt"
host_cpu1=$(host_cpu_ticks) pw_cpu1=$(cpu_ticks "$pw_pid")
pw-top -b -n 2 >"$raw/top-after.txt" 2>&1
probe_stats "$raw/probe.txt"
host_alive || die "the host died during the window: $(tail -3 "$raw/host.log" 2>/dev/null)"

ids=$(awk '$2 ~ /^effect_/ { print $1 }' "$raw/nodes.txt" | tr '\n' ' ')
python3 -I "$here/stats.py" profile "$raw/prof.log" $ids >"$raw/profile.txt"
gzip -1 "$raw/prof.log"
python3 -I "$here/stats.py" top "$raw/top-before.txt" >"$raw/err-before.txt"
python3 -I "$here/stats.py" top "$raw/top-after.txt" >"$raw/err-after.txt"
hz=$(getconf CLK_TCK)

err_delta() {
    local a b
    a=$(awk -v n="$1" '$2 == n { print $3 }' "$raw/err-before.txt" | head -1)
    b=$(awk -v n="$1" '$2 == n { print $3 }' "$raw/err-after.txt" | head -1)
    echo $(( ${b:-0} - ${a:-0} ))
}

for k in $(seq 0 $((n_fx - 1))); do
    [ -n "${refused[$k]:-}" ] && continue
    id=$(awk -v n="effect_$k" '$2 == n { print $1 }' "$raw/nodes.txt" | head -1)
    prof=$(grep "^node $id " "$raw/profile.txt")
    first=$(grep "^input rec_$k " "$raw/probe.txt" | field first_ns)
    silent=$(grep "^input rec_$k " "$raw/probe.txt" | field silent_after)
    if [ "${first:-0}" -gt 0 ]; then first_ms=$(ms "${t_add[$k]}" "$first"); else first_ms=NA; fi
    run_stats="NA,NA,NA"
    case $path in
        inproc-*)
            st=$(send "stats $k")
            echo "effect_$k $st" >>"$raw/inproc-stats.txt"
            run_stats="$(echo "$st" | field run_p50),$(echo "$st" | field run_p99),$(echo "$st" | field run_max)" ;;
    esac
    echo "$path,$rate,$quantum,$rep,${names[$k]},${add_ms[$k]},${link_ms[$k]},$first_ms,$(echo "$prof" | field cycles),$(echo "$prof" | field dur_p50),$(echo "$prof" | field dur_p99),$(echo "$prof" | field dur_max),$(echo "$prof" | field sched_p50),$(echo "$prof" | field sched_p99),$(err_delta "effect_$k"),$silent,$run_stats" >>"$out/plugins.csv"
done

drv=$(grep "^driver " "$raw/profile.txt")
lat_ref=$(grep "^input lat_0 " "$raw/probe.txt")
lat_pad=$(grep "^input lat_1 " "$raw/probe.txt")
client=$(grep "^client " "$raw/probe.txt")
drv_name=$(awk '$2 ~ /Driver$/ && $2 !~ /Freewheel/ { print $2 }' "$raw/err-after.txt" | head -1)
pct() { awk -v d="$1" -v hz="$hz" -v s="$seconds" 'BEGIN { printf "%.2f", 100 * d / hz / s }'; }
host_cpu_pct=$(pct $((host_cpu1 - host_cpu0)))
pw_cpu_pct=$(pct $((pw_cpu1 - pw_cpu0)))
echo "$path,$rate,$quantum,$rep,$((n_fx - $n_refused)),$(ms "$t_spawn" "$t_ready"),$rss_empty,$rss_loaded,$host_cpu_pct,$pw_cpu_pct,$(echo "$drv" | field cycles),$(echo "$drv" | field end_p50),$(echo "$drv" | field end_p99),$(echo "$drv" | field end_max),$(err_delta "$drv_name"),$(echo "$drv" | field overruns),$(echo "$client" | field late),$(echo "$client" | field xruns),$(echo "$client" | field max_gap_us),$(echo "$lat_ref" | field lat_min),$(echo "$lat_ref" | field lat_max),$(echo "$lat_pad" | field lat_min),$(echo "$lat_pad" | field lat_max),$(echo "$lat_pad" | field lat_n),$idle_hold,$cpu_latency" >>"$out/runs.csv"
echo "run $tag: $((n_fx - $n_refused)) plugins ($n_refused refused), driver end p50 $(echo "$drv" | field end_p50) us, pad latency $(echo "$lat_pad" | field lat_min)..$(echo "$lat_pad" | field lat_max) frames (reference $(echo "$lat_ref" | field lat_min)), idle hold $idle_hold, device $cpu_latency"

case $path in
    inproc-*) echo quit >&7 ;;
    jalv) send quit >/dev/null ;;
    *) printf 'quit\0' >&3 ;;
esac
echo quit >&5
exit 0
