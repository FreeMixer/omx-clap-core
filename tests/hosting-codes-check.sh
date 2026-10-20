#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# hosting-codes-check.sh — every hosting code the C sources write is omx-contract's: an OMX_HOSTING_* macro of
# omxcontract/omx_vocabulary.h, never a literal of their own.
#
#   hosting-codes-check.sh [--self-test] [--root <dir>] [<omx_vocabulary.h>]
#
# The sources are src/ and lv2/ (their .c and .h, lv2/tests/ left out: a test compares against the literal on purpose,
# an oracle the macro cannot be). Outside comments, a string literal holding a hosting.<word> is a failure, and so is an
# OMX_HOSTING_ name the header does not define. The header is the one the compiler finds ($CC, $CFLAGS and CPATH, the
# way the build finds it) unless named. --self-test runs the check on a copy of the sources once clean and once per
# sabotage: a hand-typed code in a define, one inside a longer string, a name the header lacks, each of which must fail;
# and a quoted code inside a comment, which must not.
# Exit 0 held; 1 a hand-typed or undeclared code; 2 usage or no header.
set -euo pipefail

self=0 root=$(cd "$(dirname "$0")/.." && pwd) header=
while [ $# -gt 0 ]; do
  case $1 in
    --self-test) self=1 ;;
    --root) root=$2; shift ;;
    -*) sed -n '8p' "$0" >&2; exit 2 ;;
    *) header=$1 ;;
  esac
  shift
done

if [ -z "$header" ]; then
  # shellcheck disable=SC2086
  header=$(printf '#include <omxcontract/omx_vocabulary.h>\n' | ${CC:-cc} ${CFLAGS:-} -E -x c - 2>/dev/null \
    | sed -n 's/^# [0-9]* "\(.*omxcontract\/omx_vocabulary\.h\)".*/\1/p' | head -1) || true
fi
[ -n "$header" ] && [ -f "$header" ] || { echo "hosting-codes-check: no omxcontract/omx_vocabulary.h (omx-contract-devel 2.19.0 or later)" >&2; exit 2; }

# the check proper, over the sources under $1: one line per failure on stderr, the count of codes used on stdout
check() {
  local dir=$1
  local files
  files=$(cd "$dir" && find src lv2 -path lv2/tests -prune -o \( -name '*.c' -o -name '*.h' \) -type f -print | sort)
  [ -n "$files" ] || { echo "hosting-codes-check: no sources under $dir" >&2; return 2; }
  (cd "$dir" && perl -e '
    my $header = shift;
    open(my $h, "<", $header) or die "hosting-codes-check: $header: $!\n";
    my %declared;
    while (<$h>) { $declared{$1} = 1 if /^#define (OMX_HOSTING_[A-Z0-9_]+) "/; }
    die "hosting-codes-check: $header defines no OMX_HOSTING_ code\n" unless %declared;
    my ($bad, %used) = (0);
    for my $f (@ARGV) {
      open(my $in, "<", $f) or die "hosting-codes-check: $f: $!\n";
      local $/; my $text = <$in>;
      my $line = 1;
      # token by token: a string literal, a character constant, a comment, an identifier, anything else
      while ($text =~ m{\G(?:("(?:\\.|[^"\\\n])*")|(\x27(?:\\.|[^\x27\\\n])*\x27)|(/\*.*?\*/)|(//[^\n]*)|([A-Za-z_][A-Za-z0-9_]*)|(.))}gs) {
        my ($tok, $str, $id) = ($&, $1, $5);
        if (defined $str && $str =~ /(?<![A-Za-z0-9_.-])(hosting\.[a-z][a-z0-9.-]*)/) {
          print STDERR "hosting-codes-check: $f:$line: \"$1\" is hand-typed: write the OMX_HOSTING_ macro of omxcontract/omx_vocabulary.h\n";
          $bad = 1;
        } elsif (defined $id && $id =~ /^OMX_HOSTING_/) {
          $used{$id} = 1;
          unless ($declared{$id}) {
            print STDERR "hosting-codes-check: $f:$line: $id is no code omx-contract declares ($header)\n";
            $bad = 1;
          }
        }
        $line += ($tok =~ tr/\n//);
      }
    }
    exit 1 if $bad;
    print scalar(keys %used), "\n";
  ' "$header" $files)
}

if [ "$self" -eq 0 ]; then
  n=$(check "$root")
  echo "hosting-codes-check: src/ and lv2/ write no hosting code by hand; the $n they name are omx-contract's ($header)"
  exit 0
fi

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
copy() { rm -rf "$work/t"; mkdir -p "$work/t"; cp -r "$root/src" "$root/lv2" "$work/t/"; }
missed=0 arms=0
arm() {  # arm <what> <expect: pass|fail> <file> <line appended to it>
  arms=$((arms + 1))
  copy
  printf '%s\n' "$4" >> "$work/t/$3"
  if check "$work/t" >/dev/null 2>"$work/err"; then got=pass; else got=fail; fi
  if [ "$got" != "$2" ]; then
    echo "hosting-codes-check: self test: $1 should $2 and did not" >&2; cat "$work/err" >&2; missed=$((missed + 1))
  fi
}
copy
check "$work/t" >/dev/null || { echo "hosting-codes-check: self test: the clean copy fails" >&2; exit 1; }
arm 'a hand-typed code in a define' fail src/effects.c '#define SABOTAGE_CODE "hosting.features.missing"'
arm 'a hand-typed code in lv2/core' fail lv2/core/lv2_core.h '#define LV2_CODE_SABOTAGE "hosting.state.unreadable"'
arm 'a code inside a longer string' fail src/main.c 'static const char *sabotage = "resp -102 hosting.clap.note-input";'
arm 'a code the core lacks' fail lv2/clap/lv2_clap.c 'static const char *sabotage = OMX_HOSTING_NOT_A_CODE;'
arm 'a quoted code in a comment' pass src/clap_host.c '/* refused as "hosting.clap.note-input" */'
arm 'a code named in a line comment' pass src/clap_host.h '// the "hosting.output.non-finite" refusal'
[ "$missed" -eq 0 ] || exit 1
echo "hosting-codes-check: self test: $arms sabotage arms, each as expected"
