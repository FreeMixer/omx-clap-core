#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# the transport omx-clap-host hands its plugins, over jack, end to end, in a PipeWire of its own: the script
# re-runs itself in a private user, net, pid and mount namespace with its own
# /proc, starts pipewire on a private runtime dir (no session manager: nodes,
# ports, links and the dummy driver are the daemon's own), then drives the
# host over its socket
# and reads the graph back after every command. Exit 0 only when every step
# held; each step is printed. Nothing here touches the PipeWire of the session
# that runs it.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5571); the host that follows the JACK transport takes the next one

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5571}
probe=$here/fake_transport.clap
ctl=$here/jack_transport_ctl

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$probe" "$ctl"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    JACK_E2E_INSIDE=1 OMX_CLAP_HOST=$host JACK_E2E_PORT=$port \
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
fi

failures=0
# a step that fails names what was alive and where it waited
step() {
    if [ "$1" -eq 0 ]; then
        echo "ok   $2"
    else
        echo "FAIL $2"
        failures=$((failures + 1))
        ps -o pid,stat,wchan:16,etimes,cmd --no-headers 2>/dev/null | sed 's/^/     /'
        tail -5 "$runtime"/host-*.log 2>/dev/null | sed 's/^/     host: /'
    fi
}
# nothing the graph answers may hang the run: 10 s and the step fails
run() {
    timeout 10 "$@"
}

runtime=$(mktemp -d /tmp/omx-clap-host-e2e.XXXXXX)
state_dir=$runtime/state
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
pids=()
# pid 1 of the namespace: every process in it dies with this script, the trap only makes it orderly
cleanup() {
    exec 3>&- 2>/dev/null
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

# start a host with the options given and open its command socket on fd 3
host_up() {
    local p=$1
    shift
    "$host" -n -p "$p" "$@" >"$runtime/host-$p.log" 2>&1 &
    pids+=($!)
    for _ in $(seq 50); do grep -q "ready!" "$runtime/host-$p.log" && break; sleep 0.1; done
    grep -q "ready!" "$runtime/host-$p.log"
    step $? "omx-clap-host $* ready on port $p"
    exec 3<>"/dev/tcp/127.0.0.1/$p"
}
send() {
    printf '%s\0' "$1" >&3
    IFS= read -r -d '' -t 5 reply <&3
    echo "$reply"
}
expect() {
    local reply
    reply=$(send "$1")
    [ "$reply" = "$2" ]
    step $? "$1 -> $reply"
}
# one parameter of the probe, as param_get answers it
probe_get() {
    send "param_get 0 $1" | awk '{ print $3 }'
}
near() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t) }'; }
settle() { sleep 0.3; }

# --- the host's own transport ---
host_up "$port"
expect "add clap:$probe#org.omx-clap-host.test.transport-probe 0" "resp 0"
settle
[ "$(probe_get 0)" = "0.0000" ]; step $? "before any tempo the plugin is handed no transport"
expect "set_bpm 120" "resp 0"
settle
[ "$(probe_get 0)" = "1.0000" ] && [ "$(probe_get 1)" = "120.0000" ] && [ "$(probe_get 2)" = "0.0000" ] && [ "$(probe_get 3)" = "4.0000" ] &&
    [ "$(probe_get 4)" = "4.0000" ] && [ "$(probe_get 5)" = "0.0000" ]
step $? "set_bpm 120: a transport at 120 bpm, 4/4, stopped at beat 0"
expect "transport 1 4 120" "resp 0"
settle
[ "$(probe_get 1)" = "120.0000" ] && [ "$(probe_get 2)" = "1.0000" ] && [ "$(probe_get 3)" = "4.0000" ]
step $? "transport 1 4 120: the probe reads 120 bpm, playing, 4 beats a bar"
b1=$(probe_get 5); s1=$(probe_get 6)
sleep 1
b2=$(probe_get 5); s2=$(probe_get 6)
db=$(awk -v a="$b1" -v b="$b2" 'BEGIN { printf "%.4f", b - a }'); ds=$(awk -v a="$s1" -v b="$s2" 'BEGIN { printf "%.4f", b - a }')
near "$db" 2 0.25 && near "$(awk -v d="$db" -v s="$ds" 'BEGIN { print d - 2 * s }')" 0 0.0005
step $? "a second of playing moves it 2 beats at 120 bpm, beats and seconds in step ($db beats, $ds s)"
expect "transport 0 4 120" "resp 0"
settle
b3=$(probe_get 5)
sleep 0.5
b4=$(probe_get 5)
[ "$(probe_get 2)" = "0.0000" ] && [ "$b3" = "$b4" ]
step $? "transport 0: stopped, the position holds ($b3, $b4)"
expect "set_bpb 3" "resp 0"
settle
[ "$(probe_get 3)" = "3.0000" ] && [ "$(probe_get 5)" = "$b4" ]; step $? "set_bpb 3: 3/4, the position kept"
expect "transport 1 3 60" "resp 0"
sleep 1
b5=$(probe_get 5)
near "$(awk -v a="$b4" -v b="$b5" 'BEGIN { print b - a }')" 1 0.3; step $? "played again at 60 bpm: it goes on from $b4, a beat a second ($b5)"
expect "set_bpm 300" "resp -207"
expect "set_bpm 10" "resp -207"
expect "set_bpb 0" "resp -207"
expect "transport 1 4 0" "resp -207"
expect "transport_sync none" "resp 0"
expect "transport_sync link" "resp -902"
expect "transport_sync midi" "resp -902"
expect "remove 0" "resp 0"
expect "quit" "resp 0"
exec 3>&-

# --- following the JACK transport (-t) ---
host_up "$((port + 1))" -t
expect "add clap:$probe#org.omx-clap-host.test.transport-probe 0" "resp 0"
out=$(run "$ctl" stop); out=$(run "$ctl" locate 0)
step $? "the JACK transport stands at frame 0: $out"
rate=$(echo "$out" | awk '{ print $3 }')
settle
[ "$(probe_get 0)" = "1.0000" ] && [ "$(probe_get 2)" = "0.0000" ] && [ "$(probe_get 6)" = "0.0000" ]
step $? "with -t and no tempo the plugin is handed the JACK transport: stopped, 0 s (beats $(probe_get 5))"
expect "set_bpm 120" "resp 0"
out=$(run "$ctl" locate "$((3 * rate))")
step $? "locate to 3 s: $out"
settle
[ "$(probe_get 6)" = "3.0000" ]; step $? "the plugin's song position is 3 s ($(probe_get 6), beats $(probe_get 5), tempo $(probe_get 1))"
out=$(run "$ctl" start)
step $? "the JACK transport starts: $out"
settle
[ "$(probe_get 2)" = "1.0000" ]; step $? "the plugin reads playing"
sleep 1
out=$(run "$ctl" stop)
step $? "and stops: $out"
frame=$(echo "$out" | awk '{ print $2 }')
settle
s=$(probe_get 6)
want=$(awk -v f="$frame" -v r="$rate" 'BEGIN { printf "%.4f", f / r }')
[ "$(probe_get 2)" = "0.0000" ] && [ "$s" = "$want" ] && [ "$frame" -gt "$((3 * rate + rate / 2))" ]
step $? "it moved the song position by the frames played: frame $frame is $want s, the plugin reads $s"
expect "transport 1 4 120" "resp 0"
out=$(run "$ctl" query)
[ "$(echo "$out" | awk '{ print $1 }')" = "0" ]; step $? "transport never moves the JACK transport it follows: $out"
expect "remove 0" "resp 0"
expect "quit" "resp 0"

if [ "$failures" -eq 0 ]; then
    echo "jack transport e2e ok"
    exit 0
fi
echo "jack transport e2e FAILED ($failures)"
exit 1
