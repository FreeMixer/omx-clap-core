#!/bin/sh
# The library exports what its version script lists and what the header declares OMX_CLAP_EXPORT, and nothing else, and it
# names no library of the jack or the protocol kind.
#   exports.sh <libomx-clap-core.so.X.Y.Z> <omx-clap-core.map> <clap_host.h>
set -eu
export LC_ALL=C

lib=$1
map=$2
header=$3
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# what the file exports, defined and default-visible
nm -D --defined-only "$lib" | awk '$2 ~ /^[TDBRVW]$/ { print $3 }' | sed 's/@.*//' | grep -v '^_' | sort -u > "$tmp/exported.txt"
# what the version script lists
sed -n '/global:/,/local:/p' "$map" | sed -n 's/^ *\([a-z_0-9]*\);$/\1/p' | sort -u > "$tmp/listed.txt"
# what the header declares
grep 'OMX_CLAP_EXPORT [a-z]' "$header" | sed 's/(.*//; s/.*[ *]//' | sort -u > "$tmp/declared.txt"

# a control that can see presence: the lists are not empty, or an empty diff proves nothing
for f in exported.txt listed.txt declared.txt; do
    test -s "$tmp/$f" || { echo "FAIL $f is empty: the check could not see the exports"; exit 1; }
done

status=0
# comm prints what only one side has; both lists are sorted
extra=$(comm -3 "$tmp/listed.txt" "$tmp/exported.txt")
[ -z "$extra" ] || { echo "$extra"; echo "FAIL the file exports other than the version script lists (left: listed only, right: exported only)"; status=1; }
extra=$(comm -3 "$tmp/listed.txt" "$tmp/declared.txt")
[ -z "$extra" ] || { echo "$extra"; echo "FAIL the header declares other than the version script lists (left: listed only, right: declared only)"; status=1; }
echo "ok   $(wc -l < "$tmp/exported.txt") exports, the version script's and the header's"

needed=$(readelf -d "$lib" | sed -n 's/.*Shared library: \[\(.*\)\]/\1/p')
echo "ok   needs: $(echo $needed)"
for n in $needed; do
    case $n in
        libc.so.*|libm.so.*|libdl.so.*|libpthread.so.*|ld-linux*.so.*) ;;
        *) echo "FAIL the core needs $n"; status=1 ;;
    esac
done
soname=$(readelf -d "$lib" | sed -n 's/.*Library soname: \[\(.*\)\]/\1/p')
case $soname in
    libomx-clap-core.so.[0-9]*) echo "ok   soname $soname" ;;
    *) echo "FAIL soname is '$soname'"; status=1 ;;
esac
exit $status
