#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# preset_list, preset_get and preset_load over the host's socket (docs/presets.md), in a PipeWire of its own: the script
# re-runs itself in a private user, net, pid and mount namespace, starts pipewire on a private runtime dir and the host
# with tests/fake_synth.clap, whose preset-discovery factory lists two presets of a PLUGIN location for synth-poly (one
# also naming a plugin of another format), one for the file's plain synth only that synth-poly's list leaves out
# (FAKE_SYNTH_PRESET_OTHER), and the .fspreset files of FAKE_SYNTH_PRESET_DIR (one file of one
# preset, one container of two). Each listed key loads, and the plugin is handed the location and key it named.
#
# OMX_CLAP_HOST:   the binary (default ./omx-clap-host)
# PRESETS_PORT:    the socket port (default 5575)
# FIXTURES:        the directory of fake_synth.clap (default tests/)
# LV2_BUNDLES:     the directory holding omx-host-fixture.lv2, built (default build/lv2)

set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
host=${OMX_CLAP_HOST:-$root/omx-clap-host}
port=${PRESETS_PORT:-5575}
fixtures=${FIXTURES:-$here}
bundles=${LV2_BUNDLES:-$root/build/lv2}

if [ -z "${PRESETS_INSIDE:-}" ]; then
    for f in "$host" "$fixtures/fake_synth.clap" "$bundles/omx-host-fixture.lv2/omx-host-fixture.so"; do
        [ -e "$f" ] || { echo "missing $f" >&2; exit 2; }
    done
    PRESETS_INSIDE=1 OMX_CLAP_HOST=$host PRESETS_PORT=$port LV2_BUNDLES=$bundles \
        exec unshare --user --map-root-user --net --pid --fork --mount-proc "$0" "$@"
fi

runtime=$(mktemp -d /tmp/omx-clap-host-presets.XXXXXX)
export XDG_RUNTIME_DIR=$runtime PIPEWIRE_RUNTIME_DIR=$runtime PULSE_RUNTIME_PATH=$runtime/pulse
unset DBUS_SESSION_BUS_ADDRESS
cleanup() {
    kill -TERM -- -1 2>/dev/null
    wait 2>/dev/null
    rm -rf "${runtime:?}"
}
trap cleanup EXIT

ip link set lo up 2>/dev/null

mkdir -p "$runtime/user presets"
printf 'File One\n' >"$runtime/user presets/one.fspreset"
printf '#container\nBox A=a\nBox B=b\n' >"$runtime/user presets/box.fspreset"
export FAKE_SYNTH_PRESET_DIR="$runtime/user presets" FAKE_SYNTH_PRESET_LOG=$runtime/loads.txt FAKE_SYNTH_PRESET_OTHER=1

pipewire >"$runtime/pw.log" 2>&1 &
for _ in $(seq 50); do [ -S "$runtime/pipewire-0" ] && break; sleep 0.1; done
[ -S "$runtime/pipewire-0" ] || { echo "pipewire did not come up" >&2; cat "$runtime/pw.log" >&2; exit 2; }

LV2_PATH=$bundles "$host" -n -p "$port" >"$runtime/host.log" 2>&1 &
pid=$!
for _ in $(seq 100); do grep -q 'ready!' "$runtime/host.log" && break; sleep 0.1; done
grep -q 'ready!' "$runtime/host.log" || { echo "omx-clap-host did not come up" >&2; cat "$runtime/host.log" >&2; exit 2; }

python3 -I - "$port" "$fixtures/fake_synth.clap" "$runtime" <<'PY'
import socket, sys
port, synth, runtime = int(sys.argv[1]), sys.argv[2], sys.argv[3]
s = socket.create_connection(('127.0.0.1', port), timeout=20)
def ask(line):
    s.sendall(line.encode() + b'\0')
    buf = b''
    while not buf.endswith(b'\0'):
        chunk = s.recv(65536)
        if not chunk:
            raise SystemExit('FAIL the host closed the socket after: ' + line)
        buf += chunk
    return buf.rstrip(b'\0').decode()
failed = 0
def check(ok, what):
    global failed
    print(('ok   ' if ok else 'FAIL ') + what)
    failed += not ok
def loads():
    try:
        return open(runtime + '/loads.txt').read().splitlines()
    except OSError:
        return []

check(ask('add clap:%s#org.omx-clap-host.test.synth-poly 1' % synth) == 'resp 1', 'the synth is added')
d = runtime + '/user%20presets/'
want = [('plugin:init', 'Init'), ('plugin:bright', 'Bright "pad"'),
        ('file:' + d + 'box.fspreset#a', 'Box A'), ('file:' + d + 'box.fspreset#b', 'Box B'),
        ('file:' + d + 'one.fspreset', 'File One')]
count = ask('preset_list 1')
check(count == 'resp %d' % len(want), 'preset_list counts the presets for synth-poly and leaves out the one for the plain synth (%s)' % count)
got = [ask('preset_get 1 %d' % i) for i in range(len(want))]
check(got == ['resp 0 %s %s' % kn for kn in want], 'preset_get gives each key, a space written %%20, and its name: %s' % got)
check(not any(k.split(' ')[2].startswith('/') for k in got), 'no key is an absolute path, which plugin-hostd reads as a preset file')
check(ask('preset_get 1 %d' % len(want)) == 'resp -902', 'an index past the list answers -902')
check(ask('preset_get 1 x') == 'resp -902', 'an index that is no number answers -902')
check(ask('preset_list 9') == 'resp -3', 'an instance that does not exist answers -3')
path = runtime + '/user presets/'
for (key, name), record in zip(want, ['1|-|init', '1|-|bright', '0|%sbox.fspreset|a' % path, '0|%sbox.fspreset|b' % path,
                                      '0|%sone.fspreset|-' % path]):
    before = len(loads())
    r = ask('preset_load 1 ' + key)
    check(r == 'resp 0' and loads()[before:] == [record], 'preset_load %s hands the plugin %s (%s, %s)' % (key, record, r, loads()[before:]))
check(ask('preset_load 1 plugin:missing') != 'resp 0', 'a key the plugin refuses is not answered resp 0')

lv2 = 'urn:openmixer:test:host-fixture'
check(ask('add %s 2' % lv2) == 'resp 2', 'the LV2 fixture is added through the adapter')
check(ask('preset_list 2') == 'resp 2', "preset_list counts the bundle's two presets for the fixture, not the CV twin's")
rows = [ask('preset_get 2 %d' % i) for i in range(2)]
check(rows == ['resp 0 %s-preset:loud loud' % lv2, 'resp 0 %s-preset:quiet quiet' % lv2],
      "preset_get gives each LV2 preset's URI as its key and its label: %s" % rows)
check(ask('preset_load 2 %s-preset:quiet' % lv2) == 'resp 0', 'a listed LV2 key loads')
check(ask('preset_load 2 %s-preset:none' % lv2) != 'resp 0', 'an LV2 key the bundle does not hold is refused')
sys.exit(1 if failed else 0)
PY
status=$?
kill -0 "$pid" 2>/dev/null || { echo "FAIL omx-clap-host is gone:"; tail -5 "$runtime/host.log"; status=1; }
exit $status
