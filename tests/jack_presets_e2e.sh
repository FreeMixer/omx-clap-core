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
    for f in "$host" "$fixtures/fake_synth.clap" "$fixtures/fake_mover.clap" "$fixtures/fake_ext.clap" \
             "$bundles/omx-host-fixture.lv2/omx-host-fixture.so"; do
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
export FAKE_MOVER_AT=1000000000

pipewire >"$runtime/pw.log" 2>&1 &
for _ in $(seq 50); do [ -S "$runtime/pipewire-0" ] && break; sleep 0.1; done
[ -S "$runtime/pipewire-0" ] || { echo "pipewire did not come up" >&2; cat "$runtime/pw.log" >&2; exit 2; }

LV2_PATH=$bundles "$host" -n -p "$port" >"$runtime/host.log" 2>&1 &
pid=$!
for _ in $(seq 100); do grep -q 'ready!' "$runtime/host.log" && break; sleep 0.1; done
grep -q 'ready!' "$runtime/host.log" || { echo "omx-clap-host did not come up" >&2; cat "$runtime/host.log" >&2; exit 2; }

python3 -I - "$port" "$fixtures/fake_synth.clap" "$runtime" "$fixtures/fake_mover.clap" "$fixtures/fake_ext.clap" <<'PY'
import base64, json, socket, sys
port, synth, runtime, mover, ext = int(sys.argv[1]), sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
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
# a file: key, a space in its path written %20 (#401; an absolute path with no file: is a preset file of ours), and a file: key the plugin reports through on_error while answering true
before = len(loads())
r = ask('preset_load 1 file:' + (runtime + '/user presets/one.fspreset').replace(' ', '%20'))
check(r == 'resp 0' and loads()[before:] == ['0|%sone.fspreset|-' % path], 'a file: key with %%20 is decoded and loads (%s, %s)' % (r, loads()[before:]))
r = ask('preset_load 1 file:' + (runtime + '/user presets/none.fspreset').replace(' ', '%20'))
check(r != 'resp 0', 'a file: key the plugin reports through on_error is not answered resp 0 (%s)' % r)

lv2 = 'urn:openmixer:test:host-fixture'
check(ask('add %s 2' % lv2) == 'resp 2', 'the LV2 fixture is added through the adapter')
check(ask('preset_list 2') == 'resp 2', "preset_list counts the bundle's two presets for the fixture, not the CV twin's")
rows = [ask('preset_get 2 %d' % i) for i in range(2)]
check(rows == ['resp 0 %s-preset:loud loud' % lv2, 'resp 0 %s-preset:quiet quiet' % lv2],
      "preset_get gives each LV2 preset's URI as its key and its label: %s" % rows)
check(ask('preset_load 2 %s-preset:quiet' % lv2) == 'resp 0', 'a listed LV2 key loads')
check(ask('preset_load 2 %s-preset:none' % lv2) != 'resp 0', 'an LV2 key the bundle does not hold is refused')

# a preset file of ours: fake_mover, parameters and no state
check(ask('add clap:%s#org.omx-clap-host.test.mover 3' % mover) == 'resp 3', 'the mover is added')
saved = runtime + '/user presets/my pad.json'
check(ask('param_set 3 0 0.3125') == 'resp 0', 'level set to 0.3125')
r = ask('preset_save 3 "%s" My Pad' % saved)
check(r == 'resp 0', 'preset_save writes the file at a path with a space, named by the rest of the line (%s)' % r)
try:
    doc = json.load(open(saved))
except (OSError, ValueError) as e:
    doc = {}
    check(False, 'the saved file reads as JSON (%s)' % e)
rows = {p['symbol']: p['value'] for p in doc.get('params', [])}
check(doc.get('format') == 'org.openmixer.preset/2' and doc.get('name') == 'My Pad' and doc.get('carries') == 'params'
      and 'state' not in doc, 'the file is /2, named, carries params and no state: %s' % {k: doc.get(k) for k in ('format', 'name', 'carries')})
plugin = doc.get('plugin', {})
check(plugin.get('id') == 'org.omx-clap-host.test.mover' and plugin.get('version') is not None
      and str(plugin.get('fingerprint', '')).startswith('omx-layout/1:'), 'its plugin is the mover with a version and a layout fingerprint: %s' % plugin)
# pin_layout (plugin-hostd asks it after an add whose pin has no layout yet) answers the fingerprint the file carries
check(ask('pin_layout 3') == 'resp 0 ' + str(plugin.get('fingerprint')), 'pin_layout 3 answers the layout the preset file names: %s' % ask('pin_layout 3'))
check(ask('pin_layout 9') == 'resp -3', 'pin_layout of an instance that does not exist answers -3')
check(rows.get('0') == 0.3125 and '3' in rows, 'level is written as 0.3125 exactly, trim beside it: %s' % rows)
check(ask('param_set 3 0 0.875') == 'resp 0', 'level moved to 0.875')
r = ask('preset_load 3 "%s"' % saved)
check(r == 'resp 0' and ask('param_get 3 0').split()[-1] in ('0.3125', '0.312500'), 'preset_load of the file puts level back at 0.3125 (%s, %s)' % (r, ask('param_get 3 0')))
doc['bank'] = 'Mine'
json.dump(doc, open(saved, 'w'))
first_id = doc.get('id')
check(ask('preset_save 3 "%s" My Pad 2' % saved) == 'resp 0', 'a save over the file')
again = json.load(open(saved))
check(again.get('id') == first_id and again.get('bank') == 'Mine' and again.get('createdAt') == doc.get('createdAt')
      and again.get('name') == 'My Pad 2', 'a rewrite keeps the id, createdAt and a member this host does not write, and takes the new name')
def refused(edit, code, what):
    bad = runtime + '/bad.json'
    d = json.load(open(saved))
    edit(d)
    json.dump(d, open(bad, 'w'))
    r = ask('preset_load 3 %s' % bad)
    check(r == 'resp -104 ' + code, '%s answers %s (%s)' % (what, code, r))
refused(lambda d: d['plugin'].__setitem__('version', '9.9.9'), 'PRESET_PLUGIN_CHANGED', 'another plugin version')
refused(lambda d: d['plugin'].__setitem__('fingerprint', 'omx-layout/1:' + '0' * 64), 'PRESET_PLUGIN_CHANGED', 'another layout')
refused(lambda d: d.__setitem__('state', {'encoding': 'base64', 'context': 'preset', 'bytes': 'AAEC'}), 'PRESET_NEEDS_STATE',
        'a state body for a plugin with no state')
refused(lambda d: d.__setitem__('format', 'org.openmixer.preset/9'), 'PRESET_INVALID', 'an unknown format version')
open(runtime + '/junk.json', 'w').write('not json')
check(ask('preset_load 3 %s/junk.json' % runtime) == 'resp -104 PRESET_INVALID', 'a file that is not JSON answers PRESET_INVALID')
check(ask('preset_save 3 %s/junk.json X' % runtime) == 'resp -104 PRESET_INVALID', 'a save over a file that is not a preset is refused')
check(ask('preset_save 3 relative.json X') == 'resp -104 PRESET_INVALID', 'a relative path is refused')
check(ask('preset_load 3 plugin:none') == 'resp -104 PRESET_NOT_OFFERED', 'a key the plugin does not offer answers PRESET_NOT_OFFERED')
r = ask('preset_load 3 urn:no:such:preset')
check(r == 'resp -902', "a location in mod-host's form is answered as mod-host answers it, with no code word: a CLAP plugin with no clap.preset-load the bare -902 (%s)" % r)

# fake_ext: a state body, saved in the preset context
check(ask('add clap:%s#org.omx-clap-host.test.ext 4' % ext) == 'resp 4', 'fake_ext is added')
withstate = runtime + '/ext.json'
check(ask('preset_save 4 %s Ext' % withstate) == 'resp 0', 'preset_save of a plugin with state')
d = json.load(open(withstate))
st = d.get('state', {})
check(d.get('carries') == 'params+state' and st.get('context') == 'preset' and base64.b64decode(st.get('bytes', '')) == b'preset',
      'it carries params+state, its body the plugin saved for the preset context: %s' % st)
check(ask('preset_load 4 %s' % withstate) == 'resp 0', 'the state file loads back')
sys.exit(1 if failed else 0)
PY
status=$?
kill -0 "$pid" 2>/dev/null || { echo "FAIL omx-clap-host is gone:"; tail -5 "$runtime/host.log"; status=1; }
exit $status
