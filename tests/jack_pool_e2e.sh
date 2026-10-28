#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# omx-clap-host's --thread-pool over jack, end to end, in a PipeWire of its own (#395): the script re-runs itself in a
# private user, net, pid and mount namespace with its own /proc, starts pipewire on a private runtime dir, then starts
# the host once per mode and adds tests/fake_pool.clap, whose process() asks for FAKE_POOL_TASKS tasks every block.
# With on the host makes one helper thread per core of its affinity mask but one (named omx-pool-<core>, at most 63),
# and after a few seconds of blocks the helpers have run: their CPU time in /proc is not zero. With qualified (the
# default, no option) and off it makes none. Nothing here touches the PipeWire of the session that runs it.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5566); the feedback port is the next one

set -u

here=$(cd "$(dirname "$0")" && pwd)
. "${PLUGIN_HOSTD_TESTS_LIB:-$here/../../plugin-hostd/tests/lib}/protocol.sh"
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5566}
fake=$here/fake_pool.clap
id=org.omx-clap-host.test.pool
uri="clap:$fake#$id"

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$fake"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    JACK_E2E_INSIDE=1 OMX_CLAP_HOST=$host JACK_E2E_PORT=$port \
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
fi

failures=0
step() {
    if [ "$1" -eq 0 ]; then
        echo "ok   $2"
    else
        echo "FAIL $2"
        failures=$((failures + 1))
        tail -5 "$runtime/host-$mode.log" 2>/dev/null | sed 's/^/     host: /'
    fi
}

runtime=$(mktemp -d /tmp/omx-clap-host-e2e.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
pids=()
cleanup() {
    exec 3>&- 4>&- 2>/dev/null
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    kill -TERM -- -1 2>/dev/null
    wait 2>/dev/null
    rm -rf "${runtime:?}"
}
trap cleanup EXIT
mode=setup
[ "$$" = 1 ]; step $? "pid 1 of a private pid namespace"

ip link set lo up

pipewire >"$runtime/pw.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do [ -S "$runtime/pipewire-0" ] && break; sleep 0.1; done
[ -S "$runtime/pipewire-0" ] || { echo "pipewire did not come up" >&2; cat "$runtime/pw.log" >&2; exit 2; }
step 0 "private pipewire on $runtime"

cores=$(nproc)
want=$((cores - 1))
[ "$want" -gt 63 ] && want=63

PROTOCOL_TIMEOUT=5
expect() {
    local reply
    reply=$(send "$1")
    [ "$reply" = "$2" ]
    step $? "$mode: $1 -> $reply"
}

# the helpers of the host `pid`: how many, and how many have run (utime + stime, fields 14 and 15 of their stat)
helpers() { grep -l '^omx-pool-' /proc/"$1"/task/*/comm 2>/dev/null | wc -l; }
helpers_ran() {
    local n=0 t
    for t in $(grep -l '^omx-pool-' /proc/"$1"/task/*/comm 2>/dev/null); do
        [ "$(awk '{ print $14 + $15 }' "${t%/comm}/stat")" -gt 0 ] && n=$((n + 1))
    done
    echo "$n"
}

for mode in on qualified off; do
    case $mode in
        qualified) opt=() ;;
        *) opt=("--thread-pool=$mode") ;;
    esac
    exec 3>&- 4>&-
    port=$((port + 2))
    FAKE_POOL_TASKS=16 FAKE_POOL_VOICES=64 "$host" -n -p "$port" -f "$((port + 1))" "${opt[@]}" >"$runtime/host-$mode.log" 2>&1 &
    hpid=$!
    pids+=($hpid)
    protocol_wait_ready "$runtime/host-$mode.log"
    step $? "$mode: omx-clap-host ${opt[*]:-(no option)} ready on port $port"
    exec 3<>"/dev/tcp/127.0.0.1/$port"
    exec 4<>"/dev/tcp/127.0.0.1/$((port + 1))"
    cat <&4 >"$runtime/feedback-$mode.log" &
    pids+=($!)
    expect "add $uri 0" "resp 0"
    sleep 3
    n=$(helpers "$hpid")
    ran=$(helpers_ran "$hpid")
    if [ "$mode" = on ]; then
        [ "$n" -eq "$want" ]
        step $? "on: $n helper threads for $cores cores (want $want)"
        [ "$want" -eq 0 ] || [ "$ran" -ge 1 ]
        step $? "on: $ran of them ran the plugin's tasks"
    else
        [ "$n" -eq 0 ]
        step $? "$mode: no helper thread ($n)"
    fi
    expect "remove 0" "resp 0"
    sleep 0.5
    [ "$(helpers "$hpid")" -eq 0 ]
    step $? "$mode: none left once the plugin is removed"
    expect "quit" "resp 0"
done

if [ "$failures" -eq 0 ]; then
    echo "jack pool e2e ok"
    exit 0
fi
echo "jack pool e2e FAILED ($failures)"
exit 1
