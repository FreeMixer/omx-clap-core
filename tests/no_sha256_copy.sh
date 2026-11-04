#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# no_sha256_copy.sh [root] | --self-test
#
# SHA-256 is OpenSSL's libcrypto here (EVP_Digest): no tracked C source or header carries an implementation of its own.
# An implementation is found by what every one must hold, the first of FIPS 180-4's round constants, 0x428a2f98, in any
# spelling of case. Exit 0 none; 1 a copy; 2 usage.
set -euo pipefail

check() { # <root>
  local root=$1 hits
  hits=$(git -c safe.directory="$root" -C "$root" grep -nIi -e '0x428a2f98' -- '*.c' '*.h' '*.S' || true)
  if [ -n "$hits" ]; then
    printf '%s\n' "$hits" >&2
    echo "no_sha256_copy: a SHA-256 of this tree's own; the digest is libcrypto's (EVP_Digest)" >&2
    return 1
  fi
  echo "no_sha256_copy: SHA-256 is libcrypto's, and no source carries one of its own"
}

if [ "${1:-}" = --self-test ]; then
  t=$(mktemp -d); trap 'rm -rf "$t"' EXIT; fail=0
  git init -q "$t/r"; printf 'int main(void) { return 0; }\n' > "$t/r/a.c"; git -C "$t/r" add -A
  if check "$t/r" >/dev/null 2>&1; then echo "ok   a tree with no digest of its own is green"; else echo "FAIL a clean tree is red"; fail=1; fi
  printf 'static const unsigned k[] = { 0x428A2F98, 0x71374491 };\n' > "$t/r/sha.h"; git -C "$t/r" add -A
  if check "$t/r" >/dev/null 2>&1; then echo "FAIL a planted SHA-256 is green"; fail=1; else echo "ok   a planted SHA-256 is red"; fi
  [ "$fail" = 0 ] && echo "no_sha256_copy self-test: every case as wanted"
  exit "$fail"
fi
check "${1:-.}"
