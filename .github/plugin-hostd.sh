#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
# plugin-hostd at the release tag PLUGIN_HOSTD_REF names, fetched into a directory: its protocol and pin headers are what
# this host builds against, and its daemon is what the pin test runs behind.
#
#   plugin-hostd.sh <dir>
set -eu

dir=${1:?usage: plugin-hostd.sh <dir>}
ref=${PLUGIN_HOSTD_REF:?}
# A release tag (vX.Y.Z), or, while plugin-hostd and this host move to a new protocol together, the full commit the
# release PR of plugin-hostd is at: the two cannot be built against each other from released tags.
case $ref in
  v[0-9]*.[0-9]*.[0-9]*) want=refs/tags/$ref ;;
  [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) want=$ref ;;
  *) echo "PLUGIN_HOSTD_REF is $ref: pin a release tag (vX.Y.Z) or a full commit, not a branch" >&2; exit 1 ;;
esac
git init -q "$dir"
git -C "$dir" fetch -q --depth 1 https://github.com/FreeMixer/plugin-hostd "$want"
git -C "$dir" checkout -q FETCH_HEAD
git config --global --add safe.directory "$(readlink -f "$dir")"
