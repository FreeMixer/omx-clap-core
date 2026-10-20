#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# clap_doc_check.sh — docs/clap-compatibility.md held to the installed CLAP headers and to this tree:
#
#   1. every id the headers spell ("clap.<...>" in a string: the extensions, their draft ids and the factories; the
#      `#include "clap.h"` of all.h is a file name, not an id) has a row: the id in backticks in a table row's first
#      cell. A missing id is named. Rows for ids the installed headers lack (an older CLAP than the desk's) are kept
#      and listed, never failed;
#   2. every id row has exactly 5 cells, and its host-side and plugin-side cells start with one status word:
#      supported, lossy, offered, untested, read, untested, refused, out of scope, unruled, unsupported, or "—" where
#      CLAP has no such half;
#   3. every table cell of the page that says "supported" names a test, and every test it names exists:
#      `tests/<file>` or `lv2/tests/<file>` tracked by git, with `:<function>` a function the file defines when
#      given, or `make <target>` the Makefile defines. A cell that names none, or names one the tree lacks, is named.
#
#   clap_doc_check.sh [--self-test] [--page <file>] [--include <dir holding clap/>]
#
# The headers are pkg-config's for clap, else /usr/include. --self-test runs the check on a copy of the page once clean
# and once per sabotage (an id taken out, a supported cell pointing at a test file the tree lacks, at a function its
# file does not define or only calls, at an untracked file, at a make target the Makefile lacks, a supported cell
# naming no test, a status word outside the list, a row that lost a cell); each must go red and name what it broke.
# Exit 0 held; 1 the page and the headers or the tree disagree; 2 usage, no page or no headers.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
page=$root/docs/clap-compatibility.md
inc=
self=0
while [ $# -gt 0 ]; do
  case $1 in
    --self-test) self=1 ;;
    --page) page=$2; shift ;;
    --include) inc=$2; shift ;;
    *) sed -n '17p' "$0" >&2; exit 2 ;;
  esac
  shift
done
[ -n "$inc" ] || inc=$(pkg-config --variable=includedir clap 2>/dev/null || echo /usr/include)
[ -d "$inc/clap" ] || { echo "clap_doc_check: no CLAP headers at $inc/clap" >&2; exit 2; }

# the ids of the installed headers, one per line
header_ids() {
  { grep -rhoE '"clap\.[A-Za-z0-9./_-]*"' "$inc/clap" || true; } | tr -d '"' | { grep -vE '\.h$' || true; } | sort -u
}

# the page's table rows, one per line: the first cell of the table's header row, then the row's cells, each trimmed,
# separated by the unit separator (an empty cell stays a cell); the header and separator rows are not printed
rows() { # <page>
  awk '
    !/^\|.*\|[[:space:]]*$/ { head = ""; next }
    /^\|[- |]+\|[[:space:]]*$/ { next }
    {
      line = $0
      sub(/^\|/, "", line); sub(/\|[[:space:]]*$/, "", line)
      n = split(line, c, "|"); out = ""
      for (i = 1; i <= n; i++) { gsub(/^[[:space:]]+|[[:space:]]+$/, "", c[i]); out = out (i > 1 ? "\037" : "") c[i] }
      if (head == "") { head = c[1]; next }
      print head "\037" out
    }' "$1"
}

# a file is in the tree: tracked by git, so build output beside the sources never passes; a plain file test only
# where there is no git tree
in_tree() { # <path relative to the root>
  if git -c safe.directory="$root" -C "$root" rev-parse --is-inside-work-tree > /dev/null 2>&1; then
    git -c safe.directory="$root" -C "$root" ls-files --error-unmatch -- "$1" > /dev/null 2>&1
  else
    [ -f "$root/$1" ]
  fi
}

# a test a cell names exists: `tests/<f>[:<fn>]`, `lv2/tests/<f>[:<fn>]` or `make <target>`; prints why not
ref_missing() { # <ref>
  local ref=$1 file fn target
  case $ref in
    make\ *)
      target=${ref#make }
      grep -qE "^([^#:=[:space:]][^:=]*[[:space:]])?${target}([[:space:]][^:=]*)?:([^=]|$)" "$root/Makefile" \
        || echo "make target '$target' is not in the Makefile" ;;
    *)
      file=${ref%%:*}
      fn=; [ "$file" = "$ref" ] || fn=${ref#*:}
      if ! in_tree "$file"; then
        echo "'$file' is not a tracked file of this tree"
      elif [ -n "$fn" ] && ! grep -qE "^(static[[:space:]]+)?[A-Za-z_][A-Za-z0-9_ *]*[[:space:]*]${fn}[[:space:]]*\(" "$root/$file"; then
        echo "'$file' defines no function '$fn'"
      fi ;;
  esac
}

check() { # <page>; prints the counts on success
  local p=$1 bad=0 ids line head first id cell n refs ref why
  local -A row_of=()
  local -a cells
  [ -f "$p" ] || { echo "clap_doc_check: no page $p" >&2; return 2; }
  ids=$(header_ids)
  [ -n "$ids" ] || { echo "clap_doc_check: $inc/clap spells no id" >&2; return 2; }

  local n_rows=0 n_sup=0 n_lossy=0 n_untested=0 n_refused=0 n_oos=0 n_unruled=0 n_unsup=0 n_none=0
  while IFS= read -r line; do
    # the closing separator keeps an empty last cell: read drops one empty field after the last separator
    IFS=$'\037' read -r -a cells <<< "$line"$'\037'
    head=${cells[0]}
    cells=("${cells[@]:1}")
    first=${cells[0]}
    # an id row: a row of a table headed "extension", "factory" or "id", its first cell one backticked id
    if [[ $head =~ ^(extension|factory|id)$ && $first =~ ^\`([a-z][A-Za-z0-9._/-]*)\`$ ]]; then
      id=${BASH_REMATCH[1]}
      row_of[$id]=1
      if [ ${#cells[@]} -ne 5 ]; then
        echo "clap_doc_check: $id: ${#cells[@]} cells, an id row has 5: $(grep -m1 -F -- "| \`$id\` |" "$p" | cut -c1-160)" >&2
        bad=1
      fi
      n_rows=$((n_rows + 1))
      for n in 1 2; do
        cell=${cells[$n]:-}
        case $cell in
          supported*) n_sup=$((n_sup + 1)) ;;
          lossy*) n_lossy=$((n_lossy + 1)) ;;
          "offered, untested"*|"read, untested"*) n_untested=$((n_untested + 1)) ;;
          refused*) n_refused=$((n_refused + 1)) ;;
          "out of scope"*) n_oos=$((n_oos + 1)) ;;
          unruled*) n_unruled=$((n_unruled + 1)) ;;
          unsupported*) n_unsup=$((n_unsup + 1)) ;;
          "—"*) n_none=$((n_none + 1)) ;;
          *) echo "clap_doc_check: $id: cell $n has no status word: '${cell:0:60}'" >&2; bad=1 ;;
        esac
      done
    fi
    # every cell that says supported names a test, and each one it names exists
    for ((n = 1; n < ${#cells[@]}; n++)); do
      cell=${cells[$n]}
      case $cell in supported*) ;; *) continue ;; esac
      refs=$(grep -oE '`(make [a-z0-9][a-z0-9-]*|(lv2/)?tests/[A-Za-z0-9_./-]+(:[A-Za-z_][A-Za-z0-9_]*)?)`' <<< "$cell" | tr -d '`' || true)
      if [ -z "$refs" ]; then
        echo "clap_doc_check: '${first}' says supported and names no test: '${cell:0:80}'" >&2
        bad=1
        continue
      fi
      while IFS= read -r ref; do
        why=$(ref_missing "$ref")
        [ -z "$why" ] || { echo "clap_doc_check: '${first}' names a test that does not exist: $why" >&2; bad=1; }
      done <<< "$refs"
    done
  done < <(rows "$p")

  local missing=() extra=()
  while IFS= read -r id; do [ -n "${row_of[$id]:-}" ] || missing+=("$id"); done <<< "$ids"
  for id in "${!row_of[@]}"; do grep -qxF -- "$id" <<< "$ids" || extra+=("$id"); done
  if [ ${#missing[@]} -gt 0 ]; then
    echo "clap_doc_check: the installed CLAP headers spell ids the page has no row for: ${missing[*]}" >&2
    bad=1
  fi
  [ $bad -eq 0 ] || return 1
  echo "clap_doc_check: $(wc -l <<< "$ids") header ids, each a row; $n_rows id rows ($(( n_rows - $(wc -l <<< "$ids") )) beyond the headers);" \
       "cells: $n_sup supported, $n_lossy lossy, $n_untested offered or read untested, $n_refused refused, $n_oos out of scope," \
       "$n_unruled unruled, $n_unsup unsupported, $n_none no such half"
}

if [ $self -eq 1 ]; then
  scratch=$(mktemp -d); trap 'rm -rf "$scratch"' EXIT
  fail=0
  cp "$page" "$scratch/clean.md"
  check "$scratch/clean.md" > /dev/null || { echo "clap_doc_check self-test: the page as it is is not green" >&2; exit 1; }
  # the first header id the page carries a supported plugin-side cell for, and that cell's first test
  victim=$(header_ids | sed -n 1p)
  sup_line=$(grep -nE '^\| `clap\.[^`]+` \|[^|]*\| supported' "$page" | sed -n 1p | cut -d: -f1)
  [ -n "$sup_line" ] || { echo "clap_doc_check self-test: no row with a supported plugin-side cell to sabotage" >&2; exit 1; }
  sup_ref=$(sed -n "${sup_line}p" "$page" | grep -oE '`(lv2/)?tests/[A-Za-z0-9_./-]+(:[A-Za-z_][A-Za-z0-9_]*)?`' | sed -n 1p)
  [ -n "$sup_ref" ] || { echo "clap_doc_check self-test: line $sup_line names no tests/ file" >&2; exit 1; }
  sup_file=${sup_ref//\`/}; sup_file=${sup_file%%:*}
  sab() { # <name> <what the red must name> <sed program>
    sed -E "$3" "$scratch/clean.md" > "$scratch/sab.md"
    if cmp -s "$scratch/clean.md" "$scratch/sab.md"; then echo "SABOTAGE $1: the sabotage changed nothing"; fail=1; return; fi
    if out=$(check "$scratch/sab.md" 2>&1); then echo "SABOTAGE $1: STAYED GREEN"; fail=1
    elif ! grep -qF -- "$2" <<< "$out"; then echo "SABOTAGE $1: red, but not naming '$2': $out"; fail=1
    else echo "SABOTAGE $1: red, names $2"; fi
  }
  esc() { printf '%s' "$1" | sed -e 's/[.[\*^$/|]/\\&/g'; }
  sab id-removed       "$victim"              "s/\`$(esc "$victim")\`/\`removed\`/g"
  sab test-file-gone   "tests/no_such_test.c" "${sup_line}s|$(esc "$sup_ref")|\`tests/no_such_test.c\`|"
  sab function-gone    "no_such_function"     "${sup_line}s|$(esc "$sup_ref")|\`${sup_file}:no_such_function\`|"
  sab make-target-gone "no-such-target"       "${sup_line}s|$(esc "$sup_ref")|\`make no-such-target\`|"
  sab no-test-named    "names no test"        "${sup_line}s/\`(make [a-z0-9-]+|(lv2\/)?tests\/[A-Za-z0-9_.\/:-]+)\`/a test/g"
  sab status-word      "no status word"       "${sup_line}s/^(\| \`[^\`]+\` \| )/\1maybe /"
  # a row that lost a cell: the plugin-side cell deleted, the note moving into its column
  sab cell-dropped     "an id row has 5"      "${sup_line}s/^(\| \`[^\`]+\` \|[^|]*\|)[^|]*\|/\1/"
  # a function the test file only calls, never defines
  grep -qE '[^A-Za-z0-9_]omx_clap_host_tick[[:space:]]*\(' "$root/$sup_file" \
    || { echo "clap_doc_check self-test: $sup_file never calls omx_clap_host_tick" >&2; exit 1; }
  sab call-site-only   "defines no function 'omx_clap_host_tick'" "${sup_line}s|$(esc "$sup_ref")|\`${sup_file}:omx_clap_host_tick\`|"
  # a file on disk that git does not track, as build output is
  if git -c safe.directory="$root" -C "$root" rev-parse --is-inside-work-tree > /dev/null 2>&1; then
    untracked=$(mktemp "$root/tests/clap_doc_check_untracked_XXXXXX.c")
    trap 'rm -rf "$scratch" "$untracked"' EXIT
    sab untracked-file "${untracked#"$root"/}"  "${sup_line}s|$(esc "$sup_ref")|\`${untracked#"$root"/}\`|"
    rm -f "$untracked"
  else
    echo "SABOTAGE untracked-file: not run, no git tree here"
  fi
  [ $fail -eq 0 ] || exit 1
  echo "clap_doc_check self-test: every sabotage red"
  exit 0
fi
check "$page"
