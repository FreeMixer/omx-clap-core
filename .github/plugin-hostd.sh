#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# plugin-hostd at the commit PLUGIN_HOSTD_REF names, fetched into a directory: its protocol and pin headers are what
# this host builds against, and its daemon is what the pin test runs behind.
#
#   plugin-hostd.sh <dir>
set -eu

dir=${1:?usage: plugin-hostd.sh <dir>}
ref=${PLUGIN_HOSTD_REF:?}
git init -q "$dir"
git -C "$dir" fetch -q --depth 1 https://github.com/FreeMixer/plugin-hostd "$ref"
git -C "$dir" checkout -q FETCH_HEAD
[ "$(git -C "$dir" rev-parse HEAD)" = "$ref" ] || { echo "plugin-hostd is not at $ref" >&2; exit 1; }
git config --global --add safe.directory "$(readlink -f "$dir")"
