#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The abi_clobber line over jack, end to end, in a PipeWire of its own: the script re-runs itself in a private user,
# net, pid and mount namespace, starts pipewire on a private runtime dir and runs the host with a feedback port. It adds
# fake.clap's abi-breaker, whose third process returns with callee-saved registers changed (src/abi_guard.h), beside a
# passthrough that keeps the convention, and holds: the host lives on and answers; exactly one line
# `abi_clobber <instance> <mask> <names> process 2` for the breaker, its mask and names this machine's; none for the
# passthrough; and nothing more however long it runs. Nothing here touches the PipeWire of the session that runs it.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5580); the feedback port is the next one
# OMX_HOST_EVENTS_ABI_CLOBBER: the word data/verbs.json declares, read from src/omx_clap_core_vocabulary.h by make

set -u

here=$(cd "$(dirname "$0")" && pwd)
. "${PLUGIN_HOSTD_TESTS_LIB:-$here/../../plugin-hostd/tests/lib}/protocol.sh"
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5580}
fbport=$((port + 1))
fake=$here/fake.clap

: "${OMX_HOST_EVENTS_ABI_CLOBBER:?}"

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$fake"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    JACK_E2E_INSIDE=1 OMX_CLAP_HOST=$host JACK_E2E_PORT=$port \
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

runtime=$(mktemp -d /tmp/omx-clap-host-abi.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
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

"$host" -n -p "$port" -f "$fbport" >"$runtime/host.log" 2>&1 &
host_pid=$!
pids+=("$host_pid")
protocol_wait_ready "$runtime/host.log"
step $? "omx-clap-host ready on port $port, feedback on $fbport"

word=$OMX_HOST_EVENTS_ABI_CLOBBER
feedback=$runtime/feedback.log
exec 3<>"/dev/tcp/127.0.0.1/$port" 4<>"/dev/tcp/127.0.0.1/$fbport"
: >"$feedback"
while IFS= read -r -d '' line <&4; do echo "$line" >>"$feedback"; done &
pids+=($!)
PROTOCOL_TIMEOUT=5

case "$(uname -m)" in
    aarch64) mask=0x20001 names=x19,d15 ;;
    x86_64) mask=0x21 names=rbx,r15 ;;
    *) echo "no expectation for $(uname -m)" >&2; exit 2 ;;
esac

reply=$(send "add clap:$fake#org.omx-clap-host.test.passthrough 2")
[ "$reply" = "resp 2" ]; step $? "the passthrough added: $reply"
reply=$(send "add clap:$fake#org.omx-clap-host.test.abi-breaker 1")
[ "$reply" = "resp 1" ]; step $? "the abi-breaker added: $reply"

for _ in $(seq 50); do grep -q "^$word " "$feedback" && break; sleep 0.1; done
lines=$(grep "^$word " "$feedback")
[ "$lines" = "$word 1 $mask $names process 2" ]
step $? "one line for the breaker: '$lines' (want '$word 1 $mask $names process 2')"
grep -q "^effect_1: the plugin returned from process with $names changed (block 2)" "$runtime/host.log"
step $? "and the same said on stderr"

sleep 2
[ "$(grep -c "^$word " "$feedback")" -eq 1 ]; step $? "said once, however long it runs"
! grep -q "^$word 2 " "$feedback"; step $? "nothing for the passthrough, which keeps the convention"
kill -0 "$host_pid" 2>/dev/null; step $? "the host lives on"
reply=$(send "cpu_load")
[ "${reply:0:6}" = "resp 0" ]; step $? "and answers: $reply"

echo "$failures failure(s)"
[ "$failures" -eq 0 ]
