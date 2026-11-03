#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# sha256_copy_check.sh: SHA-256 is the system crypto library's (libcrypto's EVP_Digest), and no tracked source carries
# an implementation of its own. An implementation is known by FIPS 180-4's constants: the first round constant
# (0x428a2f98) or the first word of the initial hash value (0x6a09e667), in any case and any spelling of the hex prefix.
#
#   sha256_copy_check.sh [ROOT]     red on any tracked C, header, assembly, Python or JavaScript source that holds one
#   sha256_copy_check.sh --self-test  each kind of copy planted in a scratch tree must be red, and the check without its
#                                     rules must pass one
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)

RULES=('428a2f98' '6a09e667')
SOURCES='\.(c|h|S|cc|cpp|py|js|mjs|ts)$'

copies() { # <root>: each tracked source that holds a constant of SHA-256's
    local tree=$1 f r found=0 me
    me=$(realpath "${BASH_SOURCE[0]}")
    while IFS= read -r f; do
        [[ $f =~ $SOURCES ]] && [ -f "$tree/$f" ] || continue
        [ "$(realpath "$tree/$f")" != "$me" ] || continue
        for r in "${RULES[@]}"; do
            [ -n "$r" ] || continue
            if grep -q -i -E "(0x)?$r" "$tree/$f"; then
                echo "FAIL $f carries an implementation of SHA-256 (FIPS 180-4's constant $r): hash with libcrypto's EVP_Digest"
                found=1; break
            fi
        done
    done < <(git -c safe.directory="$tree" -C "$tree" ls-files)
    return "$found"
}

if [ "${1:-}" = --self-test ]; then
    t=$(mktemp -d); trap 'rm -rf "$t"' EXIT; fail=0
    plant() { # <what> <path> <text>: a clean tree plus the plant must be red
        rm -rf "$t/r"; mkdir -p "$t/r/src"; git init -q "$t/r"
        echo 'int main(void) { return 0; }' > "$t/r/src/main.c"
        [ -z "$2" ] || { mkdir -p "$t/r/$(dirname "$2")"; printf '%s\n' "$3" > "$t/r/$2"; }
        git -C "$t/r" add -A
        if [ -z "$2" ]; then
            if copies "$t/r" >/dev/null; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi
        elif copies "$t/r" >/dev/null; then echo "FAIL $1 stays green"; fail=1
        else echo "ok   $1"; fi
    }
    plant "a tree with no implementation is green" "" ""
    plant "the round constants in a header are red" src/sha.h 'static const uint32_t k[64] = { 0x428a2f98, 0x71374491 };'
    plant "the initial hash value in a C file is red" lv2/clap/h.c 'static const uint32_t h0[8] = { 0x6A09E667, 0xbb67ae85 };'
    plant "a Python copy is red" tools/sha.py 'K = [0x428a2f98, 0x71374491]'
    # sabotage: with its rules gone the check passes a planted copy, so the rules are what catch it
    rm -rf "$t/s"; mkdir -p "$t/s/src"; git init -q "$t/s"
    echo 'static const unsigned k[] = { 0x428a2f98 };' > "$t/s/src/sha.h"; git -C "$t/s" add -A
    RULES=('')
    if copies "$t/s" >/dev/null; then echo "ok   sabotage: without its rules the check passes a copy"
    else echo "FAIL sabotage: the check is red without its rules"; fail=1; fi
    exit "$fail"
fi

copies "$(cd "${1:-$here/..}" && pwd)" || exit 1
echo "ok   no tracked source carries an implementation of SHA-256"
