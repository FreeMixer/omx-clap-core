#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# host-verbs-check.sh — every verb data/verbs.json declares is written in the C sources by its macro: OMX_VERB_<VERB>
# for the word and OMX_VERB_<VERB>_FMT for the line protocol_add_command takes, both of src/omx_clap_core_host_verbs.h,
# which contract-dir-check.sh holds to the data. Never a literal of their own. And no source defines a reply number: it is
# OMX_REPLY_HOST_<NAME>_RESP of src/omx_clap_core_vocabulary.h, the resp data/verbs.json declares for reply.host.<name>.
#
#   host-verbs-check.sh [--self-test] [--root <dir>]
#
# The sources are src/ and lv2/ (their .c and .h, lv2/tests/ left out, and the renders src/omx_clap_core_host_verbs.h and
# src/omx_clap_core_vocabulary.h).
# Outside comments, a string literal that starts with a declared verb's word followed by a space or its end, or that
# holds a declared verb's word followed by a conversion (a format), is a failure; so is a HOST_REPLY_ or
# OMX_REPLY_*_RESP define outside the renders. --self-test runs the check on a copy of the
# sources once clean and once per sabotage, each of which must fail, and two prose mentions, which must not.
# The check proper is tests/host_verbs_source.py. Exit 0 held; 1 a hand-typed verb or a number off its declaration; 2
# usage or no declaration.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
self=0 root=$(dirname "$here")
while [ $# -gt 0 ]; do
  case $1 in
    --self-test) self=1 ;;
    --root) root=$2; shift ;;
    *) sed -n '10p' "$0" >&2; exit 2 ;;
  esac
  shift
done

[ -f "$root/data/verbs.json" ] || { echo "host-verbs-check: no $root/data/verbs.json" >&2; exit 2; }

# the check proper, over the sources under $1: one line per failure on stderr, the count of verbs on stdout
check() {
  local dir=$1
  local files decl=$1/data/verbs.json
  files=$(cd "$dir" && find src lv2 -path lv2/tests -prune -o \( -name '*.c' -o -name '*.h' \) -type f -print | grep -vxE 'src/omx_clap_core_(host_verbs|vocabulary).h' | sort)
  [ -n "$files" ] || { echo "host-verbs-check: no sources under $dir" >&2; return 2; }
  (cd "$dir" && python3 -I "$here/host_verbs_source.py" "$decl" $files)
}

if [ "$self" -eq 0 ]; then
  n=$(check "$root")
  echo "host-verbs-check: src/ and lv2/ type none of the $n verbs data/verbs.json declares, and define no reply number of their own"
  exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
copy() { rm -rf "$work/t"; mkdir -p "$work/t"; cp -r "$root/src" "$root/lv2" "$root/data" "$work/t/"; }
missed=0 arms=0
judge() {  # judge <what> <expect: pass|fail>
  if check "$work/t" >/dev/null 2>"$work/err"; then got=pass; else got=fail; fi
  if [ "$got" != "$2" ]; then
    echo "host-verbs-check: self test: $1 should $2 and did not" >&2; cat "$work/err" >&2; missed=$((missed + 1))
  fi
}
arm() {  # arm <what> <expect: pass|fail> <file> <line appended to it>
  arms=$((arms + 1))
  copy
  printf '%s\n' "$4" >> "$work/t/$3"
  judge "$1" "$2"
}
sed_arm() {  # sed_arm <what> <file> <sed expression>: a change that must fail
  arms=$((arms + 1))
  copy
  sed -i -e "$3" "$work/t/$2"
  cmp -s "$root/$2" "$work/t/$2" && { echo "host-verbs-check: self test: $1: the sed changed nothing" >&2; missed=$((missed + 1)); return; }
  judge "$1" fail
}
copy
check "$work/t" >/dev/null || { echo "host-verbs-check: self test: the clean copy fails" >&2; exit 1; }
arm 'a format typed by hand in a define' fail src/main.c '#define SABOTAGE_FMT "track_info %i %s %s ..."'
arm 'a registration typed by hand' fail src/main.c 'static void sabotage(void) { protocol_add_command("note_drops %i", note_drops_cb); }'
arm 'a verb word typed by hand' fail src/effects.c 'static const char *const sabotage = "param_info";'
arm 'a format inside a longer string' fail lv2/clap/lv2_clap.c 'static const char *const sabotage = "send remote_page_get %i %i";'
arm 'a reply number defined by hand under its old name' fail src/host_verbs.h '#define HOST_REPLY_NO_NOTE_INPUT (-513)'
arm 'a reply number restated under its rendered name' fail src/effects.c '#define OMX_REPLY_HOST_NO_NOTE_INPUT_RESP (-513)'
arm 'a reply number defined by hand under a name of its own' fail src/host_verbs.h '#define HOST_REPLY_NOT_A_CODE (-599)'
arm 'a verb named in a comment' pass src/main.c '/* answers "track_info" lines */'
arm 'a verb named in prose' pass src/main.c 'static const char *const sabotage_prose = "the idle_hold command changes it";'
[ "$missed" -eq 0 ] || exit 1
echo "host-verbs-check: self test: $arms sabotage arms, each as expected"
