#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The verbs omx-clap-host owes, as the installed declarations state them, against the host's socket and its registration
# table (tests/host_verbs_check.py), in a PipeWire of its own: the script re-runs itself in a private user, net, pid and
# mount namespace, starts pipewire on a private runtime dir and the host with the fake plugins offering their parameter
# contract (FAKE_CONTRACT=good), and the check adds the passthrough and the synth and drives every verb declared for
# this host.
#
# OMX_CONTRACT_PATH:      the contract directories the declarations are read from, as omx-contract reads them (make
#                         test-host-verbs installs the build tree's into build/verbs-contract; the packaging job names
#                         the installed one)
# OMX_CLAP_HOST:          the binary (default ./omx-clap-host)
# HOST_VERBS_PORT:        the socket port (default 5574)
# HOST_VERBS_FIXTURES:    the directory of fake.clap and fake_synth.clap (default tests/)
# HOST_VERBS_REGISTRATION: the registration table (default src/main.c)
# PHD_PIN_LAYOUT_SCHEME:  the layout scheme plugin-hostd's pin.h declares, read from it by make test-host-verbs

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${HOST_VERBS_PORT:-5574}
fixtures=${HOST_VERBS_FIXTURES:-$here}
registration=${HOST_VERBS_REGISTRATION:-$root/src/main.c}

: "${OMX_CONTRACT_PATH:?the contract directories to read the declarations from}" "${PHD_PIN_LAYOUT_SCHEME:?}"

if [ -z "${HOST_VERBS_INSIDE:-}" ]; then
    for f in "$host" "$fixtures/fake.clap" "$fixtures/fake_synth.clap" "$registration"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    HOST_VERBS_INSIDE=1 OMX_CLAP_HOST=$host HOST_VERBS_PORT=$port \
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
fi

runtime=$(mktemp -d /tmp/omx-clap-host-verbs.XXXXXX)
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

FAKE_CONTRACT=good "$host" -n -p "$port" >"$runtime/host.log" 2>&1 &
pid=$!
for _ in $(seq 50); do grep -q "ready!" "$runtime/host.log" && break; sleep 0.1; done
grep -q "ready!" "$runtime/host.log" || { echo "omx-clap-host did not come up" >&2; cat "$runtime/host.log" >&2; exit 2; }

python3 -I "$here/host_verbs_check.py" --port "$port" --contract-path "$OMX_CONTRACT_PATH" \
    --registration "$registration" --header "$(dirname "$registration")/omx_clap_core_host_verbs.h" \
    --fake "$fixtures/fake.clap" --synth "$fixtures/fake_synth.clap" --layout-scheme "$PHD_PIN_LAYOUT_SCHEME"
status=$?
kill -0 "$pid" 2>/dev/null || { echo "FAIL omx-clap-host is gone:"; tail -5 "$runtime/host.log"; status=1; }
exit $status
