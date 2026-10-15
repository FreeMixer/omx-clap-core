#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The hosting bench: bench/run-one.sh over every path, rate, quantum and repetition, then bench/crash.sh, the results
# under one directory: plugins.csv (one row per plugin per run), runs.csv (one row per run), crash.csv, machine.txt and
# raw/ (each run's profiler log, pw-top snapshots, probe readings and host logs). Every run is a private PipeWire.
#
#   bench/bench.sh <out dir>       BENCH_PATHS, BENCH_RATES, BENCH_QUANTA, BENCH_REPS, BENCH_SECONDS as `make bench` sets

set -u
set -o pipefail

here=$(cd "$(dirname "$0")" && pwd)
out=${1:?usage: bench.sh <out dir>}
mkdir -p "$out"
paths=${BENCH_PATHS:-modhost clap adapter inproc-clap inproc-lv2}
rates=${BENCH_RATES:-48000 96000}
quanta=${BENCH_QUANTA:-64 128 256 512}
reps=${BENCH_REPS:-3}
seconds=${BENCH_SECONDS:-60}

[ -f "$out/plugins.csv" ] || echo "path,rate,quantum,rep,plugin,add_ms,link_ms,first_audio_ms,cycles,dur_p50_us,dur_p99_us,dur_max_us,sched_p50_us,sched_p99_us,node_xruns,silent_blocks,run_p50_us,run_p99_us,run_max_us" >"$out/plugins.csv"
[ -f "$out/runs.csv" ] || echo "path,rate,quantum,rep,plugins,host_ready_ms,rss_empty_kb,rss_loaded_kb,host_cpu_pct,pipewire_cpu_pct,cycles,cycle_end_p50_us,cycle_end_p99_us,cycle_end_max_us,driver_xruns,overruns,probe_late,probe_xruns,probe_max_gap_us,ref_lat_min,ref_lat_max,pad_lat_min,pad_lat_max,impulses" >"$out/runs.csv"

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
} >"$out/machine.txt"

for rate in $rates; do
    for quantum in $quanta; do
        for rep in $(seq 1 "$reps"); do
            # the paths interleaved inside each repetition, so a slow drift of the machine lands on all of them
            for path in $paths; do
                "$here/run-one.sh" "$path" "$rate" "$quantum" "$seconds" "$rep" "$out" ||
                    echo "run $path-$rate-$quantum-$rep failed" >&2
            done
        done
    done
done
echo "load after: $(cut -d' ' -f1-3 /proc/loadavg)" >>"$out/machine.txt"
"$here/crash.sh" "$out" || echo "crash drill failed" >&2
