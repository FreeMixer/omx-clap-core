#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# The waits on a host the e2e tests share: for its "ready!" line (jack_params_e2e.sh and jack_params_ready_sabotage.sh,
# so the sabotage arms hold the wait exactly as the e2e test runs it, #191), and, once it runs, for a condition it should
# bring about, given up at once when the host can answer nothing more (chain_host_e2e.sh), never a copy per test.

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

# host_cannot_answer <pid> -> 0 when the host can answer no more: gone, a zombie, stopped or dumping its core
host_cannot_answer() {
    local state
    [ -r "/proc/$1/stat" ] || return 0
    state=$(awk '{print $3}' "/proc/$1/stat" 2>/dev/null)
    case $state in Z | X | T | t | '') return 0 ;; esac
    grep -q '^CoreDumping:[[:space:]]*1' "/proc/$1/status" 2>/dev/null
}

# host_wait <pid> <tries> <condition> -> 0 once the shell condition holds, polled 0.1s apart; 1 after <tries> polls, or
# at once when the host can answer no more (host_cannot_answer), since nothing it should bring about will come
host_wait() {
    local pid=$1 tries=$2 cond=$3
    for _ in $(seq "$tries"); do
        eval "$cond" && return 0
        host_cannot_answer "$pid" && return 1
        sleep 0.1
    done
    eval "$cond"
}
