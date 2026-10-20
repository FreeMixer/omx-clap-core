#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# hosting-compare.sh: the numbers of docs/hosting-comparison.md, our LV2 adapter against mod-host and jalv.
#
#   hosting-compare.sh            clone (or reuse) the subjects at their pinned refs and print every table
#   hosting-compare.sh --verbose  also print each function with the area its name put it in (to audit the areas)
#
# The subjects are cloned into HC_DIR (default ${TMPDIR:-/tmp}/hosting-compare), never vendored. Ours is this repo at
# OURS_REF (default origin/main), extracted with git archive. The tools: scc (SCC, default scc on PATH) for files and
# lines, lizard (LIZARD, default "python3 -m lizard") for functions and cyclomatic complexity.
#
# What is measured, per subject (the host core, not its UI, its audio backend or its vendored libraries):
#   ours          lv2/core lv2/clap (the adapter), and "ours+plumbing" adding the src/ headers lv2/ includes
#   mod-host      src/*.[ch] minus the socket protocol, the command line, the readline completer and the monitor client
#   jalv          src/*.[ch] minus main.c, the JACK and PortAudio backends; the gtk/qt/console frontends are subdirs
# Areas are by function name (the first pattern of AREAS that matches, else "other"): a heuristic, --verbose shows it.
# Exit 0 printed, 1 a tool or a ref is missing.
set -euo pipefail

MOD_HOST_URL=https://github.com/mod-audio/mod-host
MOD_HOST_PIN=f14a230                  # the audinux build
MOD_HOST_MAIN=12335c2                 # main on 2026-10-20
JALV_URL=https://github.com/drobilla/jalv
JALV_TAG=v1.10.0                      # latest release on 2026-10-20
OURS_REF=${OURS_REF:-origin/main}
HC_DIR=${HC_DIR:-${TMPDIR:-/tmp}/hosting-compare}
SCC=${SCC:-scc}
LIZARD=${LIZARD:-python3 -m lizard}
VERBOSE=0
[ "${1:-}" = --verbose ] && VERBOSE=1

REPO=$(git -C "$(dirname "${BASH_SOURCE[0]}")" rev-parse --show-toplevel)

die() { echo "hosting-compare: $*" >&2; exit 1; }
command -v "${SCC%% *}" >/dev/null || die "scc not found (SCC=...)"
$LIZARD --version >/dev/null 2>&1 || die "lizard not found (LIZARD=..., pip install lizard)"

# area name|pattern on the lower-cased function name, the first match wins
AREAS='worker|work
state|state|preset|save|restore|path
cv|(^|_)cv(_|$)
port groups / layouts / aux|group|layout|aux|surround|channel_count|audio_ports|bus|side|width|ambisonic
CLAP contract / params / descriptor|param|contract|descriptor|provider|entry|factory|text_to|to_text
latency / rate / buffer change|latency|rate|options|block|bufsize|buffer_size
atoms / events / MIDI|atom|evbuf|event|midi|seq|note|urid|map|patch|time|prop|forge
ports and connect|port|connect|buffer
error / refusal|err|refus|verdict|fail|check|valid|log|requir|reject|code|unsupported
lifecycle and shutdown|activate|destroy|free|close|shutdown|cleanup|stop|start|reset|exit|remove
run / process|run|process|cycle
instantiate|instan|create|new|load|init|open|world|discover|find|plugin|add|feature|bundle|dirs|configure'

extract() { # <git dir> <ref> <dest> <paths...>
    local git=$1 ref=$2 dest=$3; shift 3
    rm -rf "$dest"; mkdir -p "$dest"
    git -C "$git" archive "$ref" "$@" | tar -x -C "$dest"
}

mkdir -p "$HC_DIR"
[ -d "$HC_DIR/mod-host.git" ] || git clone -q --bare "$MOD_HOST_URL" "$HC_DIR/mod-host.git"
[ -d "$HC_DIR/jalv.git" ] || git clone -q --bare "$JALV_URL" "$HC_DIR/jalv.git"
for r in "$MOD_HOST_PIN" "$MOD_HOST_MAIN"; do
    git -C "$HC_DIR/mod-host.git" cat-file -e "$r^{commit}" 2>/dev/null || die "mod-host has no $r"
done
git -C "$HC_DIR/jalv.git" cat-file -e "$JALV_TAG^{commit}" 2>/dev/null || die "jalv has no $JALV_TAG"
git -C "$REPO" cat-file -e "$OURS_REF^{commit}" 2>/dev/null || die "this repo has no $OURS_REF"

extract "$REPO" "$OURS_REF" "$HC_DIR/ours" lv2/core lv2/clap src
extract "$HC_DIR/mod-host.git" "$MOD_HOST_PIN" "$HC_DIR/mod-host-f14a230" src
extract "$HC_DIR/mod-host.git" "$MOD_HOST_MAIN" "$HC_DIR/mod-host-main" src
extract "$HC_DIR/jalv.git" "$JALV_TAG" "$HC_DIR/jalv" src

files_of() { # <subject>: the measured files, one per line, relative to HC_DIR
    case $1 in
    ours) ls ours/lv2/core/*.[ch] ours/lv2/clap/*.[ch] ;;
    ours+plumbing)
        files_of ours
        grep -ho '#include "[^"]*"' ours/lv2/core/*.[ch] ours/lv2/clap/*.[ch] | sed 's/#include "\(.*\)"/\1/' |
            sort -u | while read -r h; do if [ -f "ours/src/$h" ]; then echo "ours/src/$h"; fi; done ;;
    mod-host-*) ls "$1"/src/*.[ch] | grep -Ev '/(mod-host\.c|protocol\.[ch]|socket\.[ch]|completer\.[ch]|monitor\.[ch])$' ;;
    jalv) ls jalv/src/*.[ch] | grep -Ev '/(main\.c|jack\.c|jack_impl\.h|jack_internal\.c|portaudio\.c)$' ;;
    esac
}

SUBJECTS="ours ours+plumbing mod-host-f14a230 mod-host-main jalv"
cd "$HC_DIR"

echo "# hosting-compare: ours $(git -C "$REPO" rev-parse --short "$OURS_REF") ($OURS_REF)," \
    "mod-host $MOD_HOST_PIN and $MOD_HOST_MAIN, jalv $JALV_TAG ($(git -C jalv.git rev-parse --short "$JALV_TAG^{commit}"))"
echo "# scc $($SCC --version | awk '{print $NF}'), lizard $($LIZARD --version)"
echo

for s in $SUBJECTS; do
    files_of "$s" > "$s.files"
    # shellcheck disable=SC2046
    { $LIZARD $(tr "\n" " " < "$s.files") 2>/dev/null || true; } |
        awk '$6 ~ /@[0-9]+-[0-9]+@/ && !seen[$6]++ {print $1, $2, $6}' > "$s.funcs"
done

echo "## size"
echo
printf '| %-18s | %5s | %6s | %5s | %8s | %9s | %6s | %7s |\n' subject files lines code comments functions deps public
echo '|---|---:|---:|---:|---:|---:|---:|---:|'
for s in $SUBJECTS; do
    # shellcheck disable=SC2046
    read -r nfiles lines code comments < <($SCC --no-cocomo --format csv $(tr '\n' ' ' < "$s.files") |
        awk -F, 'NR > 1 {f += $8; l += $2; c += $3; m += $4} END {print f, l, c, m}')
    nfuncs=$(wc -l < "$s.funcs")
    # external dependencies: the distinct libraries the measured files include as <lib/...> (system dirs aside)
    # shellcheck disable=SC2046
    deps=$(grep -ho '#include <[^>]*/[^>]*>' $(tr '\n' ' ' < "$s.files") | sed 's/#include <\([^/]*\)\/.*/\1/' |
        grep -Ev '^(sys|bits|linux|asm|arpa|netinet)$' | sort -u | tr '\n' ' ')
    # public symbols: functions defined in a .c file whose definition (its line or the line above) is not static
    public=$(awk '{split($3, a, "@"); split(a[2], r, "-"); print a[3], r[1]}' "$s.funcs" | grep '\.c ' |
        while read -r f l; do sed -n "$((l > 1 ? l - 1 : 1)),${l}p" "$f" | grep -q static || echo x; done | wc -l)
    printf '| %-18s | %5s | %6s | %5s | %8s | %9s | %6s | %7s |\n' \
        "$s" "$nfiles" "$lines" "$code" "$comments" "$nfuncs" "$(wc -w <<< "$deps")" "$public"
    echo "$s: $deps" >> deps.txt.$$
done
echo
echo "external dependencies (include roots):"
sed 's/^/  /' deps.txt.$$; rm -f deps.txt.$$
echo

echo "## cyclomatic complexity (lizard, per function)"
echo
echo '| subject | functions | mean CCN | p95 CCN | max CCN | CCN > 15 | mean NLOC |'
echo '|---|---:|---:|---:|---:|---:|---:|'
for s in $SUBJECTS; do
    sort -n -k2 "$s.funcs" | awk -v s="$s" '{n++; c[n] = $2; t += $2; l += $1; if ($2 > 15) h++}
        END {p = int(0.95 * n + 0.999999); printf "| %s | %d | %.2f | %d | %d | %d | %.1f |\n", s, n, t / n, c[p], c[n], h, l / n}'
done
echo
for s in ours mod-host-f14a230 mod-host-main jalv; do
    echo "### top 15: $s (CCN NLOC function@lines@file)"
    sort -rn -k2 -k1 "$s.funcs" | head -15 | awk '{printf "  %3d %4d %s\n", $2, $1, $3}'
    echo
done

echo "## lines of code by area (function NLOC summed; the area from the function name)"
echo
area_of() { # <name> -> area
    local n=${1,,} name pat
    while IFS='|' read -r name pat; do
        if [[ $n =~ $pat ]]; then echo "$name"; return; fi
    done <<< "$AREAS"
    echo other
}
for s in ours mod-host-f14a230 mod-host-main jalv; do
    while read -r nloc _ loc; do
        f=${loc%%@*}; echo "$(area_of "$f")|$nloc|$loc"
    done < "$s.funcs" > "$s.areas"
done
order=$(cut -d'|' -f1 <<< "$AREAS"; echo other)
printf '| %-30s | %10s | %10s | %10s | %10s |\n' area ours mod-f14a230 mod-main jalv
echo '|---|---:|---:|---:|---:|'
while read -r a; do
    row="| $(printf '%-30s' "$a") |"
    for s in ours mod-host-f14a230 mod-host-main jalv; do
        v=$(awk -F'|' -v a="$a" '$1 == a {t += $2; n++} END {printf "%d (%d)", t, n}' "$s.areas")
        row="$row $(printf '%10s' "$v") |"
    done
    echo "$row"
done <<< "$order"
echo "(each cell: NLOC (functions))"

if [ $VERBOSE = 1 ]; then
    for s in ours mod-host-f14a230 mod-host-main jalv; do
        echo; echo "### areas: $s"; sort "$s.areas" | sed 's/^/  /'
    done
fi
