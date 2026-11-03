#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# protocol_library_check.sh: omx-clap-host reads its protocol from libplugin-hostd-protocol and keeps no copy of it.
#
#   protocol_library_check.sh <program> <deps>  the program links libplugin-hostd-protocol.so.0 and no
#                                               libmod-host-protocol; every header of the library that the sources
#                                               include (<deps>: the output of the compiler's -M over them) is the
#                                               library's own; no tracked file is a copy of a file the library ships
#   protocol_library_check.sh --copies [ROOT]   the last of the three alone, over ROOT (default: this tree)
#   protocol_library_check.sh --self-test       each kind of copy planted in a scratch tree must be red, and the check
#                                               without its rules must pass one
#
# The library is the one pkg-config finds (plugin-hostd-protocol): its headers under <includedir>/plugin-hostd-protocol
# and its tables under its protocoldir. With no library found the check is red: it cannot have run.
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
root=$(cd "$here/.." && pwd)
PKG_CONFIG=${PKG_CONFIG:-pkg-config}

# the renders of this repository's own declarations (data/verbs.json, data/vocabulary.json): the library ships copies of
# them beside protocol.h, so here they are the source and there the copy
OWNED=(src/omx_clap_core_host_verbs.h src/omx_clap_core_vocabulary.h)

library() { # sets inc and data, or fails
    inc=$($PKG_CONFIG --variable=includedir plugin-hostd-protocol 2>/dev/null)/plugin-hostd-protocol \
        && data=$($PKG_CONFIG --variable=protocoldir plugin-hostd-protocol 2>/dev/null) \
        && [ -f "$inc/protocol.h" ] && [ -d "$data" ] \
        || { echo "FAIL no plugin-hostd-protocol found by $PKG_CONFIG (install plugin-hostd-protocol-devel, or set PKG_CONFIG_PATH)"; return 1; }
}

# <root> <inc> <data>: the tracked files of <root> that are a file the library ships: the same bytes, the same name, or
# the include guard of one of its headers
copies() {
    local tree=$1 inc=$2 data=$3 f h base guard o skip found=0
    declare -A sums=() names=() guards=()
    while IFS= read -r f; do
        h=$(sha256sum < "$f"); sums[${h%% *}]=${f#"$inc/"}
        base=${f##*/}; names[$base]=1
        guard=$(sed -n '1,40{s/^#ifndef[[:space:]]\{1,\}\([A-Za-z_][A-Za-z0-9_]*\)[[:space:]]*$/\1/p}' "$f" | head -n 1)
        [ -z "$guard" ] || guards[$guard]=1
    done < <(find "$inc" "$data" -type f | sort)
    [ -z "${PROTOCOL_LIBRARY_CHECK_BLIND:-}" ] || { sums=(); names=(); guards=(); }
    while IFS= read -r f; do
        skip=; for o in "${OWNED[@]}"; do [ "$f" = "$o" ] && skip=1; done
        [ -z "$skip" ] && [ -f "$tree/$f" ] || continue
        h=$(sha256sum < "$tree/$f"); h=${h%% *}
        if [ -n "${sums[$h]:-}" ]; then echo "FAIL $f is a copy of the library's ${sums[$h]#"$data/"}"; found=1; continue; fi
        base=${f##*/}
        if [ -n "${names[$base]:-}" ]; then echo "FAIL $f has the name of a file the library ships"; found=1; continue; fi
        case $f in *.h|*.c)
            for guard in "${!guards[@]}"; do
                if grep -q -E "^#[[:space:]]*define[[:space:]]+$guard([[:space:]]|$)" "$tree/$f"; then
                    echo "FAIL $f declares $guard, the include guard of a header the library ships"; found=1; break
                fi
            done ;;
        esac
    done < <(git -c safe.directory="$tree" -C "$tree" ls-files)
    return "$found"
}

self_test() {
    local t fail=0 inc data
    library || return 1
    t=$(mktemp -d); trap 'rm -rf "$t"' RETURN
    plant() { # <what> <path> <source file or -> [text]: a clean tree plus the plant must be red
        rm -rf "$t/r"; mkdir -p "$t/r/src"; git init -q "$t/r"
        for o in "${OWNED[@]}"; do cp "$root/$o" "$t/r/$o"; done
        echo 'int main(void) { return 0; }' > "$t/r/src/main.c"
        if [ -n "$2" ]; then
            mkdir -p "$t/r/$(dirname "$2")"
            if [ "$3" = - ]; then printf '%s\n' "$4" > "$t/r/$2"; else cp "$3" "$t/r/$2"; fi
        fi
        git -C "$t/r" add -A
        if [ -z "$2" ]; then
            if copies "$t/r" "$inc" "$data" >/dev/null; then echo "ok   $1"; else echo "FAIL $1"; fail=1; fi
        elif copies "$t/r" "$inc" "$data" >/dev/null; then echo "FAIL $1 stays green"; fail=1
        else echo "ok   $1"; fi
    }
    plant "a tree with this repository's own renders is green" "" "" ""
    plant "the declared protocol.h copied under its own path is red" include/plugin-hostd/protocol.h "$inc/plugin-hostd/protocol.h"
    plant "a header of the library under another name is red by its bytes" src/dispatch_copy.h "$inc/host-dispatch.h"
    plant "the verb table under another name is red by its bytes" data/table.json "$data/plugin-hostd-protocol.json"
    plant "a file with the name of one the library ships is red" src/mod-host.h - '/* empty */'
    plant "a header that declares a guard of the library's is red" src/verbs_copy.h - '#ifndef PLUGIN_HOSTD_PROTOCOL_H
#define PLUGIN_HOSTD_PROTOCOL_H
#endif'
    # sabotage: with its rules gone the check passes a planted copy, so the rules are what catch it
    rm -rf "$t/s"; mkdir -p "$t/s/src"; git init -q "$t/s"; cp "$inc/host-dispatch.h" "$t/s/src/dispatch_copy.h"; git -C "$t/s" add -A
    if PROTOCOL_LIBRARY_CHECK_BLIND=1 copies "$t/s" "$inc" "$data" >/dev/null; then echo "ok   sabotage: without its rules the check passes a copy"
    else echo "FAIL sabotage: the check is red without its rules"; fail=1; fi
    return "$fail"
}

case ${1:-} in
--self-test) self_test; exit ;;
--copies)
    library
    copies "$(cd "${2:-$root}" && pwd)" "$inc" "$data" || exit 1
    echo "ok   no tracked file is a copy of a file plugin-hostd-protocol ships"; exit 0 ;;
esac

prog=${1:?usage: protocol_library_check.sh <program> <deps> | --copies [ROOT] | --self-test}
deps=${2:?usage: protocol_library_check.sh <program> <deps> | --copies [ROOT] | --self-test}
library
fail=0
needed=$(readelf -d "$prog" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p')
if grep -qx 'libplugin-hostd-protocol\.so\.0' <<<"$needed"; then echo "ok   $prog links libplugin-hostd-protocol.so.0"
else echo "FAIL $prog does not link libplugin-hostd-protocol.so.0 (it links: $(echo $needed))"; fail=1; fi
if grep -q 'mod-host-protocol' <<<"$needed"; then echo "FAIL $prog links libmod-host-protocol"; fail=1; fi
# every dependency that is a header the library ships (the longest path below the library's include directory it ends
# with, in a directory that holds a protocol.h) is that file
mapfile -t shipped < <(cd "$inc" && find . -name '*.h' | sed 's|^\./||' | awk '{ print length, $0 }' | sort -rn | cut -d' ' -f2-)
seen=0
while IFS= read -r d; do
    for rel in "${shipped[@]}"; do
        case /$d in */"$rel")
            # a header of the same name in a tree that is no protocol's (the C library's sys/socket.h) is not one
            top=${d%"$rel"}
            [ -f "${top}protocol.h" ] || [ -f "${top}plugin-hostd/protocol.h" ] || break
            seen=$((seen + 1))
            [ "$(readlink -f "$d")" = "$(readlink -f "$inc/$rel")" ] \
                || { echo "FAIL the sources read $d, not the library's $inc/$rel"; fail=1; }
            break ;;
        esac
    done
done < <(tr ' \\' '\n\n' < "$deps" | sed -e 's/:$//' | grep '\.h$' | sort -u)
# a verb table of plugin-hostd's from anywhere but the library (an older plugin-hostd's include tree)
while IFS= read -r d; do
    case $(readlink -f "$d") in "$(readlink -f "$inc")"/*) ;; *)
        echo "FAIL the sources read the verb table $d, not the library's"; fail=1 ;;
    esac
done < <(tr ' \\' '\n\n' < "$deps" | grep -E '(^|/)plugin-hostd/[a-z_]+_host_verbs\.h$' | sort -u)
[ "$seen" -gt 0 ] || { echo "FAIL the sources include no header of the library"; fail=1; }
[ "$fail" = 1 ] || echo "ok   every header of the library the sources include is the library's ($seen)"
copies "$root" "$inc" "$data" || fail=1
[ "$fail" = 1 ] || echo "ok   no tracked file is a copy of a file plugin-hostd-protocol ships"
exit "$fail"
