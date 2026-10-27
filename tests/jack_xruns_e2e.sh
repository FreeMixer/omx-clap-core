#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The xruns line over jack, end to end (#371), in a PipeWire of its own: the script re-runs itself in a private user,
# net, pid and mount namespace, starts pipewire on a private runtime dir and runs the host, with a feedback port, under
# tests/jack_xrun_shim.so. The shim calls the xrun callback the host registered with libjack, once per byte written to a
# FIFO, from a thread of its own: PipeWire's libjack calls it only when the driver reports an xrun and the dummy driver
# of a PipeWire with no sound card never does. The script reads the feedback socket with a time on every line and holds:
# the callback registered before its client is active; no xruns line while there is no xrun; `xruns <count>` for the
# first, at once; two lines never closer than a second, the count of the second the number of xruns so far; the count
# never going down. A host started with no feedback port takes the same xruns and goes on. Nothing here touches the
# PipeWire of the session that runs it.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5576); the feedback port is the next one
# OMX_HOST_EVENTS_XRUNS: the word data/verbs.json declares, read from src/omx_clap_core_vocabulary.h by make test-jack-xruns

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5576}
fbport=$((port + 1))
shim=$here/jack_xrun_shim.so

: "${OMX_HOST_EVENTS_XRUNS:?}"

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$shim"; do
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

runtime=$(mktemp -d /tmp/omx-clap-host-e2e.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
pids=()
cleanup() {
    exec 3>&- 4>&- 5>&- 2>/dev/null
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

fifo=$runtime/xrun.fifo
shimlog=$runtime/shim.log
mkfifo "$fifo"
: >"$shimlog"
exec 5<>"$fifo"
# xrun <n>: n xruns, as JACK reports them, one callback each
xrun() { for _ in $(seq "$1"); do printf x >&5; done; }
now_ms() { echo $(($(date +%s%N) / 1000000)); }

# start_host <socket port> [<feedback port>]: the host in the background, ready
start_host() {
    local args=(-n -p "$1")
    [ -n "${2:-}" ] && args+=(-f "$2")
    LD_PRELOAD=$shim JACK_XRUN_SHIM_FIFO=$fifo JACK_XRUN_SHIM_LOG=$shimlog "$host" "${args[@]}" >"$runtime/host.log" 2>&1 &
    host_pid=$!
    pids+=("$host_pid")
    for _ in $(seq 50); do grep -q "ready!" "$runtime/host.log" && break; sleep 0.1; done
    grep -q "ready!" "$runtime/host.log"
}

word=$OMX_HOST_EVENTS_XRUNS
feedback=$runtime/feedback.log
xlines() { grep " $word " "$feedback"; }

start_host "$port" "$fbport"
step $? "omx-clap-host ready on port $port, feedback on $fbport, under the shim"
[ "$(grep -c '^set_xrun ' "$shimlog")" -eq 1 ]; step $? "the host registered one xrun callback: $(tr '\n' '|' <"$shimlog")"
client=$(sed -n 's/^set_xrun //p' "$shimlog")
[ "$(grep -n "^set_xrun $client\$" "$shimlog" | cut -d: -f1)" -lt "$(grep -n "^activate $client\$" "$shimlog" | head -1 | cut -d: -f1)" ]
step $? "on a client that was not yet active, so that no xrun is missed between the two"

# a controller, as plugin-hostd is: the command socket and the feedback socket, which the host accepts together
connect() {
    exec 3<>"/dev/tcp/127.0.0.1/$1" 4<>"/dev/tcp/127.0.0.1/$2"
    : >"$feedback"
    while IFS= read -r -d '' line <&4; do echo "$(date +%s%N) $line" >>"$feedback"; done &
    reader=$!
    pids+=("$reader")
}
connect "$port" "$fbport"

sleep 1.5
[ "$(wc -l <"$feedback")" -eq 0 ]; step $? "nothing on the feedback port while there is no xrun"

t0=$(now_ms)
xrun 1
sleep 0.3
[ "$(xlines | wc -l)" -eq 1 ] && [ "$(xlines | sed 's/^[0-9]* //')" = "$word 1" ]
step $? "the first xrun is written at once: $(xlines | sed 's/^[0-9]* //' | tr '\n' '|')"
first=$(xlines | head -1 | cut -d' ' -f1)
[ $((first / 1000000 - t0)) -lt 500 ]; step $? "within $((first / 1000000 - t0)) ms of the callback"

xrun 3
sleep 0.5
[ "$(xlines | wc -l)" -eq 1 ]; step $? "three more inside the second wait: still one line"
sleep 1.2
[ "$(xlines | wc -l)" -eq 2 ] && [ "$(xlines | tail -1 | sed 's/^[0-9]* //')" = "$word 4" ]
step $? "then one line with the count so far: $(xlines | sed 's/^[0-9]* //' | tr '\n' '|')"
second=$(xlines | tail -1 | cut -d' ' -f1)
gap=$(((second - first) / 1000000))
[ "$gap" -ge 950 ]; step $? "the two lines are ${gap} ms apart, a second or more"

sleep 1.1
xrun 1
sleep 0.3
[ "$(xlines | wc -l)" -eq 3 ] && [ "$(xlines | tail -1 | sed 's/^[0-9]* //')" = "$word 5" ]
step $? "an xrun after a quiet second is written at once: $(xlines | tail -1 | sed 's/^[0-9]* //')"
sleep 1.5
[ "$(xlines | wc -l)" -eq 3 ]; step $? "and a count already written is not written again"
! xlines | grep -qv -E "^[0-9]+ $word [1-9][0-9]*\$"; step $? "every line is exactly '$word <count>'"

# no feedback port: the same xruns, the host runs on
kill "$host_pid"; wait "$host_pid" 2>/dev/null
start_host "$((port + 2))"; step $? "a host with no feedback port ready"
xrun 3; sleep 1.2
kill -0 "$host_pid" 2>/dev/null; step $? "and alive after xruns"
exec 3<>"/dev/tcp/127.0.0.1/$((port + 2))"
printf 'cpu_load\0' >&3
IFS= read -r -d '' -t 5 reply <&3
[ "${reply:0:6}" = "resp 0" ]; step $? "and answering: $reply"

echo "$failures failed"
[ "$failures" -eq 0 ]
