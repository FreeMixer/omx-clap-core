#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The control thread's CPU while the host idles with N plugins (and, when asked, one tool instance): the thread that runs
# effects_idle every 20 ms, read from /proc/<pid>/task/<pid>/schedstat (ns of run time) over a window, in a PipeWire of its own, median of several repeats.
#
#   OMX_CLAP_HOST=<host> bench/idle-tick.sh [-n "0 1 24"] [-t] [-s seconds] [-r repeats]
#
#   -n  the plugin counts to measure (default "0 1 24")      -t  also add one tool instance (id 9995)
#   -s  the window of each run, seconds (default 10)         -r  repeats of each count (default 5)
#
# Prints one line per count: the control thread's CPU in percent of one core for each repeat, then the median. Run on a
# quiet machine for numbers worth comparing; a loaded one says so in the load average it prints.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
counts="0 1 24"; tool=0; secs=10; reps=5
while getopts "n:ts:r:" o; do
    case $o in n) counts=$OPTARG ;; t) tool=1 ;; s) secs=$OPTARG ;; r) reps=$OPTARG ;; *) exit 2 ;; esac
done

if [ -z "${IDLE_TICK_INSIDE:-}" ]; then
    export IDLE_TICK_INSIDE=1 OMX_CLAP_HOST=$(readlink -f "${OMX_CLAP_HOST:-$root/omx-clap-host}")
    if unshare --user --map-root-user --net --pid --fork --mount-proc true 2>/dev/null; then
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
    fi
    IDLE_TICK_SESSION=1 exec unshare --user --map-root-user --net setsid -w "$0" "$@"
fi

. "${PLUGIN_HOSTD_TESTS_LIB:-$root/../plugin-hostd/tests/lib}/protocol.sh"
runtime=$(mktemp -d /tmp/omx-idle-tick.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
trap 'kill -TERM -- -1 2>/dev/null; rm -rf "${runtime:?}"' EXIT
ip link set lo up
pipewire >"$runtime/pw.log" 2>&1 &
for _ in $(seq 50); do [ -S "$runtime/pipewire-0" ] && break; sleep 0.1; done
[ -S "$runtime/pipewire-0" ] || { echo "pipewire did not come up" >&2; exit 2; }

fake=$here/../tests/fake.clap
uri="clap:$(readlink -f "$fake")#org.omx-clap-host.test.passthrough"
echo "host $OMX_CLAP_HOST; load average $(cut -d' ' -f1-3 /proc/loadavg); window ${secs}s; tool instance: $tool"

port=5600
for n in $counts; do
    results=()
    for r in $(seq "$reps"); do
        port=$((port + 1))
        "$OMX_CLAP_HOST" -n -p $port >"$runtime/host.$port.log" 2>&1 &
        pid=$!
        protocol_wait_ready "$runtime/host.$port.log" 100 || { echo "host not ready" >&2; exit 2; }
        exec 3<>/dev/tcp/127.0.0.1/$port
        for i in $(seq 0 $((n - 1))); do
            [ "$(protocol_send 3 30 "add $uri $i")" = "resp $i" ] || { echo "add $i refused" >&2; exit 2; }
        done
        if [ "$tool" = 1 ]; then
            [ "$(protocol_send 3 30 "add $uri 9995")" = "resp 9995" ] || { echo "tool add refused" >&2; exit 2; }
        fi
        sleep 2
        t0=$(awk '{print $1}' /proc/$pid/task/$pid/schedstat)
        sleep "$secs"
        t1=$(awk '{print $1}' /proc/$pid/task/$pid/schedstat)
        results+=("$(awk -v a="$t0" -v b="$t1" -v s="$secs" 'BEGIN { printf "%.4f", (b - a) / 1e9 / s * 100 }')")
        exec 3>&-
        kill $pid 2>/dev/null; wait $pid 2>/dev/null
    done
    med=$(printf '%s\n' "${results[@]}" | sort -n | awk '{ v[NR] = $1 } END { print v[int((NR + 1) / 2)] }')
    echo "plugins $n: control thread ${results[*]} % of a core; median $med %"
done
