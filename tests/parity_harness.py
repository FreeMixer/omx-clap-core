# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""The test harness lv2_parity.py and clap_coverage.py share: the TSV helpers, a host process behind its command
socket, a private PipeWire, and the private user, net, pid and mount namespace both run their hosts in.

Each of the two runs hosts it does not own (the desk's plugins, the desk's mod-host) and must reach nothing of the
live graph; the namespace and the PipeWire here are what keep them from it, so they are written once."""
import os
import signal
import socket
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.environ.get('PLUGIN_HOSTD_TESTS_LIB') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'plugin-hostd', 'tests', 'lib'))
import plugin_hostd_client  # noqa: E402  plugin-hostd-tests' lib/plugin_hostd_client.py


# ---------------------------------------------------------------- tsv


def read_tsv(path, missing_ok=False):
    """The rows of a TSV as dicts; a short row is padded with empty cells. A missing file is [] only when missing_ok."""
    if not os.path.exists(path) and missing_ok:
        return []
    with open(path) as f:
        lines = [l.rstrip('\n') for l in f if l.strip()]
    if not lines:
        return []
    cols = lines[0].split('\t')
    rows = []
    for l in lines[1:]:
        cells = l.split('\t')
        rows.append(dict(zip(cols, cells + [''] * (len(cols) - len(cells)))))
    return rows


def _line(cols, row):
    return '\t'.join(str(row.get(c, '')).replace('\t', ' ').replace('\n', ' ') for c in cols) + '\n'


def write_tsv(path, cols, rows):
    with open(path, 'w') as f:
        f.write('\t'.join(cols) + '\n')
        for r in rows:
            f.write(_line(cols, r))


def append_tsv(path, cols, row):
    new = not os.path.exists(path) or os.path.getsize(path) == 0
    with open(path, 'a') as f:
        if new:
            f.write('\t'.join(cols) + '\n')
        f.write(_line(cols, row))


# ---------------------------------------------------------------- a host process


def status_of(rc):
    return f'signal {-rc}' if rc is not None and rc < 0 else f'exit {rc}'


class Host:
    """A host process of its own session, started on argv, and its command socket on `port`: NUL-terminated lines."""

    def __init__(self, argv, port, env, logpath, cwd=None):
        self.port, self.logpath = port, logpath
        self.log = open(logpath, 'w+b')
        self.p = subprocess.Popen(argv, env=env, stdout=self.log, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                  start_new_session=True, cwd=cwd)
        self.s = None

    def connect(self, timeout=15):
        """True once the command socket accepts; False when the process ended or `timeout` seconds passed."""
        dl = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection(('127.0.0.1', self.port), timeout=1)
                return True
            except OSError:
                if self.p.poll() is not None or time.time() > dl:
                    return False
                time.sleep(0.05)

    def send(self, line, timeout):
        """(reply, None), or (None, 'closed' | 'timeout' | 'oserror <why>')"""
        return plugin_hostd_client.Client(self.s).try_send(line, timeout)

    def alive(self):
        return self.p.poll() is None

    def status(self):
        return status_of(self.p.poll())

    def cpu_ns(self):
        """CPU time of every thread of the process so far, from /proc schedstat"""
        total = 0
        try:
            for t in os.listdir(f'/proc/{self.p.pid}/task'):
                try:
                    with open(f'/proc/{self.p.pid}/task/{t}/schedstat') as f:
                        total += int(f.read().split()[0])
                except OSError:
                    pass
        except OSError:
            pass
        return total

    def log_size(self):
        self.log.flush()
        return os.path.getsize(self.logpath)

    def log_since(self, offset):
        self.log.flush()
        with open(self.logpath, 'rb') as f:
            f.seek(offset)
            return f.read().decode(errors='replace')

    def logtext(self):
        return self.log_since(0)

    def close_sockets(self):
        pass

    def kill(self):
        try:
            os.killpg(self.p.pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            self.p.wait(5)
        except subprocess.TimeoutExpired:
            pass
        self.close_sockets()
        if self.s:
            try:
                self.s.close()
            except OSError:
                pass
        self.log.close()


# ---------------------------------------------------------------- a private PipeWire


def start_pipewire(rt, conf_name, conf, who):
    """A PipeWire of the namespace's own: its runtime, configuration and state under `rt`, `conf` as the drop-in
    `conf_name`. (process, environment to start a host in); SystemExit, `who` first, when it does not come up."""
    confdir = os.path.join(rt, 'etc', 'pipewire', 'pipewire.conf.d')
    os.makedirs(confdir, exist_ok=True)
    with open(os.path.join(confdir, conf_name), 'w') as f:
        f.write(conf)
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
    raise SystemExit(f'{who}: pipewire did not come up: ' + open(os.path.join(rt, 'pipewire.log')).read())


# ---------------------------------------------------------------- the private namespace


def reenter_namespace(script, argv, inside_var, outer_var, python_flags=()):
    """Run `script argv` again as pid 1 of a new user, net, pid and mount namespace, `inside_var`=1 in its
    environment and the user's runtime dir in `outer_var`; its exit status."""
    cmd = ['unshare', '--user', '--map-root-user', '--net', '--pid', '--fork', '--mount-proc',
           sys.executable, *python_flags, os.path.abspath(script)] + list(argv)
    outer = os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'
    return subprocess.call(cmd, env=dict(os.environ, **{inside_var: '1', outer_var: outer}))


def private_runtime(who, outer_var, label, size, fallback_prefix, fallback_dir):
    """Inside the namespace: a tmpfs over the user's runtime dir, where the live PipeWire's socket is (a filesystem
    socket is not netns-scoped, so the environment alone would not keep a host or a plugin that hard-codes the path
    out), or, with none (a container), over a short dir of its own under `fallback_dir`, so that the dir itself
    stays visible. Refuses, `who` first, when it is not pid 1, when the tmpfs did not mount and when a live socket
    is still reachable. The base dir."""
    if os.getpid() != 1:
        raise SystemExit(f'{who}: not pid 1 of a private namespace, refusing')
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=False)
    outer = os.environ.get(outer_var, '')
    base = outer if outer and os.path.isdir(outer) else tempfile.mkdtemp(prefix=fallback_prefix, dir=fallback_dir)
    subprocess.run(['mount', '-t', 'tmpfs', '-o', f'size={size},mode=0700', label, base], check=True)
    if not os.path.ismount(base):
        raise SystemExit(f'{who}: no tmpfs over {base}, refusing')
    for sock in ('pipewire-0', 'pipewire-0-manager', 'pulse/native'):
        if os.path.exists(os.path.join(base, sock)):
            raise SystemExit(f'{who}: the live {sock} is still reachable at {base}, refusing')
    return base
