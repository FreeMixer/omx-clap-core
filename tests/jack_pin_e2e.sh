#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# layout pinning in omx-clap-host over jack, end to end, in a PipeWire of its
# own: the script re-runs itself in a private user, net, pid and mount
# namespace with its own /proc, starts pipewire on a private runtime dir, then
# drives the host over its socket. pin_expect names the layout the next add of
# an instance must have; the fake plugin's FAKE_LOG says which of init,
# activate, process and destroy ran, and outlives an instance the host
# destroyed. Exit 0 only when every step held; each step is printed. Nothing
# here touches the PipeWire of the session that runs it.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5564)
# PHD_*:            what plugin-hostd's headers declare, read from them by make test-jack-pin

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5564}
fake=$here/fake.clap
layout_pin=$here/clap_layout_pin
id=org.omx-clap-host.test.passthrough
uri="clap:$fake#$id"

: "${PHD_ERR_PIN_ABSENT:?}" "${PHD_ERR_PIN_LAYOUT_MISMATCH:?}" "${PHD_VERB_PIN_EXPECT:?}" "${PHD_PIN_LAYOUT_SCHEME:?}"
expect_verb=$PHD_VERB_PIN_EXPECT

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$fake" "$layout_pin"; do
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

runtime=$(mktemp -d /tmp/omx-clap-host-e2e.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS FAKE_LAYOUT_DEFAULT
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
# a host of its own with the fake's layout default in its environment ("" for the declared one) and a fresh log
start_host() {
    : >"$runtime/fake.log"
    FAKE_LAYOUT_DEFAULT=$1 FAKE_LOG=$runtime/fake.log "$host" -n -p "$port" >"$runtime/host.log" 2>&1 &
    host_pid=$!
    pids+=($host_pid)
    for _ in $(seq 50); do grep -q "ready!" "$runtime/host.log" && break; sleep 0.1; done
    grep -q "ready!" "$runtime/host.log"
    step $? "omx-clap-host ready on port $port, FAKE_LAYOUT_DEFAULT '${1}'"
    exec 3<>"/dev/tcp/127.0.0.1/$port"
}
stop_host() {
    expect "quit" "resp 0"
    exec 3>&-
    for _ in $(seq 30); do kill -0 "$host_pid" 2>/dev/null || break; sleep 0.1; done
    ! kill -0 "$host_pid" 2>/dev/null; step $? "host exited on quit"
}
logged() { grep -c -x "$id $1" "$runtime/fake.log"; }
in_graph() { timeout 10 pw-link -o 2>/dev/null | grep -q "^effect_$1:"; }

pin=$("$layout_pin" "$fake" "$id")
other=$(FAKE_LAYOUT_DEFAULT=32 "$layout_pin" "$fake" "$id")
[ -n "$pin" ] && [ -n "$other" ] && [ "$pin" != "$other" ]
step $? "the declared layout and the one with default 32 are two pins: $pin, $other"
# the bytes the digest is taken over, against the fake's declaration written out by hand: id, name, min 0, max 4096,
# default 64, CLAP_PARAM_IS_STEPPED
"$layout_pin" -b "$fake" "$id" | grep -q -x -F "$(printf '0\tlatency\t0000000000000000\t40b0000000000000\t4050000000000000\t00000001')"
step $? "parameter 0 serialises as its declaration says"
[ "${pin%%:*}" = "$PHD_PIN_LAYOUT_SCHEME" ]; step $? "the pin is in the scheme pin.h declares"
wrong_scheme="${PHD_PIN_LAYOUT_SCHEME%/*}/0:${pin#*:}"

start_host ""

# (a) the layout matches: the plugin loads, is activated and processes
expect "$expect_verb 0 $pin" "resp 0"
expect "add $uri 0" "resp 0"
for _ in $(seq 30); do [ "$(logged process)" -ge 1 ] && break; sleep 0.1; done
[ "$(logged activate)" -eq 1 ] && [ "$(logged process)" -eq 1 ]
step $? "the pinned instance was activated and processes ($(logged activate) activate, $(logged process) process)"
in_graph 0; step $? "effect_0 is in the graph"

# (d) a pin for another instance leaves this one alone, and waits for its own add, which spends it
expect "$expect_verb 2 $other" "resp 0"
expect "add $uri 1" "resp 1"
expect "add $uri 2" "resp $PHD_ERR_PIN_LAYOUT_MISMATCH"
expect "add $uri 2" "resp 2"

# (c) a scheme this host does not know is refused and pins nothing; a token outside the grammar too
expect "$expect_verb 3 $wrong_scheme" "resp $PHD_ERR_PIN_ABSENT"
expect "$expect_verb 3 omx-layout/1:0123" "resp -902"
expect "$expect_verb 3 $pin" "resp 0"
expect "$expect_verb 3 $wrong_scheme" "resp $PHD_ERR_PIN_ABSENT"
expect "add $uri 3" "resp 3"
expect "$expect_verb -1 $pin" "resp -1"
expect "$expect_verb 10000 $pin" "resp -1"

for i in 0 1 2 3; do expect "remove $i" "resp 0"; done
stop_host

# (b) the same binary with another default: destroyed after init, never activated, nothing processed, not in the graph
start_host 32
expect "$expect_verb 0 $pin" "resp 0"
expect "add $uri 0" "resp $PHD_ERR_PIN_LAYOUT_MISMATCH"
sleep 0.5
[ "$(logged init)" -eq 1 ] && [ "$(logged destroy)" -eq 1 ]
step $? "the refused instance was created and destroyed ($(logged init) init, $(logged destroy) destroy)"
[ "$(logged activate)" -eq 0 ] && [ "$(logged process)" -eq 0 ]
step $? "and never activated nor processed ($(logged activate) activate, $(logged process) process)"
! in_graph 0; step $? "effect_0 is not in the graph"
grep -q "$id: layout ${pin%%:*}:${other#*:}, pinned $pin" "$runtime/host.log"
step $? "the refusal names both layouts: $(grep -o "$id: layout.*" "$runtime/host.log")"
expect "param_get 0 0" "resp -3"
# its true pin loads it: the refusal was the pin's
expect "$expect_verb 0 $other" "resp 0"
expect "add $uri 0" "resp 0"
for _ in $(seq 30); do [ "$(logged process)" -ge 1 ] && break; sleep 0.1; done
[ "$(logged activate)" -eq 1 ] && [ "$(logged process)" -eq 1 ]
step $? "with its own pin it is activated and processes ($(logged activate) activate, $(logged process) process)"
expect "remove 0" "resp 0"
stop_host

if [ "$failures" -eq 0 ]; then
    echo "jack pin e2e ok"
    exit 0
fi
echo "jack pin e2e FAILED ($failures)"
exit 1
