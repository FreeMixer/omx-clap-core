#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# scan-fields-check.py — omx-clap-scan --fields held to the vocabulary it spells (plugin-hostd docs/formats.md, "The
# field dump"): the names `--fields --list` writes are omx-contract's TERMs, and the dumps of this tree's fixtures are
# records a table can read.
#
#   scan-fields-check.py <omx-clap-scan> <formats dir> <fake.clap> <fake_synth.clap> <lv2 fixtures dir> [<tables dir>]
#
# <formats dir> holds omx-contract's TERM files (data/formats, or <contractdir>/3/omx-contract/formats). The list holds
# every CLAP extension, plugin feature and port type TERM by its id and value, and nothing else; every LV2 property
# TERM as <prefix>:<name> (lv2:port and lv2:index aside: the scopes are those), with rdf:type, rdfs:comment,
# rdfs:seeAlso, doap:name, doap:license and doap:maintainer, and nothing else. Each dump starts with the version line,
# every record is <scope> TAB <field> TAB <value> with a scope of the format, a CLAP dump names its clap_entry and an
# LV2 dump its rdfs:seeAlso; the LV2 one is made with the bundle's binary absent. With <tables dir> (plugin-hostd's
# data/formats), every field a dump writes has a row of its table. --self-test is not offered: each check is a set
# equality whose failure names the difference.
# Exit 0 held; 1 a difference; 2 usage.
import glob, json, os, re, subprocess, sys

if len(sys.argv) not in (6, 7):
    sys.stderr.write(open(__file__).read().split('\n')[7][2:] + '\n')
    sys.exit(2)
scan, formats, fake, synth, lv2dir = sys.argv[1:6]
tables = sys.argv[6] if len(sys.argv) == 7 else None
bad = []

def terms(fmt):
    for f in sorted(glob.glob(os.path.join(formats, fmt, '**', '*.json'), recursive=True)):
        for v in json.load(open(f)).values():
            if isinstance(v, dict) and v.get('kind') == 'terms':
                yield from v['terms'].values()

listed = [l.split('\t') for l in subprocess.run([scan, '--fields', '--list'], check=True, capture_output=True, text=True).stdout.splitlines()]
if listed[0] != ['# omx-clap-scan --fields 1']:
    bad.append(f'--list starts {listed[0]}')
for kind, term in (('extension', 'extension'), ('feature', 'plugin-feature'), ('port-type', 'port-type')):
    have = {(r[2], r[3]) for r in listed[1:] if r[:2] == ['clap', kind]}
    want = {(t['id'], t['value']) for t in terms('clap') if t['kind'] == term}
    for d in sorted(want - have): bad.append(f'the list lacks the CLAP {kind} {d[0]} ({d[1]})')
    for d in sorted(have - want): bad.append(f'the list has the CLAP {kind} {d[0]} ({d[1]}), no TERM')
prefixes = {r[2]: r[3] for r in listed[1:] if r[:2] == ['lv2', 'prefix']}
def curie(uri):
    for p, ns in prefixes.items():
        if uri.startswith(ns) and re.fullmatch(r'[^#/]+', uri[len(ns):]): return f'{p}:{uri[len(ns):]}'
    bad.append(f'no prefix covers the LV2 property {uri}')
have = {r[2] for r in listed[1:] if r[:2] == ['lv2', 'field']}
want = {curie(t['uri']) for t in terms('lv2') if t['kind'] == 'property'} - {'lv2:port', 'lv2:index'}
want |= {'rdf:type', 'rdfs:comment', 'rdfs:seeAlso', 'doap:name', 'doap:license', 'doap:maintainer'}
for d in sorted(want - have): bad.append(f'the list lacks the LV2 field {d}')
for d in sorted(have - want): bad.append(f'the list has the LV2 field {d}, which no TERM or table row names')

def ids(path):
    out = subprocess.run([scan, '--json', path], check=True, capture_output=True, text=True).stdout
    return [p['id'] for p in json.loads(out)['files'][0]['plugins']]

def dump(target, env=None):
    r = subprocess.run([scan, '--fields', target], capture_output=True, text=True, env=env)
    if r.returncode != 0:
        bad.append(f'--fields {target} exits {r.returncode}: {r.stderr.strip()}')
        return []
    lines = r.stdout.splitlines()
    if not lines or lines[0] != '# omx-clap-scan --fields 1':
        bad.append(f'--fields {target}: no version line')
    recs = [l.split('\t') for l in lines[1:]]
    scope = r'plugin|param:\d+|(audio|note)_(in|out):\d+' if target.startswith('clap:') else r'plugin|port:\d+'
    for rec in recs:
        if len(rec) != 3 or not re.fullmatch(scope, rec[0]) or not rec[1] or not rec[2]:
            bad.append(f'--fields {target}: the record {rec} is no <scope> TAB <field> TAB <value>')
    return recs

dumps = []
for path in (fake, synth):
    for i in ids(path):
        recs = dump(f'clap:{path}#{i}')
        if ['plugin', 'clap_entry', os.path.basename(path)] not in recs: bad.append(f'clap:{path}#{i}: no clap_entry record')
        dumps.append(('clap', recs))
manifests = sorted(glob.glob(os.path.join(lv2dir, '*.lv2', 'manifest.ttl')))
uris = [(m, u) for m in manifests for u in re.findall(r'^<([^>]+)>\s*\n\s*a lv2:Plugin', open(m).read(), re.M)]
if not uris: bad.append(f'no LV2 plugin under {lv2dir}')
env = dict(os.environ, LV2_PATH=os.path.abspath(lv2dir))
for m, u in uris:
    if glob.glob(os.path.join(os.path.dirname(m), '*.so')): continue   # the dump is proven where no binary is
    recs = dump(f'lv2:{u}', env)
    if not any(r[:2] == ['plugin', 'rdfs:seeAlso'] and r[2].startswith(os.path.basename(os.path.dirname(m)) + '/') for r in recs):
        bad.append(f'lv2:{u}: no rdfs:seeAlso record in its bundle')
    dumps.append(('lv2', recs))
if tables:
    for fmt in ('clap', 'lv2'):
        rows = next(v for v in json.load(open(os.path.join(tables, f'{fmt}.json'))).values() if isinstance(v, dict) and 'rows' in v)['rows']
        for f in sorted({r[1] for k, recs in dumps if k == fmt for r in recs} - set(rows)):
            bad.append(f'the {fmt} table has no row for the dump\'s field {f}')
for b in bad: print(f'scan-fields-check: {b}', file=sys.stderr)
if bad: sys.exit(1)
print(f'scan-fields-check: the list is the TERMs ({len(listed) - 1} names); {len(dumps)} dumps ({sum(len(r) for _, r in dumps)} records) read{", every field a row of the tables" if tables else ""}')
