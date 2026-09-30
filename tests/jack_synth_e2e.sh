#!/usr/bin/env bash
#
# an instrument in omx-clap-host over jack, end to end, in a PipeWire of its own: the script
# re-runs itself in a private user, net, pid and mount namespace with its own
# /proc, starts pipewire on a private runtime dir (no session manager: nodes,
# ports, links and the dummy driver are the daemon's own), then drives the
# host over its socket
# and reads the graph back after every command. Exit 0 only when every step
# held; each step is printed. Nothing here touches the PipeWire of the session
# that runs it.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5561)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5561}
synth=$here/fake_synth.clap
probe=$here/jack_synth_probe

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$synth" "$probe"; do
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
        tail -5 "$runtime/host.log" 2>/dev/null | sed 's/^/     host: /'
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

"$host" -n -p "$port" >"$runtime/host.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q "ready!" "$runtime/host.log" && break; sleep 0.1; done
grep -q "ready!" "$runtime/host.log"
step $? "omx-clap-host ready on port $port"

exec 3<>"/dev/tcp/127.0.0.1/$port"
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
ports() {
    run pw-link -i 2>/dev/null; run pw-link -o 2>/dev/null
}
graph_has() {
    ports | grep -q -x -F "$1"
}
nodes() {
    run pw-dump 2>/dev/null | grep -c "\"node.name\": \"$1\""
}

midi_type() {
    run pw-dump 2>/dev/null | grep -B6 "\"port.alias\": \"$1\"" | grep -q "8 bit raw midi"
}
# the probe's line: before, note on at velocity 100, at 50, after the note off, the first note's peak
num() { echo "$1" | awk -v n="$2" '{print $n}'; }
above() { awk -v a="$1" -v b="$2" 'BEGIN { exit !(a > b) }'; }
near() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t) }'; }

instrument() {
    local n=$1 id=$2 out=$3 dialect=$4 line before on100 on50 after peak ratio p
    shift 4
    expect "add clap:$synth#org.omx-clap-host.test.$id $n" "resp $n"
    graph_has "effect_$n:midi_in"; step $? "effect_$n:midi_in is in the graph"
    midi_type "effect_$n:midi_in"; step $? "and it is a midi port"
    ! midi_type "effect_$n:out_1"; step $? "where the output beside it is not"
    for p in $(seq "$out"); do
        graph_has "effect_$n:out_$p"; step $? "effect_$n:out_$p is in the graph"
    done
    ! graph_has "effect_$n:in_1"; step $? "an instrument has no audio input port"
    ! graph_has "effect_$n:out_$((out + 1))"; step $? "and no output past its $out"
    local targets=()
    for p in $(seq "$out"); do targets+=("effect_$n:out_$p"); done
    line=$(run "$probe" "effect_$n:midi_in" "${targets[@]}" 2>"$runtime/probe.log")
    step $? "the probe played $id through $dialect: $line"
    before=$(num "$line" 1); on100=$(num "$line" 2); on50=$(num "$line" 3); after=$(num "$line" 4); peak=$(num "$line" 5)
    near "$before" 0 0.000001; step $? "silence before a note ($before)"
    # a sine of amplitude v/127 has RMS v/127/sqrt 2: 0.5567 at 100
    near "$on100" 0.5567 0.02; step $? "a note on at velocity 100 is 0.5567 RMS ($on100)"
    near "$peak" 0.7874 0.01; step $? "its peak is 100/127 ($peak)"
    ratio=$(awk -v a="$on50" -v b="$on100" 'BEGIN { printf "%.4f", a / b }')
    near "$ratio" 0.5 0.02; step $? "velocity 50 is half of velocity 100 ($ratio)"
    near "$after" 0 0.000001; step $? "silence after the note off ($after)"
}

instrument 0 synth 2 "the CLAP dialect"
instrument 1 synth-midi 1 "the MIDI dialect"

expect "add clap:$synth#org.omx-clap-host.test.synth-aux 2" "resp -102"
grep -q "1 sidechain/aux ports" "$runtime/host.log"; step $? "an instrument with a sidechain is refused with its reason"
expect "add clap:$synth#org.omx-clap-host.test.synth-wide 2" "resp -102"
grep -q "main port has 4 channels" "$runtime/host.log"; step $? "an instrument wider than stereo is refused with its reason"
expect "add clap:$synth#org.omx-clap-host.test.synth-notes 2" "resp -102"
grep -q "2 note inputs" "$runtime/host.log"; step $? "two note inputs are refused with the reason"
expect "add clap:$synth#org.omx-clap-host.test.silent 2" "resp -102"
grep -q "0 main inputs, 1 main outputs" "$runtime/host.log"; step $? "an effect with no input is refused with the reason"
! graph_has effect_2:midi_in && ! graph_has effect_2:out_1; step $? "no effect_2 port after the refusals"

expect "bypass 0 1" "resp 0"
expect "param_get 0 :bypass" "resp 0 1.0000"
expect "remove 0" "resp 0"
! graph_has effect_0:midi_in && ! graph_has effect_0:out_1; step $? "no effect_0 port after remove"
graph_has effect_1:midi_in; step $? "effect_1 still there"
expect "remove 1" "resp 0"
! graph_has effect_1:midi_in && ! graph_has effect_1:out_1; step $? "no effect_1 port after remove"
expect "quit" "resp 0"
sleep 0.3
! kill -0 "${pids[1]}" 2>/dev/null; step $? "host exited on quit"
[ "$(nodes effect_1)" -eq 0 ] && [ "$(nodes omx-clap-host)" -eq 0 ]; step $? "no host node left in the private graph"

if [ "$failures" -eq 0 ]; then
    echo "jack synth e2e ok"
    exit 0
fi
echo "jack synth e2e FAILED ($failures)"
exit 1
