#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# One LV2 plugin run four ways from the same cycles of one deterministic input,
# the outputs compared bit for bit with the plugin run through omx-clap-host's
# own LV2 path (add lv2:<uri>):
#
#   modhost  the LV2 plugin through mod-host
#   clap     the plugin's CLAP twin through omx-clap-host (add clap:<path>#<id>)
#   lv2      the LV2 plugin through omx-clap-host and the LV2 adapter it links
#   inproc   the LV2 plugin through the adapter in process, as the engine runs
#            it: libomx-clap-core's stage in a PipeWire filter (build/bench/inproc)
#
# Then every arm bypassed and compared with the input it passes: the lv2 arm by
# its lv2:enabled port, as mod-host's clients bypass, the others by their
# bypass verb. Runs in a PipeWire of its own like jack_e2e.sh; exit 0 only when
# every comparison held.
#
# IDENTITY_FIXTURE=1 runs the fixture bundle of this tree (build/lv2,
# omx-host-fixture.lv2) with no CLAP twin: what CI runs. Otherwise omx-delay,
# its LV2 bundle against its CLAP twin, from the omx-plugins packages.
#
# IDENTITY_ARMS:    the arms (default: modhost clap lv2 inproc; with IDENTITY_FIXTURE: modhost lv2 inproc); lv2 always runs
# MOD_HOST:         mod-host (default: mod-host on PATH); a tree build has no rpath, so its directory is put on
#                   LD_LIBRARY_PATH for libmod-host-protocol.so.0
# CLAP_TEST_PLUGIN: the CLAP twin (default /usr/lib/clap/omx-delay.clap)
# CLAP_ID:          its plugin id (default org.openmixer.delay)
# LV2_DIR:          the directory holding the LV2 bundle, the LV2_PATH of every arm (default /usr/lib64/lv2)
# LV2_BUNDLE:       the bundle's directory name in LV2_DIR (default omx-delay.lv2)
# LV2_URI:          the plugin's URI (default urn:openmixer:delay)
# IDENTITY_PARAMS:  "<lv2 symbol> <the CLAP twin's id> <the LV2 port index> <value>" per line, the settings every arm
#                   takes: mod-host by the symbol, the clap arm by the twin's id, the lv2 arm by the symbol (and reads
#                   it back by the port index, the adapter's CLAP id), inproc by the port index
# BYPASS_SYMBOL:    the lv2:enabled port's symbol (default enabled)
# SETTLE_LATENCY:   the latency the plugin reports once it runs, when it reports one after its first activate: the
#                   adapter holds a new latency, then restarts the plugin (docs/lv2-compatibility.md), and the first
#                   pass waits for that restart before it plays (default: the fixture's 7, none for omx-delay)
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# INPROC:           the in-process host (default ./build/bench/inproc)
# FRAMES:           frames compared (default 96000)

set -u

here=$(cd "$(dirname "$0")" && pwd)
. "${PLUGIN_HOSTD_TESTS_LIB:-$here/../../plugin-hostd/tests/lib}/protocol.sh"
root=$(dirname "$here")
if [ -n "${IDENTITY_FIXTURE:-}" ]; then
    arms=${IDENTITY_ARMS:-modhost lv2 inproc}
    lv2_dir=${LV2_DIR:-$root/build/lv2}
    bundle=${LV2_BUNDLE:-omx-host-fixture.lv2}
    lv2_uri=${LV2_URI:-urn:openmixer:test:host-fixture}
    plugin=${CLAP_TEST_PLUGIN:-}
    clap_id=${CLAP_ID:-}
    params=${IDENTITY_PARAMS:-"gain_db - 3 -12.5"}
    settle=${SETTLE_LATENCY:-7}
else
    arms=${IDENTITY_ARMS:-modhost clap lv2 inproc}
    lv2_dir=${LV2_DIR:-/usr/lib64/lv2}
    bundle=${LV2_BUNDLE:-omx-delay.lv2}
    lv2_uri=${LV2_URI:-urn:openmixer:delay}
    plugin=${CLAP_TEST_PLUGIN:-/usr/lib/clap/omx-delay.clap}
    settle=${SETTLE_LATENCY:-}
    clap_id=${CLAP_ID:-org.openmixer.delay}
    # LV2 symbol, the CLAP twin's id, the LV2 port index, value: the same settings on every arm
    params=${IDENTITY_PARAMS:-"fxDelayTime 0 4 5
fxDelayFeedback 1 5 0.3
mix 3 7 1.0
tone 2 6 0.3
pingpong 4 8 0"}
fi
mod_host=${MOD_HOST:-$(command -v mod-host || echo mod-host)}
bypass_symbol=${BYPASS_SYMBOL:-enabled}
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
inproc=${INPROC:-$root/build/bench/inproc}
frames=${FRAMES:-96000}
feeder=$here/jack_identity
port_lv2=5561
port_clap=5562
preroll=4

has() { case " $arms " in *" $1 "*) return 0;; esac; return 1; }

if [ -z "${IDENTITY_INSIDE:-}" ]; then
    has lv2 || { echo "IDENTITY_ARMS must name lv2: every arm is compared with it" >&2; exit 2; }
    need=("$lv2_dir/$bundle/manifest.ttl" "$host" "$feeder")
    has modhost && need+=("$mod_host")
    has clap && need+=("$plugin")
    has inproc && need+=("$inproc")
    for f in "${need[@]}"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    IDENTITY_INSIDE=1 IDENTITY_ARMS="$arms" SETTLE_LATENCY=$settle MOD_HOST=$(readlink -f "$mod_host") CLAP_TEST_PLUGIN=${plugin:+$(readlink -f "$plugin")} \
        LV2_DIR=$(readlink -f "$lv2_dir") OMX_CLAP_HOST=$host INPROC=$inproc FRAMES=$frames \
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
fi

failures=0
step() {
    if [ "$1" -eq 0 ]; then
        echo "ok   $2"
    else
        echo "FAIL $2"
        failures=$((failures + 1))
        ps -o pid,stat,wchan:16,etimes,cmd --no-headers 2>/dev/null | sed 's/^/     /'
    fi
}
run() {
    timeout 10 "$@"
}

runtime=$(mktemp -d /tmp/omx-clap-host-identity.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
pids=()
cleanup() {
    exec 3>&- 4>&- 6>&- 7>&- 2>/dev/null
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

# the instance and the recorder of each arm
declare -A instance=([modhost]=0 [clap]=1 [lv2]=2 [inproc]=3) recorder=()
letters=(a b c d)
order=()
for a in modhost clap lv2 inproc; do
    has "$a" || continue
    recorder[$a]=${letters[${#order[@]}]}
    order+=("$a")
done
step 0 "arms: ${order[*]}; the plugin $lv2_uri from $lv2_dir/$bundle"

if has modhost; then
    LV2_PATH=$lv2_dir LD_LIBRARY_PATH=$(dirname "$mod_host")${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
        "$mod_host" -n -p $port_lv2 >"$runtime/mod-host.log" 2>&1 &
    pids+=($!)
fi
LV2_PATH=$lv2_dir "$host" -n -p $port_clap >"$runtime/clap-host.log" 2>&1 &
pids+=($!)
logs=(clap-host)
has modhost && logs+=(mod-host)
for _ in $(seq 100); do
    up=1
    for h in "${logs[@]}"; do grep -q "ready" "$runtime/$h.log" || up=0; done
    [ $up = 1 ] && break
    sleep 0.1
done
for h in "${logs[@]}"; do
    if ! grep -q "ready" "$runtime/$h.log"; then
        step 1 "$h ready"
        sed 's/^/     '"$h"': /' "$runtime/$h.log"
        exit 2
    fi
    step 0 "$h ready"
done
has modhost && exec 3<>"/dev/tcp/127.0.0.1/$port_lv2"
exec 4<>"/dev/tcp/127.0.0.1/$port_clap"

if has inproc; then
    rate=$(pw-metadata -n settings 0 clock.rate 2>/dev/null | sed -n "s/.*value:'\([0-9]*\)'.*/\1/p" | head -1)
    [ -n "$rate" ] || rate=48000
    coproc INPROC { OMX_BENCH_GUARDS=off LV2_PATH=$lv2_dir exec "$inproc" "$rate" 2>"$runtime/inproc.log"; }
    pids+=($INPROC_PID)
    # a coprocess's own descriptors are not seen in a pipeline's subshell: 6 and 7 are
    exec 6>&"${INPROC[1]}" 7<&"${INPROC[0]}"
    IFS= read -r -t 5 line <&7
    [ "$line" = ready ]; step $? "inproc ready at $rate Hz, omx-clap-host's configuration (OMX_BENCH_GUARDS=off)"
fi

# a command to an arm, its reply
send() {
    local arm=$1 cmd=$2 reply
    case $arm in
        modhost) reply=$(protocol_send 3 5 "$cmd");;
        clap|lv2) reply=$(protocol_send 4 5 "$cmd");;
        inproc) echo "$cmd" >&6; IFS= read -r -t 5 reply <&7;;
    esac
    echo "$reply"
}
expect() {
    local reply
    reply=$(send "$1" "$2")
    [ "$reply" = "$3" ]
    step $? "$1: $2 -> $reply"
}
link() {
    run pw-link "$1" "$2" 2>/dev/null
    step $? "link $1 -> $2"
}
# an effect's ports as the graph names them (mod-host uses the LV2 symbols), in registration order
effect_ports() {
    run pw-link "-$2" 2>/dev/null | grep "^$1:" | head -2
}
link_effect() {
    local client=$1 sink=$2 ins outs
    ins=$(effect_ports "$client" i)
    outs=$(effect_ports "$client" o)
    [ -n "$ins" ] && [ -n "$outs" ]
    step $? "$client ports: $(echo $ins) / $(echo $outs)"
    for p in $ins; do link identity-src:out "$p"; done
    link "$(echo "$outs" | head -1)" "$sink"
}
sample_at() {
    od -A n -t f4 -j "$(( $2 * 4 ))" -N 4 "$1" | tr -d ' '
}
compare() {
    local a=$1 b=$2 what=$3 out byte index
    out=$(cmp "$a" "$b" 2>&1)
    if [ -z "$out" ]; then
        step 0 "$what: $(( $(stat -c %s "$a") / 4 )) samples bit-identical"
        return
    fi
    byte=$(echo "$out" | sed -n 's/.*byte \([0-9]*\).*/\1/p')
    index=$(( (byte - 1) / 4 ))
    step 1 "$what: first difference at sample $index of $(( $(stat -c %s "$a") / 4 )) ($(sample_at "$a" "$index") vs $(sample_at "$b" "$index"))"
}
run_pass() {
    local name=$1 ready a files=()
    for a in "${order[@]}"; do files+=("$runtime/$name.$a"); done
    mkfifo "$runtime/$name.in"
    "$feeder" "$frames" $preroll "$runtime/$name.source" "${files[@]}" <"$runtime/$name.in" >"$runtime/$name.out" 2>&1 &
    exec 5>"$runtime/$name.in"
    for _ in $(seq 50); do grep -q "^ready" "$runtime/$name.out" && break; sleep 0.1; done
    ready=$(grep "^ready" "$runtime/$name.out")
    [ -n "$ready" ]; step $? "$name: feeder $ready"
    for a in "${order[@]}"; do link_effect "effect_${instance[$a]}" "identity-rec:${recorder[$a]}"; done
    # the latency the adapter reports late is taken by a restart, which the stage crosses with a fade: play after it.
    # omx-clap-host says it on stderr; inproc holds a new latency for 100 ms to its 250 and runs from the links at
    # the latest, so it has restarted by then too, and half a second more is slack
    if [ -n "$settle" ]; then
        for _ in $(seq 50); do grep -q "^effect_2: latency $settle frames" "$runtime/clap-host.log" && break; sleep 0.1; done
        grep -q "^effect_2: latency $settle frames" "$runtime/clap-host.log"; step $? "$name: the lv2 arm has taken its $settle frames of latency"
        sleep 0.5
    fi
    # the in-process stage is published by its first cycle, which a linked filter gets, and takes its writes from then
    if has inproc; then
        for _ in $(seq 50); do send inproc "stats 3" | grep -q "published=1" && break; sleep 0.1; done
        send inproc "stats 3" | grep -q "published=1"; step $? "$name: the inproc stage is published"
    fi
    echo go >&5
    run tail --pid=$! -f /dev/null
    wait $!
    step $? "$name: $(grep '^done' "$runtime/$name.out" || tail -1 "$runtime/$name.out")"
    exec 5>&-
    if grep -q "peak_source=0 " "$runtime/$name.out"; then
        step 1 "$name: the source played nothing"
    else
        step 0 "$name: the source landed"
    fi
}

has modhost && expect modhost "add $lv2_uri 0" "resp 0"
has clap && expect clap "add clap:$plugin#$clap_id 1" "resp 1"
expect lv2 "add lv2:$lv2_uri 2" "resp 2"
has inproc && expect inproc "add lv2 $lv2_dir/$bundle $lv2_uri 3" "resp 3"
while read -r symbol id index value; do
    has modhost && expect modhost "param_set 0 $symbol $value" "resp 0"
    has clap && expect clap "param_set 1 $id $value" "resp 0"
    expect lv2 "param_set 2 $symbol $value" "resp 0"
    has inproc && expect inproc "param_set 3 $index $value" "resp 0"
    # the value each host holds, read back by the name each takes
    want=$(send lv2 "param_get 2 $symbol")
    [ "${want#resp 0 }" != "$want" ]; step $? "lv2: param_get 2 $symbol -> $want"
    [ "$(send lv2 "param_get 2 $index")" = "$want" ]; step $? "lv2: param_get 2 $index, the port index the adapter makes its CLAP id, reads the same"
    if has modhost; then [ "$(send modhost "param_get 0 $symbol")" = "$want" ]; step $? "modhost: param_get 0 $symbol reads the same"; fi
    if has clap; then [ "$(send clap "param_get 1 $id")" = "$want" ]; step $? "clap: param_get 1 $id reads the same"; fi
done <<<"$params"

run_pass wet
for a in "${order[@]}"; do
    ! grep -q "peak_${recorder[$a]}=0\( \|$\)" "$runtime/wet.out"; step $? "wet: the $a arm produced signal"
done
for a in "${order[@]}"; do
    [ "$a" = lv2 ] || compare "$runtime/wet.lv2" "$runtime/wet.$a" "wet: lv2 vs $a"
done

has modhost && expect modhost "bypass 0 1" "resp 0"
has clap && expect clap "bypass 1 1" "resp 0"
expect lv2 "param_set 2 $bypass_symbol 0" "resp 0"
has inproc && expect inproc "bypass 3 1" "resp 0"
has modhost && expect modhost "param_get 0 :bypass" "resp 0 1.0000"
has clap && expect clap "param_get 1 :bypass" "resp 0 1.0000"
expect lv2 "param_get 2 :bypass" "resp 0 1.0000"
expect lv2 "param_get 2 $bypass_symbol" "resp 0 0.0000"
run_pass bypass
for a in "${order[@]}"; do
    compare "$runtime/bypass.source" "$runtime/bypass.$a" "bypass: the input vs $a"
done

has modhost && expect modhost "remove 0" "resp 0"
has clap && expect clap "remove 1" "resp 0"
expect lv2 "remove 2" "resp 0"
[ -z "$(run pw-link -i 2>/dev/null | grep '^effect_2:')$(run pw-link -o 2>/dev/null | grep '^effect_2:')" ]
step $? "lv2: remove leaves no effect_2 port"
has inproc && expect inproc "remove 3" "resp 0"
expect lv2 "quit" "resp 0"
has modhost && send modhost "quit" >/dev/null
has inproc && send inproc "quit" >/dev/null

if [ "$failures" -eq 0 ]; then
    echo "identity ok"
    exit 0
fi
echo "identity FAILED ($failures)"
exit 1
