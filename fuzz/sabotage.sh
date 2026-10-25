#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# make fuzz-sabotage: A PROPERTY NO MUTANT CAN BREAK CHECKS NOTHING. fuzz/mutants/<harness>-<property>.patch is a
# one-line bug in the reader the harness drives, one for each property the harness checks. This applies each, rebuilds
# the harness, and runs it over every seed of its corpus, one seed at a time, no fuzzing: the corpus alone must find it.
# A mutant is found when at least one seed makes the harness abort and every seed that does aborts in
# check_<property> (or a check_<property>_<part>): the innermost check_ function of the stack is that one, and the
# crash is an abort(), not a sanitizer report. A seed that crashes anywhere else makes the mutant red, since the mutant
# then breaks a property it does not name. The source is restored with `git checkout --` on every exit path, and the
# sanitizer build is rebuilt as the tree is at the end.
#
# Usage: FUZZ_BUILD=build/fuzz MAKE=make sabotage.sh
set -u
ROOT=$(cd "$(dirname "$0")/.." && pwd)
cd "$ROOT"
B=${FUZZ_BUILD:-build/fuzz}
MAKE=${MAKE:-make}
OUT=$B/sabotage
mkdir -p "$OUT"

patched=
restore() {
    [ -n "$patched" ] && git checkout -- $patched
    patched=
}
trap 'restore' EXIT
trap 'restore; exit 130' INT TERM

if ! git diff --quiet -- src; then
    echo "fuzz-sabotage: src/ has uncommitted changes; refusing to patch over them" >&2
    exit 2
fi

missed=
n=0
for p in fuzz/mutants/*.patch; do
    [ -f "$p" ] || continue
    m=$(basename "$p" .patch)
    h=${m%%-*}
    prop=${m#*-}
    fn=check_$(echo "$prop" | tr - _)
    n=$((n + 1))
    files=$(git apply --numstat "$p" | awk '{ print $3 }')
    if ! git apply "$p"; then
        echo "$m: $p no longer applies"
        missed="$missed $m"
        continue
    fi
    patched=$files
    # the mutant's line, or the line it drops when it only drops one
    echo "== $m: fuzz_$h against $(grep -m1 '^+[^+]' "$p" | sed 's/^+[[:space:]]*//' | grep . ||
        grep -m1 '^-[^-]' "$p" | sed 's/^-[[:space:]]*/without: /')"
    if ! $MAKE --no-print-directory "$B/fuzz_$h" >"$OUT/$m.build.log" 2>&1; then
        cat "$OUT/$m.build.log"
        echo "$m: the mutant does not build"
        missed="$missed $m"
        restore
        continue
    fi
    found=0
    first=
    other=
    for seed in fuzz/corpus/$h/*; do
        log=$OUT/$m.$(basename "$seed").log
        "$B/fuzz_$h" -runs=1 -artifact_prefix="$OUT/$m-" "$seed" >"$log" 2>&1 && { rm -f "$log"; continue; }
        # the innermost check_ function of the stack, and whether the harness aborted
        where=$(grep -m1 -oE ' in check_[a-z_]+' "$log" | sed 's/^ in //')
        if grep -q 'ERROR: libFuzzer: deadly signal' "$log" && echo "$where" | grep -qE "^$fn(_[a-z]+)*\$"; then
            found=$((found + 1))
            [ -n "$first" ] || first=$(basename "$seed")
        else
            other="$other $(basename "$seed")(${where:-$(grep -m1 -E '^SUMMARY: ' "$log")})"
        fi
    done
    restore
    if [ "$found" -gt 0 ] && [ -z "$other" ]; then
        echo "$m: found by $found seeds, first $first, every one in $fn"
    elif [ -n "$other" ]; then
        echo "$m: seeds that crashed outside $fn:$other"
        echo "$m: RED -- the mutant breaks a property it does not name (logs in $OUT/)"
        missed="$missed $m"
    else
        echo "$m: MUTANT NOT FOUND -- no seed breaks $fn"
        missed="$missed $m"
    fi
done

# leave the sanitizer build as the tree is, not as the last mutant was
$MAKE --no-print-directory fuzz >"$OUT/rebuild.log" 2>&1 || { cat "$OUT/rebuild.log"; exit 1; }

if [ "$n" -eq 0 ]; then
    echo "make fuzz-sabotage: RED -- no mutant in fuzz/mutants/"
    exit 1
fi
if [ -n "$missed" ]; then
    echo "make fuzz-sabotage: RED --$missed"
    exit 1
fi
echo "make fuzz-sabotage: all $n mutants found, each only by its own property"
