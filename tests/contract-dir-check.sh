#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# contract-dir-check.sh — omx-clap-core's definitions read back through omx-contract's reader, and the header
# rendered from them against the one this tree commits.
#
#   contract-dir-check.sh <omx-contract checkout> <contract dir> <version>
#
# <contract dir> holds omx-clap-core's package as `make install-contract` wrote it. omx-contract's own data/ is
# installed beside it from the checkout (its npm ci done), the directory is validated with omx-clap-core required at
# <version>, and the c target rendered from it. The render's clap_host_limits.h must equal src/clap_host_limits.h
# byte for byte outside the three lines of its "Rendered ..." banner, which this tree words as its own: the render
# has no option that words it yet.
# Exit 0 equal; 1 a refusal or a difference; 2 usage.
set -eu

[ "$#" -eq 3 ] || { sed -n '8p' "$0" >&2; exit 2; }
oc=$1 dir=$2 version=$3
[ -f "$oc/bin/omx-contract.mjs" ] || { echo "contract-dir-check: no omx-contract checkout at $oc" >&2; exit 2; }
[ -f "$dir/2/omx-clap-core/MANIFEST.json" ] || { echo "contract-dir-check: no omx-clap-core package under $dir/2" >&2; exit 2; }
tool="node $oc/bin/omx-contract.mjs"
work=$(dirname "$dir")

# omx-contract's own package; -x hosting.json leaves out its copy of the hosting items until it deletes it, since one
# name declared by two packages is a refusal (contract.item.duplicate)
ocv=$(sed -n 's/^  "version": "\([0-9.]*\)",$/\1/p' "$oc/package.json")
sh "$oc/tools/install-contract-dir.sh" -x codes.json -x hosting.json omx-contract "$ocv" 2 "$oc/data" "$dir" data

$tool validate --contract-dir "$dir" --needs "omx-clap-core@>= $version"
rm -rf "$work/c"
$tool render --target c --out "$work/c" --contract-dir "$dir"

banner='/^ \* Rendered /,/never edited here\.$/d'
body() {
  sed "$banner" "$1" > "$2"
  [ $(($(wc -l < "$1") - $(wc -l < "$2"))) -eq 3 ] || { echo "contract-dir-check: $1 has no three-line banner" >&2; exit 1; }
}
body "$work/c/omxcontract/clap_host_limits.h" "$work/rendered.h"
body src/clap_host_limits.h "$work/committed.h"
grep -q '^#define CLAP_HOST_WARMUP_BLOCKS ' "$work/rendered.h" || { echo "contract-dir-check: the render carries no CLAP_HOST_ define" >&2; exit 1; }
if ! cmp -s "$work/committed.h" "$work/rendered.h"; then
  diff -u "$work/committed.h" "$work/rendered.h" >&2 || true
  echo "contract-dir-check: src/clap_host_limits.h is not the render of data/" >&2
  exit 1
fi
echo "contract-dir-check: omx-clap-core $version read back, src/clap_host_limits.h is the render of data/ outside its banner"
