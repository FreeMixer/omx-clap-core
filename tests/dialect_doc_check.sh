#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
# The message types a note input passes, as src/clap_stage.h says them and as doc/reference.md says them: the lines that
# start "Passes, <dialects>:" or "Counts, <dialects>:" must be the same set in both files, and there must be some.
#   dialect_doc_check.sh              (from anywhere)
#   dialect_doc_check.sh --self-test  drops one such line from a scratch copy of the doc and of the header in turn and
#                                     requires the check to go red each time
set -euo pipefail
cd "$(dirname "$0")/.."

if [ "${1:-}" = "--self-test" ]; then
  tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
  for victim in doc header; do
    cp doc/reference.md "$tmp/reference.md"; cp src/clap_stage.h "$tmp/clap_stage.h"
    f=$tmp/reference.md; [ $victim = header ] && f=$tmp/clap_stage.h
    n=$(grep -n -m1 -E '^[ *]*(Passes|Counts), [^:]+: ' "$f" | cut -d: -f1)
    [ -n "$n" ] || { echo "dialect_doc_check --self-test: no Passes/Counts line in the $victim to drop" >&2; exit 1; }
    sed -i "${n}d" "$f"
    if DIALECT_HEADER=$tmp/clap_stage.h DIALECT_DOC=$tmp/reference.md bash "$0" >/dev/null 2>&1; then
      echo "dialect_doc_check --self-test: the check stayed green with a line dropped from the $victim" >&2; exit 1
    fi
  done
  echo "dialect_doc_check --self-test: red with a line dropped from either side"
  exit 0
fi

lines() { sed -n -E 's/^[ *]*((Passes|Counts), [^:]+: .*)$/\1/p' "$1" | sort; }

hdr=${DIALECT_HEADER:-src/clap_stage.h}
doc=${DIALECT_DOC:-doc/reference.md}
code=$(lines "$hdr")
docs=$(lines "$doc")
[ -n "$code" ] || { echo "dialect_doc_check: $hdr lists no Passes/Counts line" >&2; exit 1; }
if [ "$code" != "$docs" ]; then
  echo "dialect_doc_check: doc/reference.md and src/clap_stage.h disagree on the message types passed" >&2
  diff <(echo "$code") <(echo "$docs") >&2 || true
  exit 1
fi
echo "dialect_doc_check: $(echo "$code" | wc -l) dialect lines agree"
