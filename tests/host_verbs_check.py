#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""
The verbs omx-clap-host owes, read from the installed declarations, against the host's socket and its registration
table (data/verbs.json, docs/verbs.md).

    host_verbs_check.py --port <n> --contract-path <dir>[:<dir>...] --registration <main.c> --header <verbs header>
                        --fake <fake.clap> --synth <fake_synth.clap> --layout-scheme <scheme>

The declarations are every verbs item of every package installed under <dir>/3/ (the first directory of the list that
holds a package wins, as OMX_CONTRACT_PATH reads), each file the package's MANIFEST.json lists, a code reference
resolved to the codes item that declares it. A verb whose implementedBy names omx-clap-host is driven on the socket
with a line built for its declared format from this file's EXAMPLES:

  required     a well-formed line is answered with the declared reply's shape, and each malformed line with the resp of
               the code EXAMPLES names, which must be one the verb declares;
  optional     a well-formed line is answered with the declared shape or with the verb's invalidOperation code;
  unsupported  a well-formed line is answered with the verb's invalidOperation code (-902).

And the reverse: every protocol_add_command of the registration table names its line by an OMX_VERB_<VERB>_FMT of the
header, by a mod-host macro, or by the word of a verb another package declares (or AWAITING names until a package
declares it); the declared verbs this table registers equal the verbs this package declares for omx-clap-host. A
registered verb with no declaration, a declared one with no handler, a verb whose answer is not its declaration's, and
a declared verb with no example are each a failure. Exit 0 held; 1 a failure; 2 usage or no declaration.
"""
import argparse
import json
import os
import re
import socket
import sys

sys.path.insert(0, os.environ.get('PLUGIN_HOSTD_TESTS_LIB') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'plugin-hostd', 'tests', 'lib'))
import plugin_hostd_client  # noqa: E402  plugin-hostd-tests' lib/plugin_hostd_client.py

HOST = 'omx-clap-host'
OWNER = 'omx-clap-core'
# a verb this host answers whose declaration belongs to another package's lane, and the package (FreeMixer/plugin-hostd#131);
# once any installed package declares it, the socket checks it as declared
AWAITING = {'chain': 'plugin-hostd'}

# per verb: the words after the verb of a well-formed line, and malformed lines with the code each must answer.
# {fx} is the passthrough instance (no note input, two remote pages, a parameter contract), {synth} the synth.
EXAMPLES = {
    'track_info': (['{fx}', 'Strip', '#FF8000', 'bus'],
                   [(['{fx}', 'a', 'FF8000'], 'invalidOperation'), (['{fx}', 'a', '-', 'aux'], 'invalidOperation'),
                    (['9', 'a', '-'], 'instanceNonExists')]),
    'remote_pages': (['{fx}'], [(['9'], 'instanceNonExists')]),
    'remote_page_get': (['{fx}', '0'], [(['{fx}', '99'], 'invalidOperation'), (['9', '0'], 'instanceNonExists')]),
    'param_info': (['{fx}', '0'], [(['{fx}', '99'], 'paramSymbolInvalid'), (['9', '0'], 'instanceNonExists')]),
    'pin_expect': (['7', '{pin}'], [(['7', '{scheme}:zz'], 'invalidOperation'), (['7', 'omx-layout/0:' + '0' * 64], 'pinAbsent'),
                                    (['-1', '{pin}'], 'instanceInvalid')]),
    'note_input': (['{synth}'], [(['x'], 'invalidOperation'), (['9'], 'instanceNonExists')]),
    'note_on': (['{synth}', '0', '60', '100'], [(['{fx}', '0', '60', '100'], 'noNoteInput'),
                                                (['{synth}', '16', '60', '100'], 'invalidOperation'),
                                                (['9', '0', '60', '100'], 'instanceNonExists')]),
    'note_off': (['{synth}', '0', '60', '0'], [(['{fx}', '0', '60', '0'], 'noNoteInput'),
                                               (['{synth}', '0', '60', '128'], 'invalidOperation'),
                                               (['9', '0', '60', '0'], 'instanceNonExists')]),
    'all_notes_off': (['{synth}'], [(['{fx}'], 'noNoteInput'), (['9'], 'instanceNonExists')]),
    'audio_ports': (['{fx}'], [(['9'], 'instanceNonExists')]),
    'hide_external_ui': (['{fx}'], [(['9'], 'instanceNonExists')]),
    'idle_hold': ([], [(['a', 'b'], 'invalidOperation')]),
    'note_drops': (['{synth}'], [(['9'], 'instanceNonExists')]),
    'note_out_drops': (['{synth}'], [(['9'], 'instanceNonExists')]),
    # the verbs of plugin-hostd's package that name this host, checked once that package is installed beside this one
    'chain': (['1', '{fx}'], [(['x', '{fx}'], 'invalidOperation')]),
    'preset_list': (['{fx}'], []),
    'preset_get': (['{fx}', '0'], []),
    'pin_layout': (['{fx}'], []),
}

failures = 0


def step(ok, what):
    global failures
    print(('ok   ' if ok else 'FAIL ') + what)
    if not ok:
        failures += 1


def packages(path):
    """Every package of the contract path: {package: directory}, the first directory that holds one winning."""
    found = {}
    for top in [d for d in path.split(':') if d]:
        base = os.path.join(top, '3')
        if not os.path.isdir(base):
            continue
        for pkg in sorted(os.listdir(base)):
            if pkg not in found and os.path.isfile(os.path.join(base, pkg, 'MANIFEST.json')):
                found[pkg] = os.path.join(base, pkg)
    return found


def declarations(path):
    """The verbs of every installed package that name this host, each {verb, format, reply, level, codes, package}."""
    items, verbs = {}, []
    for pkg, d in packages(path).items():
        with open(os.path.join(d, 'MANIFEST.json')) as f:
            files = json.load(f)['files']
        for rel in sorted(files):
            with open(os.path.join(d, rel)) as f:
                doc = json.load(f)
            for name, item in doc.items():
                if isinstance(item, dict) and 'kind' in item:
                    items[name] = (pkg, item)
    for name, (pkg, item) in items.items():
        if item['kind'] != 'verbs':
            continue
        for v in item['verbs'].values():
            if HOST not in v['implementedBy']:
                continue
            codes = {}
            for c in v.get('codes', []):
                ref_item, ref_code = c['ref'].split('.', 1)
                codes[ref_code] = items[ref_item][1]['codes'][ref_code]['resp']
            verbs.append({'verb': v['verb'], 'format': v['format'], 'reply': v['reply'],
                          'level': v['implementedBy'][HOST], 'codes': codes, 'package': pkg})
    return verbs


def words(line):
    """A reply's words, a double-quoted word with its escapes one word."""
    return re.findall(r'"(?:\\.|[^"\\])*"|\S+', line)


def shape(reply, template):
    """Whether reply has the shape of a declared reply: literal words, <x> one word, [x] an optional literal, ... the rest."""
    got, want = words(reply), template.split()
    if want[:1] != ['resp'] or got[:1] != ['resp']:
        return False
    i = 1
    for w in want[1:]:
        if w == '...':
            return True
        if w == '<count>':
            if i >= len(got) or not re.fullmatch(r'\d+', got[i]):
                return False
            i += 1
        elif w.startswith('<'):
            if i >= len(got):
                return False
            i += 1
        elif w.startswith('['):
            if i < len(got) and got[i] == w[1:-1]:
                i += 1
        else:
            if i >= len(got) or got[i] != w:
                return False
            i += 1
    return i == len(got)


def fitted(fmt, args):
    """The example's words as many as the declared format takes: a word past them dropped unless it ends in ..., a
    missing one 0, so the line the host is sent is the declared one."""
    spec = fmt.split()[1:]
    rest = bool(spec) and spec[-1] == '...'
    if rest:
        spec = spec[:-1]
    out = args[:len(spec)] + ['0'] * (len(spec) - len(args))
    return out + (args[len(spec):] if rest else [])


class Host:
    def __init__(self, port):
        self.s = socket.create_connection(('127.0.0.1', port), timeout=10)
        self.client = plugin_hostd_client.Client(self.s)

    def send(self, line):
        return self.client.send(line)


def registrations(main_c, header):
    """The registration table: [(kind, word, text)], kind 'declared' (an OMX_VERB_*_FMT), 'mod-host' or 'typed'."""
    with open(header) as f:
        macros = dict(re.findall(r'^#define (OMX_VERB_[A-Z0-9_]+) "([^"]*)"', f.read(), re.M))
    with open(main_c) as f:
        text = re.sub(r'/\*.*?\*/|//[^\n]*', '', f.read(), flags=re.S)
    local = dict(re.findall(r'^\s*#define ([A-Z_][A-Z0-9_]*)\s+"([^"]*)"', text, re.M))
    args = re.findall(r'protocol_add_command\(\s*([^,]+?),', text)
    m = re.search(r'commands\[\]\s*=\s*\{(.*?)\}', text, re.S)
    if '*command' in args and m:
        args.remove('*command')
        args += [a.strip() for a in m.group(1).split(',') if a.strip() and a.strip() != 'NULL']
    out = []
    for a in args:
        if re.fullmatch(r'OMX_VERB_[A-Z0-9_]+_FMT', a):
            fmt = macros.get(a)
            out.append(('declared' if fmt else 'unknown', fmt.split()[0] if fmt else a, a))
            continue
        parts = re.findall(r'"([^"]*)"|([A-Za-z_][A-Za-z0-9_]*)', a)
        if len(parts) == 1 and parts[0][1] and parts[0][1] not in local:
            out.append(('mod-host', parts[0][1], a))
            continue
        line = ''.join(lit if lit else local.get(ident, ident) for lit, ident in parts)
        out.append(('typed', line.split()[0] if line.split() else a, a))
    return out


def main():
    ap = argparse.ArgumentParser()
    for a in ('--port', '--contract-path', '--registration', '--header', '--fake', '--synth', '--layout-scheme'):
        ap.add_argument(a, required=True)
    o = ap.parse_args()

    verbs = declarations(o.contract_path)
    mine = {v['verb'] for v in verbs if v['package'] == OWNER}
    if not mine:
        print('host_verbs_check: no verb of %s for %s under %s' % (OWNER, HOST, o.contract_path), file=sys.stderr)
        return 2
    print('host_verbs_check: %d verbs name %s, %d of them %s\'s, from %s' % (len(verbs), HOST, len(mine), OWNER, o.contract_path))

    # the reverse: what the host registers is what is declared for it
    table = registrations(o.registration, o.header)
    declared_elsewhere = {v['verb'] for v in verbs if v['package'] != OWNER}
    registered = set()
    for kind, word, text in table:
        if kind == 'declared':
            registered.add(word)
            step(word in mine, 'registered %s is a verb %s declares for %s' % (text, OWNER, HOST))
        elif kind == 'typed':
            ok = word not in mine and (word in declared_elsewhere or word in AWAITING)
            whose = 'another package\'s' if word in declared_elsewhere else '%s\'s, not installed yet' % AWAITING.get(word)
            step(ok, 'registered %s: %s' % (text, 'its declaration is ' + whose if ok
                                             else 'a verb with no declaration, or a declared one typed by hand'))
        elif kind == 'unknown':
            step(False, 'registered %s, which the header does not define' % text)
    step(registered == mine, 'the declared verbs the host registers are the ones %s declares: missing %s, extra %s'
         % (OWNER, sorted(mine - registered) or 'none', sorted(registered - mine) or 'none'))

    host = Host(int(o.port))
    fx, synth = '0', '1'
    for line, want in (('add clap:%s#org.omx-clap-host.test.passthrough %s' % (o.fake, fx), 'resp ' + fx),
                       ('add clap:%s#org.omx-clap-host.test.synth %s' % (o.synth, synth), 'resp ' + synth)):
        got = host.send(line)
        step(got == want, '%s -> %s' % (line, got))
    subst = {'fx': fx, 'synth': synth, 'scheme': o.layout_scheme, 'pin': o.layout_scheme + ':' + '0' * 64}

    for v in verbs:
        name, level, codes = v['verb'], v['level'], v['codes']
        if name not in EXAMPLES:
            step(False, '%s is declared for %s (%s) and this test has no example of it' % (name, HOST, level))
            continue
        good, bad = EXAMPLES[name]
        line = ' '.join([name] + fitted(v['format'], [a.format(**subst) for a in good]))
        got = host.send(line)
        refused = 'invalidOperation' in codes and got == 'resp %d' % codes['invalidOperation']
        if level == 'required':
            step(shape(got, v['reply']), '%s (required) -> %s: the shape of "%s"' % (line, got, v['reply']))
        elif level == 'optional':
            step(shape(got, v['reply']) or refused, '%s (optional) -> %s: "%s" or the refusal' % (line, got, v['reply']))
        else:
            step(refused, '%s (unsupported) -> %s: refused with invalidOperation %s' % (line, got, codes.get('invalidOperation')))
        if level != 'required':
            continue
        for words_, code in bad:
            line = ' '.join([name] + [a.format(**subst) for a in words_])
            if code not in codes:
                step(False, '%s: the example answers %s, a code the verb does not declare (%s)' % (line, code, sorted(codes)))
                continue
            got = host.send(line)
            step(got == 'resp %d' % codes[code], '%s -> %s: %s, resp %d' % (line, got, code, codes[code]))

    print('host_verbs_check: %s' % ('held' if failures == 0 else '%d failures' % failures))
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
