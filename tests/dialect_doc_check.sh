#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
# The message types a note input passes, as src/clap_stage.h says them and as doc/reference.md says them: the lines that
# start "Passes, <dialects>:" or "Counts, <dialects>:" must be the same set in both files, and there must be some.
#   dialect_doc_check.sh        (from the root of a checkout)
set -euo pipefail
cd "$(dirname "$0")/.."

lines() { sed -n -E 's/^[ *]*((Passes|Counts), [^:]+: .*)$/\1/p' "$1" | sort; }

code=$(lines src/clap_stage.h)
docs=$(lines doc/reference.md)
[ -n "$code" ] || { echo "dialect_doc_check: src/clap_stage.h lists no Passes/Counts line" >&2; exit 1; }
if [ "$code" != "$docs" ]; then
  echo "dialect_doc_check: doc/reference.md and src/clap_stage.h disagree on the message types passed" >&2
  diff <(echo "$code") <(echo "$docs") >&2 || true
  exit 1
fi
echo "dialect_doc_check: $(echo "$code" | wc -l) dialect lines agree"
