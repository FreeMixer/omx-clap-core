#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# The sabotage of tests/abi-check.sh's reading of abidiff (#323): a fixture library, libfix.so.1, whose structure
# member points to another structure, as struct omx_clap_chain_member points to the instance, built at 1.0.0 as the
# baseline and at 1.1.0 once per case, each checked by abi-check.sh. The pointee growing (beside a member change abidiff
# filters) is an addition and must read ok; a function-pointer member whose signature changed, the same beside the
# growth, the pointer retyped to another structure of the same layout or of a smaller one, and the pointee's members
# reordered must each FAIL. A case that reads otherwise fails the run. Everything is built under build/abi-sabotage.
#   abi-check-sabotage.sh        (from the root of the tree; needs gcc, abidw and abidiff)
set -u

root=$(cd "$(dirname "$0")/.." && pwd)
work=$root/build/abi-sabotage
cc=${CC:-cc}
failures=0
rm -rf "$work"
mkdir -p "$work"

# build <dir> <version> <inst> <other> <member>: the fixture's header and library under <dir>/usr
build() {
    mkdir -p "$1/usr/include" "$1/usr/lib"
    printf 'struct inst { %s };\nstruct other { %s };\nstruct member { %s };\nvoid use(struct member *m);\n' "$3" "$4" "$5" > "$1/usr/include/fix.h"
    printf '#include "fix.h"\n__attribute__((visibility("default"))) void use(struct member *m) { (void)m; }\n' > "$1/fix.c"
    "$cc" -g -O0 -fPIC -shared -fvisibility=hidden -I"$1/usr/include" -Wl,-soname,libfix.so.1 -o "$1/usr/lib/libfix.so.$2" "$1/fix.c"
}

build "$work/base" 1.0.0 'int a; int b;' 'int z;' 'struct inst *instance; int flags; void (*cb)(int);' || exit 2
abidw --headers-dir "$work/base/usr/include" --out-file "$work/base.abi" "$work/base/usr/lib/libfix.so.1.0.0" || exit 2

# case <name> <want: ok or FAIL> <inst> <other> <member>
case_() {
    name=$1 want=$2 dir=$work/$1
    build "$dir" 1.1.0 "$3" "$4" "$5" || { echo "FAIL $name: the fixture did not build"; failures=$((failures + 1)); return; }
    got=$(sh "$root/tests/abi-check.sh" "$dir/usr/lib/libfix.so.1.1.0" "$dir/usr/include" "$work/base.abi" 2>&1 | grep -E '^(ok|FAIL) ' | tail -1)
    case $got in
        "$want "*) echo "ok   $name: $got" ;;
        *) echo "FAIL $name: wanted $want, abi-check.sh says: ${got:-nothing}"; failures=$((failures + 1)) ;;
    esac
}

case_ growth ok 'int a; int b; int c;' 'int z;' 'struct inst *instance; unsigned flags; void (*cb)(int);'
case_ function-pointer FAIL 'int a; int b;' 'int z;' 'struct inst *instance; int flags; void (*cb)(int, int);'
case_ growth-and-function-pointer FAIL 'int a; int b; int c;' 'int z;' 'struct inst *instance; int flags; void (*cb)(int, int);'
case_ retyped-same-layout FAIL 'int a; int b;' 'int a; int b;' 'struct other *instance; int flags; void (*cb)(int);'
case_ retyped-smaller FAIL 'int a; int b;' 'int a;' 'struct other *instance; int flags; void (*cb)(int);'
case_ pointee-reordered FAIL 'int b; int a;' 'int z;' 'struct inst *instance; int flags; void (*cb)(int);'

if [ "$failures" -eq 0 ]; then
    echo "abi-check sabotage ok: the growth reads ok, every other case FAILs"
    exit 0
fi
echo "abi-check sabotage FAILED ($failures)"
exit 1
