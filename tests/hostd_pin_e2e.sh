#!/usr/bin/env bash
#
# layout pinning behind plugin-hostd, end to end, in a PipeWire of its own:
# the script re-runs itself in a private user, net, pid and mount namespace
# with its own /proc and starts pipewire on a private runtime dir, then runs
# the daemon with require_pins 1 and omx-clap-host as its CLAP worker. The
# daemon hashes the .clap pin_set pinned and sends pin_expect with its layout
# pin before each add; this host checks the layout. Exit 0 only when every
# step held; each step is printed. Nothing here touches the PipeWire of the
# session that runs it.
#
# PLUGIN_HOSTD:     the daemon (required)
# OMX_CLAP_HOST:    the worker (default ./omx-clap-host)
# JACK_E2E_PORT:    the daemon's socket port (default 5566); the feedback port is the next one
# PHD_*:            what plugin-hostd's headers declare, read from them by make test-hostd-pin

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
daemon=${PLUGIN_HOSTD:-}
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5566}
fbport=$((port + 1))
layout_pin=$here/clap_layout_pin
id=org.omx-clap-host.test.passthrough

: "${PHD_ERR_PIN_ABSENT:?}" "${PHD_ERR_PIN_BINARY_MISMATCH:?}" "${PHD_ERR_PIN_LAYOUT_MISMATCH:?}" "${PHD_VERB_PIN_SET:?}" \
  "${PHD_VERB_PIN_CLEAR:?}" "${PHD_READY_LINE:?}"

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$daemon" "$host" "$here/fake.clap" "$layout_pin"; do
        [ -e "$f" ] || { echo "missing '$f'" >&2; exit 2; }
    done
    JACK_E2E_INSIDE=1 PLUGIN_HOSTD=$(readlink -f "$daemon") OMX_CLAP_HOST=$(readlink -f "$host") JACK_E2E_PORT=$port \
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
        tail -5 "$runtime/daemon.log" 2>/dev/null | sed 's/^/     daemon: /'
    fi
}

runtime=$(mktemp -d /tmp/omx-clap-host-e2e.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS FAKE_LAYOUT_DEFAULT
pids=()
# pid 1 of the namespace: every process in it dies with this script, the trap only makes it orderly
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

# the plugin in a directory of its own: the pin names its files relative to the .clap's directory
mkdir "$runtime/plugins"
cp "$here/fake.clap" "$runtime/plugins/fake.clap"
clap=$runtime/plugins/fake.clap
uri="clap:$clap#$id"
digest=$(sha256sum "$clap" | cut -d' ' -f1)
pin=$("$layout_pin" "$clap" "$id")
other=$(FAKE_LAYOUT_DEFAULT=32 "$layout_pin" "$clap" "$id")
[ -n "$pin" ] && [ -n "$other" ] && [ "$pin" != "$other" ]
step $? "the declared layout and the one with default 32 are two pins"

cat >"$runtime/plugin-hostd.conf" <<EOF
mod_host $OMX_CLAP_HOST
clap_host $OMX_CLAP_HOST
state_root $runtime
require_pins 1
EOF

send() {
    printf '%s\0' "$1" >&3
    IFS= read -r -d '' -t 10 reply <&3
    echo "$reply"
}
expect() {
    local reply
    reply=$(send "$1")
    [ "$reply" = "$2" ]
    step $? "$1 -> $reply"
}
logged() { grep -c -x "$id $1" "$runtime/fake.log"; }
in_graph() { timeout 10 pw-link -o 2>/dev/null | grep -q "^effect_$1:"; }

: >"$runtime/fake.log"
FAKE_LOG=$runtime/fake.log "$PLUGIN_HOSTD" -n -p "$port" -f "$fbport" -c "$runtime/plugin-hostd.conf" >"$runtime/daemon.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q -x -F "$PHD_READY_LINE" "$runtime/daemon.log" && break; sleep 0.1; done
grep -q -x -F "$PHD_READY_LINE" "$runtime/daemon.log"
step $? "plugin-hostd ready on port $port, require_pins 1, omx-clap-host its CLAP worker"
exec 3<>"/dev/tcp/127.0.0.1/$port"
exec 4<>"/dev/tcp/127.0.0.1/$fbport"

# unpinned: the daemon refuses and no worker sees it
expect "add $uri 0" "resp $PHD_ERR_PIN_ABSENT"
[ "$(logged init)" -eq 0 ]; step $? "no plugin code ran for the unpinned add"

# a wrong digest of the binary: the daemon refuses before any worker
expect "$PHD_VERB_PIN_SET $uri fake.clap=$(printf '%064d' 0) $pin" "resp 0"
expect "add $uri 0" "resp $PHD_ERR_PIN_BINARY_MISMATCH"
[ "$(logged init)" -eq 0 ]; step $? "no plugin code ran for the swapped binary"

# the true pin: the worker checks the layout and the plugin loads, is activated and processes
expect "$PHD_VERB_PIN_SET $uri fake.clap=$digest $pin" "resp 0"
expect "add $uri 0" "resp 0"
for _ in $(seq 30); do [ "$(logged process)" -ge 1 ] && break; sleep 0.1; done
[ "$(logged activate)" -eq 1 ] && [ "$(logged process)" -eq 1 ]
step $? "the pinned instance was activated and processes ($(logged activate) activate, $(logged process) process)"
in_graph 0; step $? "effect_0 is in the graph"

# the right binary with a layout pin the plugin does not have: the worker destroys it before activate
expect "$PHD_VERB_PIN_SET $uri fake.clap=$digest $other" "resp 0"
expect "add $uri 1" "resp $PHD_ERR_PIN_LAYOUT_MISMATCH"
sleep 0.5
[ "$(logged init)" -eq 2 ] && [ "$(logged destroy)" -eq 1 ]
step $? "the refused instance was created and destroyed in the worker ($(logged init) init, $(logged destroy) destroy)"
[ "$(logged activate)" -eq 1 ] && [ "$(logged process)" -eq 1 ]
step $? "and never activated nor processed: the counts are instance 0's ($(logged activate) activate, $(logged process) process)"
! in_graph 1; step $? "effect_1 is not in the graph"

expect "$PHD_VERB_PIN_CLEAR $uri" "resp 0"
expect "add $uri 1" "resp $PHD_ERR_PIN_ABSENT"
expect "remove 0" "resp 0"
expect "quit" "resp 0"

if [ "$failures" -eq 0 ]; then
    echo "plugin-hostd pin e2e ok"
    exit 0
fi
echo "plugin-hostd pin e2e FAILED ($failures)"
exit 1
