#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# What a chain member costs (#104, docs/chain.md): N instances of one plugin in one omx-clap-host, one client each
# ("own") and one chain declared by the chain line ("chain"), in a PipeWire of its own at COST_RATE / COST_QUANTUM
# (48000 / 128), the graph's clock forced. For each arm and N, after COST_SETTLE seconds:
#
#   nodes          the effect_ nodes of the graph (pw-dump)
#   busy_us        the median over COST_SAMPLES pw-top samples of each effect_ node's BUSY (the time inside its process
#                  callback), summed over the arm's nodes
#   busy_per_us    busy_us / N
#   cpu_pct        the host process's CPU over the samples' window, percent of one core (/proc/<pid>/stat)
#   rss_kb         the host process's VmRSS
#
# One line per arm and N on stdout, comma-separated, after a header. The cost of a member is the slope of the chain's
# busy_us over N; the own arm's busy_us is its nodes' callbacks alone, without the wake-ups between them, which this
# does not see. The private graph runs as its caller's scheduling allows (SCHED_OTHER in a rootless container): the
# figures are this machine's, not a tuned rig's.
#
# COST_N: the member counts (default "1 2 4 8"); COST_PLUGIN: the add line's uri (default tests/fault-0.clap's -20 dB
# pad); OMX_CLAP_HOST: the binary (default ./omx-clap-host)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
rate=${COST_RATE:-48000}
quantum=${COST_QUANTUM:-128}
counts=${COST_N:-1 2 4 8}
settle=${COST_SETTLE:-3}
samples=${COST_SAMPLES:-10}
plugin=${COST_PLUGIN:-clap:$root/tests/fault-0.clap#org.openmixer.fault.0}

if [ -z "${COST_INSIDE:-}" ]; then
    [ -x "$host" ] || { echo "missing $host" >&2; exit 2; }
    export COST_INSIDE=1 OMX_CLAP_HOST=$(readlink -f "$host")
    if unshare --user --map-root-user --net --pid --fork --mount-proc true 2>/dev/null; then
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
    fi
    COST_SESSION=1 exec unshare --user --map-root-user --net setsid -w "$0" "$@"
fi

runtime=$(mktemp -d /tmp/omx-chain-cost.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
cleanup() {
    exec 3>&- 2>/dev/null
    if [ -n "${COST_SESSION:-}" ]; then
        trap '' TERM
        kill -TERM -- -$$ 2>/dev/null
    else
        kill -TERM -- -1 2>/dev/null
    fi
    wait 2>/dev/null
    rm -rf "${runtime:?}"
}
trap cleanup EXIT

ip link set lo up 2>/dev/null
pipewire >"$runtime/pw.log" 2>&1 &
for _ in $(seq 50); do [ -S "$runtime/pipewire-0" ] && break; sleep 0.1; done
[ -S "$runtime/pipewire-0" ] || { echo "pipewire did not come up" >&2; exit 2; }
pw-metadata -n settings 0 clock.force-rate "$rate" >/dev/null
pw-metadata -n settings 0 clock.force-quantum "$quantum" >/dev/null

send() {
    local reply
    printf '%s\0' "$1" >&3
    IFS= read -r -d '' -t 30 reply <&3
    echo "$reply"
}
ticks() { awk '{ print $14 + $15 }' "/proc/$1/stat"; }
median() { sort -n | awk '{ v[NR] = $1 } END { if (NR) print (NR % 2 ? v[(NR + 1) / 2] : (v[NR / 2] + v[NR / 2 + 1]) / 2); else print "" }'; }

echo "arm,n,rate,quantum,nodes,busy_us,busy_per_us,cpu_pct,rss_kb"
port=5931
for n in $counts; do
    for arm in own chain; do
        port=$((port + 1))
        "$OMX_CLAP_HOST" -n -p "$port" >"$runtime/host-$arm-$n.log" 2>&1 &
        pid=$!
        for _ in $(seq 100); do grep -q ready "$runtime/host-$arm-$n.log" && break; sleep 0.1; done
        exec 3<>"/dev/tcp/127.0.0.1/$port"
        line="chain 0"
        for k in $(seq 0 $((n - 1))); do
            [ "$(send "add $plugin $k")" = "resp $k" ] || { echo "add $k refused" >&2; exit 1; }
            line="$line $k"
        done
        if [ "$arm" = chain ]; then
            [ "$(send "$line")" = "resp 0" ] || { echo "$line refused" >&2; exit 1; }
        fi
        sleep "$settle"
        nodes=$(pw-dump 2>/dev/null | jq -r '.[] | select(.type == "PipeWire:Interface:Node") | .info.props["node.name"] // empty' \
            | grep -c '^effect_')
        t0=$(ticks "$pid")
        s0=$(date +%s%N)
        # pw-top's BUSY of each effect_ node, one row per sample, in microseconds
        timeout $((samples + 5)) pw-top -b -n $((samples + 1)) 2>/dev/null \
            | awk '$NF ~ /^effect_/ && $6 ~ /us$|ms$/ { v = $6; f = (v ~ /ms$/) ? 1000 : 1; sub(/[mu]s$/, "", v); print $NF, v * f }' \
            >"$runtime/busy-$arm-$n.txt"
        t1=$(ticks "$pid")
        s1=$(date +%s%N)
        busy=0
        for node in $(awk '{ print $1 }' "$runtime/busy-$arm-$n.txt" | sort -u); do
            m=$(awk -v node="$node" '$1 == node { print $2 }' "$runtime/busy-$arm-$n.txt" | median)
            busy=$(awk -v a="$busy" -v b="$m" 'BEGIN { print a + b }')
        done
        cpu=$(awk -v t="$((t1 - t0))" -v hz="$(getconf CLK_TCK)" -v ns="$((s1 - s0))" 'BEGIN { printf "%.2f", 100 * t / hz / (ns / 1e9) }')
        rss=$(awk '/^VmRSS:/ { print $2 }' "/proc/$pid/status")
        printf '%s,%d,%d,%d,%d,%.1f,%.2f,%s,%s\n' "$arm" "$n" "$rate" "$quantum" "$nodes" "$busy" \
            "$(awk -v b="$busy" -v n="$n" 'BEGIN { print b / n }')" "$cpu" "$rss"
        send "quit" >/dev/null
        exec 3>&-
        wait "$pid" 2>/dev/null
    done
done
