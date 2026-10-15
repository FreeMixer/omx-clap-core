#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The crash drill of the hosting bench, one private PipeWire per path at CRASH_RATE / CRASH_QUANTUM (48000 / 128):
#
#   kill    the host process is SIGKILLed while it runs omx-comp (for the in-process paths the host process IS the
#           engine): does the graph keep cycling, the longest gap the probe saw, the driver's xruns, how long until the
#           victim's output is silence, and how long a respawned host takes to load the plugin again and sound
#   crash   (CLAP paths) tests/fault-14.clap racked beside a healthy omx-comp in the same host, then the write that
#           makes it abort(): does the host die, and does the healthy neighbour die with it
#
#   bench/crash.sh <out dir>       CRASH_REPS times (3) per path; appends to <out dir>/crash.csv; raw logs under
#                                  <out dir>/raw/crash-<path>-<rep>/

set -u
set -o pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
out=${1:?usage: crash.sh <out dir>}
rate=${CRASH_RATE:-48000}
quantum=${CRASH_QUANTUM:-128}

if [ -z "${BENCH_INSIDE:-}" ]; then
    mkdir -p "$out"
    [ -f "$out/crash.csv" ] || echo "path,event,rate,quantum,rep,host_died,host_exit_ms,pipewire_alive,probe_cycles,probe_max_gap_us,probe_late,driver_xruns,victim_silent_ms,neighbour_silent_ms,respawn_to_sound_ms" >"$out/crash.csv"
    for rep in $(seq 1 "${CRASH_REPS:-3}"); do
        for path in ${CRASH_PATHS:-modhost clap adapter inproc-clap inproc-lv2}; do
            BENCH_INSIDE=1 CRASH_PATH=$path CRASH_REP=$rep unshare --user --map-root-user --net --pid --fork --mount-proc \
                "$0" "$(readlink -f "$out")" || echo "crash drill $path $rep failed" >&2
        done
    done
    exit 0
fi

path=$CRASH_PATH
rep=$CRASH_REP
raw=$out/raw/crash-$path-$rep
rm -rf "$raw"
mkdir -p "$raw"
die() {
    echo "crash $path: $*" >&2
    echo "$path,FAILED,$rate,$quantum,$rep,\"$*\"" >>"$out/crash.csv"
    exit 1
}
. "$here/lib.sh"

graph_start
plugin_set
# rec_0 the victim, rec_1 the victim respawned, rec_2 the healthy neighbour of the crash
probe_start 3 1
victim=${adds[0]}

err_of() { python3 -I "$here/stats.py" top "$1" | awk '$2 ~ /Driver$/ && $2 !~ /Freewheel/ { print $3 }' | head -1; }
gone_ms() {
    # how long after $1 (ns) the process $2 was gone, polled every 1 ms for 5 s; NA when it lives
    local t
    for _ in $(seq 5000); do
        # gone, or a zombie not reaped yet: either way it runs no more
        case $(awk '{ print $3 }' "/proc/$2/stat" 2>/dev/null) in
            '' | Z) t=$(now_ns); ms "$1" "$t"; return ;;
        esac
        sleep 0.001
    done
    echo NA
}
silent_ms() {
    local s
    s=$(grep "^input $2 " "$raw/probe.txt" | field silent_since_ns)
    if [ "${s:-0}" -gt 0 ]; then ms "$1" "$s"; else echo NA; fi
}
sound_after() {
    # poll the probe until input $2 has sounded; ms from $1 (ns), NA after 10 s
    local f
    for _ in $(seq 200); do
        probe_stats "$raw/poll.txt"
        f=$(grep "^input $2 " "$raw/poll.txt" | field first_ns)
        [ "${f:-0}" -gt 0 ] && { ms "$1" "$f"; return; }
        sleep 0.05
    done
    echo NA
}

# ---- kill ----
host_start
added "$(send "$victim 0")" 0 || die "victim: $victim"
link_fx effect_0 bench-src:src_l bench-src:src_r bench-rec:rec_0 || die "victim ports"
sleep 2
pw-top -b -n 2 >"$raw/top-before.txt" 2>&1
probe_reset
t_kill=$(now_ns)
kill -KILL "$host_pid"
exit_ms=$(gone_ms "$t_kill" "$host_pid")
wait "$host_pid" 2>/dev/null
exec 3>&- 7>&- 8<&- 2>/dev/null
sleep 2
probe_stats "$raw/probe.txt"
pw-top -b -n 2 >"$raw/top-after.txt" 2>&1
pw_alive=$(kill -0 "$pw_pid" 2>/dev/null && echo yes || echo no)
xr=$(( $(err_of "$raw/top-after.txt") - $(err_of "$raw/top-before.txt") ))
client=$(grep "^client " "$raw/probe.txt")
t_respawn=$(now_ns)
host_start
added "$(send "$victim 0")" 0 || die "respawned victim: $victim"
link_fx effect_0 bench-src:src_l bench-src:src_r bench-rec:rec_1 || die "respawned victim ports"
respawn_ms=$(sound_after "$t_respawn" rec_1)
echo "$path,kill,$rate,$quantum,$rep,yes,$exit_ms,$pw_alive,$(echo "$client" | field cycles),$(echo "$client" | field max_gap_us),$(echo "$client" | field late),$xr,$(silent_ms "$t_kill" rec_0),NA,$respawn_ms" >>"$out/crash.csv"
echo "crash $path kill: exit ${exit_ms} ms, pipewire alive $pw_alive, probe gap $(echo "$client" | field max_gap_us) us, xruns $xr, respawn to sound $respawn_ms ms"

# ---- crash: a plugin that aborts beside a healthy one, CLAP paths ----
case $path in
    clap | inproc-clap) ;;
    *) exit 0 ;;
esac
fault=$root/tests/fault-14.clap
case $path in
    clap) fadd="add clap:$fault#org.openmixer.fault.14" ;;
    inproc-clap) fadd="add clap $fault org.openmixer.fault.14" ;;
esac
added "$(send "$fadd 1")" 1 || die "fault-14"
link_fx effect_1 bench-src:src_l bench-src:src_r bench-rec:rec_2 || die "fault-14 ports"
sleep 2 # more than the 200 blocks the fixture waits for at any quantum here
cp "$raw/host.log" "$raw/host-before-crash.log"
pw-top -b -n 2 >"$raw/top-before.txt" 2>&1
probe_reset
t_crash=$(now_ns)
send "param_set 1 0 0.95" >"$raw/crash-reply.txt"
exit_ms=$(gone_ms "$t_crash" "$host_pid")
died=$([ "$exit_ms" = NA ] && echo no || echo yes)
sleep 2
probe_stats "$raw/probe.txt"
pw-top -b -n 2 >"$raw/top-after.txt" 2>&1
pw_alive=$(kill -0 "$pw_pid" 2>/dev/null && echo yes || echo no)
xr=$(( $(err_of "$raw/top-after.txt") - $(err_of "$raw/top-before.txt") ))
client=$(grep "^client " "$raw/probe.txt")
echo "$path,crash,$rate,$quantum,$rep,$died,$exit_ms,$pw_alive,$(echo "$client" | field cycles),$(echo "$client" | field max_gap_us),$(echo "$client" | field late),$xr,$(silent_ms "$t_crash" rec_2),$(silent_ms "$t_crash" rec_1),NA" >>"$out/crash.csv"
echo "crash $path crash: host died $died (${exit_ms} ms), neighbour silent after $(silent_ms "$t_crash" rec_1) ms"
exit 0
