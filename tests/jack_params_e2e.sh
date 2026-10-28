#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A plugin that moves its own parameter, over jack, end to end, in a PipeWire of its own: the script re-runs itself in a
# private user, net, pid and mount namespace with its own /proc, starts pipewire on a private runtime dir, adds the
# mover (tests/fake_mover.clap) and reads the host's feedback socket. On its eighth block the mover takes hold of its
# level, moves it to 0.75 and lets it go: the feedback carries param_gesture begin, param_set 0.75 and param_gesture end,
# in that order, once. A second host runs it with FAKE_MOVER_FLOOD=300, more values than the library's ring holds
# between two idle ticks: the feedback ends on what the plugin holds, read again. Nothing here touches the PipeWire of
# the session that runs it. A third runs the same flood while the plugin holds its level, so the ring has no room for the
# gesture's end: the feedback still closes the gesture it opened, once, after the drop.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5566); the feedback port is the next one

set -u

here=$(cd "$(dirname "$0")" && pwd)
. "${PLUGIN_HOSTD_TESTS_LIB:-$here/../../plugin-hostd/tests/lib}/protocol.sh"
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5566}
mover=$here/fake_mover.clap
uri="clap:$mover#org.omx-clap-host.test.mover"

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$mover"; do
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
        tail -5 "$runtime/host.log" 2>/dev/null | sed 's/^/     host: /'
    fi
}

runtime=$(mktemp -d /tmp/omx-clap-host-params.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS FAKE_MOVER FAKE_MOVER_AT FAKE_MOVER_FLOOD FAKE_MOVER_HELD
pids=()
cleanup() {
    exec 3>&- 4>&- 2>/dev/null
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    kill -TERM -- -1 2>/dev/null
    wait 2>/dev/null
    rm -rf "${runtime:?}"
}
trap cleanup EXIT
[ "$$" = 1 ]; step $? "pid 1 of a private pid namespace"

ip link set lo up
pipewire >"$runtime/pw.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do [ -S "$runtime/pipewire-0" ] && break; sleep 0.1; done
[ -S "$runtime/pipewire-0" ] || { echo "pipewire did not come up" >&2; cat "$runtime/pw.log" >&2; exit 2; }
step 0 "private pipewire on $runtime"

# a host on `port`, its feedback read into $runtime/feedback-<name>.log; the environment after the name is the host's
start_host() {
    local name=$1
    shift
    exec 3>&- 4>&- 2>/dev/null
    env "$@" "$host" -n -p "$port" -f "$((port + 1))" >"$runtime/host.log" 2>&1 &
    pids+=($!)
    protocol_wait_ready "$runtime/host.log"
    step $? "omx-clap-host ($name) ready on port $port"
    exec 3<>"/dev/tcp/127.0.0.1/$port"
    exec 4<>"/dev/tcp/127.0.0.1/$((port + 1))"
    feedback=$runtime/feedback-$name.log
    : >"$feedback"
    while IFS= read -r -d '' line <&4; do echo "$line" >>"$feedback"; done &
    pids+=($!)
}

PROTOCOL_TIMEOUT=5
expect() {
    local reply
    reply=$(send "$1")
    [ "$reply" = "$2" ]
    step $? "$1 -> $reply"
}
# the feedback lines about instance 0's parameters, in order
params() { grep -E '^param_(set|gesture) 0 ' "$feedback"; }
wait_for() { for _ in $(seq 30); do grep -q -x "$1" "$feedback" && return 0; sleep 0.1; done; return 1; }

start_host moves FAKE_MOVER_AT=8
expect "add $uri 0" "resp 0"
wait_for "param_gesture 0 0 end"; step $? "the gesture's end reached the feedback port"
[ "$(params | tr '\n' '|')" = "param_gesture 0 0 begin|param_set 0 0 0.750000|param_gesture 0 0 end|" ]
step $? "the plugin's move, once and in order: $(params | tr '\n' '|')"
expect "param_get 0 0" "resp 0 0.7500"
expect "param_set 0 0 0.3" "resp 0"
expect "param_get 0 0" "resp 0 0.3000"
sleep 0.2
[ "$(params | wc -l)" -eq 3 ]; step $? "a value the controller sets is not echoed back to it"
expect "remove 0" "resp 0"
expect "quit" "resp 0"

port=$((port + 2))
start_host flood FAKE_MOVER_AT=8 FAKE_MOVER_FLOOD=300
expect "add $uri 0" "resp 0"
for _ in $(seq 30); do params | grep -q '^param_set 0 3 0.300000$' && break; sleep 0.1; done
[ "$(params | head -3 | tr '\n' '|')" = "param_gesture 0 0 begin|param_set 0 0 0.750000|param_gesture 0 0 end|" ]
step $? "the move first, in order"
[ "$(params | grep '^param_set 0 3 ' | tail -1)" = "param_set 0 3 0.300000" ]
step $? "300 values of trim, more than the ring holds: the last line is what the plugin holds ($(params | grep '^param_set 0 3 ' | tail -1))"
[ "$(params | grep -c '^param_set 0 3 ')" -lt 300 ]
step $? "and fewer lines than values: the last of each batch, then the read ($(params | grep -c '^param_set 0 3 ') lines)"
[ "$(params | grep '^param_set 0 0 ' | tail -1)" = "param_set 0 0 0.750000" ]
step $? "the read again covers every row: level 0.75"
expect "quit" "resp 0"

# the end the ring had no room for: a controller is never left holding a gesture the plugin let go
port=$((port + 2))
start_host held FAKE_MOVER_AT=8 FAKE_MOVER_FLOOD=300 FAKE_MOVER_HELD=1
expect "add $uri 0" "resp 0"
for _ in $(seq 30); do params | grep -q '^param_set 0 3 0.300000$' && break; sleep 0.1; done
wait_for "param_gesture 0 0 end"; step $? "the dropped end of level's gesture is closed on the feedback port"
[ "$(params | grep '^param_gesture 0 0 ' | tr '\n' '|')" = "param_gesture 0 0 begin|param_gesture 0 0 end|" ]
step $? "one begin and one end, in that order: $(params | grep '^param_gesture 0 0 ' | tr '\n' '|')"
[ "$(params | grep '^param_set 0 0 ' | tail -1)" = "param_set 0 0 0.750000" ]
step $? "and level's value is what the plugin holds"
expect "quit" "resp 0"

if [ "$failures" -eq 0 ]; then
    echo "jack params e2e ok"
    exit 0
fi
echo "jack params e2e FAILED ($failures)"
exit 1
