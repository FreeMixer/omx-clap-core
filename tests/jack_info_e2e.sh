#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# track_info, remote_pages, remote_page_get and param_info over jack, end to end, in a PipeWire of its own: the
# script re-runs itself in a private user, net, pid and mount namespace with its own /proc, starts pipewire on a
# private runtime dir, then drives the host over its socket and reads its feedback socket. The fake plugin's
# passthrough reads clap.track-info and has two remote pages; its FAKE_LOG says what the host's get answered and
# whether any call came from another thread than init's. Nothing here touches the PipeWire of the session that runs it.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5566); the feedback port is the next one
# PHD_*:            what plugin-hostd's header declares, read from it by make test-jack-info

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5566}
fbport=$((port + 1))
fake=$here/fake.clap
id=org.omx-clap-host.test.passthrough
uri="clap:$fake#$id"

: "${PHD_ERR_NO_PARAM_CONTRACT:?}" "${PHD_EVENT_REMOTE_PAGES_CHANGED:?}"

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

runtime=$(mktemp -d /tmp/omx-clap-host-e2e.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS FAKE_LAYOUT_DEFAULT
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

log=$runtime/fake.log
feedback=$runtime/feedback.log
: >"$log"
FAKE_LOG=$log "$host" -n -p "$port" -f "$fbport" >"$runtime/host.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q "ready!" "$runtime/host.log" && break; sleep 0.1; done
grep -q "ready!" "$runtime/host.log"
step $? "omx-clap-host ready on port $port, feedback on $fbport"
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
tracks() { grep -c "^$id track_info " "$log"; }
last_track() { grep "^$id track_info " "$log" | tail -1 | cut -d' ' -f3-; }
changed() { grep -c -x "$PHD_EVENT_REMOTE_PAGES_CHANGED $1" "$feedback"; }

expect "add $uri 0" "resp 0"

# track_info reaches the plugin byte for byte: one changed, and the host's get answers name, colour and flags
name='Kick \"In\" ñ'
expect "track_info 0 \"$name\" #FF8000 bus" "resp 0"
# CLAP_TRACK_INFO_HAS_TRACK_NAME 0x1 | HAS_TRACK_COLOR 0x2 | IS_FOR_BUS 0x10; IS_FOR_MASTER is 0x20
[ "$(tracks)" -eq 1 ] && [ "$(last_track)" = '1 13 255,255,128,0 Kick "In" ñ' ]
step $? "the plugin read it once, byte-exact: $(last_track)"
for _ in $(seq 30); do [ "$(changed 0)" -ge 1 ] && break; sleep 0.1; done
[ "$(changed 0)" -eq 1 ]
step $? "the plugin's remote_controls.changed reached the feedback port once: $(tr '\n' '|' <"$feedback")"

expect 'track_info 0 "" - master' "resp 0"
[ "$(tracks)" -eq 2 ] && [ "$(last_track)" = '1 20 0,0,0,0 ' ]
step $? "\"\" and - clear their flags, master is IS_FOR_MASTER: $(last_track)"
expect "track_info 0 Strip #0a0B0c" "resp 0"
[ "$(last_track)" = '1 3 255,10,11,12 Strip' ]
step $? "no kind is an input channel, either case of hex: $(last_track)"

long=$(printf 'x%.0s' $(seq 256))
expect "track_info 0 $long -" "resp -902"
expect "track_info 0 a #FF800" "resp -902"
expect "track_info 0 a FF8000" "resp -902"
expect "track_info 0 a #FF8000 aux" "resp -902"
expect "track_info 0 a - bus extra" "resp -902"
[ "$(tracks)" -eq 3 ] && [ "$(last_track)" = '1 3 255,10,11,12 Strip' ]
step $? "a refused line reaches no plugin and leaves the stored copy as it was"
expect "track_info 9 a -" "resp -3"

# remote pages: two, each slot the id param_set takes or - for an empty one, a read-only parameter and an unknown id
expect "remote_pages 0" "resp 2"
expect "remote_page_get 0 0" 'resp 0 7 "Main" "Page \"A\" 1" 0 - - - - - - -'
expect "remote_page_get 0 1" 'resp 0 8 "" "Two" - - - - - - - 0'
expect "remote_page_get 0 2" "resp -902"
expect "remote_page_get 0 x" "resp -902"
expect "remote_pages 9" "resp -3"
! grep -q "^$id remote_controls.get past the count" "$log"
step $? "a page past the count is refused without asking the plugin"

# param_info: a parameter with no checked contract falls back to qualification; what is not a parameter is -103
expect "param_info 0 0" "resp $PHD_ERR_NO_PARAM_CONTRACT"
expect "param_info 0 1" "resp -103"
expect "param_info 0 99" "resp -103"
expect "param_info 0 :bypass" "resp -103"
expect "param_info 9 0" "resp -3"

! grep -q "^$id wrong thread" "$log"
step $? "every CLAP call landed on the thread that ran init: $(grep -c "wrong thread" "$log") off it"

expect "remove 0" "resp 0"
expect "quit" "resp 0"

if [ "$failures" -eq 0 ]; then
    echo "jack info e2e ok"
    exit 0
fi
echo "jack info e2e FAILED ($failures)"
cat "$feedback" | sed 's/^/     feedback: /'
exit 1
