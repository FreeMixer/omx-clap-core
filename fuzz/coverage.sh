#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# THE COVERAGE WITNESS (make fuzz-coverage). Each harness, built for llvm-cov without sanitizers, has run once over its
# seed corpus (-runs=0 runs the corpus and nothing else); this prints the line coverage of the reader functions the
# harness exists to drive, and every line of them no seed reached. A file's other functions (lv2_locate's directory
# walk) are not the harness's and are not counted.
#
# THE FLOOR. fuzz/coverage-floor.txt holds, for each harness, the number of reader lines its committed corpus covers.
# A harness below its floor turns this red: a seed lost, a harness that stopped driving a function, or a reader that grew
# lines its corpus never reaches. Raise a floor when coverage rises; lower one only with the change that removed lines.
#
# THE HARNESS FLOOR. Every property check of the harness, each abort() of fuzz/fuzz_<harness>.c and the statement that
# guards it (fuzz/checks.awk), must be executed by at least one seed: a check no seed reaches is a property nobody
# tests, and the reader's line count cannot see it. Each such check is named by file and line, and turns this red.
#
# THE SEED FLOOR. A line counts as covered when any seed runs it, so a branch on a line other seeds reach is invisible
# to the floor: the seed written to take it can go and the floor holds. fuzz/reach.txt names such a branch and its seed;
# the seed is run alone and must take the branch's true side, or the branch is named by file, line and column and this
# turns red.
#
# Usage: LLVM_COV=llvm-cov LLVM_PROFDATA=llvm-profdata coverage.sh <cov build dir> <harness>...
set -eu
B=$1
shift
harnesses="$*"
LLVM_COV=${LLVM_COV:-llvm-cov}
LLVM_PROFDATA=${LLVM_PROFDATA:-llvm-profdata}
FLOOR=fuzz/coverage-floor.txt
REACH=fuzz/reach.txt
[ -f "$FLOOR" ] || { echo "coverage.sh: no $FLOOR" >&2; exit 2; }
[ -f "$REACH" ] || { echo "coverage.sh: no $REACH" >&2; exit 2; }
low=
dead=
unseeded=

# the reader functions each harness covers, its files then `::` then the functions; a static is matched by bare name
funcs() {
    case $1 in
    preset) echo src/preset_file.c :: skip_ws grow hex4 put_utf8 parse_string parse_number parse_items literal \
        parse_value pj_parse pj_free pj_get pj_get_str pj_set pj_string pj_write write_value put puts_ put_string \
        indent ;;
    state) echo src/preset_file.c :: preset_base64_encode preset_base64_decode ;;
    manifest) echo src/lv2_locate.c :: name_char plain_local has_prefixed lv2_locate_manifest_names \
        lv2_locate_manifest_readings ;;
    *) echo "coverage.sh: no function list for harness '$1'" >&2; exit 2 ;;
    esac
}

for h in "$@"; do
    set -- $(funcs "$h")
    files=
    while [ "$1" != "::" ]; do files="$files $1"; shift; done
    shift
    bin=$B/fuzz_$h prof=$B/$h.profdata tot=$B/$h.covered
    echo "== fuzz_$h: $(ls fuzz/corpus/$h | wc -l) seeds"
    # Name Regions Miss Cover Lines Miss Cover ...: the listed functions only
    "$LLVM_COV" report -show-functions "$bin" -instr-profile="$prof" $files |
    awk -v want="$*" -v tot="$tot" '
        BEGIN { n = split(want, w, " "); for (i = 1; i <= n; i++) keep[w[i]] = 1 }
        { name = $1; sub(/^.*:/, "", name) }
        (name in keep) && NF >= 10 {
            printf "  %-28s lines %4d  missed %3d  %s\n", name, $5, $6, $7
            lines += $5; missed += $6; seen[name] = 1
        }
        END {
            for (k in keep) if (!(k in seen)) printf "  %-28s NOT FOUND in the report\n", k
            printf "  %-28s lines %4d  missed %3d  %.2f%%\n", "TOTAL", lines, missed,
                   lines ? 100 * (lines - missed) / lines : 0
            print lines - missed > tot
        }'
    for f in "$@"; do
        "$LLVM_COV" show "$bin" -instr-profile="$prof" -name-regex="^(.*:)?$f\$" $files 2>/dev/null |
        awk -v f="$f" -F'|' '$2 ~ /^ *0$/ { sub(/^ */, "", $1); printf "    unreached %s:%s %s\n", f, $1, $3 }'
    done
    src=fuzz/fuzz_$h.c
    "$LLVM_COV" show "$bin" -instr-profile="$prof" "$src" | awk -v file="$src" -f fuzz/checks.awk >"$B/$h.checks"
    nchk=$(wc -l <"$B/$h.checks")
    if [ "$nchk" -eq 0 ]; then
        echo "  CHECKS: no property check found in $src"
        dead="$dead $h"
    elif awk -F: '$4 == "0" { n++ } END { exit !n }' "$B/$h.checks"; then
        awk -F: '$4 == "0" { t = $0; sub(/^([^:]*:){4}/, "", t)
            printf "  CHECK: %s:%d no seed reaches (abort at line %d): %s\n", $1, $2, $3, t }' "$B/$h.checks"
        dead="$dead $h"
    else
        echo "  checks: all $nchk property checks of $src reached"
    fi
    covered=$(cat "$tot")
    floor=$(awk -v h="$h" '$1 == h { print $2 }' "$FLOOR")
    if [ -z "$floor" ]; then
        echo "  FLOOR: fuzz_$h has no line in $FLOOR"
        low="$low $h"
    elif [ "$covered" -lt "$floor" ]; then
        echo "  FLOOR: fuzz_$h covers $covered reader lines, below its floor of $floor"
        low="$low $h"
    else
        echo "  floor $floor: held ($covered covered)"
    fi
done

# the seed floor: each branch of fuzz/reach.txt for a harness run here, its seed alone
echo "== the seed floor ($REACH)"
grep -v -e '^#' -e '^[[:space:]]*$' "$REACH" | while read -r h seed file cond; do
    case " $harnesses " in *" $h "*) ;; *) continue ;; esac
    if [ ! -f "fuzz/corpus/$h/$seed" ]; then
        echo "  REACH: fuzz/corpus/$h/$seed is gone, and with it the seed of $file: $cond"
        echo "$h/$seed" >>"$B/reach.red"
        continue
    fi
    # the line and column the condition starts at, the place llvm-cov names its branch by
    at=$(awk -v c="$cond" 'index($0, c) { n++; at = NR ":" index($0, c) } END { if (n == 1) print at }' "$file")
    if [ -z "$at" ]; then
        echo "  REACH: $file holds the condition of $h/$seed not once but none or several times: $cond"
        echo "$h/$seed" >>"$B/reach.red"
        continue
    fi
    raw=$B/reach-$h-$seed.profraw prof=$B/reach-$h-$seed.profdata
    rm -f "$raw"
    LLVM_PROFILE_FILE=$raw "$B/fuzz_$h" "fuzz/corpus/$h/$seed" >"$B/reach-$h-$seed.log" 2>&1 ||
        { echo "  REACH: fuzz_$h failed on $seed alone (log in $B/reach-$h-$seed.log)"; echo "$h/$seed" >>"$B/reach.red"; continue; }
    "$LLVM_PROFDATA" merge -sparse "$raw" -o "$prof" ||
        { echo "  REACH: no profile of $seed alone"; echo "$h/$seed" >>"$B/reach.red"; continue; }
    # "|  Branch (L:C): [True: N, False: M]"; N is 0 when the seed never takes it
    taken=$("$LLVM_COV" show -show-branches=count "$B/fuzz_$h" -instr-profile="$prof" "$file" |
        awk -v at="($at)" '$2 == "Branch" && $3 == at":" { t = $5; sub(/,$/, "", t); print t; exit }')
    if [ -z "$taken" ]; then
        echo "  REACH: $file:$at is no branch llvm-cov knows: $cond"
        echo "$h/$seed" >>"$B/reach.red"
    elif [ "$taken" = 0 ]; then
        echo "  REACH: $file:$at $seed does not take it: $cond"
        echo "$h/$seed" >>"$B/reach.red"
    else
        echo "  reach $h/$seed: $file:$at taken $taken times: $cond"
    fi
done
if [ -f "$B/reach.red" ]; then
    unseeded=$(tr '\n' ' ' <"$B/reach.red" | sed 's/ $//')
    rm -f "$B/reach.red"
fi

[ -z "$low" ] || echo "make fuzz-coverage: RED -- below the coverage floor:$low"
[ -z "$dead" ] || echo "make fuzz-coverage: RED -- harness checks no seed reaches:$dead"
[ -z "$unseeded" ] || echo "make fuzz-coverage: RED -- branches their seed does not take: $unseeded"
[ -z "$low$dead$unseeded" ] || exit 1
echo "make fuzz-coverage: every harness at or above its coverage floor, every check and seeded branch reached"
