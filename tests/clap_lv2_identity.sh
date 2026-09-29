#!/usr/bin/env bash
#
# The LV2 twin through mod-host and the CLAP twin through omx-clap-host, the
# same parameters, one deterministic input from the same cycles, both outputs
# compared bit for bit; then both bypassed and compared again, and the CLAP
# side against the input it passed through. Runs in a PipeWire of its own
# like jack_e2e.sh; exit 0 only when every comparison held.
#
# MOD_HOST:         the fork's mod-host (default ../wt-mod-host-clap/mod-host); a tree build has no
#                   rpath, so its directory is put on LD_LIBRARY_PATH for libmod-host-protocol.so.0
# CLAP_TEST_PLUGIN: omx-delay.clap (default ../openmixer/packages/omx-plugins/bin/omx-delay.clap)
# LV2_DIR:          the directory holding omx-delay.lv2 (default: the .clap's directory)
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# FRAMES:           frames compared (default 96000)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
mod_host=${MOD_HOST:-$root/../wt-mod-host-clap/mod-host}
plugin=${CLAP_TEST_PLUGIN:-$root/../openmixer/packages/omx-plugins/bin/omx-delay.clap}
lv2_dir=${LV2_DIR:-$(dirname "$plugin")}
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
frames=${FRAMES:-96000}
feeder=$here/jack_identity
lv2_uri=urn:openmixer:dpf:delay
clap_id=org.freemixer.openmixer.delay
port_lv2=5561
port_clap=5562
preroll=4

# LV2 symbol, CLAP id, value: the same settings on both twins
params="timeMs 0 5
feedback 1 0.3
mix 2 1.0
tone 3 0.3
pingpong 4 0"

if [ -z "${IDENTITY_INSIDE:-}" ]; then
    for f in "$mod_host" "$plugin" "$lv2_dir/omx-delay.lv2/manifest.ttl" "$host" "$feeder"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    IDENTITY_INSIDE=1 MOD_HOST=$(readlink -f "$mod_host") CLAP_TEST_PLUGIN=$(readlink -f "$plugin") \
        LV2_DIR=$(readlink -f "$lv2_dir") OMX_CLAP_HOST=$host FRAMES=$frames \
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

LV2_PATH=$lv2_dir LD_LIBRARY_PATH=$(dirname "$mod_host")${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} \
    "$mod_host" -n -p $port_lv2 >"$runtime/mod-host.log" 2>&1 &
pids+=($!)
"$host" -n -p $port_clap >"$runtime/clap-host.log" 2>&1 &
pids+=($!)
for _ in $(seq 100); do grep -q "ready" "$runtime/mod-host.log" && grep -q "ready" "$runtime/clap-host.log" && break; sleep 0.1; done
for h in mod-host clap-host; do
    if ! grep -q "ready" "$runtime/$h.log"; then
        step 1 "$h ready"
        sed 's/^/     '"$h"': /' "$runtime/$h.log"
        exit 2
    fi
done
step 0 "mod-host ready on port $port_lv2 (lv2 bundles from $lv2_dir)"
step 0 "omx-clap-host ready on port $port_clap"

exec 3<>"/dev/tcp/127.0.0.1/$port_lv2"
exec 4<>"/dev/tcp/127.0.0.1/$port_clap"
send() {
    printf '%s\0' "$2" >&"$1"
    IFS= read -r -d '' -t 5 reply <&"$1"
    echo "$reply"
}
expect() {
    local reply
    reply=$(send "$1" "$2")
    [ "$reply" = "$3" ]
    step $? "$4: $2 -> $reply"
}
same_value() {
    local a b
    a=$(send 3 "param_get 0 $1")
    b=$(send 4 "param_get 1 $2")
    [ "$a" = "$b" ] && [ "${a#resp 0 }" != "$a" ]
    step $? "lv2 $1 and clap $2 read back $a"
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
    [ "$(echo "$ins" | grep -c .)" = 2 ] && [ -n "$outs" ]
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
    local name=$1 ready
    mkfifo "$runtime/$name.in"
    "$feeder" "$frames" $preroll "$runtime/$name.source" "$runtime/$name.lv2" "$runtime/$name.clap" \
        <"$runtime/$name.in" >"$runtime/$name.out" 2>&1 &
    exec 5>"$runtime/$name.in"
    for _ in $(seq 50); do grep -q "^ready" "$runtime/$name.out" && break; sleep 0.1; done
    ready=$(grep "^ready" "$runtime/$name.out")
    [ -n "$ready" ]; step $? "$name: feeder $ready"
    link_effect effect_0 identity-rec:a
    link_effect effect_1 identity-rec:b
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

expect 3 "add $lv2_uri 0" "resp 0" "lv2"
expect 4 "add clap:$plugin#$clap_id 1" "resp 1" "clap"
while read -r symbol id value; do
    expect 3 "param_set 0 $symbol $value" "resp 0" "lv2"
    expect 4 "param_set 1 $id $value" "resp 0" "clap"
    same_value "$symbol" "$id"
done <<<"$params"

run_pass wet
! grep -q "peak_a=0 " "$runtime/wet.out"; step $? "wet: the lv2 twin produced signal"
! grep -q "peak_b=0 " "$runtime/wet.out"; step $? "wet: the clap twin produced signal"
compare "$runtime/wet.lv2" "$runtime/wet.clap" "wet: lv2 vs clap"

expect 3 "bypass 0 1" "resp 0" "lv2"
expect 4 "bypass 1 1" "resp 0" "clap"
expect 3 "param_get 0 :bypass" "resp 0 1.0000" "lv2"
expect 4 "param_get 1 :bypass" "resp 0 1.0000" "clap"
run_pass bypass
compare "$runtime/bypass.clap" "$runtime/bypass.source" "bypass: clap vs the input"
compare "$runtime/bypass.lv2" "$runtime/bypass.clap" "bypass: lv2 vs clap"

expect 3 "remove 0" "resp 0" "lv2"
expect 4 "remove 1" "resp 0" "clap"
expect 4 "quit" "resp 0" "clap"
send 3 "quit" >/dev/null

if [ "$failures" -eq 0 ]; then
    echo "identity ok"
    exit 0
fi
echo "identity FAILED ($failures)"
exit 1
