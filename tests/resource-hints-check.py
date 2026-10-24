#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
#
# resource-hints-check.py — the resource hints as omx-clap-scan --json writes them, held to omx-contract's RESOURCE_HINTS
# (data/resource-profile.json): every hint by its word, in the contract's order, with the contract's figureUnit, and
# tests/fake_ext.clap's declaration of two read back with the five others null, never 0. The LV2 face's namespace
# (LV2_CORE_RESOURCE_HINTS_NS, lv2/core/lv2_core.h) is the contract's extension.lv2 namespace, and each hint's property,
# its word in lower camel case, is a property share/omx-contract/omx-resource-hints.ttl declares with that word as label.
#
#   resource-hints-check.py [--self-test] <omx-clap-scan> <fake_ext.clap> <omx-contract checkout>
#
# --self-test runs the check once clean and once per sabotage of the contract as read (a unit changed, two words
# swapped, a word renamed, the LV2 namespace moved) and of the dump (an unknown hint written 0); each must be named.
# Exit 0 held; 1 a difference; 2 usage.
import copy, json, os, re, subprocess, sys

AT_REST, THREADS = 3145728, 2

def camel(word):
    head, *rest = word.split('-')
    return head + ''.join(w[:1].upper() + w[1:] for w in rest)

def check_lv2(hints, ns, ttl):
    bad = []
    if ns != hints['extension']['lv2']['namespace']:
        bad.append(f"LV2_CORE_RESOURCE_HINTS_NS is {ns}, RESOURCE_HINTS says {hints['extension']['lv2']['namespace']}")
    prefix = hints['extension']['lv2']['prefix']
    if f'@prefix {prefix}: <{ns}> .' not in ttl:
        bad.append(f'the Turtle does not bind {prefix}: to {ns}')
    for w in hints['words'].values():
        if not re.search(rf'^{prefix}:{camel(w["word"])}\n(?:    .*\n)*?    rdfs:label "{re.escape(w["word"])}"', ttl, re.M):
            bad.append(f'{prefix}:{camel(w["word"])} is no property of the Turtle labelled {w["word"]}')
    return bad

def check(hints, plugin):
    bad = []
    rh = plugin.get('resource_hints')
    if rh is None:
        return ['fake_ext writes no resource_hints']
    keys = [k for k in rh if k not in ('declared', 'refused')]
    words = [w['word'] for w in hints['words'].values()]
    if keys != words:
        bad.append(f'the hints are {keys}, RESOURCE_HINTS says {words}')
    for w in hints['words'].values():
        got = rh.get(w['word'], {}).get('unit')
        if got != w['figureUnit']:
            bad.append(f"{w['word']}: unit {got}, RESOURCE_HINTS says {w['figureUnit']}")
    want = {'footprint-at-rest': AT_REST, 'threads-spawned': THREADS}
    for word in words:
        fig = rh.get(word, {}).get('figure', 'absent')
        if fig != want.get(word):
            bad.append(f'{word}: figure {fig}, fake_ext declares {want.get(word)}')
    if rh.get('declared') != 0b1001 or 'refused' in rh:
        bad.append(f"declared {rh.get('declared')}, refused {rh.get('refused')}: fake_ext declares bits 0 and 3")
    return bad

def main(argv):
    self_test = argv[:1] == ['--self-test']
    args = argv[1:] if self_test else argv
    if len(args) != 3:
        sys.stderr.write(open(__file__).read().split('\n')[10][2:] + '\n')
        return 2
    scan, fake, oc = args
    hints = json.load(open(os.path.join(oc, 'data', 'resource-profile.json')))['RESOURCE_HINTS']
    ttl = open(os.path.join(oc, 'share', 'omx-contract', 'omx-resource-hints.ttl')).read()
    core_h = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'lv2', 'core', 'lv2_core.h')).read()
    m = re.search(r'^#define LV2_CORE_RESOURCE_HINTS_NS\s+"([^"]+)"', core_h, re.M)
    ns = m.group(1) if m else None
    out = subprocess.run([scan, '--json', fake], check=True, capture_output=True, text=True).stdout
    plugin = json.loads(out)['files'][0]['plugins'][0]
    bad = check(hints, plugin) + check_lv2(hints, ns, ttl)
    if not self_test:
        for b in bad:
            print(f'FAIL {b}')
        if not bad:
            print(f"resource-hints-check: {len(hints['words'])} hints by omx-contract's words and units, fake_ext's two read back, "
                  f"the LV2 properties in {ns}")
        return 1 if bad else 0
    if bad:
        print(f'FAIL self test: not clean to start with: {bad}')
        return 1
    def unit(h, p):
        h['words']['threadsSpawned']['figureUnit'] = 'ms'
    def swap(h, p):
        w = h['words']
        h['words'] = dict(reversed(list(w.items())))
    def rename(h, p):
        h['words']['sharedLoad']['word'] = 'shared-memory'
    def zero(h, p):
        p['resource_hints']['activate-time']['figure'] = 0
    def namespace(h, p):
        h['extension']['lv2']['namespace'] = 'https://www.openmixer.org/ns/resource#'
    failed = 0
    for name, sab in (('unit', unit), ('order', swap), ('word', rename), ('unknown-zero', zero), ('namespace', namespace)):
        h, p = copy.deepcopy(hints), copy.deepcopy(plugin)
        sab(h, p)
        got = check(h, p) + check_lv2(h, ns, ttl)
        print(f"{'ok  ' if got else 'FAIL'} sabotage {name}: {got[0] if got else 'survived'}")
        failed += not got
    print('resource-hints-check: self test: every sabotage caught' if not failed else 'resource-hints-check: self test FAILED')
    return 1 if failed else 0

sys.exit(main(sys.argv[1:]))
