#!/usr/bin/env bash
#
# the meters of omx-clap-host over jack, end to end, in a PipeWire of its own:
# the script re-runs itself in a private user, net, pid and mount namespace
# with its own /proc, starts pipewire on a private runtime dir, then drives
# the host over its socket and reads its feedback socket. The fake compressor
# is fed a constant input by jack_meter_source; every meter's derived symbol
# answers monitor_output and its value arrives as output_set. Exit 0 only
# when every step held; each step is printed. Nothing here touches the
# PipeWire of the session that runs it.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5562); the feedback port is the next one

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5562}
fbport=$((port + 1))
compressor=$here/fake_compressor.clap
fake=$here/fake.clap
source=$here/jack_meter_source

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$compressor" "$fake" "$source"; do
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
feedback=$runtime/feedback.log
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
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

"$host" -n -p "$port" -f "$fbport" >"$runtime/host.log" 2>&1 &
host_pid=$!
pids+=($host_pid)
for _ in $(seq 50); do grep -q "ready!" "$runtime/host.log" && break; sleep 0.1; done
grep -q "ready!" "$runtime/host.log"
step $? "omx-clap-host ready on port $port, feedback on $fbport"

# the command socket is accepted first, then the feedback socket; every feedback message is a line of the log
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
# the last value output_set carried for an instance's symbol, and how many lines it took
last() { awk -v i="$1" -v s="$2" '$1 == "output_set" && $2 == i && $3 == s { v = $4 } END { print v }' "$feedback"; }
lines() { awk -v i="$1" -v s="$2" '$1 == "output_set" && $2 == i && $3 == s { n++ } END { print n + 0 }' "$feedback"; }
# wait up to 3 s for a symbol's last value to be `want` within `tol`
settles() {
    local v
    for _ in $(seq 30); do
        v=$(last "$1" "$2")
        [ -n "$v" ] && near "$v" "$3" "$4" && return 0
        sleep 0.1
    done
    return 1
}
value() {
    local instance=$1 symbol=$2 want=$3 tol=$4
    settles "$instance" "$symbol" "$want" "$tol"
    step $? "output_set $instance $symbol reads $want ($(last "$instance" "$symbol"), $(lines "$instance" "$symbol") lines)"
}

expect "add clap:$compressor#org.omx-clap-host.test.compressor 0" "resp 0"
expect "add clap:$compressor#org.omx-clap-host.test.compressor-std 1" "resp 1"
expect "add clap:$fake#org.omx-clap-host.test.passthrough 3" "resp 3"

"$source" effect_0:in_1 effect_0:in_2 effect_1:in_1 effect_1:in_2 >"$runtime/source.log" 2>&1 &
source_pid=$!
pids+=($source_pid)
for _ in $(seq 50); do grep -q playing "$runtime/source.log" && break; sleep 0.1; done
grep -q playing "$runtime/source.log"; step $? "the source plays 0.5 and 0.25 into effect_0 and effect_1"

# org.openmixer.meters/1: one symbol per meter, _<channel> for a meter of two, a leading digit prefixed
expect "monitor_output 0 gain_reduction" "resp 1"
expect "monitor_output 0 input_level_0" "resp 1"
expect "monitor_output 0 input_level_1" "resp 1"
expect "monitor_output 0 _2nd_stage_GR" "resp 1"
expect "monitor_output 0 _0" "resp 1"
expect "monitor_output 0 gain_reduction" "resp 1"
# the plugin serves both extensions: its own is asked first and the standard one is then not an output
expect "monitor_output 0 gain_adjustment_metering" "resp 0"
expect "monitor_output 0 input_level" "resp 0"
expect "monitor_output 0 0" "resp 0"
expect "monitor_output 0 nothing" "resp 0"
expect "monitor_output 2 gain_reduction" "resp 0"

value 0 gain_reduction -6 0.0001
value 0 input_level_0 -6.0206 0.0001
value 0 input_level_1 -12.0412 0.0001
value 0 _2nd_stage_GR -3 0.0001
value 0 _0 -12.0206 0.0001

# clap.gain-adjustment-metering/0 alone: read on the audio thread after process(), one symbol
expect "monitor_output 1 gain_adjustment_metering" "resp 1"
expect "monitor_output 1 gain_reduction" "resp 0"
value 1 gain_adjustment_metering -6 0.0001
expect "param_get 1 0" "resp 0 0.0000"
expect "param_get 0 0" "resp 0 0.0000"

# a value that does not move is not sent again
before=$(wc -l <"$feedback")
sleep 1
after=$(wc -l <"$feedback")
[ "$before" -eq "$after" ]; step $? "no output_set while no value moves ($before lines, then $after)"
# the first value of an output monitored again is not sent twice
expect "monitor_output 1 gain_adjustment_metering" "resp 1"
sleep 0.3
[ "$(lines 1 gain_adjustment_metering)" -eq 1 ]; step $? "monitoring an output twice sends its first value once"

# bypassed, the plugin is not processing: the standard value is 0
expect "bypass 1 1" "resp 0"
value 1 gain_adjustment_metering 0 0
expect "bypass 1 0" "resp 0"
value 1 gain_adjustment_metering -6 0.0001

# a plugin with neither extension has no output
expect "monitor_output 3 gain_adjustment_metering" "resp 0"
expect "monitor_output 3 gain_reduction" "resp 0"

# a symbol two meters derive refuses the plugin, naming both
expect "add clap:$compressor#org.omx-clap-host.test.compressor-clash 2" "resp -102"
grep -q 'org.omx-clap-host.test.compressor-clash: meter 1 "level 0" channel 0 derives level_0, the symbol of meter 0 "level" channel 0' "$runtime/host.log"
step $? "the clash is refused with both meters named: $(grep -o 'compressor-clash: meter.*' "$runtime/host.log")"

# the input stops: silence is no reduction and no level
kill "$source_pid"
value 0 gain_reduction 0 0
value 1 gain_adjustment_metering 0 0
[ "$(last 0 input_level_0)" = "-inf" ]; step $? "output_set 0 input_level_0 reads -inf once the input is silent ($(last 0 input_level_0))"

echo "the output_set lines, in order:"
sed 's/^/     /' "$feedback"

expect "remove 0" "resp 0"
expect "monitor_output 0 gain_reduction" "resp 0"
expect "remove 1" "resp 0"
expect "remove 3" "resp 0"
expect "quit" "resp 0"
sleep 0.3
! kill -0 "$host_pid" 2>/dev/null; step $? "host exited on quit"

if [ "$failures" -eq 0 ]; then
    echo "jack meters e2e ok"
    exit 0
fi
echo "jack meters e2e FAILED ($failures)"
exit 1
