#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# omx-clap-host runs plugin-hostd's chain line in one JACK client (#104, docs/chain.md), in a PipeWire of its own: two
# hosts from the same cycles of one deterministic input (tests/jack_identity),
#
#   chain    effect_10 comp, effect_11 pass, effect_12 gain, declared by "chain 7 ..." lines and changed while it runs
#   series   effect_20 comp, effect_21 pass, effect_22 gain, one client each, linked in series in the chain's order
#            of the moment; effect_30 .. 32 the same plugins in the other order, which the chain must NOT equal
#
# comp is tests/fake_compressor.clap (a block peaking at 0.1 or more is cut by 6 dB, so its place in the chain shows in
# the output), pass tests/fake.clap's passthrough (64 frames of latency), gain tests/fault-14.clap (in x gain, parameter
# 0, 0.1 by default). Every phase compares the chain's left and right against the series', bit for bit:
#
#   1 the line makes three nodes one (pw-dump); the latency is the sum; the other order differs
#   2 bypass and param_set reach one member each: the chain still equals the series given the same verbs, its level moved
#   3 a line that leaves pass out: pass back in a client of its own, two nodes
#   4 a line that adds pass at the end: one node again          5 a line that moves pass to the middle
#   6 a line with another head: the chain in gain's client       7 remove of a member: the chain closes around it
#   8 remove of the head: pass in a client of its own; a line of one member
#
# and every refusal (a member it does not hold, one named twice, a word that is no number, a member or a head of
# another width, a member with a note port, a member of another chain) answers its code and changes no node.
# The last line is a CHAIN-HOST json; exit 0 only when every step held.
#
# OMX_CLAP_HOST: the binary (default ./omx-clap-host); FRAMES: frames per phase (default 24000)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
frames=${FRAMES:-24000}
feeder=$here/jack_identity
latency_probe=$here/jack_latency_probe
preroll=4

if [ -z "${CHAIN_HOST_INSIDE:-}" ]; then
    for f in "$host" "$feeder" "$latency_probe" "$here/fake.clap" "$here/fake_compressor.clap" "$here/fault-14.clap"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    export CHAIN_HOST_INSIDE=1 OMX_CLAP_HOST=$(readlink -f "$host")
    if unshare --user --map-root-user --net --pid --fork --mount-proc true 2>/dev/null; then
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
    fi
    # a container whose /proc is masked refuses a /proc of its own: a session of its own instead (chain_identity.sh)
    CHAIN_HOST_SESSION=1 exec unshare --user --map-root-user --net setsid -w "$0" "$@"
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

runtime=$(mktemp -d /tmp/omx-chain-host.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
pids=()
cleanup() {
    exec 3>&- 4>&- 7>&- 2>/dev/null
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    if [ -n "${CHAIN_HOST_SESSION:-}" ]; then
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

declare -A port=([chain]=5591 [series]=5592) fd=([chain]=3 [series]=4)
for arm in chain series; do
    "$OMX_CLAP_HOST" -n -p "${port[$arm]}" >"$runtime/$arm.log" 2>&1 &
    pids+=($!)
    for _ in $(seq 100); do grep -q ready "$runtime/$arm.log" && break; sleep 0.1; done
    grep -q ready "$runtime/$arm.log"; step $? "$arm host ready"
    eval "exec ${fd[$arm]}<>/dev/tcp/127.0.0.1/${port[$arm]}"
done
send() {
    local reply
    printf '%s\0' "$2" >&"${fd[$1]}"
    IFS= read -r -d '' -t 30 reply <&"${fd[$1]}"
    echo "$reply"
}
expect() {
    local reply
    reply=$(send "$1" "$2")
    [ "$reply" = "$3" ]
    step $? "$1: $2 -> $reply"
}

comp="clap:$here/fake_compressor.clap#org.omx-clap-host.test.compressor"
pass="clap:$here/fake.clap#org.omx-clap-host.test.passthrough"
gain="clap:$here/fault-14.clap#org.openmixer.fault.14"
notes="clap:$here/fake.clap#org.omx-clap-host.test.notes"
widen="clap:$here/fake.clap#org.omx-clap-host.test.mono-stereo"
for k in 0 1 2; do
    set -- "$comp" "$pass" "$gain"
    shift $k
    expect chain "add $1 $((10 + k))" "resp $((10 + k))"
    expect series "add $1 $((20 + k))" "resp $((20 + k))"
done
expect series "add $gain 30" "resp 30"
expect series "add $pass 31" "resp 31"
expect series "add $comp 32" "resp 32"

# the nodes of the graph named effect_10 .. effect_19: the chain host's instances
nodes() {
    pw-dump 2>/dev/null | jq -r '.[] | select(.type == "PipeWire:Interface:Node") | .info.props["node.name"] // empty' \
        | grep -c '^effect_1[0-9]$'
}
# a node's ports must be in the graph before it is linked: a closed client's are gone, a new one's come a moment later
has_ports() { timeout 10 pw-link -o 2>/dev/null | grep -q "^effect_$1:out_2$"; }
wait_nodes() {
    local want=$1
    for _ in $(seq 50); do [ "$(nodes)" = "$want" ] && return 0; sleep 0.1; done
    return 1
}
[ "$(nodes)" = 3 ]; step $? "three instances, three nodes ($(nodes))"

# one phase: the feeder's source into the chain's head and each series' first instance, every last output into a
# recorder, the latency to the first two arms' last outputs compared, then the input played and recorded;
# $runtime/<phase>.<arm>.<l|r>. Arms: name=first,last
links=()
link() { timeout 10 pw-link "$1" "$2" 2>/dev/null; step $? "link $1 -> $2"; links+=("$1|$2"); }
unlink_all() {
    local l
    for l in "${links[@]}"; do timeout 5 pw-link -d "${l%%|*}" "${l##*|}" 2>/dev/null; done
    links=()
}
# series <first> <next> ... : the instances linked in series, out_1/out_2 to in_1/in_2
series() {
    local a=$1 b
    shift
    for b in "$@"; do
        link "effect_$a:out_1" "effect_$b:in_1"
        link "effect_$a:out_2" "effect_$b:in_2"
        a=$b
    done
}
play() {
    local phase=$1 files=() recs=(a b c d e f) n=0 spec name first last ready
    shift
    for spec in "$@"; do
        name=${spec%%=*}
        files+=("$runtime/$phase.$name.l" "$runtime/$phase.$name.r")
    done
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
        link identity-src:out "effect_$first:in_1"
        link identity-src:out "effect_$first:in_2"
        link "effect_$last:out_1" "identity-rec:${recs[$n]}"
        link "effect_$last:out_2" "identity-rec:${recs[$((n + 1))]}"
        n=$((n + 2))
    done
    sleep 0.5
    set -- "$@"
    lat_a=$("$latency_probe" "effect_${1##*,}:out_1" capture 2>/dev/null)
    lat_b=$("$latency_probe" "effect_${2##*,}:out_1" capture 2>/dev/null)
    [ -n "$lat_a" ] && [ "$lat_a" = "$lat_b" ]
    step $? "$phase: the latency to the last output, ${1%%=*} $lat_a, ${2%%=*} $lat_b (min max frames)"
    echo go >&7
    timeout 60 tail --pid=$feeder_pid -f /dev/null
    wait $feeder_pid
    step $? "$phase: played and recorded ($(grep '^done' "$runtime/$phase.feed" || tail -1 "$runtime/$phase.feed"))"
    exec 7>&-
    for r in $(seq 0 $((n - 1))); do
        ! grep -q " peak_${recs[$r]}=0\( \|$\)" "$runtime/$phase.feed" || step 1 "$phase: recorder ${recs[$r]} heard nothing"
    done
}
same() {
    local what=$1 a=$2 b=$3 out
    out=$(cmp "$a" "$b" 2>&1)
    [ -z "$out" ]
    step $? "$what: $(($(stat -c %s "$a") / 4)) samples ${out:+NOT }bit-identical${out:+ ($out)}"
}
differ() {
    ! cmp -s "$2" "$3"
    step $? "$1"
}

# 1. the line: three nodes become one, its output the series', the other order not, the latency the sum
expect chain "chain 7 10 11 12" "resp 0"
wait_nodes 1; step $? "chain 7 10 11 12: one node ($(nodes))"
! has_ports 11 && ! has_ports 12; step $? "the members have no port of their own"
series 20 21 22
series 30 31 32
lat_chain=$("$latency_probe" effect_10:out_1 capture 2>/dev/null)
lat_series=$("$latency_probe" effect_22:out_1 capture 2>/dev/null)
[ -n "$lat_chain" ] && [ "$lat_chain" = "$lat_series" ] && [ "${lat_chain%% *}" = 64 ]
step $? "the chain's latency is its members' sum: chain $lat_chain, series $lat_series (min max frames)"
play p1 chain=10,10 series=20,22 other=30,32
same "p1 left: the chain against the series" "$runtime/p1.chain.l" "$runtime/p1.series.l"
same "p1 right: the chain against the series" "$runtime/p1.chain.r" "$runtime/p1.series.r"
differ "p1: the other order (gain, pass, comp) is another output" "$runtime/p1.chain.l" "$runtime/p1.other.l"
unlink_all

# the refusals: each answers its code and leaves one node
expect chain "chain 7 10 11 99" "resp -3"
expect chain "chain 7 10 11 11" "resp -902"
expect chain "chain 7 10 x" "resp -902"
expect chain "chain 7" "resp -902"
expect chain "add $pass 15" "resp 15"
expect chain "add $widen 14" "resp 14"
expect chain "add $notes 13" "resp 13"
expect chain "chain 8 15 14" "resp -902"
expect chain "chain 8 14 15" "resp -902"
expect chain "chain 8 15 13" "resp -902"
expect chain "chain 8 15 10" "resp -902"
expect chain "chain 7 10 11 12 13" "resp -902"
[ "$(nodes)" = 4 ]; step $? "the refusals changed no node: the chain and three of their own ($(nodes))"
for i in 13 14 15; do expect chain "remove $i" "resp 0"; done
[ "$(nodes)" = 1 ]; step $? "after the refusals: one node ($(nodes))"

# 2. bypass and a parameter reach one member each, the series given the same verbs
expect chain "bypass 10 1" "resp 0"
expect series "bypass 20 1" "resp 0"
expect chain "param_set 12 0 0.3" "resp 0"
expect series "param_set 22 0 0.3" "resp 0"
sleep 0.3
series 20 21 22
play p2 chain=10,10 series=20,22
same "p2 left: comp bypassed and gain 0.3 in both" "$runtime/p2.chain.l" "$runtime/p2.series.l"
same "p2 right" "$runtime/p2.chain.r" "$runtime/p2.series.r"
differ "p2: the parameter moved the chain's output" "$runtime/p1.chain.l" "$runtime/p2.chain.l"
expect chain "bypass 10 0" "resp 0"
expect series "bypass 20 0" "resp 0"
unlink_all

# 3. pass left out: it leaves the walk and is a node of its own again
expect chain "chain 7 10 12" "resp 0"
wait_nodes 2; step $? "chain 7 10 12: two nodes, the chain and pass ($(nodes))"
for _ in $(seq 50); do has_ports 11 && break; sleep 0.1; done
has_ports 11; step $? "pass has its ports again"
series 20 22
play p3 chain=10,10 series=20,22
same "p3 left: the chain without pass" "$runtime/p3.chain.l" "$runtime/p3.series.l"
same "p3 right" "$runtime/p3.chain.r" "$runtime/p3.series.r"
unlink_all

# 4. pass added at the end: it closes its client again
expect chain "chain 7 10 12 11" "resp 0"
wait_nodes 1; step $? "chain 7 10 12 11: one node ($(nodes))"
series 20 22 21
play p4 chain=10,10 series=20,21
same "p4 left: pass joined at the end" "$runtime/p4.chain.l" "$runtime/p4.series.l"
same "p4 right" "$runtime/p4.chain.r" "$runtime/p4.series.r"
unlink_all

# 5. pass moved to the middle, the head the same
expect chain "chain 7 10 11 12" "resp 0"
[ "$(nodes)" = 1 ]; step $? "chain 7 10 11 12: still one node"
series 20 21 22
play p5 chain=10,10 series=20,22
same "p5 left: pass moved to the middle" "$runtime/p5.chain.l" "$runtime/p5.series.l"
same "p5 right" "$runtime/p5.chain.r" "$runtime/p5.series.r"
unlink_all

# 6. another head: the chain runs in gain's client
expect chain "chain 7 12 10 11" "resp 0"
for _ in $(seq 50); do has_ports 12 && ! has_ports 10 && break; sleep 0.1; done
wait_nodes 1 && has_ports 12 && ! has_ports 10; step $? "chain 7 12 10 11: one node, gain's ($(nodes))"
series 22 20 21
play p6 chain=12,12 series=22,21
same "p6 left: gain heads it" "$runtime/p6.chain.l" "$runtime/p6.series.l"
same "p6 right" "$runtime/p6.chain.r" "$runtime/p6.series.r"
unlink_all

# 7. remove of a member: the chain closes around it, then plugin-hostd's line without it changes nothing
expect chain "remove 10" "resp 0"
expect chain "chain 7 12 11" "resp 0"
[ "$(nodes)" = 1 ]; step $? "remove 10: one node ($(nodes))"
series 22 21
play p7 chain=12,12 series=22,21
same "p7 left: comp removed" "$runtime/p7.chain.l" "$runtime/p7.series.l"
same "p7 right" "$runtime/p7.chain.r" "$runtime/p7.series.r"
unlink_all

# 8. remove of the head: pass in a client of its own, then a line of one member
expect chain "remove 12" "resp 0"
for _ in $(seq 50); do has_ports 11 && break; sleep 0.1; done
has_ports 11 && [ "$(nodes)" = 1 ]; step $? "remove 12: pass alone in its own client ($(nodes))"
expect chain "chain 7 11" "resp 0"
play p8 chain=11,11 series=21,21
same "p8 left: pass alone, a chain of one" "$runtime/p8.chain.l" "$runtime/p8.series.l"
same "p8 right" "$runtime/p8.chain.r" "$runtime/p8.series.r"
unlink_all

expect chain "remove -1" "resp 0"
expect chain "quit" "resp 0"
expect series "remove -1" "resp 0"
grep "^chain 7:" "$runtime/chain.log" | sed 's/^/     /'

echo "chain host: $failures failed"
printf 'CHAIN-HOST {"frames":%d,"latency_chain":"%s","latency_series":"%s","failures":%d}\n' "$frames" "$lat_chain" \
    "$lat_series" "$failures"
[ "$failures" -eq 0 ]
