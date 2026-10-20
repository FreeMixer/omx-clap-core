#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""CLAP coverage: for every CLAP plugin installed, whether this tree's omx-clap-host hosts it, and which
extensions it asked for that the host did not offer.

    clap_coverage.py scan     --dirs /usr/lib64/clap,/usr/lib/clap --out <dir> [--omx-clap-scan <path>]
    clap_coverage.py host     --out <dir> [--omx-clap-host <path>] [--batch 20] [--timeout 10] [--sandbox]
    clap_coverage.py classify --out <dir> [--clap-include /usr/include/clap]
    clap_coverage.py --self-test

scan     runs omx-clap-scan --json on each .clap file of the directories, one process per file, and writes
         plugins.tsv: one row per plugin (file, id, name, vendor, features, audio ports and channels, note ports,
         params, latency, instrument). A file whose scan crashed, timed out or failed is one row with an empty id
         and scan = crash | timeout | error.
host     adds each plugin of plugins.tsv on omx-clap-host (`add clap:<file>#<id> <n>`, then `remove <n>`), a
         fresh host per --batch plugins and after any crash or hang, --timeout seconds per reply, and writes
         host.tsv: the answer per plugin (hosted, refused with its hosting code, crash or hang, at add or at
         remove). Between a hosted add and its remove it waits for the host's log to go quiet, and takes from it
         the lines "<plugin id>: extension not offered: <extension id>" into missing.tsv (file, id, extension).
         The host says those for a hosted plugin only: a refused plugin's instance ends before the host's tick.
         Resumable: a plugin already in host.tsv is not added again (remove the out dir for a fresh run).
         --sandbox runs the hosts in a private user, net, pid and mount namespace, with a tmpfs over the user's
         runtime dir and a PipeWire of its own with a null sink, under nice 10: nothing of the desk's graph is
         reachable. Without it the host runs as is, against whatever JACK server the environment names.
classify writes coverage.tsv (id, file, answer, code, missing extensions) and coverage.md: hosted N of M,
         the refusals by code with three example ids, the crashes and hangs by file, and per extension id
         declared in the installed CLAP headers the count of plugins that asked for it and whether the host
         offers it (src/clap_host_extensions.h, and the ones omx-clap-host's configuration adds).

The desk run is `make clap-coverage` (desk only, never in CI: the plugins are the desk's); its outputs land
in build/clap-coverage/. --self-test runs scan parsing, the host loop against a fake host (hosted, refused,
crash, hang) and classify on fake data, then breaks each rule on purpose (CLAP_COVERAGE_SABOTAGE) and
requires the self-test to go red. Exit 0 only when every check held and every sabotage was seen red.
"""
import argparse
import collections
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SABOTAGE = os.environ.get('CLAP_COVERAGE_SABOTAGE', '')

PLUGIN_COLS = ['file', 'id', 'name', 'vendor', 'features', 'audio_in', 'audio_out', 'note_in', 'note_out', 'params',
               'latency', 'instrument', 'scan']
HOST_COLS = ['file', 'id', 'answer', 'code', 'reply', 'seconds', 'batch']
MISSING_COLS = ['file', 'id', 'extension']
COVERAGE_COLS = ['id', 'file', 'answer', 'code', 'missing']

# what omx-clap-host's configuration adds to the declared list (src/effects.c configure_core: preset_load, track_info,
# remote_controls), each under its id and its draft id, as host_get_extension answers them
CONFIGURED_EXTENSIONS = ['clap.preset-load/2', 'clap.preset-load.draft/2', 'clap.track-info/1', 'clap.track-info.draft/1',
                         'clap.remote-controls/2', 'clap.remote-controls.draft/2']
UNOFFERED = re.compile(r'^(.+?): extension not offered: (\S+)\s*$')


def read_tsv(path):
    if not os.path.exists(path):
        return []
    with open(path) as f:
        lines = [l.rstrip('\n') for l in f if l.strip()]
    if not lines:
        return []
    cols = lines[0].split('\t')
    return [dict(zip(cols, l.split('\t') + [''] * (len(cols) - len(l.split('\t'))))) for l in lines[1:]]


def write_tsv(path, cols, rows):
    with open(path, 'w') as f:
        f.write('\t'.join(cols) + '\n')
        for r in rows:
            f.write('\t'.join(str(r.get(c, '')).replace('\t', ' ').replace('\n', ' ') for c in cols) + '\n')


def append_tsv(path, cols, row):
    new = not os.path.exists(path) or os.path.getsize(path) == 0
    with open(path, 'a') as f:
        if new:
            f.write('\t'.join(cols) + '\n')
        f.write('\t'.join(str(row.get(c, '')).replace('\t', ' ').replace('\n', ' ') for c in cols) + '\n')


# ---------------------------------------------------------------- scan


def ports_of(ports):
    """'<ports>/<channels>' of one side: 1/2 is one port of two channels"""
    return f"{len(ports)}/{sum(p.get('channels', 0) for p in ports)}"


def rows_of_scan(path, doc):
    """plugins.tsv rows of one file's omx-clap-scan --json document"""
    rows = []
    for f in doc.get('files', []):
        if 'error' in f:
            err = f['error']
            kind = 'timeout' if 'timed out' in err else 'crash' if 'crashed' in err else 'error'
            rows.append(dict(file=f.get('path', path), scan=kind, name=err))
            continue
        for p in f.get('plugins', []):
            ap = p.get('audio_ports', {})
            np = p.get('note_ports', {})
            features = p.get('features', [])
            rows.append(dict(file=f.get('path', path), id=p.get('id', ''), name=p.get('name', ''), vendor=p.get('vendor', ''),
                             features=','.join(features), audio_in=ports_of(ap.get('inputs', [])),
                             audio_out=ports_of(ap.get('outputs', [])), note_in=np.get('inputs', 0), note_out=np.get('outputs', 0),
                             params=len(p.get('params', [])), latency=p.get('latency', ''),
                             instrument='yes' if 'instrument' in features else 'no', scan='ok'))
    if not rows and SABOTAGE != 'scan-fail-blind':
        rows.append(dict(file=path, scan='error', name='no plugin listed'))
    return rows


def scan_file(scanner, path, timeout, cwd=None):
    try:
        p = subprocess.run([scanner, '--json', path], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired:
        return [dict(file=path, scan='timeout', name=f'no answer in {timeout} s')]
    if p.returncode < 0:
        return [dict(file=path, scan='crash', name=f'signal {-p.returncode}')]
    try:
        doc = json.loads(p.stdout.decode(errors='replace'))
    except ValueError:
        return [dict(file=path, scan='error', name=f'exit {p.returncode}, no JSON')]
    rows = rows_of_scan(path, doc)
    if SABOTAGE == 'scan-fail-blind':
        rows = [r for r in rows if r.get('scan') == 'ok']
    return rows


def clap_files(dirs):
    files = []
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            path = os.path.join(d, name)
            if name.endswith('.clap') and (os.path.isfile(path) or os.path.isdir(path)):
                files.append(path)
    return files


def cmd_scan(a):
    os.makedirs(a.out, exist_ok=True)
    rows = []
    files = clap_files([d for d in a.dirs.split(',') if d])
    # a plugin that writes a file where it runs writes it here, not in the tree
    cwd = os.path.join(a.out, 'cwd')
    os.makedirs(cwd, exist_ok=True)
    scanner = os.path.abspath(a.omx_clap_scan)
    for n, path in enumerate(files, 1):
        rows += scan_file(scanner, os.path.abspath(path), a.timeout, cwd)
        if n % 25 == 0:
            print(f'scan: {n} of {len(files)} files', flush=True)
    write_tsv(os.path.join(a.out, 'plugins.tsv'), PLUGIN_COLS, rows)
    ok = sum(r.get('scan') == 'ok' for r in rows)
    print(f'scan: {len(files)} files, {ok} plugins, {len(rows) - ok} files failed -> {a.out}/plugins.tsv')
    return 0


# ---------------------------------------------------------------- host


def parse_reply(reply):
    """(answer, code) of an add's reply: 'resp <n>' is hosted, 'resp <negative> [<hosting code>]' is refused"""
    parts = (reply or '').split()
    if len(parts) < 2 or parts[0] != 'resp':
        return 'error', reply or ''
    try:
        n = int(parts[1])
    except ValueError:
        return 'error', reply
    if n >= 0:
        return 'hosted', ''
    if SABOTAGE == 'refusal-blind':
        return 'refused', ''
    return 'refused', parts[2] if len(parts) > 2 else f'resp {n}'


def unoffered_of(text, plugin_id):
    """the extension ids the host's log says the plugin asked for and was not offered, in order, each once"""
    seen = []
    for line in text.splitlines():
        m = UNOFFERED.match(line)
        if m and (m.group(1) == plugin_id or SABOTAGE == 'attribution-blind') and (m.group(2) not in seen or SABOTAGE == 'dedupe-blind'):
            seen.append(m.group(2))
    return seen


class Host:
    def __init__(self, argv, port, env, logpath, timeout, cwd=None):
        self.port, self.timeout, self.logpath = port, timeout, logpath
        self.log = open(logpath, 'wb')
        self.p = subprocess.Popen(argv + ['-n', '-p', str(port), '-f', str(port + 1)], env=env, stdout=self.log,
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True, cwd=cwd)
        self.s = self.f = None
        deadline = time.time() + 15
        while True:
            try:
                self.s = socket.create_connection(('127.0.0.1', port), timeout=1)
                break
            except OSError:
                if self.p.poll() is not None or time.time() > deadline:
                    self.kill()
                    raise SystemExit(f'host: {argv[0]} did not start (log {logpath})')
                time.sleep(0.1)
        try:
            self.f = socket.create_connection(('127.0.0.1', port + 1), timeout=1)
            self.f.setblocking(False)
        except OSError:
            self.f = None

    def drain(self):
        if self.f:
            try:
                while self.f.recv(65536):
                    pass
            except OSError:
                pass

    def send(self, line):
        """(reply, None), or (None, 'closed' | 'timeout' | 'oserror')"""
        try:
            self.s.sendall(line.encode() + b'\0')
            buf, deadline = b'', time.time() + self.timeout
            while not buf.endswith(b'\0'):
                left = deadline - time.time()
                if left <= 0:
                    return None, 'timeout'
                self.s.settimeout(left)
                d = self.s.recv(4096)
                if not d:
                    return None, 'closed'
                buf += d
            self.drain()
            return buf[:-1].decode(errors='replace'), None
        except socket.timeout:
            return None, 'timeout'
        except OSError:
            return None, 'oserror'

    def log_size(self):
        self.log.flush()
        return os.path.getsize(self.logpath)

    def log_since(self, offset):
        with open(self.logpath, 'rb') as f:
            f.seek(offset)
            return f.read().decode(errors='replace')

    def settle(self, quiet=0.25, most=2.0):
        """wait until the log has not grown for `quiet` seconds, at most `most`: the host says one unoffered id per tick"""
        start = last_change = time.time()
        size = self.log_size()
        while time.time() - start < most:
            time.sleep(0.05)
            now = self.log_size()
            if now != size:
                size, last_change = now, time.time()
            elif time.time() - last_change >= quiet:
                break

    def dead(self, wait=0.0):
        """the exit status, or None while it runs; a host whose socket closed is given `wait` seconds to end"""
        try:
            return self.p.wait(wait) if wait and SABOTAGE != 'slow-death-blind' else self.p.poll()
        except subprocess.TimeoutExpired:
            return None

    def kill(self):
        try:
            os.killpg(self.p.pid, 9)
        except OSError:
            pass
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            pass
        for x in (self.s, self.f):
            try:
                x and x.close()
            except OSError:
                pass
        self.log.close()
        time.sleep(0.2)


def status_of(rc):
    return f'signal {-rc}' if rc is not None and rc < 0 else f'exit {rc}'


def host_loop(a, env):
    plugins = [r for r in read_tsv(os.path.join(a.out, 'plugins.tsv')) if r.get('scan') == 'ok' and r.get('id')]
    hostp, missp = os.path.join(a.out, 'host.tsv'), os.path.join(a.out, 'missing.tsv')
    done = {(r['file'], r['id']) for r in read_tsv(hostp)}
    todo = [r for r in plugins if (r['file'], r['id']) not in done]
    logdir, cwd = os.path.join(a.out, 'logs'), os.path.join(a.out, 'cwd')
    os.makedirs(logdir, exist_ok=True)
    os.makedirs(cwd, exist_ok=True)
    batch0 = len({r['batch'] for r in read_tsv(hostp)})
    h, n, batchno = None, 0, batch0
    for count, r in enumerate(todo, 1):
        if h is None or n >= a.batch:
            if h:
                h.kill()
            batchno += 1
            n = 0
            h = Host([a.omx_clap_host], a.port, env, os.path.join(logdir, f'b{batchno:03d}.log'), a.timeout, cwd)
        row = dict(file=r['file'], id=r['id'], batch=f'b{batchno:03d}')
        offset = h.log_size()
        t0 = time.time()
        reply, err = h.send(f"add clap:{r['file']}#{r['id']} {n}")
        row['seconds'] = f'{time.time() - t0:.2f}'
        if err:
            rc = h.dead(2.0 if err != 'timeout' else 0.0)
            row.update(answer='crash' if rc is not None else 'hang', code=f"at add: {status_of(rc) if rc is not None else err}")
            append_tsv(hostp, HOST_COLS, row)
            h.kill()
            h = None
            continue
        row['reply'] = reply
        row['answer'], row['code'] = parse_reply(reply)
        if row['answer'] == 'hosted':
            h.settle()
            for ext in unoffered_of(h.log_since(offset), r['id']):
                append_tsv(missp, MISSING_COLS, dict(file=r['file'], id=r['id'], extension=ext))
            _, err = h.send(f'remove {n}')
            rc = h.dead(2.0 if err and err != 'timeout' else 0.05)
            if (err or rc is not None) and SABOTAGE != 'crash-blind':
                row.update(answer='crash' if rc is not None else 'hang',
                           code=f"at remove: {status_of(rc) if rc is not None else err}")
                append_tsv(hostp, HOST_COLS, row)
                h.kill()
                h = None
                continue
        n += 1
        append_tsv(hostp, HOST_COLS, row)
        if count % 20 == 0:
            print(f'host: {count} of {len(todo)} plugins', flush=True)
    if h:
        h.kill()
    print(f'host: {len(todo)} plugins added -> {hostp}')
    return 0


def start_pipewire(rt):
    """a PipeWire of the namespace's own, its runtime and configuration under rt, with one null sink"""
    conf = os.path.join(rt, 'etc', 'pipewire', 'pipewire.conf.d')
    os.makedirs(conf, exist_ok=True)
    with open(os.path.join(conf, 'clap-coverage.conf'), 'w') as f:
        f.write('context.objects = [\n  { factory = adapter\n    args = { factory.name = support.null-audio-sink\n'
                '             node.name = clap-coverage-null media.class = Audio/Sink\n'
                '             audio.position = [ FL FR ] } }\n]\n')
    env = dict(os.environ, XDG_RUNTIME_DIR=rt, PIPEWIRE_RUNTIME_DIR=rt, PULSE_RUNTIME_PATH=os.path.join(rt, 'pulse'),
               XDG_CONFIG_HOME=os.path.join(rt, 'etc'), XDG_STATE_HOME=os.path.join(rt, 'state'))
    for k in ('DBUS_SESSION_BUS_ADDRESS', 'PIPEWIRE_REMOTE', 'JACK_DEFAULT_SERVER', 'PULSE_SERVER'):
        env.pop(k, None)
    log = open(os.path.join(rt, 'pipewire.log'), 'w')
    p = subprocess.Popen(['pipewire'], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    for _ in range(100):
        if os.path.exists(os.path.join(rt, 'pipewire-0')):
            return p, env
        time.sleep(0.05)
    raise SystemExit('host: pipewire did not come up: ' + open(os.path.join(rt, 'pipewire.log')).read())


def inside_host(a):
    if os.getpid() != 1:
        raise SystemExit('host --sandbox: not pid 1 of a private namespace, refusing')
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=False)
    # a tmpfs over the user's runtime dir, where the live PipeWire's socket is: a filesystem socket is not netns-scoped
    outer = os.environ.get('CLAP_COVERAGE_OUTER_RUNTIME', '')
    base = outer if outer and os.path.isdir(outer) else tempfile.mkdtemp(prefix='clapcov.', dir='/tmp')
    subprocess.run(['mount', '-t', 'tmpfs', '-o', 'size=256m,mode=0700', 'clap-coverage', base], check=True)
    if not os.path.ismount(base):
        raise SystemExit(f'host --sandbox: no tmpfs over {base}, refusing')
    for sock in ('pipewire-0', 'pipewire-0-manager', 'pulse/native'):
        if os.path.exists(os.path.join(base, sock)):
            raise SystemExit(f'host --sandbox: the live {sock} is still reachable at {base}, refusing')
    rt = tempfile.mkdtemp(prefix='pw.', dir=base)
    pw, env = start_pipewire(rt)
    env['LD_LIBRARY_PATH'] = '/usr/lib64/pipewire-0.3/jack'
    try:
        return host_loop(a, env)
    finally:
        pw.kill()


def cmd_host(a):
    if not os.path.exists(a.omx_clap_host):
        raise SystemExit(f'host: missing {a.omx_clap_host}')
    a.omx_clap_host = os.path.abspath(a.omx_clap_host)
    if not a.sandbox:
        return host_loop(a, dict(os.environ))
    if os.environ.get('CLAP_COVERAGE_INSIDE') == '1':
        return inside_host(a)
    try:
        os.nice(10)
    except OSError:
        pass
    argv = ['unshare', '--user', '--map-root-user', '--net', '--pid', '--fork', '--mount-proc',
            sys.executable, '-I', os.path.abspath(__file__)] + sys.argv[1:]
    outer = os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'
    return subprocess.call(argv, env=dict(os.environ, CLAP_COVERAGE_INSIDE='1', CLAP_COVERAGE_OUTER_RUNTIME=outer))


# ---------------------------------------------------------------- classify


def header_extensions(include):
    """every extension id the installed CLAP headers declare, as {id: name}"""
    found = {}
    pat = re.compile(r'\b(CLAP_EXT_[A-Z0-9_]+)\s*\[\]\s*=\s*"([^"]+)"')
    for d, _, files in os.walk(include):
        for name in files:
            if name.endswith('.h'):
                with open(os.path.join(d, name), errors='replace') as f:
                    for m in pat.finditer(f.read()):
                        found[m.group(2)] = m.group(1)
    return found


def offered_extensions(header=os.path.join(ROOT, 'src', 'clap_host_extensions.h')):
    """the ids host_get_extension offers: the declared list and the configured ones"""
    with open(header) as f:
        text = f.read()
    m = re.search(r'CLAP_HOST_EXTENSIONS_INIT\s*\{(.*?)\}', text, re.S)
    declared = re.findall(r'"([^"]+)"', m.group(1)) if m else []
    if SABOTAGE == 'offered-blind':
        return set()
    return set(declared) | set(CONFIGURED_EXTENSIONS)


def classify(plugins, host, missing, extensions, offered):
    """coverage rows and the summary's numbers, a pure function of the tables"""
    answers = {(r['file'], r['id']): r for r in host}
    asked = collections.defaultdict(list)
    for r in missing:
        asked[(r['file'], r['id'])].append(r['extension'])
    rows, failed_files = [], []
    for p in plugins:
        if p.get('scan') != 'ok':
            failed_files.append(p)
            continue
        h = answers.get((p['file'], p['id']), {})
        exts = asked.get((p['file'], p['id']), []) if SABOTAGE == 'dedupe-blind' else sorted(set(asked.get((p['file'], p['id']), [])))
        rows.append(dict(id=p['id'], file=p['file'], answer=h.get('answer', 'not-run'), code=h.get('code', ''),
                         missing=','.join(exts)))
    hosted = [r for r in rows if r['answer'] == 'hosted' or (SABOTAGE == 'crash-blind' and r['answer'] == 'crash')]
    refusals = collections.defaultdict(list)
    for r in rows:
        if r['answer'] == 'refused':
            refusals[r['code']].append(r['id'])
    broken = collections.defaultdict(list)
    for r in rows:
        if r['answer'] in ('crash', 'hang') and not (SABOTAGE == 'crash-blind' and r['answer'] == 'crash'):
            broken[r['file']].append(f"{r['id']} ({r['answer']} {r['code']})")
    per_ext = collections.Counter()
    for r in rows:
        for e in r['missing'].split(',') if r['missing'] else []:
            per_ext[e] += 1
    ext_rows = []
    for e in sorted(set(extensions) | set(per_ext)):
        ext_rows.append(dict(extension=e, name=extensions.get(e, '(not in the headers)'), asked=per_ext.get(e, 0),
                             offered='yes' if e in offered else 'no'))
    return dict(rows=rows, total=len(rows), hosted=len(hosted), refusals=refusals, broken=broken, failed_files=failed_files,
                not_run=sum(r['answer'] == 'not-run' for r in rows), errors=sum(r['answer'] == 'error' for r in rows),
                extensions=ext_rows)


def render(c):
    out = ['# CLAP coverage', '',
           f"Hosted {c['hosted']} of {c['total']} plugins scanned"
           f"; {sum(len(v) for v in c['refusals'].values())} refused, {sum(len(v) for v in c['broken'].values())} crashed or hung"
           f", {c['errors']} answered no reply grammar, {c['not_run']} not run. "
           f"{len(c['failed_files'])} files failed to scan.", '',
           '## Refusals by code', '', '| code | plugins | examples |', '|---|---|---|']
    for code, ids in sorted(c['refusals'].items(), key=lambda kv: (-len(kv[1]), kv[0])):
        out.append(f"| {code} | {len(ids)} | {', '.join(ids[:3])} |")
    out += ['', '## Crashes and hangs by file', '', '| file | plugins |', '|---|---|']
    for f, items in sorted(c['broken'].items()):
        out.append(f"| {f} | {'; '.join(items)} |")
    out += ['', '## Files that failed to scan', '', '| file | scan | why |', '|---|---|---|']
    for p in c['failed_files']:
        out.append(f"| {p['file']} | {p.get('scan', '')} | {p.get('name', '')} |")
    out += ['', '## Extensions', '',
            'Plugins asked: the hosted plugins whose host log says the extension was asked for and not offered.', '',
            '| extension | header name | offered | plugins asked |', '|---|---|---|---|']
    for e in sorted(c['extensions'], key=lambda e: (-e['asked'], e['extension'])):
        asked = '-' if e['offered'] == 'yes' else str(e['asked'])
        out.append(f"| {e['extension']} | {e['name']} | {e['offered']} | {asked} |")
    return '\n'.join(out) + '\n'


def cmd_classify(a):
    plugins = read_tsv(os.path.join(a.out, 'plugins.tsv'))
    host = read_tsv(os.path.join(a.out, 'host.tsv'))
    missing = read_tsv(os.path.join(a.out, 'missing.tsv'))
    c = classify(plugins, host, missing, header_extensions(a.clap_include), offered_extensions())
    write_tsv(os.path.join(a.out, 'coverage.tsv'), COVERAGE_COLS, c['rows'])
    with open(os.path.join(a.out, 'coverage.md'), 'w') as f:
        f.write(render(c))
    print(f"classify: hosted {c['hosted']} of {c['total']} -> {a.out}/coverage.md, coverage.tsv")
    return 0


# ---------------------------------------------------------------- self-test

FAKE_HOST = r'''
import os, signal, socket, sys, time
port = int(sys.argv[sys.argv.index('-p') + 1])
fb = socket.socket(); fb.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); fb.bind(('127.0.0.1', port + 1)); fb.listen(1)
s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1); s.bind(('127.0.0.1', port)); s.listen(1)
c, _ = s.accept()
buf = b''
while True:
    d = c.recv(4096)
    if not d:
        break
    buf += d
    while b'\0' in buf:
        line, buf = buf.split(b'\0', 1)
        w = line.decode().split()
        if w[0] == 'add':
            pid = w[1].split('#', 1)[1]
            if 'crash' in pid:
                os.kill(os.getpid(), signal.SIGSEGV)
            if 'hang' in pid:
                time.sleep(60)
            if 'slowdeath' in pid:
                c.close(); time.sleep(0.5); os.kill(os.getpid(), signal.SIGABRT)
            if 'refuse' in pid:
                c.sendall(b'resp -102 hosting.clap.note-input\0')
                continue
            if 'bare' in pid:
                c.sendall(b'resp -101\0')
                continue
            for e in ('clap.gui', 'clap.gui', 'clap.render'):
                sys.stderr.write(f'{pid}: extension not offered: {e}\n'); sys.stderr.flush()
            sys.stderr.write(f'other.plugin: extension not offered: clap.undo/4\n'); sys.stderr.flush()
            c.sendall(f'resp {w[2]}\0'.encode())
        elif w[0] == 'remove':
            if os.environ.get('FAKE_DIE_ON_REMOVE') == w[1]:
                os.kill(os.getpid(), signal.SIGSEGV)
            c.sendall(b'resp 0\0')
'''


def free_port():
    s = socket.socket()
    s.bind(('127.0.0.1', 0))
    p = s.getsockname()[1]
    s.close()
    return p if p < 65000 else 5700


def self_test():
    fails, oks = [], 0

    def t(name, cond):
        nonlocal oks
        if cond:
            oks += 1
            print(f'ok   {name}')
        else:
            fails.append(name)
            print(f'FAIL {name}')

    work = tempfile.mkdtemp(prefix='clap-coverage-self.')
    try:
        # scan: one file of two plugins, a crashed file, a timed-out file, a file that lists nothing
        doc = {'files': [{'path': '/x/a.clap', 'plugins': [
            {'id': 'a.fx', 'name': 'A fx', 'vendor': 'v', 'features': ['audio-effect', 'stereo'],
             'audio_ports': {'inputs': [{'channels': 2}], 'outputs': [{'channels': 2}, {'channels': 1}]},
             'note_ports': {'inputs': 0, 'outputs': 0}, 'params': [{}, {}, {}]},
            {'id': 'a.synth', 'name': 'A synth', 'vendor': 'v', 'features': ['instrument'],
             'audio_ports': {'inputs': [], 'outputs': [{'channels': 2}]}, 'note_ports': {'inputs': 1, 'outputs': 0}, 'params': []}]}]}
        rows = rows_of_scan('/x/a.clap', doc)
        t('scan: one row per plugin, ports as ports/channels, the instrument named',
          len(rows) == 2 and rows[0]['audio_in'] == '1/2' and rows[0]['audio_out'] == '2/3' and rows[0]['params'] == 3
          and rows[1]['instrument'] == 'yes' and rows[0]['instrument'] == 'no' and rows[1]['note_in'] == 1)
        crashed = rows_of_scan('/x/b.clap', {'files': [{'path': '/x/b.clap', 'error': 'crashed with signal 11 (Segmentation fault)'}]})
        timed = rows_of_scan('/x/c.clap', {'files': [{'path': '/x/c.clap', 'error': 'timed out after 10 s'}]})
        empty = rows_of_scan('/x/d.clap', {'files': [{'path': '/x/d.clap', 'plugins': []}]})
        t('scan: a crashed file is a row, scan=crash', len(crashed) == 1 and crashed[0]['scan'] == 'crash' and not crashed[0].get('id'))
        t('scan: a timed-out file is a row, scan=timeout', len(timed) == 1 and timed[0]['scan'] == 'timeout')
        t('scan: a file that lists no plugin is a row, scan=error', len(empty) == 1 and empty[0]['scan'] == 'error')

        # replies and the log
        t('reply: resp <n> is hosted', parse_reply('resp 3') == ('hosted', ''))
        t('reply: a refusal keeps its hosting code', parse_reply('resp -102 hosting.clap.note-input') == ('refused', 'hosting.clap.note-input'))
        t('reply: a bare refusal keeps its number', parse_reply('resp -101') == ('refused', 'resp -101'))
        log = ('a.fx: extension not offered: clap.gui\nnoise\na.fx: extension not offered: clap.gui\n'
               'b.fx: extension not offered: clap.undo/4\na.fx: extension not offered: clap.render\n')
        t('log: the plugin\'s unoffered ids, each once, in order', unoffered_of(log, 'a.fx') == ['clap.gui', 'clap.render'])
        t('log: another plugin\'s lines are not this one\'s', 'clap.undo/4' not in unoffered_of(log, 'a.fx'))

        # the host loop against a fake host: hosted, refused with a code, refused bare, a crash, a hang
        out = os.path.join(work, 'out')
        os.makedirs(out)
        fake = os.path.join(work, 'fake-host')
        with open(fake, 'w') as f:
            f.write(f'#!{sys.executable} -I\n' + FAKE_HOST)
        os.chmod(fake, 0o755)
        plist = [dict(file='/x/a.clap', id=i, scan='ok') for i in
                 ('p.ok1', 'p.refuse', 'p.crash', 'p.ok2', 'p.bare', 'p.hang', 'p.ok3', 'p.slowdeath')]
        plist.append(dict(file='/x/b.clap', scan='crash', name='signal 11'))
        write_tsv(os.path.join(out, 'plugins.tsv'), PLUGIN_COLS, plist)
        a = argparse.Namespace(out=out, omx_clap_host=fake, batch=2, timeout=1.5, port=free_port(), sandbox=False)
        host_loop(a, dict(os.environ))
        host = read_tsv(os.path.join(out, 'host.tsv'))
        by = {r['id']: r for r in host}
        t('host: every scanned plugin answered, the failed file not added', set(by) == {p['id'] for p in plist if p.get('id')})
        t('host: hosted', by.get('p.ok1', {}).get('answer') == 'hosted' and by.get('p.ok3', {}).get('answer') == 'hosted')
        t('host: refused with its code', by.get('p.refuse', {}).get('code') == 'hosting.clap.note-input')
        t('host: a crash at add', by.get('p.crash', {}).get('answer') == 'crash' and 'signal 11' in by['p.crash'].get('code', ''))
        t('host: a hang at add', by.get('p.hang', {}).get('answer') == 'hang')
        t('host: a host that closes its socket and dies after is a crash, not a hang',
          by.get('p.slowdeath', {}).get('answer') == 'crash' and 'signal 6' in by['p.slowdeath'].get('code', ''))
        t('host: a fresh host after the crash and per batch of 2', len({r['batch'] for r in host}) >= 4)
        missing = read_tsv(os.path.join(out, 'missing.tsv'))
        t('host: the unoffered ids of a hosted plugin, each once',
          sorted(r['extension'] for r in missing if r['id'] == 'p.ok1') == ['clap.gui', 'clap.render'])
        t('host: another plugin\'s log lines not counted', not any(r['extension'] == 'clap.undo/4' for r in missing))
        host_loop(a, dict(os.environ))
        t('host: resumable, nothing added twice', len(read_tsv(os.path.join(out, 'host.tsv'))) == len(host))

        # a crash at remove: the answer is the crash, not hosted
        out2 = os.path.join(work, 'out2')
        os.makedirs(out2)
        write_tsv(os.path.join(out2, 'plugins.tsv'), PLUGIN_COLS, [dict(file='/x/a.clap', id='p.ok', scan='ok')])
        host_loop(argparse.Namespace(out=out2, omx_clap_host=fake, batch=20, timeout=1.5, port=free_port(), sandbox=False),
                  dict(os.environ, FAKE_DIE_ON_REMOVE='0'))
        r2 = read_tsv(os.path.join(out2, 'host.tsv'))
        t('host: a crash at remove is a crash', len(r2) == 1 and r2[0]['answer'] == 'crash' and r2[0]['code'].startswith('at remove'))

        # classify
        exts = {'clap.gui': 'CLAP_EXT_GUI', 'clap.render': 'CLAP_EXT_RENDER', 'clap.log': 'CLAP_EXT_LOG',
                'clap.preset-load/2': 'CLAP_EXT_PRESET_LOAD'}
        offered = offered_extensions()
        t('offered: the declared list and the configured ones',
          {'clap.log', 'clap.params', 'clap.tail', 'clap.preset-load/2', 'clap.track-info/1'} <= offered and 'clap.gui' not in offered)
        c = classify(read_tsv(os.path.join(out, 'plugins.tsv')), host, missing, exts, offered)
        t('classify: hosted 3 of 8, the failed file apart', c['hosted'] == 3 and c['total'] == 8 and len(c['failed_files']) == 1)
        t('classify: refusals by code', dict((k, len(v)) for k, v in c['refusals'].items()) == {'hosting.clap.note-input': 1, 'resp -101': 1})
        t('classify: crashes and hangs by file', len(c['broken'].get('/x/a.clap', [])) == 3)
        e = {r['extension']: r for r in c['extensions']}
        t('classify: per extension, the plugins that asked (each plugin once)', e['clap.gui']['asked'] == 3 and e['clap.render']['asked'] == 3)
        t('classify: offered or not', e['clap.log']['offered'] == 'yes' and e['clap.gui']['offered'] == 'no')
        md = render(c)
        t('report: hosted N of M', md.startswith('# CLAP coverage\n\nHosted 3 of 8 plugins scanned'))
        t('report: the refusal row with its examples', '| hosting.clap.note-input | 1 | p.refuse |' in md)
        t('report: the extension row', '| clap.gui | CLAP_EXT_GUI | no | 3 |' in md)
        t('report: the failed file', '| /x/b.clap | crash | signal 11 |' in md)
        t('headers: the installed CLAP headers\' ids read as strings',
          not os.path.isdir('/usr/include/clap') or header_extensions('/usr/include/clap').get('clap.gui') == 'CLAP_EXT_GUI')
    finally:
        shutil.rmtree(work, ignore_errors=True)

    print(f"self-test: {oks} ok, {len(fails)} failed" + (f" ({SABOTAGE})" if SABOTAGE else ''))
    return 0 if not fails else 1


def self_test_with_sabotage():
    rc = self_test()
    if SABOTAGE:
        return rc
    red = 0
    sabotages = ['scan-fail-blind', 'refusal-blind', 'attribution-blind', 'dedupe-blind', 'crash-blind', 'offered-blind', 'slow-death-blind']
    for s in sabotages:
        p = subprocess.run([sys.executable, '-I', os.path.abspath(__file__), '--self-test'],
                           env=dict(os.environ, CLAP_COVERAGE_SABOTAGE=s), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        failed = [l[5:] for l in p.stdout.splitlines() if l.startswith('FAIL ')]
        ok = p.returncode != 0 and failed
        red += bool(ok)
        print(f"{'ok  ' if ok else 'FAIL'} sabotage {s}: {'red, ' + str(len(failed)) + ' checks failed (' + failed[0] + ')' if ok else 'stayed green'}")
    print(f'sabotage: {red} of {len(sabotages)} red')
    return 0 if rc == 0 and red == len(sabotages) else 1


# ---------------------------------------------------------------- main


def main():
    if '--self-test' in sys.argv[1:]:
        return self_test_with_sabotage()
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('scan')
    p.add_argument('--dirs', default='/usr/lib64/clap,/usr/lib/clap')
    p.add_argument('--out', required=True)
    p.add_argument('--omx-clap-scan', default=os.path.join(ROOT, 'omx-clap-scan'))
    p.add_argument('--timeout', type=float, default=30)
    p = sub.add_parser('host')
    p.add_argument('--out', required=True)
    p.add_argument('--omx-clap-host', default=os.path.join(ROOT, 'omx-clap-host'))
    p.add_argument('--batch', type=int, default=20)
    p.add_argument('--timeout', type=float, default=10)
    p.add_argument('--port', type=int, default=5594)
    p.add_argument('--sandbox', action='store_true')
    p = sub.add_parser('classify')
    p.add_argument('--out', required=True)
    p.add_argument('--clap-include', default='/usr/include/clap')
    a = ap.parse_args()
    a.out = os.path.abspath(a.out)
    return {'scan': cmd_scan, 'host': cmd_host, 'classify': cmd_classify}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
