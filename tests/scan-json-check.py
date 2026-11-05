#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# scan-json-check.py: omx-clap-scan --json held to what docs/scan.md says of it. For each given path the scanner is run
# twice, as text and as JSON, and
#   - the JSON validates against data/omx-clap-scan.schema.json (the file the package installs);
#   - the JSON holds the same data as the text: every plugin, parameter, port, note name, latency, voice figure, resource
#     hint, panel count, preset and file failure the text lists, each with the same value.
# The fields only the JSON carries (description, url, remote controls, enum labels, the panel's tables, ports' ids) are
# held by the schema alone.
#
#   scan-json-check.py [--self-test] <omx-clap-scan> <schema> <fixture>...
#
# --self-test runs the check once clean and once for each of a set of breaks, a field of the JSON changed after the scan,
# and each must be named; and requires that the fixtures between them list every
# kind of line the text has, so a fixture that goes missing is a failure and not a narrower test.
# Exit 0 held; 1 a difference; 2 usage.
import copy, json, math, os, re, subprocess, sys, tempfile

LINE_KINDS = ('param', 'in ', 'out', 'passthrough', 'note ports', 'note name', 'latency', 'voices', 'resource hints',
              'panel', 'preset', 'error')


# ---- a validator for the part of JSON Schema the schema file uses ----

def resolve(root, ref):
    node = root
    for part in ref[2:].split('/'):
        node = node[part]
    return node

def kind_of(value):
    if value is None: return 'null'
    if isinstance(value, bool): return 'boolean'
    if isinstance(value, int): return 'integer'
    if isinstance(value, float): return 'number'
    if isinstance(value, str): return 'string'
    if isinstance(value, list): return 'array'
    return 'object'

def type_ok(kind, want):
    return kind == want or (want == 'number' and kind == 'integer')

def validate(root, schema, value, where, out):
    if '$ref' in schema:
        return validate(root, resolve(root, schema['$ref']), value, where, out)
    kind = kind_of(value)
    if 'const' in schema and (value != schema['const'] or kind_of(schema['const']) != kind):
        out.append(f'{where}: not {schema["const"]!r}')
    if 'enum' in schema and value not in schema['enum']:
        out.append(f'{where}: {value!r} is not one of {schema["enum"]}')
    if 'type' in schema:
        want = schema['type'] if isinstance(schema['type'], list) else [schema['type']]
        if not any(type_ok(kind, w) for w in want):
            out.append(f'{where}: {kind} where {"/".join(want)} is declared')
            return
    if 'pattern' in schema and kind == 'string' and not re.search(schema['pattern'], value):
        out.append(f'{where}: {value!r} does not match {schema["pattern"]}')
    if kind in ('integer', 'number'):
        if 'minimum' in schema and value < schema['minimum']:
            out.append(f'{where}: {value} is below {schema["minimum"]}')
    if kind == 'array':
        if 'minItems' in schema and len(value) < schema['minItems']:
            out.append(f'{where}: {len(value)} items, {schema["minItems"]} at least')
        if 'maxItems' in schema and len(value) > schema['maxItems']:
            out.append(f'{where}: {len(value)} items, {schema["maxItems"]} at most')
        if 'items' in schema:
            for i, item in enumerate(value):
                validate(root, schema['items'], item, f'{where}[{i}]', out)
    if kind == 'object':
        props = schema.get('properties', {})
        for name in schema.get('required', []):
            if name not in value:
                out.append(f'{where}: no "{name}"')
        for name, item in value.items():
            if name in props:
                validate(root, props[name], item, f'{where}.{name}', out)
            elif schema.get('additionalProperties') is False:
                out.append(f'{where}: "{name}" is not declared')
            elif isinstance(schema.get('additionalProperties'), dict):
                validate(root, schema['additionalProperties'], item, f'{where}.{name}', out)
    if 'oneOf' in schema:
        held = []
        for alt in schema['oneOf']:
            sub = []
            validate(root, alt, value, where, sub)
            held.append(sub)
        if sum(1 for sub in held if not sub) != 1:
            out.append(f'{where}: matches {sum(1 for sub in held if not sub)} of the {len(held)} shapes it may take, one expected'
                       + ('' if any(not s for s in held) else ' (' + '; '.join(s[0] for s in held if s) + ')'))


# ---- the text, as the data it lists ----

def put_string(s):
    """what the text writes a string as: a control byte a ?"""
    return ''.join('?' if ord(c) < 0x20 or ord(c) == 0x7f else c for c in s)

def quoted(s):
    """what the text writes a quoted name as: a control byte a space"""
    return ''.join(' ' if ord(c) < 0x20 or ord(c) == 0x7f else c for c in s)

def num(text):
    return math.inf if text == 'inf' else float(text)

def unquote(text):
    m = re.match(r'"((?:[^"\\]|\\.)*)"', text)
    return re.sub(r'\\(.)', r'\1', m.group(1)), text[m.end():]

def text_port(line):
    m = re.match(r'(in |out) (main|aux) (\d+) channels (\w+)(.*)$', line)
    side, role, channels, kind, rest = m.groups()
    port = {'role': role, 'channels': int(channels), 'kind': kind}
    rest = rest.lstrip(' ')
    if kind == 'surround' and rest.startswith('map'):
        ids = re.match(r'map((?: \d+)*) ?', rest)
        port['map'] = [int(i) for i in ids.group(1).split()]
        rest = rest[ids.end():]
    elif kind == 'ambisonic':
        words = rest.split(' ', 3)
        port['ordering'], port['normalization'] = words[0], words[1]
        rest = ' '.join(words[2:])
        if rest.startswith('assumed'):
            port['assumed'] = True
            rest = rest[len('assumed'):].lstrip(' ')
    name, rest = unquote(rest)
    port['name'] = name
    port['jack'] = rest.split()
    return side.strip(), port

def parse_text(text):
    files, cur, plug = [], None, None
    for line in text.splitlines():
        if line.startswith('file '):
            cur, plug = {'path': line[5:], 'plugins': [], 'presets': []}, None
            files.append(cur)
        elif line.startswith('  error: '):
            cur['error'] = line[9:]
        elif line.startswith('  plugin '):
            m = re.match(r'  plugin (.*?) ("(?:[^"\\]|\\.)*") vendor (.*?) version (.*)$', line)
            name, _ = unquote(m.group(2))
            plug = {'id': m.group(1), 'name': name, 'vendor': m.group(3), 'version': m.group(4), 'params': [],
                    'inputs': [], 'outputs': [], 'note_names': []}
            cur['plugins'].append(plug)
        elif line.startswith('  preset "'):
            m = re.match(r'  preset "(.*)" (plugin|file) (\S+) key (\S+)(?: for (.*))?$', line)
            cur['presets'].append({'name': m.group(1), 'location_kind': m.group(2), 'location': None if m.group(3) == '-' else m.group(3),
                                   'load_key': None if m.group(4) == '-' else m.group(4), 'plugins': (m.group(5) or '').split(',') if m.group(5) else []})
        elif line.startswith('    '):
            body = line[4:]
            if body.startswith('features'):
                plug['features'] = body[len('features'):].strip()
            elif body.startswith('param '):
                m = re.match(r'param (\d+) (.*) \[(\S+) \.\. (\S+)\] default (\S+) (\S*)$', body)
                plug['params'].append({'id': int(m.group(1)), 'name': m.group(2), 'min': num(m.group(3)), 'max': num(m.group(4)),
                                       'default': num(m.group(5)), 'flags': [f for f in m.group(6).split(',') if f]})
            elif body.startswith(('in ', 'out ')):
                side, port = text_port(body)
                plug['inputs' if side == 'in' else 'outputs'].append(port)
            elif body.startswith('passthrough: host'):
                plug['passthrough'] = 'host'
            elif body.startswith('note ports'):
                m = re.match(r'note ports (\d+) in, (\d+) out$', body)
                plug['note_ports'] = [int(m.group(1)), int(m.group(2))]
            elif body.startswith('note name '):
                # a note name is written between quotes as it is, a quote in it not escaped: the last quote ends it
                m = re.match(r'note name "(.*)" port (-?\d+) key (-?\d+) channel (-?\d+)$', body)
                plug['note_names'].append({'name': m.group(1), 'port': int(m.group(2)), 'key': int(m.group(3)), 'channel': int(m.group(4))})
            elif body.startswith('latency '):
                plug['latency'] = int(body.split()[1])
            elif body.startswith('voices '):
                m = re.match(r'voices (\d+) of (\d+)(, overlapping notes)?$', body)
                plug['voice_info'] = {'count': int(m.group(1)), 'capacity': int(m.group(2)), 'overlapping_notes': bool(m.group(3))}
            elif body.startswith('resource hints refused: '):
                plug['hints_refused'] = body[len('resource hints refused: '):]
            elif body.startswith('resource hints'):
                hints = {}
                for item in body[len('resource hints'):].split(','):
                    word = item.split()
                    hints[word[0]] = (word[1] == 'yes') if len(word) == 2 else (int(word[1]), word[2])
                plug['hints'] = hints
            elif body.startswith('panel '):
                parts = body[len('panel '):].split(', ')
                family = None if parts[0] == 'of no family' else parts[0]
                counts = {}
                for p in parts[1:]:
                    m = re.match(r'(\d+) (.+)$', p)
                    if m:
                        counts[m.group(2)] = int(m.group(1))
                    else:
                        m = re.match(r'(.+) refused \((\d+) past (\d+)\)$', p)
                        counts[m.group(1)] = ('refused', int(m.group(2)), int(m.group(3)))
                plug['panel'] = {'family': family, 'counts': counts}
            elif body.startswith('error: '):
                plug['error'] = body[len('error: '):]
            else:
                raise ValueError(f'a text line the check does not know: {line!r}')
        else:
            raise ValueError(f'a text line the check does not know: {line!r}')
    return files

def kinds_in(text):
    return {k for k in LINE_KINDS if re.search(r'^( {2}| {4})' + re.escape(k), text, re.M)}


# ---- the JSON, as the same data ----

PANEL_TABLES = {'sections': 'sections', 'controls': 'controls', 'roles': 'roles', 'displays': 'displays',
                'placementDefaults': 'placement defaults'}

def json_port(p):
    port = {'role': p['role'], 'channels': p['channels'], 'kind': p['kind'], 'name': quoted(p['name']), 'jack': p.get('jack', [])}
    for key in ('map', 'ordering', 'normalization'):
        if key in p:
            port[key] = p[key]
    if p.get('assumed'):
        port['assumed'] = True
    return port

def json_data(doc):
    files = []
    for f in doc['files']:
        out = {'path': put_string(f['path']), 'plugins': [], 'presets': []}
        if 'error' in f:
            out['error'] = put_string(f['error'])
        for p in f.get('plugins', []):
            plug = {'id': put_string(p['id']), 'name': quoted(p['name']), 'vendor': put_string(p['vendor']),
                    'version': put_string(p['version']), 'features': ' '.join(put_string(x) for x in p['features']),
                    'params': [], 'inputs': [], 'outputs': [], 'note_names': []}
            if 'error' in p:
                plug['error'] = p['error']
            else:
                for q in p['params']:
                    plug['params'].append({'id': q['id'], 'name': put_string(q['name']), 'min': math.inf if q['min'] is None else q['min'],
                                           'max': math.inf if q['max'] is None else q['max'],
                                           'default': math.inf if q['default'] is None else q['default'], 'flags': q['flags']})
                plug['inputs'] = [json_port(x) for x in p['audio_ports']['inputs']]
                plug['outputs'] = [json_port(x) for x in p['audio_ports']['outputs']]
                if 'passthrough' in p['audio_ports']:
                    plug['passthrough'] = p['audio_ports']['passthrough']
                plug['note_ports'] = [p['note_ports']['inputs'], p['note_ports']['outputs']]
                plug['note_names'] = [{**n, 'name': put_string(n['name'])} for n in p.get('note_names', [])]
                if 'latency' in p:
                    plug['latency'] = p['latency']
                if 'voice_info' in p:
                    plug['voice_info'] = p['voice_info']
                if 'resource_hints' in p:
                    h = p['resource_hints']
                    if 'refused' in h:
                        plug['hints_refused'] = h['refused']
                    else:
                        known = {w: v for w, v in h.items() if w != 'declared' and v['figure'] is not None}
                        if known:
                            plug['hints'] = {w: v['figure'] if isinstance(v['figure'], bool) else (v['figure'], v['unit']) for w, v in known.items()}
                if p['panel'] and 'family' in p['panel']:
                    pan = p['panel']
                    counts = {}
                    for key, label in PANEL_TABLES.items():
                        t = pan[key]
                        counts[label] = ('refused', t['count'], t['max']) if isinstance(t, dict) else len(t)
                    plug['panel'] = {'family': pan['family'] if isinstance(pan['family'], str) else None, 'counts': counts}
            out['plugins'].append(plug)
        for r in f.get('presets', []):
            out['presets'].append({'name': put_string(r['name']), 'location_kind': r['location_kind'],
                                   'location': None if r['location'] is None else put_string(r['location']),
                                   'load_key': None if r['load_key'] is None else put_string(r['load_key']), 'plugins': r['plugins']})
        files.append(out)
    return files


# ---- the run ----

def scan(binary, args, json_mode):
    with tempfile.TemporaryDirectory() as scratch:
        r = subprocess.run([binary] + (['--json'] if json_mode else []) + args, capture_output=True, text=True, cwd=scratch)
    return r.returncode, r.stdout

def differences(text_files, json_files):
    bad = []
    if [f['path'] for f in text_files] != [f['path'] for f in json_files]:
        bad.append(f'files: text {[f["path"] for f in text_files]}, JSON {[f["path"] for f in json_files]}')
        return bad
    for t, j in zip(text_files, json_files):
        where = t['path']
        if t.get('error') != j.get('error') and not ('error' in t and 'error' in j):
            bad.append(f'{where}: the text says error {t.get("error")!r}, the JSON {j.get("error")!r}')
        if len(t['plugins']) != len(j['plugins']):
            bad.append(f'{where}: the text lists {len(t["plugins"])} plugins, the JSON {len(j["plugins"])}')
            continue
        if t['presets'] != j['presets']:
            bad.append(f'{where}: presets differ: text {t["presets"]}, JSON {j["presets"]}')
        for a, b in zip(t['plugins'], j['plugins']):
            for key in sorted(set(a) | set(b)):
                if a.get(key) != b.get(key):
                    bad.append(f'{where} {a["id"]}: {key}: text {a.get(key)!r}, JSON {b.get(key)!r}')
    return bad

def check(binary, schema, args, mutate=None):
    """the differences between the text and the JSON of one scan, and the schema's complaints about the JSON"""
    rc_t, text = scan(binary, args, False)
    rc_j, raw = scan(binary, args, True)
    bad = []
    if rc_t != rc_j:
        bad.append(f'exit status: text {rc_t}, JSON {rc_j}')
    try:
        doc = json.loads(raw)
    except ValueError as e:
        return [f'the JSON does not parse: {e}'], text
    if mutate:
        mutate(doc)
    complaints = []
    validate(schema, schema, doc, '$', complaints)
    bad += [f'schema: {c}' for c in complaints]
    try:
        bad += differences(parse_text(text), json_data(doc))
    except (KeyError, TypeError) as e:
        bad.append(f'the JSON is not the shape the text is compared with ({type(e).__name__}: {e})')
    return bad, text

def sabotages(doc):
    """(name, mutation of the JSON) each of which must be named by the check, on a plugin of the fixtures that has what it changes"""
    def first_plugin(d, has):
        for f in d['files']:
            for p in f.get('plugins', []):
                if has(p):
                    return p
        raise SystemExit(f'self-test: no fixture plugin to break "{has.__name__}" on')
    def with_params(p): return p.get('params')
    def with_inputs(p): return p.get('audio_ports', {}).get('inputs')
    def with_latency(p): return 'latency' in p
    def with_notes(p): return p.get('note_names')
    def with_voices(p): return 'voice_info' in p
    def with_hints(p): return 'resource_hints' in p
    def with_panel(p): return p.get('panel')
    def with_features(p): return p.get('features')
    def with_map(p): return any('map' in x for x in p.get('audio_ports', {}).get('inputs', []) + p.get('audio_ports', {}).get('outputs', []))
    def a_param_default(d): first_plugin(d, with_params)['params'][0].__setitem__('default', 12345.5)
    def a_param_flag(d): first_plugin(d, with_params)['params'][0]['flags'].append('hidden')
    def a_channel(d): first_plugin(d, with_inputs)['audio_ports']['inputs'][0].__setitem__('channels', 99)
    def a_jack_name(d): first_plugin(d, with_inputs)['audio_ports']['inputs'][0].__setitem__('jack', ['x'])
    def a_latency(d): first_plugin(d, with_latency).__setitem__('latency', 7)
    def a_note_name(d): first_plugin(d, with_notes)['note_names'][0].__setitem__('key', 1000)
    def a_voice(d): first_plugin(d, with_voices)['voice_info'].__setitem__('count', 999)
    def a_hint(d):
        h = first_plugin(d, with_hints)['resource_hints']
        word = next(w for w, v in h.items() if w != 'declared' and v['figure'] is not None)
        h[word]['figure'] = 42
    def a_panel_row(d): first_plugin(d, with_panel)['panel']['sections'].append({})
    def a_feature(d): first_plugin(d, with_features)['features'].append('extra')
    def a_map(d):
        p = first_plugin(d, with_map)
        port = next(x for x in p['audio_ports']['inputs'] + p['audio_ports']['outputs'] if 'map' in x)
        port['map'][0] += 1
    def a_plugin_dropped(d): d['files'][0]['plugins'].pop()
    def an_unknown_field(d): first_plugin(d, with_params)['params'][0]['colour'] = 'red'
    def a_wrong_type(d): first_plugin(d, with_params)['params'][0]['id'] = '0'
    def a_missing_field(d): del first_plugin(d, with_params)['audio_ports']
    def a_scanner(d): d['scanner'] = 'other'
    def a_failure_code(d):
        f = next(f for f in d['files'] if 'code' in f)
        f['code'] = 'who-knows'
    return [('a parameter default', a_param_default), ('a parameter flag', a_param_flag), ('a port width', a_channel),
            ('a JACK name', a_jack_name), ('a latency', a_latency), ('a note name', a_note_name), ('a voice count', a_voice),
            ('a resource hint', a_hint), ('a panel table', a_panel_row), ('a feature', a_feature), ('a surround map', a_map),
            ('a plugin dropped', a_plugin_dropped), ('a field the schema does not declare', an_unknown_field),
            ('a field of the wrong type', a_wrong_type), ('a field the schema requires, gone', a_missing_field),
            ('the scanner word', a_scanner), ('a failure code outside the schema', a_failure_code)]

def main(argv):
    self_test = argv[:1] == ['--self-test']
    if self_test:
        argv = argv[1:]
    if len(argv) < 3:
        sys.stderr.write('usage: scan-json-check.py [--self-test] <omx-clap-scan> <schema> <fixture>...\n')
        return 2
    binary, schema_path, fixtures = os.path.abspath(argv[0]), argv[1], argv[2:]
    schema = json.load(open(schema_path))
    scratch = tempfile.mkdtemp()
    broken = os.path.join(scratch, 'broken.clap')
    open(broken, 'w').write('not a plugin\n')
    # every fixture alone, then all of them with a file that is no plugin and a path that does not exist, in one document
    runs = [[os.path.abspath(f)] for f in fixtures]
    runs.append([os.path.abspath(f) for f in fixtures] + [broken, os.path.join(scratch, 'absent.clap')])
    seen, status = set(), 0
    for args in runs:
        bad, text = check(binary, schema, args)
        seen |= kinds_in(text)
        label = ' '.join(os.path.basename(a) for a in args) if len(args) < 3 else f'{len(args)} paths'
        if bad:
            status = 1
            print(f'FAIL {label}')
            for b in bad[:12]:
                print(f'     {b}')
        else:
            print(f'ok   {label}: the JSON validates and holds what the text lists')
    missing = [k for k in LINE_KINDS if k not in seen]
    if missing:
        status = 1
        print(f'FAIL the fixtures list no text line of kind {missing}: the check would hold nothing of them')
    # a failure of every kind the scanner has a code for is in the corpus when a file is broken and when one is absent
    rc, raw = scan(binary, runs[-1], True)
    codes = sorted(f['code'] for f in json.loads(raw)['files'] if 'code' in f)
    if len(codes) < 2:
        status = 1
        print(f'FAIL the broken and the absent file gave {codes} as failures, two expected')
    if self_test:
        everything = runs[-1]
        for name, mutate in sabotages(json.loads(raw)):
            bad, _ = check(binary, schema, everything, mutate)
            if bad:
                print(f'ok   self-test: {name} is named ({bad[0][:80]})')
            else:
                status = 1
                print(f'FAIL self-test: {name} changed in the JSON and the check did not notice')
    return status

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
