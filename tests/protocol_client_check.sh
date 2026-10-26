#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# No test or bench writes a NUL-ended command by hand: the line protocol's client is plugin-hostd-tests' lib/protocol.sh
# and lib/plugin_hostd_client.py (FreeMixer/plugin-hostd#177), not a send() of a script's own. A copy of the shell form
# (printf '%s\0' ... >&<fd>) or of the Python form (sendall(<line>.encode() + b'\0')) is red; the check proves it can
# see one by running on a copy first.
#
#   protocol_client_check.sh [<dir>...]   (default: tests and bench)
set -u
here=$(cd "$(dirname "$0")" && pwd)
shell_copy="printf '%s\\\\0' .*>&"
py_copy="sendall\\(.*b.\\\\0"

scan() {
    grep -rEn --include='*.sh' --exclude=protocol_client_check.sh -e "$shell_copy" "$@" ; grep -rEn --include='*.py' -e "$py_copy" "$@"
}

# the check sees a copy of each form, so an empty result below means none, not a pattern that matches nothing
probe=$(mktemp -d)
trap 'rm -rf "$probe"' EXIT
printf '%s\n' "send() {" "    printf '%s\\0' \"\$1\" >&3" "}" >"$probe/copy.sh"
printf '%s\n' "    def send(self, line):" "        self.s.sendall(line.encode() + b'\\0')" >"$probe/copy.py"
[ "$(scan "$probe" | wc -l)" -eq 2 ] || { echo "protocol_client_check: cannot see a hand-written send in the probe" >&2; exit 2; }

[ $# -gt 0 ] || set -- "$here" "$here/../bench"
found=$(scan "$@")
if [ -n "$found" ]; then
    echo "a hand-written NUL-framed send: source lib/protocol.sh or import plugin_hostd_client instead" >&2
    echo "$found" >&2
    exit 1
fi
echo "protocol_client_check: no test or bench writes its own send"
