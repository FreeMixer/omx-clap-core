#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
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
fake=$here/fake.clap
probe=$here/jack_synth_probe

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$synth" "$fake" "$probe"; do
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
pw_pid=$!
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

# ---- plugin-hostd's note verbs on the command socket (#122): the same notes, played without a jack MIDI client ----
expect "note_input 0" "resp 0 midi_in"
expect "add clap:$fake#org.omx-clap-host.test.passthrough 3" "resp 3"
expect "note_input 3" "resp 0 -"
expect "note_on 3 0 60 100" "resp -513"
expect "note_off 3 0 60 0" "resp -513"
expect "all_notes_off 3" "resp -513"
expect "remove 3" "resp 0"
expect "note_input 3" "resp -3"
expect "note_on 0 16 60 100" "resp -902"
expect "note_on 0 0 128 100" "resp -902"
expect "note_on 0 0 60 0" "resp -902"
expect "note_off 0 0 60 128" "resp -902"
coproc listen { timeout 20 "$probe" - effect_0:out_1 effect_0:out_2 2>"$runtime/listen.log"; }
window() { echo >&"${listen[1]}"; IFS= read -r -t 5 w <&"${listen[0]}"; echo "$w"; }
sleep 0.6
w=$(window); near "${w%% *}" 0 0.000001; step $? "socket notes: silence before a note ($w)"
expect "note_on 0 0 69 100" "resp 0"
sleep 0.1
w=$(window); near "${w%% *}" 0.5567 0.02; step $? "note_on 0 0 69 100 on the socket sounds at 0.5567 RMS, as through midi_in ($w)"
near "${w##* }" 0.7874 0.01; step $? "its peak is 100/127 (${w##* })"
expect "note_off 0 0 69 0" "resp 0"
sleep 0.1
w=$(window); near "${w%% *}" 0 0.000001; step $? "note_off silences it ($w)"
expect "note_on 0 0 60 100" "resp 0"
expect "note_on 0 0 64 100" "resp 0"
expect "note_on 0 0 67 100" "resp 0"
sleep 0.1
w=$(window); above "${w%% *}" 0.1; step $? "a chord on the socket sounds ($w)"
expect "all_notes_off 0" "resp 0"
sleep 0.1
w=$(window); near "${w%% *}" 0 0.000001; step $? "all_notes_off silences the chord ($w)"
exec {listen[1]}>&-
wait "$listen_PID" 2>/dev/null


expect "add clap:$synth#org.omx-clap-host.test.synth-aux 2" "resp 2"
expect "remove 2" "resp 0"
expect "add clap:$synth#org.omx-clap-host.test.synth-wide 2" "resp -102 hosting.topology.wider-than-strip"
grep -q "hosting.topology.wider-than-strip" "$runtime/host.log"; step $? "an instrument wider than stereo is refused with its reason"
expect "add clap:$synth#org.omx-clap-host.test.synth-notes 2" "resp -102 hosting.clap.note-input"
grep -q "hosting.clap.note-input" "$runtime/host.log"; step $? "two note inputs are refused with the reason"
expect "add clap:$synth#org.omx-clap-host.test.silent 2" "resp -102 hosting.topology.no-audio-input"
grep -q "hosting.topology.no-audio-input" "$runtime/host.log"; step $? "an effect with no input is refused with the reason"
! graph_has effect_2:midi_in && ! graph_has effect_2:out_1; step $? "no effect_2 port after the refusals"

# the notes the probe played fit their blocks: nothing dropped, no release lost, no sysex, nothing unmappable
expect "note_drops 0" "resp 0 0 0 0 0"
expect "note_drops 1" "resp 0 0 0 0 0"
expect "note_drops 7" "resp -3"

# a graph that runs no cycle drains no note: the queue fills, and a verb with no room answers -517 and queues nothing
kill -STOP "$pw_pid"
full=0 queued=0
for _ in $(seq 600); do
    case $(send "note_on 0 0 60 100") in
        "resp 0") queued=$((queued + 1)) ;;
        "resp -517") full=$((full + 1)) ;;
    esac
done
[ "$full" -gt 0 ] && [ "$queued" -gt 0 ] && [ $((queued + full)) -eq 600 ]
step $? "with the graph stopped, $queued note_on are queued and the other $full answer -517 (the note queue is full)"
expect "all_notes_off 0" "resp -517"
kill -CONT "$pw_pid"
sleep 0.5
expect "all_notes_off 0" "resp 0"
# the 511 reach one cycle, which holds 256 note events and keeps half for releases: the rest are dropped and counted
[ "$(send "note_drops 0" | awk '{ print $3 }')" -gt 0 ]; step $? "the cycle that took them counts the note ons it had no room for"
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
