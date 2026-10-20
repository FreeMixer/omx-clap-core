#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# omx-clap-host over jack, end to end, in a PipeWire of its own: the script
# re-runs itself in a private user, net, pid and mount namespace with its own
# /proc, starts pipewire on a private runtime dir (no session manager: nodes,
# ports, links and the dummy driver are the daemon's own), then drives the
# host over its socket
# and reads the graph back after every command. Exit 0 only when every step
# held; each step is printed. Nothing here touches the PipeWire of the session
# that runs it.
#
# CLAP_TEST_PLUGIN: the omx-delay .clap (default /usr/lib/clap/omx-delay.clap)
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5560)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
plugin=${CLAP_TEST_PLUGIN:-/usr/lib/clap/omx-delay.clap}
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5560}
fake=$here/fake.clap
probe=$here/jack_latency_probe
source=$here/jack_meter_source
listen=$here/jack_synth_probe

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$plugin" "$host" "$fake" "$probe" "$source" "$listen"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    plugin=$(readlink -f "$plugin")
    JACK_E2E_INSIDE=1 CLAP_TEST_PLUGIN=$plugin OMX_CLAP_HOST=$host JACK_E2E_PORT=$port \
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

FAKE_LOG=$runtime/fake.log "$host" -n -p "$port" >"$runtime/host.log" 2>&1 &
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

expect "add clap:$plugin#org.freemixer.openmixer.delay 0" "resp 0"
for p in effect_0:in_1 effect_0:in_2 effect_0:out_1 effect_0:out_2; do
    graph_has "$p"; step $? "port $p in the graph"
done
[ "$(nodes effect_0)" -ge 1 ]; step $? "pw-dump sees node effect_0 ($(nodes effect_0))"

expect "param_get 0 2" "resp 0 0.3000"
expect "param_set 0 2 1.0" "resp 0"
expect "param_get 0 2" "resp 0 1.0000"
expect "param_set 0 0 5" "resp 0"
expect "param_get 0 0" "resp 0 5.0000"
expect "param_set 0 99 1" "resp -103"
expect "param_get 0 :bypass" "resp 0 0.0000"
expect "bypass 0 1" "resp 0"
expect "param_get 0 :bypass" "resp 0 1.0000"
expect "bypass 0 0" "resp 0"
expect "param_get 0 :bypass" "resp 0 0.0000"

expect "state_save $state_dir" "resp 0"
[ -s "$state_dir/effect_0.clapstate" ]; step $? "state file effect_0.clapstate ($(stat -c %s "$state_dir/effect_0.clapstate" 2>/dev/null || echo 0) bytes)"
expect "param_set 0 2 0.5" "resp 0"
expect "param_get 0 2" "resp 0 0.5000"
expect "state_load $state_dir" "resp 0"
expect "param_get 0 2" "resp 0 1.0000"

expect "add clap:$fake#org.omx-clap-host.test.passthrough 1" "resp 1"
sleep 0.5
latency=$(run "$probe" effect_1:out_1 capture)
[ "$latency" = "64 64" ]; step $? "effect_1:out_1 capture latency $latency (plugin reports 64)"
latency=$(run "$probe" effect_1:in_1 playback)
[ "$latency" = "64 64" ]; step $? "effect_1:in_1 playback latency $latency"
expect "param_set 1 0 200" "resp 0"
sleep 0.5
latency=$(run "$probe" effect_1:out_1 capture)
[ "$latency" = "200 200" ]; step $? "effect_1:out_1 capture latency $latency after the plugin changed it"

# ---- a main of any width (#92): a JACK port per channel, each carried ----
# the window the probe reads on the ports named: "<rms> <peak>"
level() { echo | run "$listen" - "$@" 2>>"$runtime/listen.log"; }
near() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t) }'; }
expect "add clap:$fake#org.omx-clap-host.test.wide 2" "resp 2"
all=0
for k in 1 2 3 4 5 6; do graph_has "effect_2:in_$k" && graph_has "effect_2:out_$k" || all=1; done
[ "$all" -eq 0 ]; step $? "the wide plugin has in_1 to in_6 and out_1 to out_6"
! graph_has effect_2:in_7 && ! graph_has effect_2:out_7; step $? "and no port past its six"
"$source" effect_2:in_1 effect_2:in_6 >"$runtime/source.log" 2>&1 &
source_pid=$!
pids+=("$source_pid")
for _ in $(seq 50); do grep -q playing "$runtime/source.log" && break; sleep 0.1; done
grep -q playing "$runtime/source.log"; step $? "the source plays 0.5 into effect_2:in_1 and 0.25 into effect_2:in_6"
# the fake reverses its channels: a dry pass would read 0.5 on out_1 and 0.25 on out_6
w=$(level effect_2:out_1); near "${w%% *}" 0.25 0.0001 && near "${w##* }" 0.25 0.0001; step $? "effect_2:out_1 carries in_6, the plugin ran ($w)"
w=$(level effect_2:out_6); near "${w%% *}" 0.5 0.0001 && near "${w##* }" 0.5 0.0001; step $? "effect_2:out_6 carries in_1 ($w)"
w=$(level effect_2:out_3); near "${w%% *}" 0 0.000001; step $? "effect_2:out_3 carries the silent in_4 ($w)"
expect "add clap:$fake#org.omx-clap-host.test.mono-stereo 3" "resp 3"
graph_has effect_3:in_1 && ! graph_has effect_3:in_2 && graph_has effect_3:out_1 && graph_has effect_3:out_2 && ! graph_has effect_3:out_3
step $? "the mono-in, stereo-out plugin has in_1, out_1 and out_2"
run pw-link jack_meter_source:out_l effect_3:in_1; step $? "the source's 0.5 into effect_3:in_1"
sleep 0.3
w=$(level effect_3:out_1); near "${w%% *}" 0.5 0.0001 && near "${w##* }" 0.5 0.0001; step $? "effect_3:out_1 is the input, 0.5 ($w)"
w=$(level effect_3:out_2); near "${w%% *}" 0.5 0.0001; step $? "effect_3:out_2 is as loud ($w)"
coproc both { timeout 10 "$listen" - effect_3:out_1 2>>"$runtime/listen.log"; }
sleep 0.2
run pw-link effect_3:out_2 jack_synth_probe:in_1; step $? "effect_3:out_2 summed with out_1 into one probe input"
sleep 0.3
echo >&"${both[1]}"; IFS= read -r -t 5 w <&"${both[0]}"
near "${w%% *}" 0 0.0001; step $? "the sum is silent: out_2 is -0.5, the plugin's second output, not the dry input ($w)"
exec {both[1]}>&-
wait "$both_PID" 2>/dev/null
expect "add clap:$fake#org.omx-clap-host.test.stereo-mono 4" "resp 4"
graph_has effect_4:in_1 && graph_has effect_4:in_2 && graph_has effect_4:out_1 && ! graph_has effect_4:out_2
step $? "the stereo-in, mono-out plugin has in_1, in_2 and out_1"
run pw-link jack_meter_source:out_l effect_4:in_1 && run pw-link jack_meter_source:out_r effect_4:in_2
step $? "the source's 0.5 into effect_4:in_1 and 0.25 into effect_4:in_2"
sleep 0.3
w=$(level effect_4:out_1); near "${w%% *}" 0.75 0.0001 && near "${w##* }" 0.75 0.0001; step $? "effect_4:out_1 is the sum of both inputs, 0.75 ($w)"

# ---- the lane's width asked of a plugin that offers layouts (#92): -c 2, the default ----
selected() { grep -c -x -F "org.omx-clap-host.test.configs select 2" "$runtime/fake.log" 2>/dev/null; }
expect "add clap:$fake#org.omx-clap-host.test.configs 5" "resp 5"
graph_has effect_5:in_1 && graph_has effect_5:in_2 && graph_has effect_5:out_1 && graph_has effect_5:out_2 && ! graph_has effect_5:in_3 && ! graph_has effect_5:out_3
step $? "the configs plugin, declared mono in and stereo out, asked for the stereo lane: in_1, in_2, out_1 and out_2 (config 2)"
[ "$(selected)" = 1 ]; step $? "config 2 selected once, at add ($(selected))"
expect "param_get 5 1" "resp 0 1.0000"
# a write makes the plugin say its ports changed (audio-ports rescan) while it runs: the lane's width is asked again
expect "param_set 5 0 100" "resp 0"
for _ in $(seq 30); do [ "$(selected)" = 2 ] && break; sleep 0.1; done
[ "$(selected)" = 2 ]; step $? "the reported ports rescan asks for the lane's width again: config 2 selected a second time ($(selected))"
expect "param_get 5 1" "resp 0 2.0000"
graph_has effect_5:in_2 && graph_has effect_5:out_2 && ! graph_has effect_5:in_3; step $? "the description is the same: the ports stand, the plugin activated again"
calls=$(send "param_get 5 2"); sleep 0.3
[ "$(send "param_get 5 2")" != "$calls" ]; step $? "and it runs again: its process calls move past $calls"
sleep 0.3
[ "$(selected)" = 2 ]; step $? "and no relayout loop follows ($(selected))"
run pw-link jack_meter_source:out_l effect_5:in_1 && run pw-link jack_meter_source:out_r effect_5:in_2
step $? "the source's 0.5 into effect_5:in_1 and 0.25 into effect_5:in_2"
sleep 0.3
# the configs plugin writes each output half its input: a dry pass would read 0.5 and 0.25
w=$(level effect_5:out_1); near "${w%% *}" 0.25 0.0001 && near "${w##* }" 0.25 0.0001; step $? "effect_5:out_1 is half in_1, the stereo config ran ($w)"
w=$(level effect_5:out_2); near "${w%% *}" 0.125 0.0001; step $? "effect_5:out_2 is half in_2 ($w)"
off_role() { grep -c -x -F "org.omx-clap-host.test.configs process off the audio role" "$runtime/fake.log"; }
[ "$(off_role)" = 0 ]; step $? "after the relayout process() runs on the audio role (thread-check)"
pw-metadata -n settings 0 clock.force-quantum 128 >/dev/null 2>&1; step $? "the quantum forced to 128"
sleep 0.5
pw-metadata -n settings 0 clock.force-quantum 0 >/dev/null 2>&1; step $? "and unforced"
sleep 0.5
w=$(level effect_5:out_1); near "${w%% *}" 0.25 0.0001; step $? "after the buffer size changed, effect_5:out_1 is still half in_1 ($w)"
[ "$(off_role)" = 0 ]; step $? "and process() still runs on the audio role"
# a rescan the plugin makes in the activate of a relayout is reported, and answered by one more relayout
expect "param_set 5 0 103" "resp 0"
for _ in $(seq 30); do [ "$(selected)" = 4 ] && break; sleep 0.1; done
sleep 0.3
[ "$(selected)" = 4 ]; step $? "a rescan made in the relayout's activate is answered too: two relayouts ($(selected))"
# a plugin that rescans in every activate: the relayouts in a row that find its layout unchanged stop, said once
expect "param_set 5 0 104" "resp 0"
for _ in $(seq 50); do grep -q "relayouts in a row" "$runtime/host.log" && break; sleep 0.1; done
grep -q "relayouts in a row" "$runtime/host.log"; step $? "relayouts in a row with the layout unchanged are capped, said on stderr"
n=$(selected); sleep 0.5
[ "$(selected)" = "$n" ] && [ "$(grep -c "relayouts in a row" "$runtime/host.log")" = 1 ]; step $? "and no more follow ($n)"
w=$(level effect_5:out_1); near "${w%% *}" 0.25 0.0001; step $? "the plugin still runs: effect_5:out_1 half in_1 ($w)"
# the plugin loses its main input in a restart's deactivate: the description read again is refused, the lane passes dry
expect "param_set 5 0 102" "resp 0"
for _ in $(seq 30); do grep -q "is refused: hosting.topology.port-without-channels" "$runtime/host.log" && break; sleep 0.1; done
grep -q "test.configs: its layout after the ports rescan is refused: hosting.topology.port-without-channels" "$runtime/host.log"
step $? "a refused description after the restart is said on stderr"
sleep 0.3
w=$(level effect_5:out_1); near "${w%% *}" 0.5 0.0001; step $? "and effect_5 passes dry: out_1 is in_1 ($w)"
w=$(level effect_5:out_2); near "${w%% *}" 0.25 0.0001; step $? "out_2 is in_2 ($w)"
r=$(send "param_get 5 1"); [ "${r%% *}" = resp ] && [ "$(echo "$r" | cut -d' ' -f2)" = 0 ]; step $? "the host still answers for effect_5 ($r)"
expect "remove 5" "resp 0"

# ---- auxiliary ports (#92): a JACK port per auxiliary channel, aux_in_<p>_<k> and aux_out_<p>_<k>, bound to the plugin ----
expect "add clap:$fake#org.omx-clap-host.test.sidechain 6" "resp 6"
graph_has effect_6:aux_in_1_1 && ! graph_has effect_6:aux_in_1_2 && ! graph_has effect_6:aux_in_2_1 && ! graph_has effect_6:aux_out_1_1
step $? "the sidechain plugin's mono side chain is effect_6:aux_in_1_1, its one auxiliary port"
w=$(level effect_6:out_1); near "${w%% *}" 0 0.000001; step $? "nothing into it: effect_6:out_1 is silent ($w)"
run pw-link jack_meter_source:out_l effect_6:aux_in_1_1; step $? "the source's 0.5 into effect_6:aux_in_1_1, nothing on the mains"
sleep 0.3
# the fake adds its side chain onto out_1: a dry pass or an unbound side chain reads silence
w=$(level effect_6:out_1); near "${w%% *}" 0.5 0.0001 && near "${w##* }" 0.5 0.0001; step $? "effect_6:out_1 is the side chain the plugin added, 0.5 ($w)"
w=$(level effect_6:out_2); near "${w%% *}" 0 0.000001; step $? "effect_6:out_2 stays silent ($w)"
expect "add clap:$fake#org.omx-clap-host.test.auxout 7" "resp 7"
graph_has effect_7:aux_out_1_1 && graph_has effect_7:aux_out_1_2 && ! graph_has effect_7:aux_out_1_3 && ! graph_has effect_7:aux_in_1_1
step $? "the auxout plugin's stereo auxiliary output is effect_7:aux_out_1_1 and aux_out_1_2"
# the fake writes 0.125 / k on channel k of its auxiliary output: an unbound one writes its scratch, JACK reads nothing
w=$(level effect_7:aux_out_1_1); near "${w%% *}" 0.125 0.0001 && near "${w##* }" 0.125 0.0001; step $? "effect_7:aux_out_1_1 carries the plugin's 0.125 ($w)"
w=$(level effect_7:aux_out_1_2); near "${w%% *}" 0.0625 0.0001; step $? "effect_7:aux_out_1_2 carries its 0.0625 ($w)"
# a growth past the size the blocks were made for, then a shrink: the blocks follow and stay bound
for q in 2048 128; do
    pw-metadata -n settings 0 clock.force-quantum "$q" >/dev/null 2>&1; step $? "the quantum forced to $q"
    sleep 0.5
    w=$(level effect_6:out_1); near "${w%% *}" 0.5 0.0001; step $? "at $q, effect_6:out_1 still carries the side chain ($w)"
    w=$(level effect_7:aux_out_1_1); near "${w%% *}" 0.125 0.0001; step $? "at $q, effect_7:aux_out_1_1 still carries 0.125 ($w)"
done
pw-metadata -n settings 0 clock.force-quantum 0 >/dev/null 2>&1; step $? "and unforced"
sleep 0.5
expect "remove 7" "resp 0"
expect "remove 6" "resp 0"
! graph_has effect_6:aux_in_1_1 && ! graph_has effect_7:aux_out_1_1; step $? "no auxiliary port after remove"
kill "$source_pid" 2>/dev/null
wait "$source_pid" 2>/dev/null
expect "remove 4" "resp 0"
expect "remove 3" "resp 0"
expect "remove 2" "resp 0"
! graph_has effect_2:in_1 && ! graph_has effect_3:out_1; step $? "no effect_2 or effect_3 port after remove"

expect "remove 0" "resp 0"
! graph_has effect_0:in_1 && ! graph_has effect_0:out_1; step $? "no effect_0 port after remove"
graph_has effect_1:in_1; step $? "effect_1 still there"
expect "remove 1" "resp 0"
! graph_has effect_1:out_1; step $? "no effect_1 port after remove"
expect "remove 0" "resp -3"
expect "quit" "resp 0"
sleep 0.3
! kill -0 "${pids[1]}" 2>/dev/null; step $? "host exited on quit"
[ "$(nodes effect_1)" -eq 0 ] && [ "$(nodes omx-clap-host)" -eq 0 ]; step $? "no host node left in the private graph"

# ---- -c 1: a mono lane asked; the configs plugin offers no mono pair, its declared layout stands ----
run "$host" -n -c 3 -p "$((port + 1))" >"$runtime/host3.log" 2>&1
[ $? -ne 0 ] && grep -q -- "-c" "$runtime/host3.log"; step $? "-c 3 is refused: the lane is one or two channels ($(head -1 "$runtime/host3.log"))"
"$host" -n -c 1 -p "$((port + 1))" >"$runtime/host1.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q "ready!" "$runtime/host1.log" && break; sleep 0.1; done
grep -q "ready!" "$runtime/host1.log"; step $? "omx-clap-host -c 1 ready on port $((port + 1))"
exec 3>&-
exec 3<>"/dev/tcp/127.0.0.1/$((port + 1))"
expect "add clap:$fake#org.omx-clap-host.test.configs 0" "resp 0"
graph_has effect_0:in_1 && ! graph_has effect_0:in_2 && graph_has effect_0:out_1 && graph_has effect_0:out_2
step $? "under -c 1 the mono pair is asked, no config offers it: the declared in_1, out_1 and out_2 stand"
expect "add clap:$fake#org.omx-clap-host.test.configurable 1" "resp 1"
graph_has effect_1:in_1 && ! graph_has effect_1:in_2 && graph_has effect_1:out_1 && ! graph_has effect_1:out_2
step $? "the configurable plugin, declared stereo, applies the mono pair asked: in_1 and out_1"
"$source" effect_0:in_1 effect_1:in_1 >"$runtime/source1.log" 2>&1 &
source_pid=$!
pids+=("$source_pid")
for _ in $(seq 50); do grep -q playing "$runtime/source1.log" && break; sleep 0.1; done
sleep 0.3
w=$(level effect_0:out_1); near "${w%% *}" 0.25 0.0001; step $? "effect_0:out_1 is half its mono input, config 1 ran ($w)"
w=$(level effect_0:out_2); near "${w%% *}" 0.25 0.0001; step $? "effect_0:out_2 too ($w)"
# the plugin switches itself to the stereo config in a restart's deactivate: two inputs, one JACK port for them
expect "param_set 0 0 101" "resp 0"
for _ in $(seq 30); do grep -q "main ports are now" "$runtime/host1.log" && break; sleep 0.1; done
grep -q "test.configs: its main ports are now 2 in and 2 out, not the 1 and 2 it was added with" "$runtime/host1.log"
step $? "a width the registered ports do not carry is said on stderr"
sleep 0.3
w=$(level effect_0:out_1); near "${w%% *}" 0.5 0.0001; step $? "and effect_0 passes dry: out_1 is in_1 ($w)"
w=$(level effect_0:out_2); near "${w%% *}" 0.5 0.0001; step $? "out_2 is in_1 too ($w)"
run pw-link jack_meter_source:out_r effect_0:in_1; step $? "one more link into effect_0 (its latency read again)"
expect "param_get 0 1" "resp 0 1.0000"
kill "$source_pid" 2>/dev/null
wait "$source_pid" 2>/dev/null
expect "quit" "resp 0"
sleep 0.3
[ "$(nodes effect_0)" -eq 0 ]; step $? "the -c 1 host left no node"

if [ "$failures" -eq 0 ]; then
    echo "jack e2e ok"
    exit 0
fi
echo "jack e2e FAILED ($failures)"
exit 1
