#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# An LV2 plugin through omx-clap-host over jack, end to end, in a PipeWire of
# its own: the host finds the fixture bundle on LV2_PATH, runs it through the
# LV2 adapter on the port layout of a CLAP plugin, answers each refusal of the
# adapter with its hosting code, addresses the control ports by their LV2
# symbols, maps lv2:enabled onto its bypass, saves, loads and takes a preset
# by its URI, makes the path fixture's state files in its state root
# ($XDG_STATE_HOME/omx-clap-host, or -s) and takes them back on remove, and
# removes the plugin leaving no port behind. The values are
# read from the plugin's own meter port: a constant input (jack_meter_source,
# 0.5) through the fixture, out = in * 10^((gain_db + offset_db) / 20), the
# offset -6 dB from the default state and -3 dB from the preset `quiet`, and a
# second fixture after the first as a probe of what the first passes. Exit 0
# only when every step held; each step is printed.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# LV2_BUNDLES:      the directory of the fixture bundles (default ./build/lv2)
# JACK_E2E_PORT:    the socket port (default 5566); the feedback port is the next one

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
bundles=${LV2_BUNDLES:-$root/build/lv2}
port=${JACK_E2E_PORT:-5566}
fbport=$((port + 1))
fake=$here/fake.clap
source=$here/jack_meter_source
probe=$here/jack_synth_probe
fixture=urn:openmixer:test:host-fixture

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$bundles/omx-host-fixture.lv2/manifest.ttl" "$fake" "$source" "$probe"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    JACK_E2E_INSIDE=1 OMX_CLAP_HOST=$host LV2_BUNDLES=$(readlink -f "$bundles") JACK_E2E_PORT=$port \
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

runtime=$(mktemp -d /tmp/omx-clap-host-lv2.XXXXXX)
feedback=$runtime/feedback.log
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
# the host's state root is $XDG_STATE_HOME/omx-clap-host: here, never the caller's
export XDG_STATE_HOME=$runtime/xdg-state
sroot=$XDG_STATE_HOME/omx-clap-host
pathfix=urn:openmixer:test:path-fixture
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
[ "$$" = 1 ]; step $? "pid 1 of a private pid namespace"

ip link set lo up
pipewire >"$runtime/pw.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do [ -S "$runtime/pipewire-0" ] && break; sleep 0.1; done
[ -S "$runtime/pipewire-0" ] || { echo "pipewire did not come up" >&2; cat "$runtime/pw.log" >&2; exit 2; }
step 0 "private pipewire on $runtime"

# a bundle whose plugin has no binary: the adapter reads it and refuses it at init
mkdir -p "$runtime/extra/nobin.lv2"
cat >"$runtime/extra/nobin.lv2/manifest.ttl" <<'TTL'
@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .
@prefix t:    <urn:openmixer:test:> .
t:nobin a lv2:Plugin ; lv2:binary <nobin.so> ; rdfs:seeAlso <nobin.ttl> .
TTL
cat >"$runtime/extra/nobin.lv2/nobin.ttl" <<'TTL'
@prefix doap: <http://usefulinc.com/ns/doap#> .
@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
<urn:openmixer:test:nobin> a lv2:Plugin ; doap:name "no binary" ;
    lv2:port [ a lv2:AudioPort , lv2:InputPort ; lv2:index 0 ; lv2:symbol "in" ; lv2:name "In" ] ,
             [ a lv2:AudioPort , lv2:OutputPort ; lv2:index 1 ; lv2:symbol "out" ; lv2:name "Out" ] .
TTL

# the bundles are found on LV2_PATH, as plugin-hostd narrows it, and nowhere else
LV2_PATH=$bundles:$runtime/extra "$host" -n -p "$port" -f "$fbport" >"$runtime/host.log" 2>&1 &
host_pid=$!
pids+=($host_pid)
for _ in $(seq 50); do grep -q "ready!" "$runtime/host.log" && break; sleep 0.1; done
grep -q "ready!" "$runtime/host.log"
step $? "omx-clap-host ready on port $port with LV2_PATH=$bundles:$runtime/extra"

exec 3<>"/dev/tcp/127.0.0.1/$port"
exec 4<>"/dev/tcp/127.0.0.1/$fbport"
: >"$feedback"
while IFS= read -r -d '' line <&4; do echo "$line" >>"$feedback"; done &
pids+=($!)

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
near() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t) }'; }
db() { awk -v x="$1" -v d="$2" 'BEGIN { printf "%.6f", x * exp(d / 20 * log(10)) }'; }
# param_get of a symbol settles on `want` within `tol` in up to 3 s: a write, a bypass or a preset lands on a later
# cycle; param_get answers 4 decimals, so 0.00006 is the tolerance of a value read through it
reads() {
    local instance=$1 symbol=$2 want=$3 tol=$4 what=$5 reply v
    for _ in $(seq 30); do
        reply=$(send "param_get $instance $symbol")
        v=${reply#resp 0 }
        [ "$v" != "$reply" ] && near "$v" "$want" "$tol" && break
        sleep 0.1
    done
    [ "$v" != "$reply" ] && near "$v" "$want" "$tol"
    step $? "$what: param_get $instance $symbol reads $want ($reply)"
}
last() { awk -v i="$1" -v s="$2" '$1 == "output_set" && $2 == i && $3 == s { v = $4 } END { print v }' "$feedback"; }
ports() { pw-link "-$2" 2>/dev/null | grep "^$1:" | sort | tr '\n' ' '; }

# ---- the add: lv2:<uri> and the bare URI, the layout of a CLAP plugin ----
expect "add lv2:$fixture 0" "resp 0"
[ "$(ports effect_0 i)" = "effect_0:in_1 " ]; step $? "effect_0's inputs: $(ports effect_0 i)(in_1 alone: a mono plugin with no MIDI input has no midi_in)"
[ "$(ports effect_0 o)" = "effect_0:out_1 " ]; step $? "effect_0's outputs: $(ports effect_0 o)"
expect "add $fixture 1" "resp 1"
[ "$(ports effect_1 i)" = "effect_1:in_1 " ]; step $? "the bare URI, as mod-host takes it, is the same plugin: $(ports effect_1 i)"
expect "add lv2:$fixture 3" "resp 3"

# ---- refusals: the adapter's code after the error code, and on stderr ----
expect "add lv2:$fixture#wide 2" "resp -102 hosting.topology.wider-than-strip"
expect "add lv2:$fixture#atom 2" "resp -102 hosting.features.atom-in-unfed"
# a second MIDI input is admitted, fed empty (#199): the client carries one midi_in, the first port's
expect "add lv2:$fixture#midi-two 2" "resp 2"
[ "$(ports effect_2 i)" = "effect_2:in_1 effect_2:midi_in " ]; step $? "the two-MIDI-input twin's inputs: $(ports effect_2 i)(one midi_in: the first port's)"
expect "remove 2" "resp 0"
# bufsz:fixedBlockLength is declared by omx-clap-host (JACK's cycle is the buffer size), so the twin requiring it is admitted;
# state:mapPath is offered since #94, so the host fixture's #mappath twin passes the feature check and is refused later, no
# realisation: that bundle holds no code for it (the path fixture holds the realisation, tested in lv2_host_test and lv2_clap_test)
expect "add lv2:$fixture#fixed 2" "resp 2"
expect "remove 2" "resp 0"
expect "add lv2:$fixture#mappath 2" "resp -102 hosting.no-realisation"
expect "add lv2:$fixture#no-ports 2" "resp -102 hosting.topology.no-audio-output"
for code in hosting.topology.wider-than-strip hosting.features.atom-in-unfed hosting.no-realisation hosting.topology.no-audio-output; do
    grep -q "^$fixture#[a-z-]*: $code\$" "$runtime/host.log"; step $? "stderr names $code"
done
expect "add lv2:urn:openmixer:test:on-no-path 2" "resp -101"
grep -q "^urn:openmixer:test:on-no-path: no bundle on the LV2 path holds it\$" "$runtime/host.log"; step $? "a URI no bundle holds is an invalid URI, said on stderr"
expect "add lv2:not-a-uri 2" "resp -101"
# found through a prefixed name in its manifest, refused at init: the binary is not there
expect "add lv2:urn:openmixer:test:nobin 2" "resp -102 hosting.no-realisation"
# a CLAP plugin's refusal carries its code the same way
expect "add clap:$fake#org.omx-clap-host.test.wide 2" "resp -102 hosting.topology.wider-than-strip"
# a refusal names no later add
expect "add lv2:$fixture 2" "resp 2"
expect "remove 2" "resp 0"

# ---- a MIDI tool (#183): notes in and out, no audio port; hosted with the strip's pair passing through it ----
expect "add lv2:urn:openmixer:test:midi-echo#tool 2" "resp 2"
[ "$(ports effect_2 i)" = "effect_2:in_1 effect_2:in_2 effect_2:midi_in " ]; step $? "the tool's inputs: $(ports effect_2 i)(the strip's pair and midi_in)"
[ "$(ports effect_2 o)" = "effect_2:midi_out effect_2:out_1 effect_2:out_2 " ]; step $? "and its outputs: $(ports effect_2 o)"
grep -q "midi-echo#tool: passthrough: host" "$runtime/host.log"; step $? "stderr says the audio is the host's pass-through"
expect "remove 2" "resp 0"
# ---- a generator (#184): a stereo output and no input of any kind, admitted as a source and heard; a MIDI source
# (a MIDI output alone) hosted with the strip's pair passing through ----
expect "add lv2:$fixture#gen 2" "resp 2"
[ -z "$(ports effect_2 i)" ]; step $? "the generator's inputs: none at all ($(ports effect_2 i))"
[ "$(ports effect_2 o)" = "effect_2:out_1 effect_2:out_2 " ]; step $? "and its outputs: $(ports effect_2 o)"
line=$(echo listen | timeout 10 "$probe" - effect_2:out_1 effect_2:out_2 2>"$runtime/probe.log")
step $? "the probe listened to the generator: $line"
set -- $line
# 0.5 left and 0.25 right over both ports: sqrt((0.25 + 0.0625) / 2) = 0.3953 RMS, 0.5 peak
near "${1:-0}" 0.3953 0.01; step $? "the generator is heard at 0.3953 RMS over its pair (${1:-})"
near "${2:-0}" 0.5 0.005; step $? "its peak is 0.5, the left channel (${2:-})"
expect "remove 2" "resp 0"
expect "add lv2:urn:openmixer:test:midi-echo#source 2" "resp 2"
[ "$(ports effect_2 i)" = "effect_2:in_1 effect_2:in_2 " ]; step $? "the MIDI source's inputs: $(ports effect_2 i)(the strip's pair, no midi_in)"
[ "$(ports effect_2 o)" = "effect_2:midi_out effect_2:out_1 effect_2:out_2 " ]; step $? "and its outputs: $(ports effect_2 o)"
grep -q "midi-echo#source: passthrough: host" "$runtime/host.log"; step $? "stderr says the audio is the host's pass-through"
expect "remove 2" "resp 0"
# ---- control ports alone and a MIDI sink (#253): hosted with the strip's pair passing through, the control output read
# after the block; a plugin with no port at all is refused (above) ----
expect "add lv2:$fixture#ctl-alone 2" "resp 2"
[ "$(ports effect_2 i)" = "effect_2:in_1 effect_2:in_2 " ]; step $? "the control-only plugin's inputs: $(ports effect_2 i)(the strip's pair)"
[ "$(ports effect_2 o)" = "effect_2:out_1 effect_2:out_2 " ]; step $? "and its outputs: $(ports effect_2 o)"
grep -q "host-fixture#ctl-alone: passthrough: host" "$runtime/host.log"; step $? "stderr says the audio is the host's pass-through"
expect "param_set 2 amount 0.25" "resp 0"
reads 2 twice 0.5 0.00006 "the control-only plugin's output"
expect "remove 2" "resp 0"
expect "add lv2:$fixture#ctl-only 2" "resp 2"
[ "$(ports effect_2 i)" = "effect_2:in_1 effect_2:in_2 effect_2:midi_in " ]; step $? "the MIDI sink's inputs: $(ports effect_2 i)(the strip's pair and midi_in)"
[ "$(ports effect_2 o)" = "effect_2:out_1 effect_2:out_2 " ]; step $? "and its outputs: $(ports effect_2 o)(no midi_out)"
expect "remove 2" "resp 0"
# ---- an LV2 instrument (#132): a MIDI input and a stereo output, no audio input, admitted as a source and played
# through its midi_in ----
expect "add lv2:urn:openmixer:test:midi-echo#synth 2" "resp 2"
[ "$(ports effect_2 i)" = "effect_2:midi_in " ]; step $? "the instrument's inputs: $(ports effect_2 i)(midi_in alone, no audio input)"
[ "$(ports effect_2 o)" = "effect_2:out_1 effect_2:out_2 " ]; step $? "and its outputs: $(ports effect_2 o)"
line=$(timeout 10 "$probe" effect_2:midi_in effect_2:out_1 effect_2:out_2 2>"$runtime/probe.log")
step $? "the probe played the instrument through midi_in: $line"
set -- $line
near "${1:-1}" 0 0.000001; step $? "silence before a note (${1:-})"
# a sine of amplitude v/127 has RMS v/127/sqrt 2: 0.5567 at 100
near "${2:-0}" 0.5567 0.02; step $? "a note on at velocity 100 is 0.5567 RMS (${2:-})"
near "${5:-0}" 0.7874 0.01; step $? "its peak is 100/127 (${5:-})"
near "$(awk -v a="${3:-0}" -v b="${2:-1}" 'BEGIN { printf "%.4f", a / b }')" 0.5 0.02; step $? "velocity 50 is half of velocity 100 (${3:-})"
near "${4:-1}" 0 0.000001; step $? "silence after the note off (${4:-})"
expect "remove 2" "resp 0"
[ -z "$(ports effect_2 i)$(ports effect_2 o)" ]; step $? "remove leaves no effect_2 port"

# ---- the signal: 0.5 into effect_0, effect_0 into the probe effect_1, 0.25 into effect_3 ----
"$source" effect_0:in_1 effect_3:in_1 >"$runtime/source.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q playing "$runtime/source.log" && break; sleep 0.1; done
grep -q playing "$runtime/source.log"; step $? "the source plays 0.5 into effect_0 and 0.25 into effect_3"
pw-link effect_0:out_1 effect_1:in_1; step $? "effect_0:out_1 -> effect_1:in_1"

# the default state: -20 dB from the port's default, -6 dB from state:loadDefaultState
reads 3 meter "$(db 0.25 -26)" 0.00006 "the default state"
reads 0 meter "$(db 0.5 -26)" 0.00006 "the default state"
# ---- the fixed block promise over a change of the JACK buffer size: a shrink, then a growth, re-activate the plugins
# at the new length and the audio keeps flowing (before, only a growth re-activated; a shrink left the plugin refusing
# every cycle) ----
pw-metadata -n settings 0 clock.force-quantum 128 >/dev/null 2>&1; step $? "the quantum forced to 128 (a shrink)"
reads 0 meter "$(db 0.5 -26)" 0.00006 "after the shrink effect_0 still passes audio"
reads 3 meter "$(db 0.25 -26)" 0.00006 "after the shrink effect_3 still passes audio"
pw-metadata -n settings 0 clock.force-quantum 2048 >/dev/null 2>&1; step $? "the quantum forced to 2048 (a growth)"
reads 0 meter "$(db 0.5 -26)" 0.00006 "after the growth effect_0 still passes audio"
pw-metadata -n settings 0 clock.force-quantum 1024 >/dev/null 2>&1; step $? "the quantum forced back to 1024"
reads 0 meter "$(db 0.5 -26)" 0.00006 "and back at 1024, effect_0 still passes audio"

# ---- parameters by symbol, and by id ----
expect "param_get 0 gain_db" "resp 0 -20.0000"
expect "param_set 0 gain_db -14" "resp 0"
expect "param_get 0 gain_db" "resp 0 -14.0000"
expect "param_get 0 3" "resp 0 -14.0000"
expect "param_set 0 3 -12" "resp 0"
expect "param_get 0 gain_db" "resp 0 -12.0000"
expect "param_set 0 gain_db -14" "resp 0"
reads 0 meter "$(db 0.5 -20)" 0.00006 "gain_db -14 and the -6 dB state"
expect "param_set 1 gain_db 0" "resp 0"
reads 1 meter "$(db 0.5 -26)" 0.00006 "the probe after effect_0, -20 dB then -6 dB"
expect "param_set 0 nosuch 1" "resp -103"
expect "param_get 0 in" "resp -103"
expect "param_set 0 meter 1" "resp -103"
expect "param_info 0 gain_db" "resp 0 none linear -60 0 -20 0 gain_db"
expect "param_info 0 nosuch" "resp -103"

# ---- monitoring an output port by its symbol ----
expect "monitor_output 0 meter" "resp 1"
expect "monitor_output 0 gain_db" "resp 0"
for _ in $(seq 30); do near "$(last 0 meter)" "$(db 0.5 -20)" 0.00001 && break; sleep 0.1; done
near "$(last 0 meter)" "$(db 0.5 -20)" 0.00001; step $? "output_set 0 meter carries $(db 0.5 -20) ($(last 0 meter))"

# ---- lv2:enabled is the host's bypass: 0 on, 1 (its processing value) off ----
expect "param_set 0 enabled 0" "resp 0"
expect "param_get 0 :bypass" "resp 0 1.0000"
expect "param_get 0 enabled" "resp 0 0.0000"
reads 1 meter "$(db 0.5 -6)" 0.00006 "bypassed by enabled 0, effect_0 passes its input"
expect "param_set 0 enabled 1" "resp 0"
expect "param_get 0 :bypass" "resp 0 0.0000"
expect "param_get 0 enabled" "resp 0 1.0000"
reads 1 meter "$(db 0.5 -26)" 0.00006 "enabled 1 again"
expect "bypass 0 1" "resp 0"
expect "param_get 0 enabled" "resp 0 0.0000"
expect "param_get 0 2" "resp 0 0.0000"
expect "bypass 0 0" "resp 0"
expect "param_set 0 2 0" "resp 0"
expect "param_get 0 :bypass" "resp 0 1.0000"
expect "param_set 0 :bypass 0" "resp 0"
expect "param_get 0 enabled" "resp 0 1.0000"
reads 1 meter "$(db 0.5 -26)" 0.00006 "the bypass off"

# ---- state and presets ----
expect "state_save $runtime/state" "resp 0"
grep -q 'offset_db' "$runtime/state/effect_0.clapstate" && grep -q -- '-6' "$runtime/state/effect_0.clapstate"
step $? "effect_0.clapstate holds the -6 dB offset as LV2 Turtle"
expect "preset_load 0 $fixture-preset:quiet" "resp 0"
reads 0 meter "$(db 0.5 -17)" 0.00006 "the preset quiet: -14 dB and -3 dB"
expect "preset_load 0 $fixture-preset:quiet-wrong" "resp -104"
expect "preset_load 0 urn:openmixer:test:no-preset" "resp -104"
reads 0 meter "$(db 0.5 -17)" 0.00006 "a refused preset changes nothing"
expect "state_load $runtime/state" "resp 0"
reads 0 meter "$(db 0.5 -20)" 0.00006 "the state loaded: -6 dB again"
reads 3 meter "$(db 0.25 -26)" 0.00006 "the other fixture untouched"

# ---- state:makePath in the state root: the path fixture saves its level to a file it makes, a load reads it back ----
expect "add lv2:$pathfix 4" "resp 4"
[ -d "$sroot" ]; step $? "the state root $sroot is made at start"
expect "add lv2:$fixture 5" "resp 5"
expect "param_set 5 gain_db 0" "resp 0"
pw-link effect_3:out_1 effect_4:in_1; step $? "effect_3:out_1 -> effect_4:in_1"
pw-link effect_4:out_1 effect_5:in_1; step $? "effect_4:out_1 -> effect_5:in_1"
reads 5 meter "$(db 0.25 -32)" 0.00006 "the path fixture passes at gain 1, the probe after it at -6 dB"
expect "param_set 4 level 0.5" "resp 0"
expect "state_save $runtime/state2" "resp 0"
made=$(find "$sroot" -path "$sroot/lv2-*" -name level.txt ! -path '*.saved/*' | head -1)
[ -n "$made" ] && [ "$(cat "$made")" = 0.5 ]; step $? "the save made takes/level.txt in the instance's directory of the state root: ${made:-none}"
copy=$(find "$sroot" -path '*.saved/*' -name level.txt | head -1)
[ -n "$copy" ] && [ "$(cat "$copy")" = 0.5 ]; step $? "and the save's copy of it beside: ${copy:-none}"
[ -z "$(find "$runtime" -name level.txt ! -path "$sroot/*")" ]; step $? "no file of it outside the state root"
expect "state_load $runtime/state2" "resp 0"
reads 5 meter "$(db 0.125 -32)" 0.00006 "the load read the level back from the file: the fixture's gain is 0.5"
expect "remove 4" "resp 0"
[ ! -e "$(dirname "$(dirname "$made")")" ]; step $? "remove takes the instance's directory away"
[ -e "$copy" ]; step $? "the save's copy stays: the saved state names it"
expect "remove 5" "resp 0"

# ---- remove ----
expect "remove 0" "resp 0"
[ -z "$(ports effect_0 i)$(ports effect_0 o)" ]; step $? "remove leaves no effect_0 port"
expect "param_get 0 gain_db" "resp -3"
expect "remove 1" "resp 0"
expect "remove 3" "resp 0"
[ -z "$(pw-link -i 2>/dev/null | grep '^effect_')$(pw-link -o 2>/dev/null | grep '^effect_')" ]; step $? "no effect_ port is left in the graph"
expect "quit" "resp 0"
sleep 0.3
! kill -0 "$host_pid" 2>/dev/null; step $? "host exited on quit"

# ---- a state root that cannot be made (a file where a directory should be): makePath is not offered, the rest of LV2
# is, and a plugin that requires it is refused with the code it had before there was a root ----
: >"$runtime/afile"
port=$((port + 2))
LV2_PATH=$bundles "$host" -n -p "$port" -f "$((port + 1))" -s "$runtime/afile/state" >"$runtime/host-noroot.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q "ready!" "$runtime/host-noroot.log" && break; sleep 0.1; done
grep -q "^the state root $runtime/afile/state is not a directory this host can write: state:makePath is not offered\$" "$runtime/host-noroot.log"
step $? "-s under a file says so: $(grep 'state root' "$runtime/host-noroot.log")"
exec 3>&- 4>&-
exec 3<>"/dev/tcp/127.0.0.1/$port"
exec 4<>"/dev/tcp/127.0.0.1/$((port + 1))"
expect "add lv2:$pathfix 0" "resp -102 hosting.features.missing"
expect "add lv2:$fixture 1" "resp 1"
expect "quit" "resp 0"

if [ "$failures" -eq 0 ]; then
    echo "jack lv2 e2e ok"
    exit 0
fi
echo "jack lv2 e2e FAILED ($failures)"
exit 1
