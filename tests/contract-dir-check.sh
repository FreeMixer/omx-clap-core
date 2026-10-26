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
# <version>, and the package's own renders (`render --package omx-clap-core`: the headers and module the `render` word of
# data/hosting.json frames, the vocabulary of data/hosting/ and data/verbs.json, and the verbs of data/verbs.json) are compared with the files this tree commits, byte for
# byte: a header with src/<header>, a TypeScript module with ts/<module>.js and .d.ts. A committed render the output
# lacks, or an output this tree does not commit, is a failure too.
# Exit 0 equal; 1 a refusal or a difference; 2 usage.
set -eu

[ "$#" -eq 3 ] || { sed -n '8p' "$0" >&2; exit 2; }
oc=$1 dir=$2 version=$3
[ -f "$oc/bin/omx-contract.mjs" ] || { echo "contract-dir-check: no omx-contract checkout at $oc" >&2; exit 2; }
[ -f "$dir/3/omx-clap-core/MANIFEST.json" ] || { echo "contract-dir-check: no omx-clap-core package under $dir/3" >&2; exit 2; }
tool="node $oc/bin/omx-contract.mjs"
work=$(dirname "$dir")

# omx-contract's own package, less its reader's table (codes.json), as omx-contract installs itself
ocv=$(sed -n 's/^  "version": "\([0-9.]*\)",$/\1/p' "$oc/package.json")
sh "$oc/tools/install-contract-dir.sh" -x codes.json omx-contract "$ocv" 3 "$oc/data" "$dir" data

$tool validate --contract-dir "$dir" --needs "omx-clap-core@>= $version"

# every impl link of the package's items names code that reads the item (contract.source.unread names the code and the
# link), found from this tree's root
$tool links --check --root . --contract-dir "$dir" --package omx-clap-core

# the renders this tree commits, as <target>:<committed path>; the output's name is the committed file's
committed='c:src/clap_host_limits.h c:src/hosted_stage_limits.h c:src/omx_clap_core_vocabulary.h c:src/omx_clap_core_host_verbs.h ts:ts/omx-clap-core-hosting.js ts:ts/omx-clap-core-hosting.d.ts ts:ts/omx-clap-core-vocabulary.js ts:ts/omx-clap-core-vocabulary.d.ts'

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
  [ -f "$work/${c%%:*}/$(basename "${c#*:}")" ] || { echo "contract-dir-check: the ${c%%:*} render has no $(basename "${c#*:}")" >&2; bad=1; }
done
[ "$bad" -eq 0 ] || exit 1
# the program hands the LV2 adapter the declared worker numbers, by name: every assignment of them in src/ reads
# hosted_stage_limits.h's define, which the render above holds to data/hosting.json
for f in worker_poll_us:WORKER_POLL_US worker_ring_bytes:WORKER_RING_BYTES; do
  set -- $(grep -rhE "\.${f%%:*} = " src/*.c | sed 's/^ *//' | sort -u)
  [ "$*" = "config.${f%%:*} = OMX_HOSTED_STAGE_${f#*:};" ] || {
    echo "contract-dir-check: src/ sets ${f%%:*} from \"$*\", not from the declared OMX_HOSTED_STAGE_${f#*:}" >&2; exit 1; }
done
# each module loads as an ES module and exports its files' items, nothing else: the hosting module data/hosting.json's,
# the vocabulary module every item of data/hosting/ and the codes and words of data/verbs.json, a words item with its _WORDS
node --input-type=module -e "import('$(pwd)/ts/omx-clap-core-hosting.js').then((m) => { const k = Object.keys(m).sort().join(' '); if (k !== 'HOSTED_STAGE_LIMITS HOSTING_IDLE_CODES HOSTING_WINDOW_CODES') { console.error('contract-dir-check: ts/omx-clap-core-hosting.js exports ' + k); process.exit(1); } })"
node --input-type=module -e "
import { readdirSync, readFileSync } from 'node:fs';
const own = (f) => Object.entries(JSON.parse(readFileSync(f, 'utf8'))).filter(([k, v]) => !k.startsWith('\$') && k !== 'requires' && v.kind !== 'verbs').flatMap(([k, v]) => (v.kind === 'words' ? [k, k + '_WORDS'] : [k]));
const items = [...readdirSync('data/hosting').flatMap((f) => own('data/hosting/' + f)), ...own('data/verbs.json')].sort().join(' ');
import('$(pwd)/ts/omx-clap-core-vocabulary.js').then((m) => { const k = Object.keys(m).sort().join(' '); if (k !== items) { console.error('contract-dir-check: ts/omx-clap-core-vocabulary.js exports ' + k + ', data/ declares ' + items); process.exit(1); } })"
echo "contract-dir-check: omx-clap-core $version read back; $(echo "$committed" | sed 's/[a-z]*://g') the render of data/, byte for byte"
