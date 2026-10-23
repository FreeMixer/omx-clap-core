#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The wait for a host's "ready!" line, shared by jack_params_e2e.sh and jack_params_ready_sabotage.sh so the sabotage
# arms hold the wait exactly as the e2e test runs it, never a copy that could drift from what it checks (#191).

# port_busy <port> -> 0 if something already answers 127.0.0.1:<port>, 1 otherwise; instant, no retry
port_busy() {
    ( exec 9<>"/dev/tcp/127.0.0.1/$1" ) 2>/dev/null
}

# host_ready_wait <log> <tries> -> 0 once <log> holds a "ready!" line; 1 on timeout, <tries> polls of 0.1s apart
host_ready_wait() {
    local log=$1 tries=$2
    for _ in $(seq "$tries"); do
        grep -q "ready!" "$log" 2>/dev/null && return 0
        sleep 0.1
    done
    grep -q "ready!" "$log" 2>/dev/null
}

# host_ready_cause <held_before> -> the text start_host-style callers name on timeout: whether the port already
# answered before the host was started (held_before=0), or the host just never said ready!
host_ready_cause() {
    if [ "$1" -eq 0 ]; then
        echo "the port was already held"
    else
        echo "the log never said ready!"
    fi
}
