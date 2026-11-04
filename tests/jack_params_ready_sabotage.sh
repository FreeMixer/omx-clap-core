#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The sabotage of tests/host_ready_wait.sh's two causes (#191): a host that holds but never writes its "ready!"
# line, and a port a listener already held before the host even started. Both must come back named, not just a
# bare timeout; a host merely slow to log ready! inside the bound must still pass. No PipeWire, jack or
# omx-clap-host binary needed: the wait is pure log and socket watching, so the arms run against fakes, on ports
# well off the jack e2e scripts' own, directly in this process's network namespace.

set -u

here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=tests/host_ready_wait.sh
. "$here/host_ready_wait.sh"

failures=0
step() {
    if [ "$1" -eq 0 ]; then
        echo "ok   $2"
    else
        echo "FAIL $2"
        failures=$((failures + 1))
    fi
}

runtime=$(mktemp -d /tmp/host-ready-sabotage.XXXXXX)
pids=()
cleanup() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    wait 2>/dev/null
    rm -rf "${runtime:?}"
}
trap cleanup EXIT

# arm 1: a log that never gets a "ready!" line. host_ready_wait must time out, and since nothing held a port before
# it, the cause named is the log, never the port.
log=$runtime/never.log
: >"$log"
if host_ready_wait "$log" 3; then
    step 1 "a log that never says ready! is reported ready"
else
    cause=$(host_ready_cause 1)
    [ "$cause" = "the log never said ready!" ]
    step $? "a log that never gets ready! times out named \"$cause\""
fi

# arm 2: another process already holds the port before the host is even started. host_ready_wait still times out
# (nothing of ours writes "ready!" to this log either), but the cause named must be the port: port_busy saw it
# occupied ahead of the attempt, the way start_host checks it.
port=$((25566 + RANDOM % 1000))
python3 -c "
import socket, time
s = socket.socket()
s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(('127.0.0.1', $port))
s.listen(1)
time.sleep(5)
" &
pids+=($!)
for _ in $(seq 30); do port_busy "$port" && break; sleep 0.1; done
held_before=1
port_busy "$port" && held_before=0
[ "$held_before" -eq 0 ]
step $? "the listener on $port is seen busy before a host would start"
log=$runtime/held.log
: >"$log"
if host_ready_wait "$log" 3; then
    step 1 "a log that never says ready! is reported ready"
else
    cause=$(host_ready_cause "$held_before")
    [ "$cause" = "the port was already held" ]
    step $? "the held port is named, not the log: \"$cause\""
fi

# arm 3: a host merely slow under load, logging ready! just past the first poll: the longer bound must still catch
# it, not time out the way the 5s wait once did (#191).
log=$runtime/slow.log
: >"$log"
( sleep 0.3; echo "omx-clap-host ready!" >>"$log" ) &
pids+=($!)
host_ready_wait "$log" 20
step $? "a host slow to log ready! is still caught inside the bound"

if [ "$failures" -eq 0 ]; then
    echo "host ready wait sabotage ok"
    exit 0
fi
echo "host ready wait sabotage FAILED ($failures)"
exit 1
