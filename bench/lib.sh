# SPDX-License-Identifier: GPL-3.0-or-later
#
# What bench/run-one.sh and bench/crash.sh share: the private graph, the probe, the five hosts behind one `send`, and
# the plugin lines each path takes. Sourced, with `here`, `root`, `path`, `rate`, `quantum`, `out` and `raw` set, as
# pid 1 of a private namespace (the scaffolding of tests/clap_lv2_identity.sh).

mod_host=${MOD_HOST:-$(command -v mod-host)}
clap_host=${OMX_CLAP_HOST:-$root/omx-clap-host}
hostd=${PLUGIN_HOSTD:-$(command -v plugin-hostd)}
jalv=${JALV:-$(command -v jalv)}
# the CPU idle hold of the run (#296, #105): default (the library's own, 10 us), off, 0 or a latency in microseconds. A
# host that links libomx-clap-core holds the device itself (omx-clap-host --idle-hold, inproc through its
# configuration, plugin-hostd's workers through the wrapper below); mod-host and jalv have no library, so the bench
# keeps the same request on the device for them (hold_external), which is what an operator's playbook would do
idle_hold=${BENCH_IDLE_HOLD:-default}
# the jalv path's -s (show the plugin's UI where it has one, through ui:showInterface: the plugin opens its own window
# on the display the bench inherits), BENCH_JALV_UI=1; off by default, so no window lands on the operator's screen
jalv_ui=${BENCH_JALV_UI:-0}
device=${OMX_CLAP_IDLE_HOLD_DEVICE:-/dev/cpu_dma_latency}
plugins=${PLUGINS:-$here/plugins.tsv}
probe=$root/build/bench/probe
inproc=$root/build/bench/inproc
bridge=$root/build/bench/omx-lv2-bridge.clap
fakes=$root/build/lv2/omx-lv2-fakes.lv2
pad_clap=$root/tests/fault-0.clap
pad_clap_id=org.openmixer.fault.0
pad_lv2=urn:omx:test:pad:stereo#0
port=5570

runtime=$(mktemp -d /tmp/omx-bench.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
cleanup() {
    exec 3>&- 5>&- 6<&- 7>&- 8<&- 2>/dev/null
    kill -TERM -- -1 2>/dev/null
    wait 2>/dev/null
    rm -rf "${runtime:?}"
}
trap cleanup EXIT
now_ns() { date +%s%N; }
ms() { awk -v a="$1" -v b="$2" 'BEGIN { printf "%.2f", (b - a) / 1000000 }'; }
rss_kb() { awk '/^VmRSS/ { print $2 }' "/proc/$1/status" 2>/dev/null || echo 0; }
cpu_ticks() { awk '{ print $14 + $15 }' "/proc/$1/stat" 2>/dev/null || echo 0; }
field() { sed -n "s/.*[ ]$1=\([^ ]*\).*/\1/p"; }

# the private graph at the run's rate and quantum, driven by PipeWire's dummy driver (no device in the namespace)
graph_start() {
    [ "$$" = 1 ] || die "not pid 1 of a private pid namespace"
    ip link set lo up
    pipewire >"$raw/pipewire.log" 2>&1 &
    pw_pid=$!
    for _ in $(seq 50); do [ -S "$runtime/pipewire-0" ] && break; sleep 0.1; done
    [ -S "$runtime/pipewire-0" ] || die "pipewire did not come up"
    pw-metadata -n settings 0 clock.allowed-rates "[ $rate ]" >/dev/null
    pw-metadata -n settings 0 clock.rate "$rate" >/dev/null
    pw-metadata -n settings 0 clock.force-rate "$rate" >/dev/null
    pw-metadata -n settings 0 clock.force-quantum "$quantum" >/dev/null
}

# the probe on fds 5 (commands) and 6 (answers): <noise inputs> <impulse inputs>; the impulse's reference link made
probe_start() {
    local line p_rate p_quantum
    mkfifo "$runtime/probe.in" "$runtime/probe.out"
    "$probe" "$1" "$2" <"$runtime/probe.in" >"$runtime/probe.out" 2>"$raw/probe.err" &
    exec 5>"$runtime/probe.in" 6<"$runtime/probe.out"
    read -r -t 10 line <&6 || die "probe did not start"
    read -r _ p_rate p_quantum <<<"$line"
    [ "$p_rate" = "$rate" ] && [ "$p_quantum" = "$quantum" ] || die "graph runs $p_rate/$p_quantum, not $rate/$quantum"
    pw-link bench-src:imp bench-rec:lat_0 || die "reference link"
}

# the probe's readings into $1
probe_stats() {
    local line
    echo stats >&5
    : >"$1"
    while IFS= read -r -t 5 line <&6; do
        [ "$line" = end ] && break
        echo "$line" >>"$1"
    done
}

probe_reset() {
    echo reset >&5
    read -r -t 5 _ <&6
}

# every plugin of the table this path hosts: names[] and adds[] (the add line without its instance number), pad
plugin_set() {
    local name cfile cid bundle uri a
    names=() adds=()
    lv2_bundles=$fakes
    while IFS=$'\t' read -r name cfile cid bundle uri; do
        case $name in '#'* | '') continue ;; esac
        case $path in
            clap | inproc-clap) [ "$cfile" = - ] && continue ;;
            *) [ "$bundle" = - ] && continue ;;
        esac
        [ "$bundle" != - ] && lv2_bundles=$lv2_bundles:$bundle
        a=$(add_line "$cfile" "$cid" "$bundle" "$uri")
        names+=("$name")
        adds+=("$a")
    done <"$plugins"
    case $path in
        clap | inproc-clap) pad=$(add_line "$pad_clap" "$pad_clap_id" - -) ;;
        *) pad=$(add_line - - "$fakes" "$pad_lv2") ;;
    esac
}

# the add line of one plugin for this path: <clap file> <clap id> <lv2 bundle> <lv2 uri>
add_line() {
    case $path in
        modhost | jalv) echo "add $4" ;;
        hostd) echo "add lv2:$4" ;;
        clap) echo "add clap:$1#$2" ;;
        adapter) echo "add clap:$bridge#$4" ;;
        inproc-clap) echo "add clap $1 $2" ;;
        inproc-lv2) echo "add lv2 $3 $4" ;;
        *) die "unknown path $path" ;;
    esac
}

# the hold as omx-clap-host's option: nothing for default
hold_opt() { [ "$idle_hold" = default ] || echo "--idle-hold=$idle_hold"; }

# the request a host with no library would make, kept by the bench for the run: nothing for default and off
hold_external() {
    case $idle_hold in default | off) return 0 ;; esac
    python3 -I -c 'import os, signal, struct, sys
fd = os.open(sys.argv[1], os.O_RDWR)
os.pwrite(fd, struct.pack("i", int(sys.argv[2])), 0)
signal.pause()' "$device" "$idle_hold" &
    holder_pid=$!
}

# what the device holds across every requester, or - when it cannot be read (the device is root's without the rule)
device_reads() {
    local v
    v=$(od -An -td4 "$device" 2>/dev/null | tr -d ' ')
    echo "${v:--}"
}

# the directories of the run's LV2 bundles, for a host that searches a path
lv2_dirs() {
    echo "$lv2_bundles" | tr ':' '\n' | xargs -n1 dirname | sort -u | paste -sd:
}

# the path's host, its pid in host_pid, ready for `send`; t_spawn and t_ready around it
host_start() {
    local line
    t_spawn=$(now_ns)
    case $path in
        modhost)
            hold_external
            LV2_PATH=$(dirname "$fakes"):/usr/lib64/lv2:/usr/lib/lv2 "$mod_host" -n -p $port >"$raw/host.log" 2>&1 &
            host_pid=$! ;;
        clap | adapter)
            # shellcheck disable=SC2046
            OMX_BENCH_LV2_BUNDLES=$lv2_bundles "$clap_host" -n $(hold_opt) -p $port >"$raw/host.log" 2>&1 &
            host_pid=$! ;;
        hostd)
            # the daemon, its LV2 worker omx-clap-host through the adapter (the lane's binary, with the hold as its
            # option, behind a wrapper the daemon spawns), every plugin in one pool worker; no pins, as the other
            # paths check none
            printf '#!/bin/sh\nexec "%s" %s "$@"\n' "$clap_host" "$(hold_opt)" >"$runtime/omx-clap-host"
            chmod 755 "$runtime/omx-clap-host"
            printf 'clap_host %s\nlv2_worker omx-clap-host\nlv2_path %s\nrequire_pins 0\nstate_root %s\n' \
                "$runtime/omx-clap-host" "$(lv2_dirs)" "$runtime" >"$runtime/hostd.conf"
            "$hostd" -n -p $port -c "$runtime/hostd.conf" >"$raw/host.log" 2>&1 &
            host_pid=$! ;;
        jalv)
            # one jalv process per plugin, as Zynthian runs its layers: spawned by send; no host before the first add
            hold_external
            : >"$runtime/jalv.pids"
            host_pid= ;;
        inproc-*)
            rm -f "$runtime/host.in" "$runtime/host.out"
            mkfifo "$runtime/host.in" "$runtime/host.out"
            [ "$idle_hold" = default ] || export OMX_BENCH_IDLE_HOLD=$idle_hold
            "$inproc" "$rate" <"$runtime/host.in" >"$runtime/host.out" 2>"$raw/host.log" &
            host_pid=$!
            exec 7>"$runtime/host.in" 8<"$runtime/host.out" ;;
    esac
    case $path in
        inproc-*)
            read -r -t 10 line <&8 && [ "$line" = ready ] || die "inproc did not start" ;;
        jalv) ;;
        *)
            for _ in $(seq 500); do grep -q ready "$raw/host.log" && break; sleep 0.01; done
            grep -q ready "$raw/host.log" || die "host not ready: $(tail -3 "$raw/host.log")"
            exec 3<>"/dev/tcp/127.0.0.1/$port" ;;
    esac
    t_ready=$(now_ns)
    case $path in
        hostd) added "$(send "policy_set lv2 pool:bench")" 0 || die "policy_set: $(send "policy_set lv2 pool:bench")" ;;
    esac
}

# the host's processes: one; every jalv (send records them in a file: it runs in a subshell); the daemon and its workers
host_pids() {
    case $path in
        jalv) cat "$runtime/jalv.pids" 2>/dev/null ;;
        hostd) echo "$host_pid"; pgrep -P "$host_pid" 2>/dev/null ;;
        *) echo "$host_pid" ;;
    esac
}
host_rss_kb() { local p t=0; for p in $(host_pids); do t=$((t + $(rss_kb "$p"))); done; echo "$t"; }
host_cpu_ticks() { local p t=0; for p in $(host_pids); do t=$((t + $(cpu_ticks "$p"))); done; echo "$t"; }
host_alive() { local p; for p in $(host_pids); do kill -0 "$p" 2>/dev/null || return 1; done; return 0; }

send() {
    local reply uri k p
    case $path in
        inproc-*)
            echo "$1" >&7
            IFS= read -r -t 30 reply <&8 ;;
        jalv)
            # "add <uri> <k>": jalv -s -n effect_<k> <uri>, ready when its client's ports are in the graph; "quit" ends them
            case $1 in
                add\ *)
                    uri=$(echo "$1" | awk '{ print $2 }'); k=$(echo "$1" | awk '{ print $3 }')
                    # shellcheck disable=SC2046
                    LV2_PATH=$(dirname "$fakes"):/usr/lib64/lv2:/usr/lib/lv2 "$jalv" -i $([ "$jalv_ui" = 0 ] || echo -s) -n "effect_$k" "$uri" </dev/null >"$raw/jalv-$k.log" 2>&1 &
                    p=$!
                    echo "$p" >>"$runtime/jalv.pids"
                    reply="resp -102"
                    for _ in $(seq 200); do
                        if python3 -I "$here/stats.py" ports "effect_$k" | grep -q .; then reply="resp $k"; break; fi
                        kill -0 "$p" 2>/dev/null || break
                        sleep 0.05
                    done ;;
                quit)
                    for p in $(host_pids); do kill "$p" 2>/dev/null; done
                    reply="resp 0" ;;
                *) reply="resp -1" ;;
            esac ;;
        *)
            printf '%s\0' "$1" >&3
            IFS= read -r -d '' -t 30 reply <&3 ;;
    esac
    echo "$reply"
}

# what the host's threads run at: name, policy (0 other, 1 fifo, 2 rr), rt priority
host_threads() {
    local p
    for p in $(host_pids); do
        for t in /proc/"$p"/task/*; do
            echo "$(cat "$t/comm") policy=$(awk '{ print $41 }' "$t/stat") rtprio=$(awk '{ print $40 }' "$t/stat")"
        done
    done
}

# link a racked effect: its first two audio inputs from $2 and $3, its first audio output to $4
link_fx() {
    local node=$1 a=$2 b=$3 sink=$4 ins outs
    for _ in $(seq 100); do
        ins=$(python3 -I "$here/stats.py" ports "$node" | awk '$1 == "in" { print $2 }' | head -2)
        outs=$(python3 -I "$here/stats.py" ports "$node" | awk '$1 == "out" { print $2 }' | head -1)
        [ "$(echo "$ins" | grep -c .)" = 2 ] && [ -n "$outs" ] && break
        sleep 0.02
    done
    [ "$(echo "$ins" | grep -c .)" = 2 ] && [ -n "$outs" ] || return 1
    pw-link "$a" "$(echo "$ins" | sed -n 1p)" && pw-link "$b" "$(echo "$ins" | sed -n 2p)" && pw-link "$outs" "$sink"
}

# `resp <n>` for an add of instance <n>
added() {
    [ "${1%% *}" = resp ] && [ "$(echo "$1" | awk '{ print $2 }')" = "$2" ]
}
