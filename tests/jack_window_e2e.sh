#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A plugin's own window in omx-clap-host (#180), over clap.gui and X11, against tests/fake_gui.clap, in a PipeWire and an
# Xvfb of their own: the script re-runs itself in a private user, net, pid and mount namespace with its own /proc, starts
# pipewire on a private runtime dir and Xvfb on a free display, and drives the host over its socket. show_external_ui
# puts the plugin's window on the display inside a window of the host's titled with the plugin's name; the plugin's
# timer and its X descriptor are served on the host's tick while it is open; the plugin's request_resize resizes the
# host's window, and the host's window resized is the plugin's set_size; hide_external_ui takes it off, a second show
# brings the same one back; a plugin with no clap.gui, and a host with no display, are refused by code while the audio
# runs on; remove with the window open destroys it and the host keeps no X connection; a plugin that only floats is
# shown by -S at its add. Exit 0 only when every step held; each step is printed.
#
# OMX_CLAP_HOST:    the binary (default ./omx-clap-host)
# JACK_E2E_PORT:    the socket port (default 5582)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${JACK_E2E_PORT:-5582}
gui=$here/fake_gui.clap
fake=$here/fake.clap
source=$here/jack_meter_source
listen=$here/jack_synth_probe

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$gui" "$fake" "$source" "$listen"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    for t in Xvfb xdotool pipewire pw-link; do
        command -v "$t" >/dev/null || { echo "missing $t" >&2; exit 2; }
    done
    JACK_E2E_INSIDE=1 OMX_CLAP_HOST=$host JACK_E2E_PORT=$port \
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
fi

# a /tmp of the namespace's own: Xvfb's lock and socket (/tmp/.X<n>-lock, /tmp/.X11-unix) never meet the session's, whose
# lock names a pid this pid namespace cannot see and would read as stale
mount -t tmpfs tmpfs /tmp || { echo "no private /tmp in the namespace" >&2; exit 2; }

failures=0
step() {
    if [ "$1" -eq 0 ]; then
        echo "ok   $2"
    else
        echo "FAIL $2"
        failures=$((failures + 1))
        tail -5 "$runtime/host.log" 2>/dev/null | sed 's/^/     host: /'
        tail -8 "$runtime/gui.log" 2>/dev/null | sed 's/^/     plugin: /'
    fi
}
run() {
    timeout 10 "$@"
}

runtime=$(mktemp -d /tmp/omx-clap-host-window.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS WAYLAND_DISPLAY DISPLAY
pids=()
cleanup() {
    exec 3>&- 2>/dev/null
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

# a free display, its number written by Xvfb itself once it listens
Xvfb -displayfd 5 -nolisten tcp -screen 0 1024x768x24 5>"$runtime/display" >"$runtime/xvfb.log" 2>&1 &
pids+=($!)
for _ in $(seq 100); do [ -s "$runtime/display" ] && break; sleep 0.1; done
[ -s "$runtime/display" ] || { echo "Xvfb did not come up" >&2; cat "$runtime/xvfb.log" >&2; exit 2; }
display=":$(head -1 "$runtime/display")"
step 0 "Xvfb on $display"

host_pid=
start_host() {
    local log=$1; shift
    "$@" -n -p "$port" >"$log" 2>&1 &
    pids+=($!)
    host_pid=$!
    for _ in $(seq 50); do grep -q "ready!" "$log" && break; sleep 0.1; done
    grep -q "ready!" "$log"
    step $? "omx-clap-host ready on port $port ($*)"
    exec 3>&- 2>/dev/null
    exec 3<>"/dev/tcp/127.0.0.1/$port"
}
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
# the source's 0.5 and 0.25 into effect_<N>:in_1 and in_2, the source of the host before it stopped
src_pid=
feed() {
    [ -n "$src_pid" ] && kill "$src_pid" 2>/dev/null && wait "$src_pid" 2>/dev/null
    : >"$runtime/source.log"
    "$source" "effect_$1:in_1" "effect_$1:in_2" >"$runtime/source.log" 2>&1 &
    src_pid=$!
    pids+=($src_pid)
    for _ in $(seq 50); do grep -q playing "$runtime/source.log" && break; sleep 0.1; done
    grep -q playing "$runtime/source.log"; step $? "the source plays 0.5 and 0.25 into effect_$1:in_1 and in_2"
}
level() { echo | run "$listen" - "$@" 2>>"$runtime/listen.log"; }
near() { awk -v a="$1" -v b="$2" -v t="$3" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t) }'; }
# the audio of instance $1, halved by the plugin: out_1 carries 0.25 when the source's 0.5 goes into in_1
audio_runs() {
    local w
    for _ in $(seq 20); do
        w=$(level "effect_$1:out_1") && near "${w%% *}" 0.25 0.0001 && break
        sleep 0.1
    done
    w=$(level "effect_$1:out_1"); near "${w%% *}" 0.25 0.0001; step $? "$2: effect_$1:out_1 carries the halved 0.5 ($w)"
}
xwin() { DISPLAY=$display run xdotool "$@" 2>/dev/null; }
# a window named exactly $1, mapped and viewable
shown() { [ -n "$(xwin search --onlyvisible --name "^$1\$")" ]; }
named() { [ -n "$(xwin search --name "^$1\$")" ]; }
logged() { grep -c -x -F "$1" "$runtime/gui.log" 2>/dev/null; }
wait_logged() {
    for _ in $(seq 50); do [ "$(logged "$1")" -ge 1 ] && return 0; sleep 0.1; done
    return 1
}
# the host's connections to the X server: the server's ends (ss, local address the display's socket) whose peer is one
# of the host's sockets; the plugin's own connection is in the host's process too
x_connections() {
    ls -l "/proc/$host_pid/fd" 2>/dev/null | sed -n 's/.*socket:\[\([0-9]*\)\]$/\1/p' >"$runtime/inodes"
    ss -xpn 2>/dev/null | awk -v f="$runtime/inodes" 'BEGIN { while ((getline i < f) > 0) mine[i] = 1 }
        $5 ~ /X11-unix\/X/ && ($8 in mine) { n++ } END { print n + 0 }'
}

# ---- embedded: the host's window, the plugin's inside it ----
: >"$runtime/gui.log"
start_host "$runtime/host.log" env DISPLAY="$display" FAKE_GUI_LOG="$runtime/gui.log" FAKE_GUI_RESIZE=320x240 "$host"
expect "add clap:$gui#org.omx-clap-host.test.gui 0" "resp 0"
[ "$(logged "host gui offered")" = 1 ]; step $? "the plugin is offered the host's clap.gui at its init"
feed 0
audio_runs 0 "before any window"
[ "$(x_connections)" = 0 ]; step $? "no connection to the display before a show"
expect "show_external_ui 0" "resp 0"
for _ in $(seq 30); do shown "fake gui" && break; sleep 0.1; done
shown "fake gui"; step $? "a window titled with the plugin's name, fake gui, is on the display"
for _ in $(seq 30); do shown "fake gui view" && break; sleep 0.1; done
shown "fake gui view"; step $? "and the plugin's own window, fake gui view, viewable inside it"
[ "$(x_connections)" = 2 ]; step $? "two connections to the display, the host's and the plugin's own ($(x_connections))"
[ "$(logged "create x11 0")" = 1 ] && [ "$(logged "scale 1.0")" = 1 ] && [ "$(logged "set_parent x11")" = 1 ] && [ "$(logged "show")" = 1 ]
step $? "the plugin was asked create x11 embedded, set_scale 1.0, set_parent, show, in that order once each"
wait_logged "timer on the tick"; step $? "the plugin's timer is called on the host's tick while its window is open"
wait_logged "fd on the tick"; step $? "and its X descriptor polled on the host's tick"
wait_logged "request_resize 320 240 1"; step $? "the plugin's request_resize 320x240 is accepted"
xid=$(xwin search --name "^fake gui\$" | head -1)
geometry() { xwin getwindowgeometry "$xid" | sed -n 's/^ *Geometry: //p'; }
for _ in $(seq 30); do [ "$(geometry)" = "320x240" ] && break; sleep 0.1; done
[ "$(geometry)" = "320x240" ]; step $? "and the host's window is resized to it ($(geometry))"
xwin windowsize "$xid" 500 400; step $? "the host's window resized to 500x400 on the display"
wait_logged "set_size 500 400"; step $? "is the plugin's set_size 500 400"
audio_runs 0 "with the window open"
expect "hide_external_ui 0" "resp 0"
for _ in $(seq 30); do shown "fake gui" || break; sleep 0.1; done
! shown "fake gui" && named "fake gui"; step $? "hidden: the window is off the display, kept"
[ "$(logged "hide")" = 1 ]; step $? "the plugin was asked hide"
expect "hide_external_ui 0" "resp 0"
expect "show_external_ui 0" "resp 0"
for _ in $(seq 30); do shown "fake gui" && break; sleep 0.1; done
shown "fake gui" && [ "$(logged "create x11 0")" = 1 ] && [ "$(logged "show")" = 2 ]
step $? "shown again: the same window, no second create"
expect "add clap:$fake#org.omx-clap-host.test.passthrough 1" "resp 1"
expect "show_external_ui 1" "resp -404 hosting.window.unsupported"
expect "show_external_ui 7" "resp -3"
expect "hide_external_ui 7" "resp -3"
expect "remove 0" "resp 0"
[ "$(logged "destroy")" = 1 ]; step $? "remove with the window open: the plugin's window destroyed first"
for _ in $(seq 30); do named "fake gui" || break; sleep 0.1; done
! named "fake gui" && ! named "fake gui view"; step $? "and no window of it is left on the display"
kill -0 "$host_pid" 2>/dev/null; step $? "the host runs on"
[ "$(x_connections)" = 0 ]; step $? "and keeps no connection to the display, neither its own nor the plugin's ($(x_connections))"
expect "remove 1" "resp 0"
expect "quit" "resp 0"
sleep 0.3

# ---- floating: -S shows it at the add, the plugin's own top-level window ----
port=$((port + 1))
: >"$runtime/gui.log"
start_host "$runtime/host2.log" env DISPLAY="$display" FAKE_GUI_LOG="$runtime/gui.log" FAKE_GUI_FLOATING=1 "$host" -S
expect "add clap:$gui#org.omx-clap-host.test.gui 0" "resp 0"
for _ in $(seq 30); do shown "fake gui" && break; sleep 0.1; done
shown "fake gui"; step $? "-S: the floating plugin's own window is on the display at its add"
[ "$(logged "create x11 1")" = 1 ] && [ "$(logged "title fake gui")" = 1 ] && [ "$(logged "set_parent x11")" = 0 ]
step $? "the plugin was asked create x11 floating and told its title, never set_parent"
expect "hide_external_ui 0" "resp 0"
for _ in $(seq 30); do shown "fake gui" || break; sleep 0.1; done
! shown "fake gui"; step $? "hidden"
expect "remove 0" "resp 0"
expect "quit" "resp 0"
sleep 0.3

# ---- no display: unset, and one no server answers ----
for d in "" ":$(( $(head -1 "$runtime/display") + 50 ))"; do
    port=$((port + 1))
    : >"$runtime/gui.log"
    if [ -z "$d" ]; then
        start_host "$runtime/host3.log" env -u DISPLAY FAKE_GUI_LOG="$runtime/gui.log" "$host"
        what="DISPLAY unset"
    else
        start_host "$runtime/host3.log" env DISPLAY="$d" FAKE_GUI_LOG="$runtime/gui.log" "$host"
        what="DISPLAY=$d, no server"
    fi
    expect "add clap:$gui#org.omx-clap-host.test.gui 0" "resp 0"
    feed 0
    expect "show_external_ui 0" "resp -404 hosting.window.no-display"
    [ "$(logged "create x11 0")" = 0 ]; step $? "$what: the plugin is never asked to create a window"
    audio_runs 0 "$what, after the refusal"
    expect "remove 0" "resp 0"
    expect "quit" "resp 0"
    sleep 0.3
done

if [ "$failures" -eq 0 ]; then
    echo "jack window e2e ok"
    exit 0
fi
echo "jack window e2e FAILED ($failures)"
exit 1
