#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A chain of plugins in one JACK client against the same plugins in one client each, linked in series, bit for bit
# (#302): three omx-clap-host processes from the same cycles of one deterministic input (tests/jack_identity), left
# and right recorded,
#
#   series   one client per plugin, effect_0 .. effect_<N-1>, each output linked to the next one's input
#   control  the same again, effect_200 .. : two arms that do not differ, so a plugin that is not deterministic
#            between two instances of itself shows here and is not taken for the chain's difference
#   chain    effect_100 .., declared one chain by "chain 100 100 101 .." after the adds (docs/chain.md): one client,
#            effect_100's, the members in its cycle
#
# then control against series and chain against series, every pair cmp-equal or the first differing sample said, and
# the latency each arm's last output reports to the graph equal. With CHAIN_KIND=lv2 the plugins are added by their
# LV2 URI through the adapter, every host's LV2_PATH one directory holding the plugins' bundles alone; then a host whose
# LV2_PATH holds the first bundle alone (what plugin-hostd's per-plugin narrowing gives a worker) tries the chain's
# second member. CHAIN_PINS=1: every add follows a pin_expect of its layout, and the chain's first add each a wrong pin
# first, which must be refused (the pin is read off that refusal: the LV2 layout has no tool of its own).
#
# Runs in a PipeWire of its own (a private user, network and pid namespace; where the container refuses a /proc of its
# own, a session of its own instead). The last line is a CHAIN-IDENTITY json; exit 0 only when every step held.
#
# PLUGINS:      the plugin table (default bench/plugins.tsv); PLUGIN_NAMES: the rows by name, in order (default every
#               row the kind can take); PLUGIN_ROOT: a prefix for the table's absolute paths (an unpacked tree); a
#               relative path is this tree's (tests/chain_fixture.tsv, what CI runs)
# CHAIN_KIND:   clap (default) or lv2
# CHAIN_PINS:   1 pins every add (default 0)
# FRAMES:       frames compared (default 96000); SETTLE: seconds after the links before the input plays (default 2)
# OMX_CLAP_HOST: the binary (default ./omx-clap-host)

set -u

here=$(cd "$(dirname "$0")" && pwd)
. "${PLUGIN_HOSTD_TESTS_LIB:-$here/../../plugin-hostd/tests/lib}/protocol.sh"
root=$(dirname "$here")
plugins=${PLUGINS:-$root/bench/plugins.tsv}
kind=${CHAIN_KIND:-clap}
pick=${PLUGIN_NAMES:-}
proot=${PLUGIN_ROOT:-}
pins=${CHAIN_PINS:-0}
frames=${FRAMES:-96000}
settle=${SETTLE:-2}
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
feeder=$here/jack_identity
latency_probe=$here/jack_latency_probe
scheme=omx-layout/1
preroll=4

if [ -z "${IDENTITY_INSIDE:-}" ]; then
    for f in "$host" "$feeder" "$latency_probe" "$plugins"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    export IDENTITY_INSIDE=1 PLUGINS=$plugins OMX_CLAP_HOST=$(readlink -f "$host")
    if unshare --user --map-root-user --net --pid --fork --mount-proc true 2>/dev/null; then
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
    fi
    # a container whose /proc is masked refuses a /proc of its own: no pid namespace, the run its own session, and
    # its cleanup that session's processes alone
    IDENTITY_SESSION=1 exec unshare --user --map-root-user --net setsid -w "$0" "$@"
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

runtime=$(mktemp -d /tmp/omx-chain-identity.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
pids=()
cleanup() {
    exec 3>&- 4>&- 5>&- 6>&- 7>&- 2>/dev/null
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    if [ -n "${IDENTITY_SESSION:-}" ]; then
        trap '' TERM
        kill -TERM -- -$$ 2>/dev/null
    else
        kill -TERM -- -1 2>/dev/null
    fi
    wait 2>/dev/null
    rm -rf "${runtime:?}"
}
trap cleanup EXIT
if [ -n "${IDENTITY_SESSION:-}" ]; then
    [ "$(ps -o pgid= -p $$ | tr -d ' ')" = $$ ]; step $? "a session of its own (the container refuses a pid namespace's /proc)"
else
    [ "$$" = 1 ]; step $? "pid 1 of a private pid namespace"
fi

# the rows: the add line of each, without its instance; the LV2 bundles in one directory of their own
names=() adds=() bundles=()
mkdir -p "$runtime/world" "$runtime/world-one"
while IFS=$'\t' read -r name cfile cid bundle uri; do
    case $name in '#'* | '') continue ;; esac
    [ -z "$pick" ] || case " $pick " in *" $name "*) ;; *) continue ;; esac
    case $kind in
        clap) [ "$cfile" = - ] && continue
              case $cfile in /*) cfile=$proot$cfile ;; *) cfile=$root/$cfile ;; esac
              a="add clap:$cfile#$cid" ;;
        lv2) [ "$bundle" = - ] && continue; a="add lv2:$uri"; bundles+=("$proot$bundle")
             ln -sfn "$proot$bundle" "$runtime/world/$(basename "$bundle")" ;;
        *) echo "CHAIN_KIND is clap or lv2" >&2; exit 2 ;;
    esac
    names+=("$name") adds+=("$a")
done <"$plugins"
if [ -n "$pick" ]; then
    # PLUGIN_NAMES' order, not the table's
    o_names=() o_adds=()
    for want in $pick; do
        for i in "${!names[@]}"; do [ "${names[$i]}" = "$want" ] && o_names+=("$want") o_adds+=("${adds[$i]}"); done
    done
    names=("${o_names[@]}") adds=("${o_adds[@]}")
fi
n=${#names[@]}
[ "$n" -ge 2 ]; step $? "$n $kind plugins: ${names[*]}"
[ "$n" -ge 2 ] || exit 1
[ "$kind" = lv2 ] && ln -sfn "${bundles[0]}" "$runtime/world-one/$(basename "${bundles[0]}")"

ip link set lo up
pipewire >"$runtime/pw.log" 2>&1 &
pids+=($!)
for _ in $(seq 50); do [ -S "$runtime/pipewire-0" ] && break; sleep 0.1; done
[ -S "$runtime/pipewire-0" ] || { echo "pipewire did not come up" >&2; cat "$runtime/pw.log" >&2; exit 2; }
step 0 "private pipewire on $runtime"

# the arms: their port, first instance and fd
declare -A port=([series]=5581 [control]=5582 [chain]=5583 [narrow]=5584) base=([series]=0 [control]=200 [chain]=100 [narrow]=300)
declare -A fd=([series]=3 [control]=4 [chain]=5 [narrow]=6)
start_host() {
    local arm=$1 world=$2
    LV2_PATH="$world" "$OMX_CLAP_HOST" -n -p "${port[$arm]}" >"$runtime/$arm.log" 2>&1 &
    pids+=($!)
    for _ in $(seq 100); do grep -q ready "$runtime/$arm.log" && break; sleep 0.1; done
    grep -q ready "$runtime/$arm.log"; step $? "$arm host ready"
    eval "exec ${fd[$arm]}<>/dev/tcp/127.0.0.1/${port[$arm]}"
}
send() { protocol_send "${fd[$1]}" 30 "$2"; }
expect() {
    local reply
    reply=$(send "$1" "$2")
    [ "$reply" = "$3" ]
    step $? "$1: $2 -> $reply"
}

for arm in series control chain; do start_host "$arm" "$runtime/world"; done

# every add, a pin first with CHAIN_PINS: the chain arm's taken off a wrong pin's refusal, then given to the others
declare -A pin
zero=$(printf '0%.0s' $(seq 64))
for arm in chain series control; do
    for k in $(seq 0 $((n - 1))); do
        i=$((${base[$arm]} + k))
        if [ "$pins" = 1 ]; then
            if [ "$arm" = chain ]; then
                expect chain "pin_expect $i $scheme:$zero" "resp 0"
                expect chain "${adds[$k]} $i" "resp -510"
                pin[$k]=$(sed -n "s|.*: layout \($scheme:[0-9a-f]*\), pinned $scheme:$zero$|\1|p" "$runtime/chain.log" | tail -1)
                [ -n "${pin[$k]}" ]; step $? "chain: ${names[$k]} refused its wrong pin, its layout ${pin[$k]}"
            fi
            expect "$arm" "pin_expect $i ${pin[$k]}" "resp 0"
        fi
        expect "$arm" "${adds[$k]} $i" "resp $i"
    done
done
# the chain arm's instances one chain, in order, as plugin-hostd's line declares it
expect chain "chain ${base[chain]} $(seq -s ' ' "${base[chain]}" $((${base[chain]} + n - 1)))" "resp 0"

if [ "$kind" = lv2 ]; then
    # the chain in a world of the first plugin's bundle alone, as plugin-hostd narrows a worker's LV2_PATH
    start_host narrow "$runtime/world-one"
    expect narrow "${adds[0]} ${base[narrow]}" "resp ${base[narrow]}"
    reply=$(send narrow "${adds[1]} $((${base[narrow]} + 1))")
    [ "$reply" = "resp -101" ]
    step $? "narrow: ${names[1]} as the chain's second member in a world of ${names[0]}'s bundle alone -> $reply (not found)"
    narrow_reply=$reply
    send narrow "remove -1" >/dev/null
fi

# the graph: the input into each arm's first plugin, each output into the next plugin's input, the last into the recorder
link() { timeout 10 pw-link "$1" "$2" 2>/dev/null; step $? "link $1 -> $2"; }
has_ports() { timeout 10 pw-link -"$2" 2>/dev/null | grep -q "^$1:${3}_2$"; }
for k in $(seq 0 $((n - 1))); do
    has_ports "effect_$k" i in && has_ports "effect_$k" o out; step $? "effect_$k is stereo: in_1, in_2, out_1, out_2"
done
mkfifo "$runtime/feed.in"
"$feeder" "$frames" $preroll "$runtime/source" "$runtime/series.l" "$runtime/series.r" "$runtime/control.l" \
    "$runtime/control.r" "$runtime/chain.l" "$runtime/chain.r" <"$runtime/feed.in" >"$runtime/feed.out" 2>&1 &
feeder_pid=$!
exec 7>"$runtime/feed.in"
for _ in $(seq 50); do grep -q "^ready" "$runtime/feed.out" && break; sleep 0.1; done
ready=$(grep "^ready" "$runtime/feed.out")
[ -n "$ready" ]; step $? "feeder $ready"
declare -A rec=([series]="a b" [control]="c d" [chain]="e f")
for arm in series control; do
    b=${base[$arm]}
    link identity-src:out "effect_$b:in_1"
    link identity-src:out "effect_$b:in_2"
    for k in $(seq 1 $((n - 1))); do
        link "effect_$((b + k - 1)):out_1" "effect_$((b + k)):in_1"
        link "effect_$((b + k - 1)):out_2" "effect_$((b + k)):in_2"
    done
    set -- ${rec[$arm]}
    link "effect_$((b + n - 1)):out_1" "identity-rec:$1"
    link "effect_$((b + n - 1)):out_2" "identity-rec:$2"
done
b=${base[chain]}
[ "$(timeout 10 pw-link -o 2>/dev/null | grep -c "^effect_\(10[0-9]\|1[1-9][0-9]\):")" = 2 ]
step $? "the chain is one client: effect_$b's two outputs, no member's"
link identity-src:out "effect_$b:in_1"
link identity-src:out "effect_$b:in_2"
link "effect_$b:out_1" identity-rec:e
link "effect_$b:out_2" identity-rec:f

# a latency an LV2 plugin reports after its first activate is taken by a restart, crossed with a fade: play after it
sleep "$settle"
lat_series=$("$latency_probe" "effect_$((${base[series]} + n - 1)):out_1" capture 2>/dev/null)
lat_control=$("$latency_probe" "effect_$((${base[control]} + n - 1)):out_1" capture 2>/dev/null)
lat_chain=$("$latency_probe" "effect_${base[chain]}:out_1" capture 2>/dev/null)
[ -n "$lat_series" ] && [ "$lat_series" = "$lat_chain" ] && [ "$lat_series" = "$lat_control" ]
step $? "latency to the last output: series $lat_series, control $lat_control, chain $lat_chain (min max frames)"
grep -h "latency [1-9]" "$runtime/series.log" "$runtime/chain.log" | sed 's/^/     /'

echo go >&7
timeout 60 tail --pid=$feeder_pid -f /dev/null
wait $feeder_pid
step $? "feeder: $(grep '^done' "$runtime/feed.out" || tail -1 "$runtime/feed.out")"
! grep -q "peak_source=0 " "$runtime/feed.out"; step $? "the source landed"
for r in a b c d e f; do
    ! grep -q " peak_$r=0\( \|$\)" "$runtime/feed.out" || { step 1 "recorder $r heard nothing"; }
done

sample_at() { od -A n -t f4 -j "$(($2 * 4))" -N 4 "$1" | tr -d ' '; }
declare -A verdict
compare() {
    local a=$1 b=$2 what=$3 out byte index
    out=$(cmp "$a" "$b" 2>&1)
    if [ -z "$out" ]; then
        verdict[$what]=identical
        step 0 "$what: $(($(stat -c %s "$a") / 4)) samples bit-identical"
        return 0
    fi
    byte=$(echo "$out" | sed -n 's/.*byte \([0-9]*\).*/\1/p')
    index=$(((byte - 1) / 4))
    # how far apart, beside where: the difference's RMS against the first recording's, in dB
    rel=$(python3 -I -c 'import array, math, sys
a, b = (array.array("f", open(f, "rb").read()) for f in sys.argv[1:3])
d = math.sqrt(sum((x - y) ** 2 for x, y in zip(a, b)) / len(a)); r = math.sqrt(sum(x * x for x in a) / len(a))
print("%.1f" % (20 * math.log10(d / r)) if d and r else "-inf")' "$a" "$b")
    verdict[$what]="first difference at sample $index ($(sample_at "$a" "$index") vs $(sample_at "$b" "$index")), difference ${rel} dB"
    step 1 "$what: ${verdict[$what]} of $(($(stat -c %s "$a") / 4))"
    return 1
}
compare "$runtime/series.l" "$runtime/control.l" "control left"
compare "$runtime/series.r" "$runtime/control.r" "control right"
compare "$runtime/series.l" "$runtime/chain.l" "chain left"
compare "$runtime/series.r" "$runtime/chain.r" "chain right"

# the chain's members leave it one by one and the head goes last, the host alive and answering after each
for k in $(seq $((n - 1)) -1 1); do expect chain "remove $((${base[chain]} + k))" "resp 0"; done
expect chain "remove ${base[chain]}" "resp 0"
expect series "remove -1" "resp 0"

echo "chain identity, $n $kind plugins: $failures failed"
printf 'CHAIN-IDENTITY {"kind":"%s","plugins":%d,"names":"%s","pins":%s,"frames":%d,"control_left":"%s","control_right":"%s","chain_left":"%s","chain_right":"%s","latency_series":"%s","latency_chain":"%s","narrow_second_member":"%s","failures":%d}\n' \
    "$kind" "$n" "${names[*]}" "$pins" "$frames" "${verdict[control left]:-}" "${verdict[control right]:-}" \
    "${verdict[chain left]:-}" "${verdict[chain right]:-}" "$lat_series" "$lat_chain" "${narrow_reply:-}" "$failures"
[ "$failures" -eq 0 ]
