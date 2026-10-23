#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# omx-dsp-floor-check.sh — the Makefile's omx-dsp gate (#77): pkg-config when it names omxdsp at or above
# OMX_DSP_FLOOR, else an OMX_DSP_DIR checkout whose own VERSION file must read at or above the same floor. Neither
# route may silently accept a dependency below the floor omx-clap-core.pc.in and debian/control both carry; above
# the floor there is no ceiling (#77: a floor-only policy, not a defect).
#
#   tests/omx-dsp-floor-check.sh    every case below, each a `make print-dsp-cflags` with a fake PKG_CONFIG
#
# Exit 0 every case behaved; 1 one did not.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

fail=0

# a fake pkg-config that fails --exists and has no omxdsp.cflags: every case below forces the checkout fallback
cat > "$WORK/pkg-config" <<'EOF'
#!/bin/sh
exit 1
EOF
chmod +x "$WORK/pkg-config"

mk() { # <version-line-or-empty>
  rm -rf "$WORK/dsp"; mkdir -p "$WORK/dsp/include"
  [ -z "$1" ] || printf '%s\n' "$1" > "$WORK/dsp/VERSION"
}

run() { # fallback path: always via the fake pkg-config above
  make --no-print-directory -C "$HERE" print-dsp-cflags PKG_CONFIG="$WORK/pkg-config" OMX_DSP_DIR="$WORK/dsp" OMX_CONTRACT_DIR="$WORK/dsp" \
    >"$WORK/out" 2>"$WORK/err"
}

case_refuses() { # <label> <version-line-or-empty>
  mk "$2"
  if run; then echo "FAIL $1: make print-dsp-cflags accepted $2 (should refuse)"; cat "$WORK/err"; fail=1
  else echo "ok   $1: refused ($(tail -1 "$WORK/err"))"; fi
}

case_accepts() { # <label> <version-line> <must-appear-in-output>
  mk "$2"
  if run; then
    if grep -qF -- "$3" "$WORK/out"; then echo "ok   $1: accepted, DSP_CFLAGS has '$3'"
    else echo "FAIL $1: accepted but DSP_CFLAGS lacks '$3': $(cat "$WORK/out")"; fail=1; fi
  else echo "FAIL $1: make print-dsp-cflags refused $2 (should accept)"; cat "$WORK/err"; fail=1
  fi
}

# sabotage arm: every version below the floor, and the floor-less checkout, must refuse
case_refuses "below-floor-patch"  "0.17.9"
case_refuses "below-floor-minor"  "0.9.0"
case_refuses "no-version-file"    ""
case_refuses "garbage-version"    "not-a-version"

# the floor and anything above it accept, with no ceiling (#77 item 1: a floor-only policy)
case_accepts "at-floor"           "0.18.0" "-ffp-contract=off"
case_accepts "above-floor-patch"  "0.18.1" "-ffp-contract=off"
case_accepts "above-floor-minor"  "0.24.1" "-ffp-contract=off"
case_accepts "above-floor-major"  "1.0.0"  "-ffp-contract=off"

# the Makefile's own --exists call must carry the floor atom (an installed omxdsp below it must not pass unversioned):
# a fake pkg-config that logs its argv and refuses everything, so the only way print-dsp-cflags can reach "ok   logged"
# below is through the checkout fallback this script already proved refuses below-floor and accepts at/above it
cat > "$WORK/pkg-config-log" <<EOF
#!/bin/sh
echo "\$@" >> "$WORK/argv.log"
exit 1
EOF
chmod +x "$WORK/pkg-config-log"
: > "$WORK/argv.log"
mk "0.18.0"
make --no-print-directory -C "$HERE" print-dsp-cflags PKG_CONFIG="$WORK/pkg-config-log" OMX_DSP_DIR="$WORK/dsp" OMX_CONTRACT_DIR="$WORK/dsp" \
    >"$WORK/out" 2>"$WORK/err" || true
if grep -qF -- "--exists omxdsp >= 0.18.0" "$WORK/argv.log"; then
  echo "ok   exists-atom-carries-floor: --exists was called with 'omxdsp >= 0.18.0', not a bare name"
else
  echo "FAIL exists-atom-carries-floor: argv.log lacks the floor atom: $(cat "$WORK/argv.log")"; fail=1
fi

[ "$fail" = 0 ] || exit 1
echo "omx-dsp-floor-check: every case behaved"
