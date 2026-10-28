#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
# Every extension header the installed CLAP headers carry (clap/ext/*.h and clap/ext/draft/*.h) has a row in
# doc/reference.md's table of CLAP extensions, with a status of supported, not supported or not planned. A row for a
# header the installed CLAP lacks (an older one: Fedora's stock clap-devel trails the one the desk builds with) is kept
# and named, not failed.
#   ext_table_check.sh [<clap include dir>]   (from anywhere; the include dir defaults to pkg-config's for clap)
#   ext_table_check.sh --self-test [<dir>]    drops one table row from a scratch copy of the doc, and requires the check
#                                             to go red
set -euo pipefail
cd "$(dirname "$0")/.."

if [ "${1:-}" = "--self-test" ]; then
  shift
  tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
  n=$(grep -n -m1 -E '^\| `ext/[^`]+\.h` \| (supported|not supported) ' doc/reference.md | cut -d: -f1)
  [ -n "$n" ] || { echo "ext_table_check --self-test: no table row to drop" >&2; exit 1; }
  # a row for an installed header; a stale row would be kept, not failed
  cp doc/reference.md "$tmp/reference.md"
  if ! EXT_TABLE_DOC=doc/reference.md bash "$0" "$@" >/dev/null 2>&1; then
    echo "ext_table_check --self-test: the unbroken doc is already red" >&2; exit 1
  fi
  sed -i "${n}d" "$tmp/reference.md"
  if EXT_TABLE_DOC=$tmp/reference.md bash "$0" "$@" >/dev/null 2>&1; then
    echo "ext_table_check --self-test: the check stayed green with a row dropped" >&2; exit 1
  fi
  echo "ext_table_check --self-test: red with a row dropped"
  exit 0
fi

inc=${1:-$(pkg-config --variable=includedir clap 2>/dev/null || echo /usr/include)}
ext=$inc/clap/ext
[ -d "$ext" ] || { echo "ext_table_check: no $ext" >&2; exit 1; }

# the table's rows: | `ext/<name>.h` | <status> | ...
rows=$(sed -n -E 's/^\| `(ext\/[^`]+\.h)` \| ([^|]+) \|.*$/\1|\2/p' ${EXT_TABLE_DOC:-doc/reference.md})
[ -n "$rows" ] || { echo "ext_table_check: doc/reference.md has no extension table" >&2; exit 1; }

bad=0
while IFS='|' read -r header status; do
  status=${status% }
  case $status in
    "supported"|"not supported"|"not planned"|"not planned in the console") ;;
    *) echo "ext_table_check: $header has the status '$status'" >&2; bad=1 ;;
  esac
done <<< "$rows"

installed=$(cd "$ext/.." && ls ext/*.h ext/draft/*.h 2>/dev/null | sort)
named=$(echo "$rows" | cut -d'|' -f1 | sort)
missing=$(comm -23 <(echo "$installed") <(echo "$named"))
stale=$(comm -13 <(echo "$installed") <(echo "$named"))
dupes=$(echo "$named" | uniq -d)
if [ -n "$missing" ]; then
  echo "ext_table_check: the installed CLAP headers name extensions doc/reference.md's table does not:" >&2
  echo "$missing" >&2
  bad=1
fi
if [ -n "$stale" ]; then
  echo "ext_table_check: rows for headers $inc/clap lacks, kept for a newer CLAP: $(echo $stale)"
fi
if [ -n "$dupes" ]; then
  echo "ext_table_check: the table names a header twice:" >&2
  echo "$dupes" >&2
  bad=1
fi
[ $bad -eq 0 ] || exit 1
echo "ext_table_check: $(echo "$installed" | wc -l) extension headers, each in the table"
