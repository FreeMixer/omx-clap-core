#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# omx-clap-host's command socket against tests/socket_conformance.py, in a PipeWire of its own: the script re-runs
# itself in a private user, net, pid and mount namespace, starts pipewire on a private runtime dir, starts the host
# and runs every framing case of the conformance script against its socket (a command of exactly the receive buffer
# size, many commands in one write, one command over many writes, clients that leave mid-command, the size limit,
# a random chunking fuzz). The host's protocol library is what is checked: omx-clap-host only links it.
#
# OMX_CLAP_HOST:           the binary (default ./omx-clap-host)
# SOCKET_E2E_PORT:         the socket port (default 5570)
# SOCKET_CONFORMANCE_ARGS: extra arguments for the conformance script (--seed N to repeat a fuzz run)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${SOCKET_E2E_PORT:-5570}

if [ -z "${SOCKET_E2E_INSIDE:-}" ]; then
    [ -e "$host" ] || { echo "missing $host" >&2; exit 2; }
    SOCKET_E2E_INSIDE=1 OMX_CLAP_HOST=$host SOCKET_E2E_PORT=$port \
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
fi

runtime=$(mktemp -d /tmp/omx-clap-host-socket.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
cleanup() {
    kill -TERM -- -1 2>/dev/null
    wait 2>/dev/null
    rm -rf "${runtime:?}"
}
trap cleanup EXIT

ip link set lo up

pipewire >"$runtime/pw.log" 2>&1 &
for _ in $(seq 50); do [ -S "$runtime/pipewire-0" ] && break; sleep 0.1; done
[ -S "$runtime/pipewire-0" ] || { echo "pipewire did not come up" >&2; cat "$runtime/pw.log" >&2; exit 2; }

"$host" -n -p "$port" >"$runtime/host.log" 2>&1 &
pid=$!
for _ in $(seq 50); do grep -q "ready!" "$runtime/host.log" && break; sleep 0.1; done
grep -q "ready!" "$runtime/host.log" || { echo "omx-clap-host did not come up" >&2; cat "$runtime/host.log" >&2; exit 2; }
echo "omx-clap-host on port $port, protocol library: $(grep -o '/[^ ]*libmod-host-protocol[^ ]*' /proc/$pid/maps | sort -u)"

# shellcheck disable=SC2086
python3 -I "$here/socket_conformance.py" --port "$port" --pid "$pid" ${SOCKET_CONFORMANCE_ARGS:-}
status=$?
kill -0 "$pid" 2>/dev/null || { echo "omx-clap-host is gone:"; tail -5 "$runtime/host.log"; status=1; }
exit $status
