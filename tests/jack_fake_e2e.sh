#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The auxiliary ports of omx-clap-host over jack (#92), against the fake plugins alone, in a PipeWire of its own: the
# script re-runs itself in a private user, net, pid and mount namespace with its own /proc, starts pipewire on a
# private runtime dir and drives the host over its socket. A JACK port per auxiliary channel, aux_in_<p>_<k> and
# aux_out_<p>_<k>, bound to the plugin: a side chain heard, an auxiliary output read, both again after the buffer
# size grows past the size the blocks were made for and shrinks, and a bypassed plugin's auxiliary output silent.
# Exit 0 only when every step held; each step is printed. Nothing here touches the PipeWire of the session that runs it.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5572)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5572}
fake=$here/fake.clap
source=$here/jack_meter_source
listen=$here/jack_synth_probe

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$fake" "$source" "$listen"; do
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

runtime=$(mktemp -d /tmp/omx-clap-host-fake.XXXXXX)
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
host_pid=$!
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
graph_has() {
    { run pw-link -i 2>/dev/null; run pw-link -o 2>/dev/null; } | grep -q -x -F "$1"
}
# the window the probe reads on the ports named: "<rms> <peak>"
level() { echo | run "$listen" - "$@" 2>>"$runtime/listen.log"; }
near() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t) }'; }

# the source plays 0.5 on out_l and 0.25 on out_r, each into the port named
expect "add clap:$fake#org.omx-clap-host.test.sidechain 0" "resp 0"
graph_has effect_0:aux_in_1_1 && ! graph_has effect_0:aux_in_1_2 && ! graph_has effect_0:aux_in_2_1 && ! graph_has effect_0:aux_out_1_1
step $? "the sidechain plugin's mono side chain is effect_0:aux_in_1_1, its one auxiliary port"
w=$(level effect_0:out_1); near "${w%% *}" 0 0.000001; step $? "nothing into it: effect_0:out_1 is silent ($w)"
expect "add clap:$fake#org.omx-clap-host.test.aux-wide 2" "resp 2"
graph_has effect_2:aux_in_1_1 && graph_has effect_2:aux_in_1_4 && ! graph_has effect_2:aux_in_1_5
step $? "the aux-wide plugin's four-channel side chain is effect_2:aux_in_1_1 to aux_in_1_4"
"$source" effect_0:aux_in_1_1 effect_2:aux_in_1_4 >"$runtime/source.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q playing "$runtime/source.log" && break; sleep 0.1; done
grep -q playing "$runtime/source.log"; step $? "the source's 0.5 into effect_0:aux_in_1_1 and 0.25 into effect_2:aux_in_1_4, nothing on the mains"
sleep 0.3
# the fake adds its side chain onto out_1: a dry pass or an unbound side chain reads silence
w=$(level effect_0:out_1); near "${w%% *}" 0.5 0.0001 && near "${w##* }" 0.5 0.0001; step $? "effect_0:out_1 is the side chain the plugin added, 0.5 ($w)"
w=$(level effect_0:out_2); near "${w%% *}" 0 0.000001; step $? "effect_0:out_2 stays silent ($w)"

expect "add clap:$fake#org.omx-clap-host.test.auxout 1" "resp 1"
graph_has effect_1:aux_out_1_1 && graph_has effect_1:aux_out_1_2 && ! graph_has effect_1:aux_out_1_3 && ! graph_has effect_1:aux_in_1_1
step $? "the auxout plugin's stereo auxiliary output is effect_1:aux_out_1_1 and aux_out_1_2"
# the fake writes 0.125 / k on channel k of its auxiliary output: an unbound one writes its scratch, JACK reads nothing
w=$(level effect_1:aux_out_1_1); near "${w%% *}" 0.125 0.0001 && near "${w##* }" 0.125 0.0001; step $? "effect_1:aux_out_1_1 carries the plugin's 0.125 ($w)"
w=$(level effect_1:aux_out_1_2); near "${w%% *}" 0.0625 0.0001; step $? "effect_1:aux_out_1_2 carries its 0.0625 ($w)"

run pw-link jack_meter_source:out_l effect_2:aux_in_1_1; step $? "and its 0.5 into effect_2:aux_in_1_1"
sleep 0.3
# the fake copies its side chain's fourth channel onto out_1: the first channel's 0.5 must not show
w=$(level effect_2:out_1); near "${w%% *}" 0.25 0.0001 && near "${w##* }" 0.25 0.0001; step $? "effect_2:out_1 is the fourth side-chain channel, 0.25 ($w)"

# a growth past the size the blocks were made for, then a shrink: the blocks follow and stay bound
for q in 2048 128; do
    pw-metadata -n settings 0 clock.force-quantum "$q" >/dev/null 2>&1; step $? "the quantum forced to $q"
    sleep 0.5
    w=$(level effect_0:out_1); near "${w%% *}" 0.5 0.0001; step $? "at $q, effect_0:out_1 still carries the side chain ($w)"
    w=$(level effect_1:aux_out_1_1); near "${w%% *}" 0.125 0.0001; step $? "at $q, effect_1:aux_out_1_1 still carries 0.125 ($w)"
    w=$(level effect_2:out_1); near "${w%% *}" 0.25 0.0001; step $? "at $q, effect_2:out_1 still carries the fourth channel ($w)"
done
pw-metadata -n settings 0 clock.force-quantum 0 >/dev/null 2>&1; step $? "and unforced"
sleep 0.5

# bypassed, the plugin is not run: its auxiliary output is silent, never its last block again
expect "bypass 1 1" "resp 0"
sleep 0.3
w=$(level effect_1:aux_out_1_1); near "${w%% *}" 0 0.000001 && near "${w##* }" 0 0.000001; step $? "bypassed, effect_1:aux_out_1_1 is silent ($w)"
w=$(level effect_1:aux_out_1_2); near "${w%% *}" 0 0.000001; step $? "and aux_out_1_2 ($w)"
expect "bypass 1 0" "resp 0"
sleep 0.3
w=$(level effect_1:aux_out_1_1); near "${w%% *}" 0.125 0.0001; step $? "the bypass off, effect_1:aux_out_1_1 carries 0.125 again ($w)"

for i in 2 1 0; do expect "remove $i" "resp 0"; done
! graph_has effect_0:aux_in_1_1 && ! graph_has effect_1:aux_out_1_1 && ! graph_has effect_2:aux_in_1_4; step $? "no auxiliary port after remove"
expect "quit" "resp 0"
sleep 0.3
! kill -0 "$host_pid" 2>/dev/null; step $? "host exited on quit"

if [ "$failures" -eq 0 ]; then
    echo "jack fake e2e ok"
    exit 0
fi
echo "jack fake e2e FAILED ($failures)"
exit 1
