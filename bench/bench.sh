#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The hosting bench: bench/run-one.sh over every path, rate, quantum and repetition, then bench/crash.sh, the results
# under one directory: plugins.csv (one row per plugin per run), runs.csv (one row per run), crash.csv, machine.txt and
# raw/ (each run's profiler log, pw-top snapshots, probe readings and host logs). Every run is a private PipeWire.
#
#   bench/bench.sh <out dir>       BENCH_PATHS, BENCH_RATES, BENCH_QUANTA, BENCH_REPS, BENCH_SECONDS as `make bench` sets
#
# BENCH_IDLE_HOLD: the CPU idle hold of every run (default, off, 0 or microseconds; bench/lib.sh), a column of runs.csv
# beside the device's own answer. BENCH_ENERGY_CMD: a command printing an energy counter in microjoules (a RAPL
# energy_uj, read with sudo where it is root's), sampled four times a second from outside the runs' namespace for
# power.csv: the mean package power over each run's measured window alone (the samples inside the window the run
# records; the window is wall-clock, so the live rig's own load is in it). BENCH_CRASH=0 leaves the crash drill out.

set -u
set -o pipefail

here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: bench.sh <out dir>}
mkdir -p "$out"
paths=${BENCH_PATHS:-modhost clap adapter inproc-clap inproc-lv2}
rates=${BENCH_RATES:-48000 96000}
quanta=${BENCH_QUANTA:-64 128 256 512}
reps=${BENCH_REPS:-3}
rep_from=${BENCH_REP_FROM:-1}   # the first repetition's number: a driver that interleaves arms runs one rep per call
seconds=${BENCH_SECONDS:-60}

[ -f "$out/plugins.csv" ] || echo "path,rate,quantum,rep,plugin,add_ms,link_ms,first_audio_ms,cycles,dur_p50_us,dur_p99_us,dur_max_us,sched_p50_us,sched_p99_us,node_xruns,silent_blocks,run_p50_us,run_p99_us,run_max_us" >"$out/plugins.csv"
[ -f "$out/runs.csv" ] || echo "path,rate,quantum,rep,plugins,host_ready_ms,rss_empty_kb,rss_loaded_kb,host_cpu_pct,pipewire_cpu_pct,cycles,cycle_end_p50_us,cycle_end_p99_us,cycle_end_max_us,driver_xruns,overruns,probe_late,probe_xruns,probe_max_gap_us,ref_lat_min,ref_lat_max,pad_lat_min,pad_lat_max,impulses,idle_hold,cpu_latency_us" >"$out/runs.csv"
suffix=
[ "${BENCH_IDLE_HOLD:-default}" = default ] || suffix=-hold-$BENCH_IDLE_HOLD
sampler=
if [ -n "${BENCH_ENERGY_CMD:-}" ]; then
    [ -f "$out/power.csv" ] || echo "path,rate,quantum,rep,idle_hold,seconds,joules,watts" >"$out/power.csv"
    (while :; do echo "$(date +%s%N) $(sh -c "$BENCH_ENERGY_CMD" 2>/dev/null | tr -d ' ')"; sleep 0.25; done) >"$out/energy.log" &
    sampler=$!
    trap 'kill $sampler 2>/dev/null' EXIT
fi
# the run's window from its raw dir, the samples inside it, a power.csv row: nothing when fewer than two samples fit
power_row() {
    local raw=$1 label=$2
    [ -n "$sampler" ] && [ -s "$raw/window.txt" ] || return 0
    awk -v t0="$(sed -n 1p "$raw/window.txt")" -v t1="$(sed -n 2p "$raw/window.txt")" -v label="$label" '
        NF == 2 && $1 >= t0 && $1 <= t1 { if (!n) { ta = $1; ea = $2 } tb = $1; eb = $2; n++ }
        END { if (n >= 2 && eb >= ea) { s = (tb - ta) / 1e9; j = (eb - ea) / 1e6; printf "%s,%.1f,%.1f,%.2f\n", label, s, j, j / s } }' \
        "$out/energy.log" >>"$out/power.csv"
}

{
    echo "date: $(date -u +%FT%TZ)"
    echo "host: $(uname -n) $(uname -r)"
    echo "cpu: $(awk -F': ' '/^model name/ { print $2; exit }' /proc/cpuinfo), $(nproc) threads"
    echo "governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo unknown)"
    echo "load before: $(cut -d' ' -f1-3 /proc/loadavg)"
    echo "container: $([ -f /run/.containerenv ] && sed -n 's/^image="\(.*\)"/\1/p' /run/.containerenv || echo none)"
    echo "pipewire: $(pipewire --version 2>/dev/null | tail -1)"
    echo "mod-host: $(rpm -q mod-host 2>/dev/null || echo unknown)"
    echo "omx-clap-host: $(git -C "$here/.." describe --always --dirty 2>/dev/null || echo tree) (built from this tree)"
    echo "omx-plugins: $(rpm -q omx-plugins-clap omx-plugins-lv2 2>/dev/null | tr '\n' ' ')"
    echo "stock: $(rpm -q lsp-plugins-clap lsp-plugins-lv2 clap-dragonfly-reverb lv2-dragonfly-reverb lv2-x42-plugins 2>/dev/null | tr '\n' ' ')"
    echo "rtprio limit: $(ulimit -r)"
    echo "idle hold: ${BENCH_IDLE_HOLD:-default}"
    echo "cpu_dma_latency at rest: $(od -An -td4 "${OMX_CLAP_IDLE_HOLD_DEVICE:-/dev/cpu_dma_latency}" 2>/dev/null | tr -d ' ' || echo unreadable) us"
    echo "cpuidle: $(cat /sys/devices/system/cpu/cpuidle/current_driver 2>/dev/null || echo unknown); states: $(for s in /sys/devices/system/cpu/cpu0/cpuidle/state*; do printf '%s=%sus ' "$(cat "$s/name")" "$(cat "$s/latency")"; done 2>/dev/null)"
    echo "plugin-hostd: $(rpm -q plugin-hostd 2>/dev/null || echo unknown); jalv: $(rpm -q jalv 2>/dev/null || echo unknown)"
} >"$out/machine.txt"

for rate in $rates; do
    for quantum in $quanta; do
        for rep in $(seq "$rep_from" "$reps"); do
            # the paths interleaved inside each repetition, so a slow drift of the machine lands on all of them
            for path in $paths; do
                "$here/run-one.sh" "$path" "$rate" "$quantum" "$seconds" "$rep" "$out" ||
                    echo "run $path-$rate-$quantum-$rep failed" >&2
                power_row "$out/raw/$path-$rate-$quantum-$rep$suffix" "$path,$rate,$quantum,$rep,${BENCH_IDLE_HOLD:-default}"
            done
        done
    done
done
echo "load after: $(cut -d' ' -f1-3 /proc/loadavg)" >>"$out/machine.txt"
[ "${BENCH_CRASH:-1}" = 0 ] || "$here/crash.sh" "$out" || echo "crash drill failed" >&2
