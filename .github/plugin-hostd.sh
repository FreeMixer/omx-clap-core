#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# plugin-hostd at the release tag PLUGIN_HOSTD_REF names, fetched into a directory: its protocol and pin headers are what
# this host builds against, and its daemon is what the pin test runs behind.
#
#   plugin-hostd.sh <dir>
set -eu

dir=${1:?usage: plugin-hostd.sh <dir>}
ref=${PLUGIN_HOSTD_REF:?}
case $ref in v[0-9]*.[0-9]*.[0-9]*) ;; *) echo "PLUGIN_HOSTD_REF is $ref: pin a release tag (vX.Y.Z), not a branch or a commit" >&2; exit 1 ;; esac
git init -q "$dir"
git -C "$dir" fetch -q --depth 1 https://github.com/FreeMixer/plugin-hostd "refs/tags/$ref"
git -C "$dir" checkout -q FETCH_HEAD
git config --global --add safe.directory "$(readlink -f "$dir")"
