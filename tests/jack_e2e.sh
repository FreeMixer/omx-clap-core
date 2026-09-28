#!/usr/bin/env bash
#
# omx-clap-host over jack, end to end, in a PipeWire of its own: the script
# re-runs itself in a private user, net and pid namespace, starts pipewire and
# wireplumber on a private runtime dir, then drives the host over its socket
# and reads the graph back after every command. Exit 0 only when every step
# held; each step is printed. Nothing here touches the PipeWire of the session
# that runs it.
#
# CLAP_TEST_PLUGIN: the omx-delay .clap (default ../openmixer/packages/omx-plugins/bin/omx-delay.clap)
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5560)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
plugin=${CLAP_TEST_PLUGIN:-$root/../openmixer/packages/omx-plugins/bin/omx-delay.clap}
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5560}
fake=$here/fake.clap
probe=$here/jack_latency_probe

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$plugin" "$host" "$fake" "$probe"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    plugin=$(readlink -f "$plugin")
    JACK_E2E_INSIDE=1 CLAP_TEST_PLUGIN=$plugin OMX_CLAP_HOST=$host JACK_E2E_PORT=$port \
        exec unshare --user --map-root-user --net --pid --fork "$0" "$@"
fi

failures=0
step() {
    if [ "$1" -eq 0 ]; then
        echo "ok   $2"
    else
        echo "FAIL $2"
        failures=$((failures + 1))
    fi
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
    rm -rf "$runtime"
}
trap cleanup EXIT
[ "$$" = 1 ]; step $? "pid 1 of a private pid namespace"

ip link set lo up

pipewire >"$runtime/pw.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do [ -S "$runtime/pipewire-0" ] && break; sleep 0.1; done
[ -S "$runtime/pipewire-0" ] || { echo "pipewire did not come up" >&2; cat "$runtime/pw.log" >&2; exit 2; }
wireplumber >"$runtime/wp.log" 2>&1 &
pids+=($!)
sleep 1
step $? "private pipewire on $runtime"

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
    pw-link -i 2>/dev/null; pw-link -o 2>/dev/null
}
graph_has() {
    ports | grep -q -x -F "$1"
}
nodes() {
    pw-dump 2>/dev/null | grep -c "\"node.name\": \"$1\""
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

expect "add clap:$fake#org.omx-clap-host.test.wide 1" "resp -102"
grep -q "main port has 4 channels" "$runtime/host.log"; step $? "the wide plugin's refusal names its reason"
! graph_has effect_1:in_1; step $? "no effect_1 port after the refusal"

expect "add clap:$fake#org.omx-clap-host.test.passthrough 1" "resp 1"
sleep 0.5
latency=$("$probe" effect_1:out_1 capture)
[ "$latency" = "64 64" ]; step $? "effect_1:out_1 capture latency $latency (plugin reports 64)"
latency=$("$probe" effect_1:in_1 playback)
[ "$latency" = "64 64" ]; step $? "effect_1:in_1 playback latency $latency"
expect "param_set 1 0 200" "resp 0"
sleep 0.5
latency=$("$probe" effect_1:out_1 capture)
[ "$latency" = "200 200" ]; step $? "effect_1:out_1 capture latency $latency after the plugin changed it"

expect "remove 0" "resp 0"
! graph_has effect_0:in_1 && ! graph_has effect_0:out_1; step $? "no effect_0 port after remove"
graph_has effect_1:in_1; step $? "effect_1 still there"
expect "remove 1" "resp 0"
! graph_has effect_1:out_1; step $? "no effect_1 port after remove"
expect "remove 0" "resp -3"
expect "quit" "resp 0"
sleep 0.3
! kill -0 "${pids[2]}" 2>/dev/null; step $? "host exited on quit"
[ "$(nodes effect_1)" -eq 0 ] && [ "$(nodes omx-clap-host)" -eq 0 ]; step $? "no host node left in the private graph"

if [ "$failures" -eq 0 ]; then
    echo "jack e2e ok"
    exit 0
fi
echo "jack e2e FAILED ($failures)"
exit 1
