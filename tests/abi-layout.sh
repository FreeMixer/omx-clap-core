#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# The layout of every public structure, member by member, against the one recorded at the last release, read with
# pahole from the debug info of an object that includes the installed headers. abidw leaves _Atomic members out of its
# dump (#117), so abi-check.sh would pass a counter of struct omx_clap_stage added, removed or moved; this check sees
# every member, atomic or not.
#   abi-layout.sh <include dir> <baseline.layout> <version>             compare
#   abi-layout.sh <include dir> <baseline.layout> <version> --record    write the baseline
# CC and CPPFLAGS name the compiler and the flags the headers need (clap's and omx-dsp's include directories).
# Rules, as abi-check.sh's: a member removed, moved, resized or retyped, or a structure that shrank, is incompatible
# and needs a new soname major; a member or a structure added, or a structure that grew, is compatible and needs a new
# version.
set -eu
export LC_ALL=C

inc=$1
baseline=$2
version=$3
record=${4:-}
work=$(dirname "$baseline")/../build/abi-layout
mkdir -p "$work"
cc=${CC:-cc}

command -v pahole >/dev/null || { echo "FAIL pahole is not installed (dwarves): the layout cannot be read"; exit 1; }

# the structures and unions the installed headers define, by tag
names=$(cat "$inc"/omx-clap-host/*.h | sed -n -E 's/^(typedef )?(struct|union) ([A-Za-z_0-9]+)[[:space:]]*\{?[[:space:]]*$/\2 \3/p' | sort -u)
test -n "$names" || { echo "FAIL no structure found under $inc/omx-clap-host"; exit 1; }

# one object that includes every header and holds one of each, so its debug info describes them all
{
    for h in "$inc"/omx-clap-host/*.h; do echo "#include \"$(basename "$h")\""; done
    echo "$names" | while read -r kind tag; do echo "__attribute__((used)) static $kind $tag abi_layout_$tag;"; done
} > "$work/types.c"
# shellcheck disable=SC2086
$cc -std=gnu99 -D_GNU_SOURCE -g -O0 -c -w -I"$inc/omx-clap-host" ${CPPFLAGS:-} -o "$work/types.o" "$work/types.c"

# struct <tag> size <n>, then member <tag> <name> <offset> <size> <type>, each at the top level of its structure
: > "$work/new.layout"
echo "version $version" >> "$work/new.layout"
echo "$names" | while read -r kind tag; do
    pahole -C "$tag" "$work/types.o" > "$work/one.txt" || { echo "FAIL pahole cannot read $kind $tag"; exit 1; }
    awk -v kind="$kind" -v tag="$tag" '
        /^\t[^\t\/].*;[ \t]*\/\*/ {
            line = $0
            sub(/^\t/, "", line)
            decl = line; sub(/;.*/, "", decl)
            nums = line; sub(/.*\/\*[ \t]*/, "", nums); sub(/[ \t]*\*\/.*/, "", nums)
            split(nums, n, /[ \t]+/)
            name = decl; sub(/.*[ \t*]/, "", name); sub(/\[.*/, "", name)
            type = decl; gsub(/[ \t]+/, " ", type)
            printf "member %s %s %s %s %s %s\n", kind, tag, name, n[1], n[2], type
        }
        /\/\* size: [0-9]+/ { s = $0; sub(/.*size: /, "", s); sub(/,.*/, "", s); printf "size %s %s %s\n", kind, tag, s }
    ' "$work/one.txt" >> "$work/new.layout"
done
test "$(grep -c '^member ' "$work/new.layout")" -gt 0 || { echo "FAIL no member read"; exit 1; }

if [ -n "$record" ]; then
    cp "$work/new.layout" "$baseline"
    echo "ok   recorded the layout of $(grep -c '^size ' "$baseline") structures, $(grep -c '^member ' "$baseline") members, at $version"
    exit 0
fi
test -f "$baseline" || { echo "ok   no layout baseline $baseline: the first release. Record it with 'make abi-baseline' and commit it."; exit 0; }

old_version=$(sed -n 's/^version //p' "$baseline")
bad=0
added=0
# every member of the baseline, at the same offset, of the same size and type
if grep '^member ' "$baseline" | grep -vxF -f "$work/new.layout" > "$work/missing.txt"; then
    sed 's/^/FAIL changed, moved or removed: /' "$work/missing.txt"
    bad=1
fi
# every structure no smaller
while read -r _ kind tag size; do
    now=$(awk -v k="$kind" -v t="$tag" '$1 == "size" && $2 == k && $3 == t { print $4 }' "$work/new.layout")
    if [ -z "$now" ]; then
        echo "FAIL $kind $tag is gone"; bad=1
    elif [ "$now" -lt "$size" ]; then
        echo "FAIL $kind $tag shrank from $size to $now bytes"; bad=1
    elif [ "$now" -gt "$size" ]; then
        echo "     $kind $tag grew from $size to $now bytes"; added=1
    fi
done <<EOT
$(grep '^size ' "$baseline")
EOT
# whatever is new: a member or a structure
if grep -v '^version ' "$work/new.layout" | grep -vxF -f "$baseline" > "$work/added.txt"; then
    sed 's/^/     added: /' "$work/added.txt"
    added=1
fi
if [ "$bad" -ne 0 ]; then
    echo "FAIL an incompatible change in the layout under $old_version: a member removed, moved or changed needs a new soname major"
    exit 1
fi
if [ "$added" -ne 0 ]; then
    if [ "$version" = "$old_version" ]; then
        echo "FAIL a compatible change in the layout (an addition) under the version $old_version: it needs a new minor"
        exit 1
    fi
    echo "ok   the layout grew, and the version moved $old_version to $version"
    exit 0
fi
echo "ok   no change in the layout of $(grep -c '^size ' "$baseline") structures since $old_version"
