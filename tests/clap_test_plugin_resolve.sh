#!/bin/bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# clap_test_plugin_resolve.sh — the Makefile's CLAP_TEST_PLUGIN default (#200): an installed omx-plugins
# package (its clap dir via pkg-config omxplugins) or an OMX_PLUGINS_SOURCE checkout's build output, tried in
# that order; neither found, test-scan, test-jack and test-identity print a named SKIP and exit 0 rather than
# failing on a path that is not there. A path given on the command line or in the environment is tried as
# given, unresolved included, so pointing it at nothing on purpose still goes red.
#
#   clap_test_plugin_resolve.sh --self-test
#
# Reads `make -n <target>` (a dry run: no build toolchain needed, so this runs on any desk) over the real
# Makefile with PKG_CONFIG_LIBDIR, PKG_CONFIG_PATH and OMX_PLUGINS_SOURCE it controls, scratch directories
# standing in for an installed omx-plugins package and a source checkout, and checks the SKIP line and the
# plugin path the recipe would use.
set -euo pipefail

[ "${1:-}" = --self-test ] || { echo "usage: $0 --self-test" >&2; exit 2; }

root=$(cd "$(dirname "$0")/.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
failures=0

# a dry run of <target> with the environment and make variables that follow; the output on stdout. PKG_CONFIG_PATH
# is $pkg_config_path (empty unless a caller sets it): test-jack and test-identity need mod-host-protocol.pc found,
# or the dry run still really runs the sub-make that builds a checkout's library (GNU make always runs a recipe
# that names $(MAKE), -n included), which fails here with no ../wt-mod-host-clap checkout. That sub-make is no part
# of what this script checks, so it is routed around rather than worked around with a real checkout.
pkg_config_path=
dry() { # <target> [make-var=value...]
  local target=$1; shift
  ( cd "$root" && PKG_CONFIG_LIBDIR="$scratch/no-pkgconfig" PKG_CONFIG_PATH="$pkg_config_path" \
      make -n "$target" "$@" ) 2>/dev/null
}

check() { # <name> <ok-or-not> <description>
  if [ "$2" = 1 ]; then echo "ok   $1: $3"; else echo "FAIL $1: $3"; failures=$((failures + 1)); fi
}

none=$scratch/nothing-here.clap          # named but never created: stands for "not installed, no checkout"
src_ok=$scratch/plugins-checkout
mkdir -p "$src_ok/bin"; : > "$src_ok/bin/omx-delay.clap"
pc_dir=$scratch/pkgconfig
pc_clapdir=$scratch/pkg-clapdir
mkdir -p "$pc_dir" "$pc_clapdir"; : > "$pc_clapdir/omx-delay.clap"
cat > "$pc_dir/omxplugins.pc" <<EOF
clapdir=$pc_clapdir
Name: omxplugins
Description: stand-in for the self-test
Version: 0.1.0
EOF
proto_dir=$scratch/pkgconfig-proto        # mod-host-protocol found, omxplugins not: test-jack, test-identity
mkdir -p "$proto_dir"
cat > "$proto_dir/mod-host-protocol.pc" <<'EOF'
Name: mod-host-protocol
Description: stand-in for the self-test
Version: 0.1.0
Cflags:
Libs:
EOF

# neither pkg-config omxplugins nor an OMX_PLUGINS_SOURCE checkout: a named SKIP, "-" to the test (never a path
# that is not there)
out=$(dry test-scan "OMX_PLUGINS_SOURCE=$none")
check neither-found "$([ "$(printf '%s' "$out" | grep -c 'SKIP.*test-scan')" -ge 1 ] && printf 1 || printf 0)" \
  "test-scan prints a named SKIP when no omx-delay.clap is found"
check neither-found-dash "$(printf '%s' "$out" | grep -qE 'clap_scan_test .* -\s' && printf 1 || printf 0)" \
  "test-scan passes the no-plugin sentinel, not the unresolved default"

# an OMX_PLUGINS_SOURCE checkout: its built omx-delay.clap, no SKIP
out=$(dry test-scan "OMX_PLUGINS_SOURCE=$src_ok")
check source-checkout "$(printf '%s' "$out" | grep -q SKIP && printf 0 || printf 1)" \
  "no SKIP once an OMX_PLUGINS_SOURCE checkout has the plugin"
check source-checkout-path "$(printf '%s' "$out" | grep -qF "$src_ok/bin/omx-delay.clap" && printf 1 || printf 0)" \
  "test-scan is given the checkout's omx-delay.clap"

# pkg-config omxplugins installed: its clapdir, no SKIP, OMX_PLUGINS_SOURCE never consulted
out=$(cd "$root" && PKG_CONFIG_LIBDIR= PKG_CONFIG_PATH="$pc_dir" make -n test-scan "OMX_PLUGINS_SOURCE=$none" 2>/dev/null)
check pkg-config "$(printf '%s' "$out" | grep -q SKIP && printf 0 || printf 1)" \
  "no SKIP once pkg-config omxplugins resolves a clapdir"
check pkg-config-path "$(printf '%s' "$out" | grep -qF "$pc_clapdir/omx-delay.clap" && printf 1 || printf 0)" \
  "test-scan is given the installed package's omx-delay.clap"

# a path named on purpose, real or not: tried as given, never turned into a SKIP (the sabotage this issue's
# acceptance names: point CLAP_TEST_PLUGIN at a bad path, expect red, not a quiet skip)
bad=$scratch/sabotage-bad-path.clap
out=$(dry test-scan "OMX_PLUGINS_SOURCE=$none" "CLAP_TEST_PLUGIN=$bad")
check sabotage-no-skip "$(printf '%s' "$out" | grep -q SKIP && printf 0 || printf 1)" \
  "an explicit CLAP_TEST_PLUGIN is tried as given, no SKIP"
check sabotage-path "$(printf '%s' "$out" | grep -qF "$bad" && printf 1 || printf 0)" \
  "test-scan is given the named path even though it does not exist, so the run goes red"

# test-jack and test-identity: the same named SKIP when neither is found, and the explicit override still used
pkg_config_path=$proto_dir
for t in test-jack test-identity; do
  out=$(dry "$t" "OMX_PLUGINS_SOURCE=$none")
  check "$t-skip" "$(printf '%s' "$out" | grep -q "SKIP.*$t" && printf 1 || printf 0)" \
    "$t prints a named SKIP when no omx-delay.clap is found"
  out=$(dry "$t" "OMX_PLUGINS_SOURCE=$none" "CLAP_TEST_PLUGIN=$bad")
  check "$t-sabotage" "$(printf '%s' "$out" | grep -q SKIP && printf 0 || printf 1)" \
    "$t tries an explicit CLAP_TEST_PLUGIN as given, no SKIP"
done

echo "${failures} failing"
[ "$failures" -eq 0 ]
