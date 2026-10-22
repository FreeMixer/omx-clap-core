#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# adapter-seam.sh: the numbers of docs/adapter-seam-and-vst3.md, part 1: every function of the LV2 adapter and of the
# src/ headers it includes, classed FORMAT-NEUTRAL, MIXED or FORMAT-SPECIFIC, with its NLOC, per file and per area.
#
#   adapter-seam.sh                  measure OURS_REF (default: the "# ref:" line of the table) and print every table
#   adapter-seam.sh --list           also print each function with its class, area and thread
#   adapter-seam.sh --vst3-packages  the repository count of VST3 plugin packages with and without an LV2/CLAP build
#                                    (needs dnf; the count of the report was made on Fedora 44 with audinux enabled)
#
# The class of each function is a judgement read from its body, kept in docs/adapter-seam.tsv (file, function, class,
# area, thread, reason). This script only measures and sums, the way tools/hosting-compare.sh of #267 does: lizard
# (LIZARD, default the "lizard" console script on PATH, falling back to "python3 -m lizard") for the functions and
# their NLOC. A function the table does not class, or a row whose function is gone, is a failure: the table moves
# with the code or the numbers stop being reproducible.
# Exit 0 printed, 1 a tool, a ref or a row is missing.
set -euo pipefail

# a pipx-style install puts only the "lizard" script on PATH, not the module on python3's sys.path
LIZARD=${LIZARD:-$(command -v lizard || true)}
LIZARD=${LIZARD:-python3 -m lizard}
REPO=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)
TABLE=$REPO/docs/adapter-seam.tsv
MODE=${1:-}
# the ref the classes were read from: later code may add or drop functions the table has not seen
OURS_REF=${OURS_REF:-$(sed -n 's/^# ref: *\([0-9a-f]*\).*/\1/p' "$TABLE" 2>/dev/null | head -1)}
OURS_REF=${OURS_REF:-HEAD}

die() { echo "adapter-seam: $*" >&2; exit 1; }

if [ "$MODE" = --vst3-packages ]; then
    command -v dnf >/dev/null || die "dnf not found"
    tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
    dnf repoquery --qf '%{name}\n' 2>/dev/null | grep -Ev 'debug(info|source)$' | sort -u > "$tmp/all"
    grep -E '^(vst3|lv2|clap)-' "$tmp/all" > "$tmp/fmt" || true
    # a vst3-<x> package; its family is <x>, case-folded; the LV2 or CLAP build is lv2-<x>, clap-<x>, or <x> itself
    # shipping files under a lv2/ or clap/ directory
    awk -F- '{k=$1; sub(/^[^-]*-/, ""); print tolower($0), k}' "$tmp/fmt" | sort -u > "$tmp/fam"
    awk '$2 == "vst3" {print $1}' "$tmp/fam" | sort -u > "$tmp/v"
    awk '$2 != "vst3" {print $1}' "$tmp/fam" | sort -u > "$tmp/o"
    comm -23 "$tmp/v" "$tmp/o" | grep -vx vst3sdk > "$tmp/only" || true
    : > "$tmp/bundled"
    while read -r b; do
        base=$(grep -ix "$b" "$tmp/all" | head -1) || true
        [ -n "$base" ] || continue
        if dnf repoquery -l "$base" 2>/dev/null | grep -Eq '/(lv2|clap)/'; then echo "$b" >> "$tmp/bundled"; fi
    done < "$tmp/only"
    echo "vst3-* plugin packages: $(grep -vxc vst3sdk "$tmp/v")"
    echo "  with an lv2-* or clap-* package of the same name: $(comm -12 "$tmp/v" "$tmp/o" | wc -l)"
    echo "  without one, but the base package ships LV2 or CLAP files: $(wc -l < "$tmp/bundled") ($(tr '\n' ' ' < "$tmp/bundled"))"
    echo "  VST3-only by name and file list: $(comm -23 "$tmp/only" <(sort "$tmp/bundled") | wc -l)"
    comm -23 "$tmp/only" <(sort "$tmp/bundled") | tr '\n' ' ' | fold -s -w 116 | sed 's/^/    /'; echo
    echo "lv2-* packages: $(grep -c '^lv2-' "$tmp/fmt"), clap-* packages: $(grep -c '^clap-' "$tmp/fmt")"
    exit 0
fi

$LIZARD --version >/dev/null 2>&1 || die "lizard not found (LIZARD=..., pip install lizard)"
[ -f "$TABLE" ] || die "no $TABLE"
git -C "$REPO" cat-file -e "$OURS_REF^{commit}" 2>/dev/null || die "this repo has no $OURS_REF"

tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
git -C "$REPO" archive "$OURS_REF" lv2/core lv2/clap src | tar -x -C "$tmp"
cd "$tmp"
# the measured files: lv2/core, lv2/clap, and the src/ headers they include (the "ours+plumbing" set of hosting-compare)
{
    ls lv2/core/*.[ch] lv2/clap/*.[ch]
    grep -ho '#include "[^"]*"' lv2/core/*.[ch] lv2/clap/*.[ch] | sed 's/#include "\(.*\)"/\1/' | sort -u |
        while read -r h; do if [ -f "src/$h" ]; then echo "src/$h"; fi; done
} > files
# shellcheck disable=SC2046
{ $LIZARD $(tr '\n' ' ' < files) 2>/dev/null || true; } |
    awk '$6 ~ /@[0-9]+-[0-9]+@/ && !seen[$6]++ {split($6, a, "@"); print a[3] "\t" a[1] "\t" a[2] "\t" $1 "\t" $2}' |
    sort > funcs                                    # file, name, start-end, NLOC, CCN
grep -v '^#' "$TABLE" | awk -F'\t' 'NF >= 5 {print $1 "\t" $2 "\t" $3 "\t" $4 "\t" $5}' | sort > classes
join -t $'\t' -j1 <(awk -F'\t' '{print $1 "|" $2 "\t" $3 "\t" $4 "\t" $5}' funcs | sort) \
                  <(awk -F'\t' '{print $1 "|" $2 "\t" $3 "\t" $4 "\t" $5}' classes | sort) > joined
missing=$(join -t $'\t' -v1 <(cut -f1,2 funcs | tr '\t' '|' | sort) <(cut -f1,2 classes | tr '\t' '|' | sort) || true)
stale=$(join -t $'\t' -v2 <(cut -f1,2 funcs | tr '\t' '|' | sort) <(cut -f1,2 classes | tr '\t' '|' | sort) || true)
if [ -n "$missing$stale" ]; then
    [ -z "$missing" ] || { echo "functions docs/adapter-seam.tsv does not class:"; tr '|' ' ' <<< "$missing" | sed 's/^/  /'; }
    [ -z "$stale" ] || { echo "rows of docs/adapter-seam.tsv whose function is gone:"; tr '|' ' ' <<< "$stale" | sed 's/^/  /'; }
    exit 1
fi
bad=$(awk -F'\t' '$5 !~ /^(NEUTRAL|MIXED|SPECIFIC)$/ || $7 !~ /^(AUDIO|MAIN|WORKER|ANY)$/ {print $1}' joined)
[ -z "$bad" ] || die "a class or thread that is not NEUTRAL/MIXED/SPECIFIC, AUDIO/MAIN/WORKER/ANY: $bad"

echo "# adapter-seam: ours $(git -C "$REPO" rev-parse --short "$OURS_REF") ($OURS_REF), lizard $($LIZARD --version)," \
    "$(wc -l < funcs) functions in $(wc -l < files) files"
echo
cell() { # <awk filter> -> "NLOC (functions)" per class, then the total
    awk -F'\t' "$1"' {t[$5] += $3; n[$5]++; T += $3; N++}
        END {printf "%d (%d) | %d (%d) | %d (%d) | %d (%d)", t["NEUTRAL"], n["NEUTRAL"], t["MIXED"], n["MIXED"],
             t["SPECIFIC"], n["SPECIFIC"], T, N}' joined
}
echo "## by file (NLOC (functions))"
echo
echo '| file | NEUTRAL | MIXED | SPECIFIC | total |'
echo '|---|---:|---:|---:|---:|'
while read -r f; do
    grep -q "^$f|" joined || continue
    echo "| $f | $(cell "\$1 ~ /^${f//\//\\/}\\|/") |"
done < files
echo "| **all** | $(cell 1) |"
echo
echo "## by area (NLOC (functions))"
echo
echo '| area | NEUTRAL | MIXED | SPECIFIC | total |'
echo '|---|---:|---:|---:|---:|'
cut -f6 joined | sort -u | while read -r a; do echo "| $a | $(cell "\$6 == \"$a\"") |"; done
echo
echo "## by thread (NLOC (functions))"
echo
echo '| thread | NEUTRAL | MIXED | SPECIFIC | total |'
echo '|---|---:|---:|---:|---:|'
for t in AUDIO ANY WORKER MAIN; do echo "| $t | $(cell "\$7 == \"$t\"") |"; done
echo
echo "## the audio-thread functions (file:line NLOC class, and how each is defined)"
echo
sort -t$'\t' -k1,1 joined | awk -F'\t' '$7 == "AUDIO" {split($1, k, "|"); split($2, r, "-"); print k[1], r[1], $3, $5, k[2]}' |
    while read -r f l n c name; do
        def=$(sed -n "$((l > 1 ? l - 1 : 1)),${l}p" "$f" | tr '\n' ' ')
        case $def in *"static inline"*) how="static inline" ;; *static*) how="static" ;; *) how="extern" ;; esac
        printf '  %-28s %s:%s %4s %-8s %s\n' "$name" "$f" "$l" "$n" "$c" "$how"
    done
if [ "$MODE" = --list ]; then
    echo
    echo "## every function (file:lines NLOC CCN class area thread name)"
    sort -t$'\t' -k1,1 joined | awk -F'\t' '{split($1, k, "|"); printf "  %s:%s %d %d %s %s %s %s\n", k[1], $2, $3, $4, $5, $6, $7, k[2]}'
fi
