#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# contract-dir-check.sh — omx-clap-core's definitions read back through omx-contract's reader, and every file this
# tree commits as their render held to it.
#
#   contract-dir-check.sh <omx-contract checkout> <contract dir> <version>
#
# <contract dir> holds omx-clap-core's package as `make install-contract` wrote it. omx-contract's own data/ is
# installed beside it from the checkout (its npm ci done), the directory is validated with omx-clap-core required at
# <version>, and the package's own renders (`render --package omx-clap-core`, framed by the `render` word of
# data/hosting.json) are compared with the files this tree commits, byte for byte: a header with src/<header>, the
# TypeScript module with ts/<module>.js and .d.ts. A
# committed render the output lacks, or an output this tree does not commit, is a failure too.
# Exit 0 equal; 1 a refusal or a difference; 2 usage.
set -eu

[ "$#" -eq 3 ] || { sed -n '8p' "$0" >&2; exit 2; }
oc=$1 dir=$2 version=$3
[ -f "$oc/bin/omx-contract.mjs" ] || { echo "contract-dir-check: no omx-contract checkout at $oc" >&2; exit 2; }
[ -f "$dir/2/omx-clap-core/MANIFEST.json" ] || { echo "contract-dir-check: no omx-clap-core package under $dir/2" >&2; exit 2; }
tool="node $oc/bin/omx-contract.mjs"
work=$(dirname "$dir")

# omx-contract's own package; -x hosting.json leaves out its copy of the hosting items while it still ships one, since
# one name declared by two packages is a refusal (contract.item.duplicate)
ocv=$(sed -n 's/^  "version": "\([0-9.]*\)",$/\1/p' "$oc/package.json")
sh "$oc/tools/install-contract-dir.sh" -x codes.json -x hosting.json omx-contract "$ocv" 2 "$oc/data" "$dir" data

$tool validate --contract-dir "$dir" --needs "omx-clap-core@>= $version"

# the renders this tree commits, as <target>:<committed path>; the output's name is the committed file's
committed='c:src/clap_host_limits.h c:src/hosted_stage_limits.h ts:ts/omx-clap-core-hosting.js ts:ts/omx-clap-core-hosting.d.ts'

bad=0
for target in c ts; do
  rm -rf "$work/$target"
  $tool render --target "$target" --package omx-clap-core --out "$work/$target" --contract-dir "$dir"
  for out in "$work/$target"/*; do
    name=$(basename "$out")
    path=
    for c in $committed; do
      [ "${c%%:*}" = "$target" ] && [ "$(basename "${c#*:}")" = "$name" ] && path=${c#*:}
    done
    [ -n "$path" ] || { echo "contract-dir-check: the $target render writes $name, which this tree does not commit" >&2; bad=1; continue; }
    if ! cmp -s "$path" "$out"; then
      diff -u "$path" "$out" >&2 || true
      echo "contract-dir-check: $path is not the render of data/" >&2
      bad=1
    fi
  done
done
for c in $committed; do
  [ -f "$work/${c%%:*}/$(basename "${c#*:}")" ] || { echo "contract-dir-check: the ${c%%:*} render has no $(basename "${c#*:}") (data/hosting.json render)" >&2; bad=1; }
done
[ "$bad" -eq 0 ] || exit 1
# the program hands the LV2 adapter the declared worker numbers, by name: every assignment of them in src/ reads
# hosted_stage_limits.h's define, which the render above holds to data/hosting.json
for f in worker_poll_us:WORKER_POLL_US worker_ring_bytes:WORKER_RING_BYTES; do
  set -- $(grep -rhE "\.${f%%:*} = " src/*.c | sed 's/^ *//' | sort -u)
  [ "$*" = "config.${f%%:*} = OMX_HOSTED_STAGE_${f#*:};" ] || {
    echo "contract-dir-check: src/ sets ${f%%:*} from \"$*\", not from the declared OMX_HOSTED_STAGE_${f#*:}" >&2; exit 1; }
done
# the module loads as an ES module and exports the file's items, nothing else
node --input-type=module -e "import('$(pwd)/ts/omx-clap-core-hosting.js').then((m) => { const k = Object.keys(m).sort().join(' '); if (k !== 'HOSTED_STAGE_LIMITS HOSTING_CODES') { console.error('contract-dir-check: ts/omx-clap-core-hosting.js exports ' + k); process.exit(1); } })"
echo "contract-dir-check: omx-clap-core $version read back; $(echo "$committed" | sed 's/[a-z]*://g') the render of data/, byte for byte"
