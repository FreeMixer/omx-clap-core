#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# a plugin's note output in omx-clap-host over jack, end to end, in a PipeWire of its own: the script
# re-runs itself in a private user, net, pid and mount namespace with its own
# /proc, starts pipewire on a private runtime dir (no session manager: nodes,
# ports, links and the dummy driver are the daemon's own), then drives the
# host over its socket
# and reads the graph back after every command. Exit 0 only when every step
# held; each step is printed. Nothing here touches the PipeWire of the session
# that runs it.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# LV2_BUNDLES:      the directory of the fixture bundles (default ./build/lv2)
# JACK_E2E_PORT:    the socket port (default 5569)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
bundles=${LV2_BUNDLES:-$root/build/lv2}
port=${JACK_E2E_PORT:-5569}
synth=$here/fake_synth.clap
probe=$here/jack_notes_out_probe

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$synth" "$probe" "$bundles/omx-midi-echo.lv2/manifest.ttl"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    JACK_E2E_INSIDE=1 OMX_CLAP_HOST=$host LV2_BUNDLES=$(readlink -f "$bundles") JACK_E2E_PORT=$port \
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

LV2_PATH=$bundles "$host" -n -p "$port" >"$runtime/host.log" 2>&1 &
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
# the probe's output, then what it must hold: "<frame> <bytes>" lines, frames counted from the note on it sent
probe_lines() {
    run "$probe" "effect_$1:midi_in" "effect_$1:midi_out" 2>"$runtime/probe.log"
}

# a plugin with a note output gets midi_out, a midi port, beside its midi_in
ports_of() {
    local n=$1 what=$2
    graph_has "effect_$n:midi_out"; step $? "$what: effect_$n:midi_out is in the graph"
    midi_type "effect_$n:midi_out"; step $? "$what: and it is a midi port"
    graph_has "effect_$n:midi_in"; step $? "$what: beside its midi_in"
}

# synth-arp: a note on of key 60 at frame f sends 60, 64 and 67 at f, f + s and f + 2s (s 2 ms at the graph's rate);
# its note off sends their three note offs at its frame, and the plugin's NOTE_END is not sent
expect "add clap:$synth#org.omx-clap-host.test.synth-arp 0" "resp 0"
ports_of 0 "an arpeggiator"
out=$(probe_lines 0); rc=$?
step $rc "the probe played one note into the arpeggiator"
rate=$(echo "$out" | awk '$1 == "rate" { print $2 }')
off=$(echo "$out" | awk '$1 == "off" { print $2 }')
s=$(awk -v r="${rate:-0}" 'BEGIN { printf "%d", r * 2 / 1000 + 0.5 }')
got=$(echo "$out" | grep -v -e '^rate' -e '^off')
want=$(printf '0 90 3c 64\n%d 90 40 64\n%d 90 43 64\n%d 80 3c 00\n%d 80 40 00\n%d 80 43 00' "$s" "$((2 * s))" "$off" "$off" "$off")
[ -n "$rate" ] && [ "$got" = "$want" ]
step $? "at $rate Hz, step $s: the three note ons at 0, $s and $((2 * s)), the three note offs at $off, nothing else: $(echo $got)"
expect "note_out_drops 0" "resp 0 0 0 0"

# the same through CLAP_EVENT_MIDI
expect "add clap:$synth#org.omx-clap-host.test.synth-arp-midi 1" "resp 1"
ports_of 1 "an arpeggiator in the MIDI dialect"
out=$(probe_lines 1)
step $? "the probe played one note into it"
off=$(echo "$out" | awk '$1 == "off" { print $2 }')
got=$(echo "$out" | grep -v -e '^rate' -e '^off')
want=$(printf '0 90 3c 64\n%d 90 40 64\n%d 90 43 64\n%d 80 3c 00\n%d 80 40 00\n%d 80 43 00' "$s" "$((2 * s))" "$off" "$off" "$off")
[ "$got" = "$want" ]
step $? "the same messages at the same frames: $(echo $got)"
expect "note_out_drops 1" "resp 0 0 0 0"

# an LV2 plugin's MIDI output, through the adapter: the echo arrives on midi_out at the frame it was played
expect "add lv2:urn:openmixer:test:midi-echo 2" "resp 2"
ports_of 2 "the LV2 midi echo"
out=$(probe_lines 2)
step $? "the probe played one note into the LV2 echo"
off=$(echo "$out" | awk '$1 == "off" { print $2 }')
got=$(echo "$out" | grep -v -e '^rate' -e '^off')
want=$(printf '0 90 3c 64\n%d 80 3c 00' "$off")
[ "$got" = "$want" ]
step $? "the echo of the note on at 0 and of its note off at $off: $(echo $got)"
expect "note_out_drops 2" "resp 0 0 0 0"

# a plugin with no note output has no midi_out
expect "add clap:$synth#org.omx-clap-host.test.synth 3" "resp 3"
graph_has effect_3:midi_in && ! graph_has effect_3:midi_out; step $? "an instrument with no note output has midi_in and no midi_out"
expect "note_out_drops 7" "resp -3"

for n in 0 1 2 3; do
    expect "remove $n" "resp 0"
    ! graph_has "effect_$n:midi_out" && ! graph_has "effect_$n:midi_in"; step $? "no effect_$n port after remove"
done
expect "quit" "resp 0"
sleep 0.3
! kill -0 "${pids[1]}" 2>/dev/null; step $? "host exited on quit"

if [ "$failures" -eq 0 ]; then
    echo "jack notes out e2e ok"
    exit 0
fi
echo "jack notes out e2e FAILED ($failures)"
exit 1
