#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# A plugin's own window in omx-clap-host over clap.gui with Wayland first (#329), against tests/fake_gui_wl.clap, a
# plugin that offers a Wayland window and an X11 one, in a PipeWire, a headless weston and an Xvfb of their own: the
# script re-runs itself in a private user, net, pid and mount namespace with its own /proc and /tmp, starts the three on
# a private runtime dir and drives the host over its socket. With both displays there, show_external_ui takes Wayland:
# the plugin is asked create("wayland", true), never X11, its toplevel is configured and drawn by the compositor, its
# descriptor and timer are served on the host's tick, and the host holds no connection of its own to either display;
# hide_external_ui and a second show work on it as on X11, and remove destroys it. The same plugin with no Wayland
# window falls to X11, and one with only a Wayland window on an X11 session is refused hosting.window.no-display; with
# neither display, or none that answers, the show is refused hosting.window.no-display and
# the plugin is never asked to create a window; a plugin with no clap.gui is refused hosting.window.unsupported; the
# audio runs on throughout. Then the host as X11=0 builds it, which links no xcb: the same plugin's Wayland window is
# shown and mapped by weston, a session with an X display and no compositor is hosting.window.no-display, and a plugin
# with only an X11 window is hosting.window.unsupported. Exit 0 only when every step held; each step is printed.
#
# OMX_CLAP_HOST:        the binary (default ./omx-clap-host)
# OMX_CLAP_HOST_NOX11:  the binary built with X11=0 (default build/nox11/omx-clap-host, make test-jack-window-wl's)
# JACK_E2E_PORT:        the socket port (default 5592)

set -u

here=$(cd "$(dirname "$0")" && pwd)
. "${PLUGIN_HOSTD_TESTS_LIB:-$here/../../plugin-hostd/tests/lib}/protocol.sh"
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
nox11=${OMX_CLAP_HOST_NOX11:-$root/build/nox11/omx-clap-host}
case $nox11 in /*) ;; *) nox11=$root/$nox11 ;; esac
port=${JACK_E2E_PORT:-5592}
gui=$here/fake_gui_wl.clap
fake=$here/fake.clap
source=$here/jack_meter_source
listen=$here/jack_synth_probe

if [ -z "${JACK_E2E_INSIDE:-}" ]; then
    for f in "$host" "$nox11" "$gui" "$fake" "$source" "$listen"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    for t in weston Xvfb xdotool pipewire pw-link ss ldd; do
        command -v "$t" >/dev/null || { echo "missing $t" >&2; exit 2; }
    done
    JACK_E2E_INSIDE=1 OMX_CLAP_HOST=$host OMX_CLAP_HOST_NOX11=$nox11 JACK_E2E_PORT=$port \
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
fi

# pid 1 of the private namespace, or nothing at all: JACK_E2E_INSIDE leaked into a session's shell would run the rest
# there, and the exit's kill -TERM -- -1 with it
[ "$$" = 1 ] || { echo "not pid 1 of a private pid namespace (JACK_E2E_INSIDE set outside it?): nothing run" >&2; exit 2; }

# a /tmp of the namespace's own: Xvfb's lock and socket never meet the session's, and the runtime dir weston's socket
# lives in is made in it
mount -t tmpfs tmpfs /tmp || { echo "no private /tmp in the namespace" >&2; exit 2; }

failures=0
step() {
    if [ "$1" -eq 0 ]; then
        echo "ok   $2"
    else
        echo "FAIL $2"
        failures=$((failures + 1))
        tail -5 "${runtime:-/nonexistent}/host.log" 2>/dev/null | sed 's/^/     host: /'
        tail -8 "${runtime:-/nonexistent}/gui.log" 2>/dev/null | sed 's/^/     plugin: /'
    fi
}
run() {
    timeout 10 "$@"
}

step 0 "pid 1 of a private pid namespace"
fstype=$(awk '$2 == "/tmp" { t = $3 } END { print t }' /proc/self/mounts)
[ "$fstype" = tmpfs ]; step $? "/tmp is a tmpfs ($fstype)"
[ "$failures" = 0 ] || exit 1
[ -z "$(ls -A /tmp)" ] && [ ! -e /tmp/.X11-unix ]; step $? "/tmp is empty, no /tmp/.X11-unix in it: the namespace's own"
[ "$failures" = 0 ] || exit 1

runtime=$(mktemp -d /tmp/omx-clap-host-window-wl.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS WAYLAND_DISPLAY WAYLAND_SOCKET DISPLAY
pids=()
cleanup() {
    exec 3>&- 2>/dev/null
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    kill -TERM -- -1 2>/dev/null
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

# the compositor: weston's headless backend, its socket in the private runtime dir
wl_name=omx-wl
weston --backend=headless-backend.so --socket="$wl_name" --idle-time=0 --no-config --debug --debug-scopes=scene-graph \
    >"$runtime/weston.log" 2>&1 &
pids+=($!)
for _ in $(seq 100); do [ -S "$runtime/$wl_name" ] && break; sleep 0.1; done
[ -S "$runtime/$wl_name" ] || { echo "weston did not come up" >&2; cat "$runtime/weston.log" >&2; exit 2; }
step 0 "headless weston on $runtime/$wl_name"

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
    protocol_wait_ready "$log"
    step $? "omx-clap-host ready on port $port ($*)"
    exec 3>&- 2>/dev/null
    exec 3<>"/dev/tcp/127.0.0.1/$port"
}
PROTOCOL_TIMEOUT=5
expect() {
    local reply
    reply=$(send "$1")
    [ "$reply" = "$2" ]
    step $? "$1 -> $reply"
}
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
audio_runs() {
    local w
    for _ in $(seq 20); do
        w=$(level "effect_$1:out_1") && near "${w%% *}" 0.25 0.0001 && break
        sleep 0.1
    done
    w=$(level "effect_$1:out_1"); near "${w%% *}" 0.25 0.0001; step $? "$2: effect_$1:out_1 carries the halved 0.5 ($w)"
}
xwin() { DISPLAY=$display run xdotool "$@" 2>/dev/null; }
shown() { [ -n "$(xwin search --onlyvisible --name "^$1\$")" ]; }
logged() { grep -c -x -F "$1" "$runtime/gui.log" 2>/dev/null; }
created() { grep -c '^create ' "$runtime/gui.log" 2>/dev/null; }
at() { grep -n -x -F "$1" "$runtime/gui.log" 2>/dev/null | head -1 | cut -d: -f1; }
in_order() {
    local prev=0 n
    [ "$#" -gt 0 ] || return 1
    for n in "$@"; do
        [ -n "$n" ] && [ "$n" -gt "$prev" ] || return 1
        prev=$n
    done
}
wait_count() {
    for _ in $(seq 50); do [ "$(logged "$1")" -ge "$2" ] && return 0; sleep 0.1; done
    return 1
}
# the host's connections to a server whose socket path matches $1: the server's ends (ss, local address the socket)
# whose peer is one of the host's sockets; a plugin's own connection is in the host's process too
connections() {
    ls -l "/proc/$host_pid/fd" 2>/dev/null | sed -n 's/.*socket:\[\([0-9]*\)\]$/\1/p' >"$runtime/inodes"
    ss -xpn 2>/dev/null | awk -v f="$runtime/inodes" -v s="$1" 'BEGIN { while ((getline i < f) > 0) mine[i] = 1 }
        index($5, s) && ($8 in mine) { n++ } END { print n + 0 }'
}
wl_connections() { connections "$runtime/$wl_name"; }
x_connections() { connections "X11-unix/X"; }
# weston's scene graph, as its debug protocol dumps it: a view of the host's process (the plugin runs in it) that is an
# xdg_toplevel titled $1, "View 0 (role xdg_toplevel, PID <pid>, surface ID 3, top-level window '<title>' of <app id>"
scene() { WAYLAND_DISPLAY=$wl_name run weston-debug scene-graph 2>/dev/null; }
mapped() { scene | grep -q -E "role xdg_toplevel, PID $host_pid, .*top-level window '$1' of org\.omx-clap-host\.test\.gui"; }

# ---- both displays: Wayland is taken ----
: >"$runtime/gui.log"
start_host "$runtime/host.log" env WAYLAND_DISPLAY="$wl_name" DISPLAY="$display" FAKE_GUI_LOG="$runtime/gui.log" "$host"
expect "add clap:$gui#org.omx-clap-host.test.gui 0" "resp 0"
[ "$(logged "host gui offered")" = 1 ]; step $? "the plugin is offered the host's clap.gui at its init"
feed 0
audio_runs 0 "before any window"
[ "$(wl_connections)" = 0 ] && [ "$(x_connections)" = 0 ]; step $? "no connection to either display before a show"
expect "show_external_ui 0" "resp 0"
[ "$(logged "create wayland 1")" = 1 ] && [ "$(created)" = 1 ]
step $? "Wayland first: the plugin was asked create wayland floating, and no X11 window ($(created) create)"
order="$(at "create wayland 1") $(at "title fake gui") $(at "show")"
[ "$(echo $order | wc -w)" = 3 ] && in_order $order; step $? "told its title, then shown: the plugin's log lines $order"
[ "$(logged "set_parent x11")" = 0 ] && [ "$(logged "scale 1.0")" = 0 ]; step $? "never set_parent nor set_scale: a floating window"
wait_count "wayland configured with its buffer" 1; step $? "the compositor configured the plugin's toplevel, its buffer attached"
wait_count "wayland frame" 1; step $? "and drew it (a frame callback from weston)"
mapped "fake gui"; step $? "weston maps it: a toplevel of the host's process titled with the plugin's name, fake gui"
wait_count "timer on the tick" 1; step $? "the plugin's timer is called on the host's tick while its window is open"
wait_count "fd on the tick" 1; step $? "and its Wayland descriptor polled on the host's tick"
[ "$(wl_connections)" = 1 ]; step $? "one connection to the compositor, the plugin's own: the host keeps none ($(wl_connections))"
[ "$(x_connections)" = 0 ]; step $? "and none to the X display ($(x_connections))"
audio_runs 0 "with the window open"
expect "hide_external_ui 0" "resp 0"
[ "$(logged "hide")" = 1 ]; step $? "the plugin was asked hide"
for _ in $(seq 30); do mapped "fake gui" || break; sleep 0.1; done
! mapped "fake gui"; step $? "hidden: no view of it is left in weston's scene graph"
expect "show_external_ui 0" "resp 0"
[ "$(logged "create wayland 1")" = 1 ] && [ "$(logged "show")" = 2 ]; step $? "shown again: no second create"
wait_count "wayland frame" 2; step $? "and its toplevel drawn again"
mapped "fake gui"; step $? "and mapped by weston again"
expect "remove 0" "resp 0"
[ "$(logged "destroy")" = 1 ]; step $? "remove with the window open: the plugin's window destroyed first"
for _ in $(seq 30); do [ "$(wl_connections)" = 0 ] && break; sleep 0.1; done
[ "$(wl_connections)" = 0 ]; step $? "and no connection to the compositor is left ($(wl_connections))"
! mapped "fake gui"; step $? "nor any view of it in weston's scene graph"
kill -0 "$host_pid" 2>/dev/null; step $? "the host runs on"
expect "add clap:$fake#org.omx-clap-host.test.passthrough 1" "resp 1"
expect "show_external_ui 1" "resp -404 hosting.window.unsupported"
expect "remove 1" "resp 0"
expect "quit" "resp 0"
sleep 0.3

# ---- both displays, a plugin with no Wayland window: X11 embedded ----
port=$((port + 1))
: >"$runtime/gui.log"
start_host "$runtime/host2.log" env WAYLAND_DISPLAY="$wl_name" DISPLAY="$display" FAKE_GUI_LOG="$runtime/gui.log" \
    FAKE_GUI_NO_WAYLAND=1 "$host"
expect "add clap:$gui#org.omx-clap-host.test.gui 0" "resp 0"
expect "show_external_ui 0" "resp 0"
[ "$(logged "create x11 0")" = 1 ] && [ "$(created)" = 1 ]; step $? "no Wayland window: the plugin was asked create x11 embedded"
for _ in $(seq 30); do shown "fake gui view" && break; sleep 0.1; done
shown "fake gui" && shown "fake gui view"; step $? "the host's window, fake gui, and the plugin's inside it are on the X display"
[ "$(wl_connections)" = 0 ]; step $? "and nothing is connected to the compositor ($(wl_connections))"
expect "remove 0" "resp 0"
expect "quit" "resp 0"
sleep 0.3

# ---- an X11 session, a plugin with only a Wayland window: no display it can go on ----
port=$((port + 1))
: >"$runtime/gui.log"
start_host "$runtime/host2b.log" env -u WAYLAND_DISPLAY DISPLAY="$display" FAKE_GUI_LOG="$runtime/gui.log" FAKE_GUI_NO_X11=1 "$host"
expect "add clap:$gui#org.omx-clap-host.test.gui 0" "resp 0"
feed 0
expect "show_external_ui 0" "resp -404 hosting.window.no-display"
[ "$(created)" = 0 ]; step $? "a Wayland-only plugin on an X11 session: never asked to create a window"
[ "$(x_connections)" = 0 ]; step $? "and the host keeps no X connection after the refusal ($(x_connections))"
audio_runs 0 "a Wayland-only plugin on an X11 session, after the refusal"
expect "remove 0" "resp 0"
expect "quit" "resp 0"
sleep 0.3

# ---- no display: neither named, a compositor no one answers, neither answering ----
none=":$(( $(head -1 "$runtime/display") + 50 ))"
for what in "neither display named" "WAYLAND_DISPLAY=omx-none, no compositor" "WAYLAND_DISPLAY=omx-none and DISPLAY=$none, no server"; do
    port=$((port + 1))
    : >"$runtime/gui.log"
    case $what in
    neither*) start_host "$runtime/host3.log" env -u WAYLAND_DISPLAY -u DISPLAY FAKE_GUI_LOG="$runtime/gui.log" "$host" ;;
    *DISPLAY=:*) start_host "$runtime/host3.log" env WAYLAND_DISPLAY=omx-none DISPLAY="$none" FAKE_GUI_LOG="$runtime/gui.log" "$host" ;;
    *) start_host "$runtime/host3.log" env -u DISPLAY WAYLAND_DISPLAY=omx-none FAKE_GUI_LOG="$runtime/gui.log" "$host" ;;
    esac
    expect "add clap:$gui#org.omx-clap-host.test.gui 0" "resp 0"
    feed 0
    expect "show_external_ui 0" "resp -404 hosting.window.no-display"
    [ "$(created)" = 0 ]; step $? "$what: the plugin is never asked to create a window"
    audio_runs 0 "$what, after the refusal"
    expect "remove 0" "resp 0"
    expect "quit" "resp 0"
    sleep 0.3
done

# ---- the host as X11=0 builds it: no xcb, the Wayland window all the same ----
! ldd "$nox11" | grep -q 'libxcb'; step $? "the X11=0 host links no xcb ($(ldd "$nox11" | grep -c libxcb) libxcb)"
ldd "$host" | grep -q 'libxcb'; step $? "and the default host does: the two builds differ"
port=$((port + 1))
: >"$runtime/gui.log"
start_host "$runtime/host4.log" env WAYLAND_DISPLAY="$wl_name" DISPLAY="$display" FAKE_GUI_LOG="$runtime/gui.log" "$nox11"
expect "add clap:$gui#org.omx-clap-host.test.gui 0" "resp 0"
feed 0
expect "show_external_ui 0" "resp 0"
[ "$(logged "create wayland 1")" = 1 ] && [ "$(created)" = 1 ]; step $? "X11=0: the plugin was asked create wayland floating"
wait_count "wayland frame" 1; step $? "X11=0: and its toplevel drawn by weston"
mapped "fake gui"; step $? "X11=0: weston maps it, a toplevel of the host's process titled fake gui"
[ "$(x_connections)" = 0 ]; step $? "X11=0: nothing connected to the X display ($(x_connections))"
audio_runs 0 "X11=0, with the window open"
expect "remove 0" "resp 0"
expect "quit" "resp 0"
sleep 0.3

# X11=0 and an X display but no Wayland session: Wayland is the one API built, and nothing answers it
port=$((port + 1))
: >"$runtime/gui.log"
start_host "$runtime/host5.log" env -u WAYLAND_DISPLAY DISPLAY="$display" FAKE_GUI_LOG="$runtime/gui.log" "$nox11"
expect "add clap:$gui#org.omx-clap-host.test.gui 0" "resp 0"
feed 0
expect "show_external_ui 0" "resp -404 hosting.window.no-display"
[ "$(created)" = 0 ]; step $? "X11=0, no Wayland session: the plugin is never asked to create a window"
audio_runs 0 "X11=0, no Wayland session, after the refusal"
expect "remove 0" "resp 0"
expect "quit" "resp 0"
sleep 0.3

# X11=0, both displays, a plugin with no Wayland window: the X11 window the default host shows is not built
port=$((port + 1))
: >"$runtime/gui.log"
start_host "$runtime/host6.log" env WAYLAND_DISPLAY="$wl_name" DISPLAY="$display" FAKE_GUI_LOG="$runtime/gui.log" \
    FAKE_GUI_NO_WAYLAND=1 "$nox11"
expect "add clap:$gui#org.omx-clap-host.test.gui 0" "resp 0"
expect "show_external_ui 0" "resp -404 hosting.window.unsupported"
[ "$(created)" = 0 ]; step $? "X11=0, a plugin with only an X11 window: never asked to create one"
expect "remove 0" "resp 0"
expect "quit" "resp 0"
sleep 0.3

if [ "$failures" -eq 0 ]; then
    echo "jack window wayland e2e ok"
    exit 0
fi
echo "jack window wayland e2e FAILED ($failures)"
exit 1
