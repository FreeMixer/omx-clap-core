#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Every tracked C, header, shell and python source names its licence; the C and header files carry the copyright line too.
#   spdx-check.sh        (from the root of a git checkout)
set -euo pipefail
cd "$(dirname "$0")/.."

mapfile -t sources < <(git ls-files -- '*.c' '*.h' '*.sh' '*.py')
[ "${#sources[@]}" -gt 0 ] || { echo "spdx-check: no tracked source found" >&2; exit 1; }

missing=$(grep -L 'SPDX-License-Identifier: ' -- "${sources[@]}" || true)
mapfile -t cfiles < <(git ls-files -- '*.c' '*.h')
nocopy=$(grep -L 'Copyright (C) ' -- "${cfiles[@]}" || true)

[ -z "$missing" ] || printf 'spdx-check: no SPDX-License-Identifier: %s\n' $missing >&2
[ -z "$nocopy" ] || printf 'spdx-check: no Copyright line: %s\n' $nocopy >&2
[ -z "$missing$nocopy" ] || exit 1
echo "spdx-check: ${#sources[@]} sources ok"
