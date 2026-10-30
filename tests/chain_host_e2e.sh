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
#   2 bypass and param_set reach one member each: the chain still equals the series given the same verbs, its level moved;
#     then the bypass of a member that is not the head (2b)
#   3 a line that leaves pass out: pass back in a client of its own, two nodes
#   4 a line that adds pass at the end: one node again          5 a line that moves pass to the middle, in one walk
#   5b pass at the head, comp moved after gain in one walk: a move the output hears (pass copies, so its own move is
#     silent), the order before it another output
#   6 a line with another head: the chain in gain's client       7 remove of a member: the chain closes around it
#   8 remove of the head: pass in a client of its own; a line of one member
#
# and every refusal (a member it does not hold, one named twice, a word that is no number, a member or a head of
# another width, a member with an auxiliary port, a member of another chain) answers its code and
# changes no node.
# The last line is a CHAIN-HOST json; exit 0 only when every step held.
#
# OMX_CLAP_HOST: the binary (default ./omx-clap-host); FRAMES: frames per phase (default 24000)

set -u

here=$(cd "$(dirname "$0")" && pwd)
. "${PLUGIN_HOSTD_TESTS_LIB:-$here/../../plugin-hostd/tests/lib}/protocol.sh"
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
    FAKE_LOG=$runtime/fake.log "$OMX_CLAP_HOST" -n -p "${port[$arm]}" >"$runtime/$arm.log" 2>&1 &
    pids+=($!)
    for _ in $(seq 100); do grep -q ready "$runtime/$arm.log" && break; sleep 0.1; done
    grep -q ready "$runtime/$arm.log"; step $? "$arm host ready"
    eval "exec ${fd[$arm]}<>/dev/tcp/127.0.0.1/${port[$arm]}"
done
send() { protocol_send "${fd[$1]}" 30 "$2"; }
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
sidechain="clap:$here/fake.clap#org.omx-clap-host.test.sidechain"
auxout="clap:$here/fake.clap#org.omx-clap-host.test.auxout"
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
# linked <output> <input>: the graph links the two (pw-link -l: each output, then "  |-> <input>" per link)
linked() {
    timeout 10 pw-link -l 2>/dev/null | awk -v a="$1" -v b="$2" '$0 == a { o = 1; next } /^[^ ]/ { o = 0 } o && $NF == b && $1 == "|->" { f = 1 } END { exit !f }'
}
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
# the walks the library has published for chain $1, out of the last read-back: "chain <n>: ... walk <w>"
walk_of() { readback "$1" | sed -n 's/.*, walk \([0-9][0-9]*\)$/\1/p'; }
# the last read-back of chain $1 (a refusal's reason on stderr starts "chain <n>: effect_<m>" as well)
readback() { grep -E "^chain $1: (effect_[0-9]+ )+in effect_[0-9]+'s client, " "$runtime/chain.log" | tail -1; }

# the drills (CHAIN_HOST_FAULT, tests/chain_host_sabotage.sh: a copy of the tree where one JACK call fails for one
# instance): a line that fails part way answers the error and frees no instance; one refused changes nothing
if [ -n "${CHAIN_HOST_FAULT:-}" ]; then
    expect chain "chain 7 10 11 12" "resp 0"
    wait_nodes 1; step $? "chain 7 10 11 12: one node ($(nodes))"
    walk_before=$(walk_of 7)
    held="10 11 12"
    case $CHAIN_HOST_FAULT in
    client)
        line="chain 7 10 12" what="effect_11's own client cannot be made" want="effect_10 effect_11 effect_12" want_nodes=1 code=-201
        ;;
    stop)
        expect chain "add $pass 13" "resp 13"
        held="10 11 12 13"
        line="chain 7 10 12 13" what="effect_13's own client cannot be stopped" want="effect_10 effect_11 effect_12" want_nodes=2 code=-203
        ;;
    activate)
        line="chain 7 10 12" what="effect_11's own client cannot be activated" want="effect_10 effect_12 effect_11" want_nodes=1 code=-202
        ;;
    # the library refuses the line's order (a pure move): the walk keeps the order it had, the line answers the refusal
    order)
        line="chain 7 10 12 11" what="the library refuses the order" want="effect_10 effect_11 effect_12" want_nodes=1 code=-902
        ;;
    # another head: effect_13's client will not stop, after everything else is ready, before anything changes
    rehead-stop)
        expect chain "add $pass 13" "resp 13"
        held="10 11 12 13"
        line="chain 7 12 10 13" what="another head, effect_13's own client cannot be stopped" want="effect_10 effect_11 effect_12"
        want_nodes=2 code=-203
        ;;
    # another head: effect_12, the new head, gets its client of its own first (the last member first), then effect_11's,
    # left out, will not activate: both go back to their places in the old walk
    rehead-activate)
        line="chain 7 12 10" what="another head, effect_11's own client cannot be activated" want="effect_10 effect_11 effect_12"
        want_nodes=1 code=-202
        ;;
    # another head: effect_12 and effect_11 are in clients of their own when effect_10, the old head named as a member,
    # will not stop, last: both go back to their places in the old walk, which effect_10's client still runs
    rehead-head-stop)
        line="chain 7 12 10" what="another head, effect_10's own client cannot be stopped" want="effect_10 effect_11 effect_12"
        want_nodes=1 code=-203
        ;;
    # effect_13, named anew and linked in a client of its own, is stopped, then effect_14's client will not stop:
    # effect_13 runs again with the links it had
    stop-links)
        expect chain "add $pass 13" "resp 13"
        expect chain "add $pass 14" "resp 14"
        held="10 11 12 13 14" relinked=13
        line="chain 7 10 12 13 14" what="effect_14's own client cannot be stopped after effect_13's was" want="effect_10 effect_11 effect_12"
        want_nodes=3 code=-203
        ;;
    # another head: effect_13, named anew and linked in a client of its own, is stopped, then effect_11's client will not
    # activate: effect_13 runs again with the links it had
    rehead-activate-links)
        expect chain "add $pass 13" "resp 13"
        held="10 11 12 13" relinked=13
        line="chain 7 12 10 13" what="another head, effect_13's own client stopped, effect_11's cannot be activated"
        want="effect_10 effect_11 effect_12" want_nodes=2 code=-202
        ;;
    *)
        echo "CHAIN_HOST_FAULT=$CHAIN_HOST_FAULT: client, stop, activate, order, rehead-stop, rehead-activate, rehead-head-stop," \
            "stop-links or rehead-activate-links" >&2
        exit 2
        ;;
    esac
    # a member named anew linked in its own client before the line: out_1 and out_2 into effect_30's inputs (the series
    # host's, which no phase records)
    if [ -n "${relinked:-}" ]; then
        for _ in $(seq 50); do has_ports "$relinked" && break; sleep 0.1; done
        link "effect_$relinked:out_1" effect_30:in_1
        link "effect_$relinked:out_2" effect_30:in_2
        linked "effect_$relinked:out_1" effect_30:in_1 && linked "effect_$relinked:out_2" effect_30:in_2
        step $? "fault $CHAIN_HOST_FAULT: effect_$relinked linked before the line"
    fi
    reply=$(send chain "$line")
    [ "$reply" = "resp $code" ]; step $? "$line, $what: answers the error, resp $code ($reply)"
    members=$(readback 7 | sed -n 's/^chain 7: \(.*\) in effect_.*$/\1/p')
    [ "$members" = "$want" ]; step $? "fault $CHAIN_HOST_FAULT: the chain runs $members (want $want)"
    if [ -n "${relinked:-}" ]; then
        for _ in $(seq 50); do linked "effect_$relinked:out_1" effect_30:in_1 && linked "effect_$relinked:out_2" effect_30:in_2 && break; sleep 0.1; done
        linked "effect_$relinked:out_1" effect_30:in_1 && linked "effect_$relinked:out_2" effect_30:in_2
        step $? "fault $CHAIN_HOST_FAULT: effect_$relinked, stopped and run again, has its links again ($(pw-link -l 2>/dev/null | grep -A2 "^effect_$relinked:out_" | tr '\n' ' '))"
    fi
    case $CHAIN_HOST_FAULT in
    activate | rehead-activate | rehead-head-stop | rehead-activate-links) ;;
    *) [ "$(walk_of 7)" = "$walk_before" ]; step $? "fault $CHAIN_HOST_FAULT: refused, no walk published (walk $walk_before, then $(walk_of 7))" ;;
    esac
    wait_nodes "$want_nodes"; step $? "fault $CHAIN_HOST_FAULT: $want_nodes node(s) ($(nodes))"
    for e in $held; do
        reply=$(send chain "param_get $e 0")
        case $reply in "resp 0 "*) r=0 ;; *) r=1 ;; esac
        step $r "fault $CHAIN_HOST_FAULT: effect_$e is still an instance of this host: param_get $e 0 -> $reply"
    done
    # the output is the chain the read-back names, bit for bit
    if [ "$CHAIN_HOST_FAULT" = activate ]; then series 20 22 21; last=21; else series 20 21 22; last=22; fi
    play fault chain=10,10 series=20,$last
    same "fault $CHAIN_HOST_FAULT left: the chain as the read-back names it" "$runtime/fault.chain.l" "$runtime/fault.series.l"
    same "fault $CHAIN_HOST_FAULT right" "$runtime/fault.chain.r" "$runtime/fault.series.r"
    unlink_all
    expect chain "remove -1" "resp 0"
    expect series "remove -1" "resp 0"
    echo "chain host: $failures failed"
    [ "$failures" -eq 0 ]
    exit
fi

# the stress (CHAIN_HOST_STRESS rounds, make test-chain-host-tsan): the quantum changed all along, each change the head's
# buffer-size callback walking its members on JACK's thread, while each round members join, two move (one walk), one is
# freed by its remove and added again, the chain takes another head (effect_12, a member, gets a client of its own
# whose thread-init callback names its thread for the members while their resize may be pending) and then its first
# head back, alone
if [ -n "${CHAIN_HOST_STRESS:-}" ]; then
    (
        q=64
        while :; do
            pw-metadata -n settings 0 clock.force-quantum $q >/dev/null 2>&1
            if [ $q = 64 ]; then q=256; else q=64; fi
            sleep 0.02
        done
    ) &
    flipper=$!
    bad=0
    for k in $(seq "$CHAIN_HOST_STRESS"); do
        r="$(send chain "chain 7 10 11 12") $(send chain "chain 7 10 12 11") $(send chain "remove 11") $(send chain "add $pass 11")"
        r="$r $(send chain "chain 7 12 10") $(send chain "chain 7 10")"
        # a host that stops answering stops the rounds: each send would wait out its timeout
        [ "$r" = "resp 0 resp 0 resp 0 resp 11 resp 0 resp 0" ] || { bad=$((bad + 1)); echo "     round $k: $r"; break; }
    done
    kill "$flipper" 2>/dev/null
    wait "$flipper" 2>/dev/null
    pw-metadata -n settings 0 clock.force-quantum 0 >/dev/null 2>&1
    [ "$bad" = 0 ]; step $? "$CHAIN_HOST_STRESS rounds of joins, a move, a member freed and added, another head and the first back while the quantum changed ($bad off)"
    expect chain "chain 7 10 11 12" "resp 0"
    wait_nodes 1; step $? "and then chain 7 10 11 12 is one node ($(nodes))"
    expect chain "remove -1" "resp 0"
    expect series "remove -1" "resp 0"
    echo "chain host: $failures failed"
    [ "$failures" -eq 0 ]
    exit
fi

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
expect chain "add $sidechain 16" "resp 16"
expect chain "add $auxout 17" "resp 17"
expect chain "chain 8 15 14" "resp -902"
expect chain "chain 8 14 15" "resp -902"
expect chain "chain 8 15 10" "resp -902"
# an auxiliary port goes with its client: a member's side chain would read silence, its auxiliary output reach nothing,
# so the line is refused and says why; a head keeps its own
expect chain "chain 8 15 16" "resp -902"
grep -q "^chain 8: effect_16 has an auxiliary port" "$runtime/chain.log"
step $? "a member with a side chain is refused: $(grep "^chain 8: effect_16" "$runtime/chain.log" | tail -1)"
expect chain "chain 8 15 17" "resp -902"
grep -q "^chain 8: effect_17 has an auxiliary port" "$runtime/chain.log"
step $? "a member with an auxiliary output is refused: $(grep "^chain 8: effect_17" "$runtime/chain.log" | tail -1)"
[ "$(nodes)" = 6 ]; step $? "the refusals changed no node: the chain and five of their own ($(nodes))"
# a refusal forms nothing: no read-back of chain 8 (a head of another width refused only when its member would join
# answers the same code, but has formed the chain first)
[ -z "$(readback 8)" ]; step $? "the refusals formed no chain 8 ($(readback 8))"
expect chain "chain 8 16 15" "resp 0"
wait_nodes 5; step $? "a head with a side chain keeps it: chain 8 16 15 is one node ($(nodes))"
expect chain "chain 8 16" "resp 0"
wait_nodes 6; step $? "chain 8 16: 15 in a client of its own again ($(nodes))"
# a member with a note port is no refusal (#400: an effect whose atom port supports MIDI is a plugin with a note input, a
# third-party LV2 effect among them): the line forms, the member's note port goes with its client, the note verbs still
# reach it, and it gets the port again in a client of its own
expect chain "chain 9 15 13" "resp 0"
wait_nodes 5; step $? "a member with a note port joins: chain 9 15 13 is one node ($(nodes))"
expect chain "note_on 13 0 60 100" "resp 0"
expect chain "note_off 13 0 60 0" "resp 0"
expect chain "chain 9 15" "resp 0"
wait_nodes 6; step $? "chain 9 15: 13 in a client of its own again ($(nodes))"
for _ in $(seq 50); do timeout 10 pw-link -i 2>/dev/null | grep -q "^effect_13:midi_in$" && break; sleep 0.1; done
timeout 10 pw-link -i 2>/dev/null | grep -q "^effect_13:midi_in$"
step $? "13 has its note port again"
for i in 13 14 15 16 17; do expect chain "remove $i" "resp 0"; done
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

# a member that is not the head is bypassed by its own number as well: gain's own bypass, in the chain
expect chain "bypass 12 1" "resp 0"
expect series "bypass 22 1" "resp 0"
sleep 0.3
series 20 21 22
play p2b chain=10,10 series=20,22
same "p2b left: gain, a member, bypassed in both" "$runtime/p2b.chain.l" "$runtime/p2b.series.l"
same "p2b right" "$runtime/p2b.chain.r" "$runtime/p2b.series.r"
differ "p2b: the member's bypass moved the chain's output" "$runtime/p2.chain.l" "$runtime/p2b.chain.l"
expect chain "bypass 12 0" "resp 0"
expect series "bypass 22 0" "resp 0"
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

# 5. pass moved to the middle, the head the same: the new order is one walk the library publishes, so no cycle runs
# without pass or in an order the line never named (docs/chain.md; the read-back's walk counts the walks published)
walk_before=$(walk_of 7)
expect chain "chain 7 10 11 12" "resp 0"
walk_after=$(walk_of 7)
[ -n "$walk_before" ] && [ -n "$walk_after" ] && [ $((walk_after - walk_before)) = 1 ]
step $? "the move is one walk: walk ${walk_before:-?} to ${walk_after:-?}"
[ "$(nodes)" = 1 ]; step $? "chain 7 10 11 12: still one node"
series 20 21 22
play p5 chain=10,10 series=20,22
same "p5 left: pass moved to the middle" "$runtime/p5.chain.l" "$runtime/p5.series.l"
same "p5 right" "$runtime/p5.chain.r" "$runtime/p5.series.r"
unlink_all

# 5b. a move the output hears: pass copies its input, so where it stands is silent. With pass at the head, comp moved
# after gain sees a block ten times quieter, and the chain must equal pass, gain, comp in series, not pass, comp, gain
expect chain "chain 7 11 10 12" "resp 0"
for _ in $(seq 50); do has_ports 11 && ! has_ports 10 && break; sleep 0.1; done
walk_before=$(walk_of 7)
expect chain "chain 7 11 12 10" "resp 0"
walk_after=$(walk_of 7)
[ -n "$walk_before" ] && [ -n "$walk_after" ] && [ $((walk_after - walk_before)) = 1 ]
step $? "comp moved after gain in one walk: walk ${walk_before:-?} to ${walk_after:-?}"
wait_nodes 1; step $? "chain 7 11 12 10: one node, pass's ($(nodes))"
series 21 22 20
series 31 32 30
play p5b chain=11,11 series=21,20 other=31,30
same "p5b left: comp moved after gain" "$runtime/p5b.chain.l" "$runtime/p5b.series.l"
same "p5b right" "$runtime/p5b.chain.r" "$runtime/p5b.series.r"
differ "p5b: the order before the move (pass, comp, gain) is another output" "$runtime/p5b.chain.l" "$runtime/p5b.other.l"
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

# pass (tests/fake.clap) asks its thread-check in every process() and says once when it ran off the audio role: in its
# own client from that client's first cycle, and as a member on its head's thread, no instance ever did
ran=$(grep -c -x -F "org.omx-clap-host.test.passthrough process" "$runtime/fake.log" 2>/dev/null)
off=$(grep -c -x -F "org.omx-clap-host.test.passthrough process off the audio role" "$runtime/fake.log" 2>/dev/null)
[ "${ran:-0}" -gt 0 ] && [ "${off:-0}" = 0 ]
step $? "pass ran on the audio role from its first cycle, in its own client and in the chain: ${off:-0} of ${ran:-0} instances off it"

# a host that saw a guard broken says so in its log (the lock probe of tests/chain_host_sabotage.sh)
logged=$(grep -h -m 1 '^FAIL' "$runtime/chain.log" "$runtime/series.log" 2>/dev/null | head -1)
[ -z "$logged" ]; step $? "the hosts logged no FAIL line${logged:+: ${logged#FAIL }}"

echo "chain host: $failures failed"
printf 'CHAIN-HOST {"frames":%d,"latency_chain":"%s","latency_series":"%s","failures":%d}\n' "$frames" "$lat_chain" \
    "$lat_series" "$failures"
[ "$failures" -eq 0 ]
