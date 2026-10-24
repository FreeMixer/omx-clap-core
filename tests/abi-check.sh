#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The library against the baseline of its last release, with libabigail.
#   abi-check.sh <libomx-clap-core.so.X.Y.Z> <include dir> <baseline.abi>
# An added function or field is compatible and needs a new minor version; a removed or changed one is incompatible and needs a
# new soname major, whose first build records a new baseline. No baseline is the first release: nothing to compare, and the
# file to commit is written next to the check.
set -eu
export LC_ALL=C

lib=$1
headers=$2
baseline=$3
out=$(dirname "$lib")/../../../new.abi

abidw --headers-dir "$headers" --out-file "$out" "$lib"
test -s "$out" || { echo "FAIL abidw wrote nothing for $lib"; exit 1; }

if [ ! -f "$baseline" ]; then
    echo "ok   no baseline $baseline: the first release. Record it with 'make abi-baseline' and commit it."
    exit 0
fi

soname_of() { sed -n "s/.*soname='\([^']*\)'.*/\1/p" "$1" | head -1; }
version_of() { sed -n "s/.*path='[^']*\.so\.\([0-9][0-9.]*\)'.*/\1/p" "$1" | head -1; }

old_soname=$(soname_of "$baseline")
new_soname=$(soname_of "$out")
old_version=$(version_of "$baseline")
new_version=$(version_of "$out")
test -n "$old_soname" && test -n "$new_soname" && test -n "$old_version" && test -n "$new_version" \
    || { echo "FAIL the soname and the version could not be read off the baseline ($old_soname $old_version) and the build ($new_soname $new_version)"; exit 1; }

report=$(dirname "$lib")/../../../abidiff.txt
rc=0
abidiff "$baseline" "$out" > "$report" || rc=$?
cat "$report"
echo "abidiff exit $rc: baseline $old_soname $old_version, build $new_soname $new_version"

if [ "$new_soname" != "$old_soname" ]; then
    echo "ok   a new soname ($old_soname to $new_soname): the new major's first build; record its baseline"
    exit 0
fi
if [ $((rc & 1)) -ne 0 ] || [ $((rc & 2)) -ne 0 ]; then
    echo "FAIL abidiff could not compare"
    exit 1
fi

# abidiff's own exit bits call a removed or moved data member a compatible change (4) when the type is reached through a
# pointer, which every type here is; the rule of the library is stricter, so the report is read: a function or variable
# removed or changed, a data member removed, moved or changed, a size that shrank, an enumerator changed is incompatible.
# "no data member change (N filtered)" is abidiff saying the members it filtered out (the _Atomic ones, #117) did not
# change: an appended field beside them is no change, and abi-layout.sh holds every member's offset itself.
# A member that points to a structure keeps its offset, size and type when that structure grows: abidiff still lists it
# under a "data member change" heading ("type of 'omx_clap_instance* instance' changed", then "in pointed to type
# 'struct omx_clap_instance'"; struct omx_clap_chain_member, #323), with the structure's own changes below it. Every
# member under such a heading is read: one that points to a structure is passed and its structure's lines are judged as
# any others (an insertion is an addition; a deletion, a moved offset, a shrink, a renamed type is not); any other member
# under it (a function pointer whose signature changed, a member retyped in place) keeps the heading, which refuses.
judged=$(dirname "$lib")/../../../abidiff-judged.txt
awk '
    function indent(l) { match(l, /^ */); return RLENGTH }
    { line[NR] = $0 }
    END {
        depth = -1
        for (i = 1; i <= NR; i++) {
            l = line[i]
            if (depth >= 0 && l != "" && indent(l) <= depth)
                depth = -1
            if (l ~ /^ *no data member change/)
                continue
            if (l ~ /^ *[0-9]+ data member changes?( \([0-9]+ filtered\))?:$/) {
                depth = indent(l)
                continue
            }
            # a member entry directly under the heading
            if (depth >= 0 && indent(l) == depth + 2) {
                pointee = line[i + 1]
                if (!(l ~ /^ *type of \x27[^\x27(]*\* [^\x27(]*\x27 changed:$/ && pointee ~ /^ *in pointed to type \x27struct /))
                    print "data member change: " l
            }
            print l
        }
    }
' "$report" > "$judged"
if [ $((rc & 8)) -ne 0 ] \
   || grep -Eq "Removed function|Removed variable|data member deletion|data member change|offset changed|return type changed|parameter [0-9]+ of type .* (changed|was added|was removed)|pointer type changed|type name changed|enumerator|Changed variable" "$judged" \
   || awk '/type size changed from/ { if ($6 + 0 < $4 + 0) bad = 1 } END { exit !bad }' "$judged"; then
    echo "FAIL an incompatible change under the soname $old_soname: a function or a field removed, moved or changed needs a new soname major"
    exit 1
fi
# a report whose changes are all filtered (exit 4 with no addition, no insertion and no size that moved) is no change
if [ $((rc & 4)) -ne 0 ] && ! grep -Eq "[1-9][0-9]* Added (function|variable)|data member insertion|type size changed from" "$judged"; then
    echo "ok   no change in the ABI since $old_version: abidiff's report holds only what it filtered"
    exit 0
fi
if [ $((rc & 4)) -ne 0 ]; then
    if [ "$new_version" = "$old_version" ]; then
        echo "FAIL a compatible change (an addition) under the version $old_version: it needs a new minor"
        exit 1
    fi
    echo "ok   a compatible change, and the version moved $old_version to $new_version"
    exit 0
fi
echo "ok   no change in the ABI since $old_version"
