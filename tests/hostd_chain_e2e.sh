#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A chain behind plugin-hostd, its crash drill, end to end (#104, docs/chain.md), in a PipeWire of its own: the daemon
# with this host as its CLAP worker declares a chain of three, comp (tests/fake_compressor.clap: -6 dB on a block
# peaking at 0.1 or more, so its place shows), gain (tests/fault-14.clap: in x gain, and an abort() on a gain of 0.9 or
# more once it has run 200 blocks) and pad (tests/fault-0.clap, -20 dB). Then:
#
#   1 the chain is one node (pw-dump) and its output equals comp, gain, pad one client each in series, bit for bit
#   2 the write that makes gain abort: the daemon names gain quarantined and forms the chain again with comp and pad, in
#     that order; gain is a node of its own, the chain one node, pad none
#   3 the chain's output equals comp then pad one client each, bit for bit, and differs from pad then comp; the
#     quarantined gain still answers in its own worker
#
# The references run in a second omx-clap-host of their own, outside the daemon, fed from the same cycles
# (tests/jack_identity). Exit 0 only when every step held; each step is printed.
#
# PLUGIN_HOSTD:     the daemon (required)
# OMX_CLAP_HOST:    the worker (default ./omx-clap-host)
# JACK_E2E_PORT:    the daemon's socket port (default 5576); the feedback port is the next one
# FRAMES:           frames per phase (default 24000)
# PHD_*:            what plugin-hostd's headers declare, read from them by make test-hostd-chain

set -u

here=$(cd "$(dirname "$0")" && pwd)
. "${PLUGIN_HOSTD_TESTS_LIB:-$here/../../plugin-hostd/tests/lib}/protocol.sh"
root=$(dirname "$here")
daemon=${PLUGIN_HOSTD:-}
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5576}
fbport=$((port + 1))
refport=$((port + 2))
frames=${FRAMES:-24000}
feeder=$here/jack_identity
preroll=4

: "${PHD_READY_LINE:?}" "${OMX_VERB_CHAIN:?}" "${PHD_EVENT_CHAIN_FORMED:?}" "${PHD_EVENT_INSTANCE_QUARANTINED:?}" \
  "${OMX_VERB_WORKER_LIST:?}" "${PHD_PLACE_CHAIN_PREFIX:?}"

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$daemon" "$host" "$feeder" "$here/fake_compressor.clap" "$here/fake_state.clap" "$here/fault-14.clap" "$here/fault-0.clap"; do
        [ -e "$f" ] || { echo "missing '$f'" >&2; exit 2; }
    done
    export JACK_E2E_INSIDE=1 PLUGIN_HOSTD=$(readlink -f "$daemon") OMX_CLAP_HOST=$(readlink -f "$host") JACK_E2E_PORT=$port
    if unshare --user --map-root-user --net --pid --fork --mount-proc true 2>/dev/null; then
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
    fi
    # a container whose /proc is masked refuses a /proc of its own: a session of its own instead (chain_identity.sh)
    JACK_E2E_SESSION=1 exec unshare --user --map-root-user --net setsid -w "$0" "$@"
fi

failures=0
step() {
    if [ "$1" -eq 0 ]; then
        echo "ok   $2"
    else
        echo "FAIL $2"
        failures=$((failures + 1))
        tail -5 "$runtime/daemon.log" 2>/dev/null | sed 's/^/     daemon: /'
    fi
}

runtime=$(mktemp -d /tmp/omx-hostd-chain.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
pids=()
cleanup() {
    exec 3>&- 4>&- 5>&- 7>&- 2>/dev/null
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    if [ -n "${JACK_E2E_SESSION:-}" ]; then
        trap '' TERM
        kill -TERM -- -$$ 2>/dev/null
    else
        kill -TERM -- -1 2>/dev/null
    fi
    wait 2>/dev/null
    rm -rf "${runtime:?}"
}
trap cleanup EXIT

ip link set lo up
pipewire >"$runtime/pw.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do [ -S "$runtime/pipewire-0" ] && break; sleep 0.1; done
[ -S "$runtime/pipewire-0" ] || { echo "pipewire did not come up" >&2; cat "$runtime/pw.log" >&2; exit 2; }
step 0 "private pipewire on $runtime"

cat >"$runtime/plugin-hostd.conf" <<EOF
mod_host $OMX_CLAP_HOST
clap_host $OMX_CLAP_HOST
state_root $runtime
require_pins 0
EOF
"$PLUGIN_HOSTD" -n -p "$port" -f "$fbport" -c "$runtime/plugin-hostd.conf" >"$runtime/daemon.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do grep -q -x -F "$PHD_READY_LINE" "$runtime/daemon.log" && break; sleep 0.1; done
grep -q -x -F "$PHD_READY_LINE" "$runtime/daemon.log"
step $? "plugin-hostd ready on port $port, omx-clap-host its CLAP worker"
exec 3<>"/dev/tcp/127.0.0.1/$port"
exec 4<>"/dev/tcp/127.0.0.1/$fbport"
# the feedback port's lines, one per line as each arrives (a read loop: tr would hold them in its buffer)
while IFS= read -r -d '' line; do printf '%s\n' "$line"; done <&4 >"$runtime/events.log" &
pids+=($!)

"$OMX_CLAP_HOST" -n -p "$refport" >"$runtime/ref.log" 2>&1 &
pids+=($!)
for _ in $(seq 100); do grep -q ready "$runtime/ref.log" && break; sleep 0.1; done
grep -q ready "$runtime/ref.log"; step $? "the references' host ready"
exec 5<>"/dev/tcp/127.0.0.1/$refport"

send() { protocol_send "$1" 30 "$2"; }
expect() {
    local reply
    reply=$(send "$1" "$2")
    [ "$reply" = "$3" ]
    step $? "$( [ "$1" = 3 ] && echo daemon || echo reference): $2 -> $reply"
}
event() {
    for _ in $(seq 150); do grep -q -E "$1" "$runtime/events.log" && return 0; sleep 0.1; done
    return 1
}
# the nodes of the graph named effect_0 .. effect_9: the daemon's instances
nodes() {
    pw-dump 2>/dev/null | jq -r '.[] | select(.type == "PipeWire:Interface:Node") | .info.props["node.name"] // empty' \
        | grep '^effect_[0-9]$' | sort | tr '\n' ' '
}
wait_nodes() {
    for _ in $(seq 100); do [ "$(nodes)" = "$1" ] && return 0; sleep 0.1; done
    return 1
}

comp="clap:$here/fake_compressor.clap#org.omx-clap-host.test.compressor"
gain="clap:$here/fault-14.clap#org.openmixer.fault.14"
pad="clap:$here/fault-0.clap#org.openmixer.fault.0"
expect 3 "add $comp 0" "resp 0"
expect 3 "add $gain 1" "resp 1"
expect 3 "add $pad 2" "resp 2"
expect 5 "add $comp 20" "resp 20"
expect 5 "add $gain 21" "resp 21"
expect 5 "add $pad 22" "resp 22"
expect 5 "add $pad 30" "resp 30"
expect 5 "add $comp 31" "resp 31"
wait_nodes "effect_0 effect_1 effect_2 "; step $? "three instances, three nodes: $(nodes)"

# 1. the chain: one worker, one node
expect 3 "$OMX_VERB_CHAIN 5 0 1 2" "resp 0"
event "^$PHD_EVENT_CHAIN_FORMED 5 w[0-9]+ 0,1,2$"; step $? "the daemon formed chain 5: $(grep "^$PHD_EVENT_CHAIN_FORMED" "$runtime/events.log" | tail -1)"
wait_nodes "effect_0 "; step $? "chain 5 is one node: $(nodes)"

links=()
link() { timeout 10 pw-link "$1" "$2" 2>/dev/null; step $? "link $1 -> $2"; links+=("$1|$2"); }
unlink_all() {
    local l
    for l in "${links[@]}"; do timeout 5 pw-link -d "${l%%|*}" "${l##*|}" 2>/dev/null; done
    links=()
}
series() {
    local a=$1 b
    shift
    for b in "$@"; do
        link "effect_$a:out_1" "effect_$b:in_1"
        link "effect_$a:out_2" "effect_$b:in_2"
        a=$b
    done
}
# one phase: the chain connected through the daemon (its ledger keeps the connection, the head's), the references
# linked directly; arms name=first,last, the first the chain's
play() {
    local phase=$1 files=() recs=(a b c d e f) n=0 spec name first last
    shift
    for spec in "$@"; do files+=("$runtime/$phase.${spec%%=*}.l" "$runtime/$phase.${spec%%=*}.r"); done
    rm -f "$runtime/feed.in"
    mkfifo "$runtime/feed.in"
    "$feeder" "$frames" $preroll "$runtime/$phase.source" "${files[@]}" <"$runtime/feed.in" >"$runtime/$phase.feed" 2>&1 &
    local feeder_pid=$!
    exec 7>"$runtime/feed.in"
    for _ in $(seq 50); do grep -q "^ready" "$runtime/$phase.feed" && break; sleep 0.1; done
    for spec in "$@"; do
        first=${spec#*=}
        last=${first#*,}
        first=${first%,*}
        if [ $n = 0 ]; then
            expect 3 "connect identity-src:out effect_$first:in_1" "resp 0"
            expect 3 "connect identity-src:out effect_$first:in_2" "resp 0"
            expect 3 "connect effect_$last:out_1 identity-rec:${recs[$n]}" "resp 0"
            expect 3 "connect effect_$last:out_2 identity-rec:${recs[$((n + 1))]}" "resp 0"
        else
            link identity-src:out "effect_$first:in_1"
            link identity-src:out "effect_$first:in_2"
            link "effect_$last:out_1" "identity-rec:${recs[$n]}"
            link "effect_$last:out_2" "identity-rec:${recs[$((n + 1))]}"
        fi
        n=$((n + 2))
    done
    sleep 0.5
    echo go >&7
    timeout 60 tail --pid=$feeder_pid -f /dev/null
    wait $feeder_pid
    step $? "$phase: played and recorded ($(grep '^done' "$runtime/$phase.feed" || tail -1 "$runtime/$phase.feed"))"
    exec 7>&-
    # a recorder that heard nothing makes any bit identity trivial: each must have heard the source
    local silent=
    for r in $(seq 0 $((n - 1))); do
        if ! grep -q " peak_${recs[$r]}=" "$runtime/$phase.feed" || grep -q " peak_${recs[$r]}=0\( \|$\)" "$runtime/$phase.feed"; then
            silent="$silent ${recs[$r]}"
        fi
    done
    [ -z "$silent" ]; step $? "$phase: every recorder heard the source${silent:+, not:$silent} ($(grep -o ' peak_[a-z]*=[^ ]*' "$runtime/$phase.feed" | tr -d '\n'))"
}
same() {
    local out
    out=$(cmp "$2" "$3" 2>&1)
    [ -z "$out" ]
    step $? "$1: $(($(stat -c %s "$2") / 4)) samples ${out:+NOT }bit-identical${out:+ ($out)}"
}

series 20 21 22
series 30 31
play p1 chain=0,0 series=20,22
same "p1 left: chain 5 (comp, gain, pad) against them one client each" "$runtime/p1.chain.l" "$runtime/p1.series.l"
same "p1 right" "$runtime/p1.chain.r" "$runtime/p1.series.r"
unlink_all

# 2. gain crashes the chain's worker: it has run its 200 blocks, then the write that makes it abort
for _ in $(seq 200); do
    blocks=$(send 3 "param_get 1 1" | awk '{ print int($3) }')
    [ "${blocks:-0}" -ge 200 ] && break
    sleep 0.1
done
[ "${blocks:-0}" -ge 200 ]; step $? "gain has run ${blocks:-0} blocks in the chain"
# the chain's worker's pid, out of worker_list's w<k>:<pid>:<format>:<state>:<core>:<pool>:<place>:<members>; the place
# holds a colon (chain:5), so the record is read from the right
chain_pid() { send 3 "$OMX_VERB_WORKER_LIST" | tr ' ' '\n' | awk -F: -v place="${PHD_PLACE_CHAIN_PREFIX%:}" 'NF >= 9 && $(NF-2) == place && $(NF-1) == 5 { print $2 }'; }
pid_before=$(chain_pid)
reply=$(send 3 "param_set 1 0 0.95")
step 0 "daemon: param_set 1 0 0.95 -> $reply (the write that aborts gain)"
event "^$PHD_EVENT_INSTANCE_QUARANTINED 1 w[0-9]+$"
step $? "gain is quarantined: $(grep "^$PHD_EVENT_INSTANCE_QUARANTINED" "$runtime/events.log" | tail -1)"
event "^$PHD_EVENT_CHAIN_FORMED 5 w[0-9]+ 0,2$"
step $? "the chain rejoins with comp and pad in order: $(grep "^$PHD_EVENT_CHAIN_FORMED" "$runtime/events.log" | tail -1)"
wait_nodes "effect_0 effect_1 "; step $? "the chain one node, gain one of its own, pad none: $(nodes)"
pid_after=$(chain_pid)
[ -n "$pid_before" ] && [ -n "$pid_after" ] && [ "$pid_before" != "$pid_after" ] && ! kill -0 "$pid_before" 2>/dev/null
step $? "the chain's worker died and was spawned again: pid $pid_before, then $pid_after"
list=$(send 3 "$OMX_VERB_WORKER_LIST")
echo "$list" | tr '\n' ' ' | grep -q "$PHD_PLACE_CHAIN_PREFIX""5"; step $? "worker_list names the chain's worker: $(echo "$list" | tr '\n' ' ')"
reply=$(send 3 "param_get 1 0")
case $reply in "resp 0 "*) step 0 "the quarantined gain answers in its own worker: param_get 1 0 -> $reply" ;;
    *) step 1 "the quarantined gain answers in its own worker: param_get 1 0 -> $reply" ;; esac

# 3. the chain that rejoined: comp then pad, bit for bit, and not pad then comp
series 20 22
series 30 31
play p2 chain=0,0 series=20,22 other=30,31
same "p2 left: the rejoined chain against comp, pad one client each" "$runtime/p2.chain.l" "$runtime/p2.series.l"
same "p2 right" "$runtime/p2.chain.r" "$runtime/p2.series.r"
! cmp -s "$runtime/p2.chain.l" "$runtime/p2.other.l"; step $? "p2: pad then comp is another output"
grep -h "peak_" "$runtime/p1.feed" "$runtime/p2.feed" | sed 's/^/     /'
unlink_all

# 4. a member's value set before the chain forms: its state reaches the plugin, whose load answers false while it
# processes, when the chain forms and again when a worker killed outright comes back
state="clap:$here/fake_state.clap#org.omx-clap-host.test.state"
expect 3 "add $state 3" "resp 3"
expect 3 "add $state 4" "resp 4"
expect 3 "param_set 3 0 7.5" "resp 0"
expect 3 "param_get 3 0" "resp 0 7.5000"
# the value leaves the ledger once the checkpoint of the move holds it: only the state can bring it back
expect 3 "$OMX_VERB_CHAIN 6 3 4" "resp 0"
event "^$PHD_EVENT_CHAIN_FORMED 6 w[0-9]+ 3,4$"; step $? "chain 6 formed: $(grep "^$PHD_EVENT_CHAIN_FORMED 6" "$runtime/events.log" | tail -1)"
expect 3 "param_get 3 0" "resp 0 7.5000"
! grep -q "refused 'state_load" "$runtime/daemon.log"; step $? "no worker refused the state: $(grep "refused 'state_load" "$runtime/daemon.log" | tail -1)"
pid6=$(send 3 "$OMX_VERB_WORKER_LIST" | tr ' ' '\n' | awk -F: '$(NF-1) == "6" && $(NF-2) == "chain" { print $2 }')
[ -n "$pid6" ] && kill -9 "$pid6"; step $? "kill -9 chain 6's worker, pid $pid6"
got=
for _ in $(seq 100); do got=$(send 3 "param_get 3 0"); [ "$got" = "resp 0 7.5000" ] && break; sleep 0.1; done
[ "$got" = "resp 0 7.5000" ]; step $? "after the kill the value is back: param_get 3 0 -> $got"
! grep -q "refused 'state_load" "$runtime/daemon.log"; step $? "and no worker refused the state"

expect 3 "remove -1" "resp 0"
expect 5 "remove -1" "resp 0"
grep -h "^chain " "$runtime"/*.log 2>/dev/null | sed 's/^/     /'
echo "hostd chain: $failures failed"
[ "$failures" -eq 0 ]
