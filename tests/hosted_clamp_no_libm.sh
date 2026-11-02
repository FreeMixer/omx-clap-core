#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
# tests/hosted_clamp_no_libm.sh <binary>: a binary compiling the hosted stage's inline body at -O2 imports no fminf or
# fmaxf. A libm call per sample per leg was three quarters of what the host added to an empty plugin.
set -euo pipefail
if nm -D --undefined-only "$1" | grep -qE '\b(fminf|fmaxf)\b'; then
    echo "FAIL $1 calls libm's fminf or fmaxf from the RT body"
    exit 1
fi
echo "ok $1: no libm min or max in the RT body"
