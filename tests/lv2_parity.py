#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Pau Aliagas <linuxnow@gmail.com>
"""LV2 parity: every LV2 plugin that mod-host and omx-clap-host both host, run through both from the
same cycles of one deterministic input, the outputs compared.

    lv2_parity.py probe    --coverage <tsv> --out <dir> [--sample N | --full] [--seed S]
    lv2_parity.py run      --out <dir> --rates 48000,96000 [--plugins <dir>/plugins.tsv] [--limit N]
    lv2_parity.py classify --out <dir>
    lv2_parity.py report   --out <dir>
    lv2_parity.py --self-test

probe   reads a coverage table (one row per URI, the column bucket = both for a plugin both hosts add, and
        the layout columns), or takes --uris as added by both, and lilv's view of each plugin, refuses the layouts this
        runner does not feed (more than two audio inputs, a CV port: omx-clap-core#92), and writes
        plugins.tsv (the set, or a stratified sample of it) and skipped.tsv.
run     per rate, one private PipeWire in its own user, net, pid and mount namespace (with an empty tmpfs over the
        user's runtime dir, so the live PipeWire's socket is not there to reach), whose only driver is
        the feeder's clock (tests/jack_parity): nothing runs until it triggers a cycle, so every instance gets
        its first run() in the same cycle, and the cycles run back to back, as fast as the graph completes
        them (--paced: one quantum of wall time apart). Per plugin, a fresh mod-host and a fresh omx-clap-host each add the
        plugin twice (instances 0, 1 and 2, 3), default parameters; the feeder runs 0.3 s of silence (past
        the adapter's latency hold of 250 ms of blocks), stops the graph for --hold-ms of wall time (the
        host's restart for that latency, a worker's first answer), runs 16 blocks more (the restart's fade),
        then plays --seconds of noise + sine + impulse (and a fixed note sequence to a MIDI input) and
        records every output. The measurements go to raw/<rate>.jsonl, one line per plugin; a crash or a
        hang is retried with one host's instances only, to name the host. Resumable: a URI already in
        raw/<rate>.jsonl is not run again. The recordings of a plugin that does not come out IDENTICAL
        are kept under keep/ (--keep none|differs|all).
classify maps each measurement to a class and a cause (the rules below, a pure function of the
        measurement) and writes parity.tsv.
report  writes summary.md: counts per class and rate, the causes of DIFFERS with example plugins, the
        latencies the hosts disagree on, CPU, and the time the run took with the estimate for the full set.

Classes. IDENTICAL: both instances of each host agree bit for bit, and the two hosts agree bit for bit.
WITHIN-TOLERANCE: the hosts differ, the largest difference is under 1e-6 and the reason is named (an
offset of N samples found by cross-correlation, subnormal values only, rounding). DIFFERS: the largest
difference, where, and a cause. NONDETERMINISTIC: one host's two instances differ from each other.
CRASH and HANG: which host, and when (add or run). REFUSED: a host refused the add this time.

--self-test runs the rules on fake recordings and a fake coverage table, then breaks each rule on
purpose (LV2_PARITY_SABOTAGE) and requires the self-test to go red. Exit 0 only when every check held
and every sabotage was seen red. LV2_PARITY_SABOTAGE=offset on a run delays omx-clap-host's recordings by
one sample before the comparison: tests/lv2_parity_fixture.sh requires that run to read red.
"""
import argparse
import hashlib
import json
import os
import random
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time

TOL = 1e-6
SUBNORMAL = 1.1754944e-38
QUANTUM = 128
SABOTAGE = os.environ.get('LV2_PARITY_SABOTAGE', '')
HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# ---------------------------------------------------------------- measurement (pure, numpy)


def _np():
    import numpy
    return numpy


def bits_equal(a, b):
    np = _np()
    return a.shape == b.shape and np.array_equal(a.view(np.uint32), b.view(np.uint32))


def best_lag(a, b, maxlag):
    """The lag L (|L| <= maxlag) that best aligns b to a (b[t + L] ~ a[t]), by FFT cross-correlation."""
    np = _np()
    if SABOTAGE == 'lag-blind':
        return 0
    n = len(a)
    if n == 0 or not np.any(a) or not np.any(b):
        return 0
    size = 1
    while size < 2 * n:
        size *= 2
    fa = np.fft.rfft(a.astype(np.float64), size)
    fb = np.fft.rfft(b.astype(np.float64), size)
    xc = np.fft.irfft(np.conj(fa) * fb, size)
    lags = np.concatenate((np.arange(0, maxlag + 1), np.arange(-maxlag, 0)))
    vals = np.concatenate((xc[:maxlag + 1], xc[size - maxlag:]))
    return int(lags[int(np.argmax(vals))])


def shifted_maxdiff(a, b, lag):
    """max |a[t] - b[t + lag]| over the overlap, both channels (a, b: [ch][n])."""
    np = _np()
    n = a.shape[1]
    if lag >= 0:
        x, y = a[:, :n - lag], b[:, lag:]
    else:
        x, y = a[:, -lag:], b[:, :n + lag]
    if x.shape[1] == 0:
        return float('inf')
    return float(np.max(np.abs(x.astype(np.float64) - y.astype(np.float64))))


def measure(m0, m1, o0, o1, rate, signal_end):
    """The measurement of one plugin at one rate: mod-host's two instances m0, m1 and omx-clap-host's
    o0, o1, each [channels][frames] float32. signal_end: the frame where the input stops (the tail)."""
    np = _np()
    r = {'ch_mod': int(m0.shape[0]), 'ch_omx': int(o0.shape[0]), 'frames': int(m0.shape[1])}
    r['self_mod'] = bits_equal(m0, m1)
    r['self_omx'] = bits_equal(o0, o1)
    r['self_mod_max'] = 0.0 if r['self_mod'] or m0.shape != m1.shape else float(np.nanmax(np.abs(m0 - m1)))
    r['self_omx_max'] = 0.0 if r['self_omx'] or o0.shape != o1.shape else float(np.nanmax(np.abs(o0 - o1)))
    if SABOTAGE == 'nondet-blind':
        r['self_mod'] = r['self_omx'] = True
    r['peak_mod'] = float(np.nanmax(np.abs(m0))) if m0.size else 0.0
    r['peak_omx'] = float(np.nanmax(np.abs(o0))) if o0.size else 0.0
    r['rms_mod'] = float(np.sqrt(np.nanmean(np.square(m0.astype(np.float64))))) if m0.size else 0.0
    r['rms_omx'] = float(np.sqrt(np.nanmean(np.square(o0.astype(np.float64))))) if o0.size else 0.0
    r['nonfinite_mod'] = int(np.count_nonzero(~np.isfinite(m0)))
    r['nonfinite_omx'] = int(np.count_nonzero(~np.isfinite(o0)))
    r['sha_mod'] = hashlib.sha256(m0.tobytes()).hexdigest()[:16]
    r['sha_omx'] = hashlib.sha256(o0.tobytes()).hexdigest()[:16]
    if m0.shape != o0.shape:
        r['equal'] = False
        return r
    r['equal'] = bits_equal(m0, o0)
    if r['equal']:
        return r
    a = np.nan_to_num(m0.astype(np.float64), nan=1e9, posinf=1e9, neginf=-1e9)
    b = np.nan_to_num(o0.astype(np.float64), nan=1e9, posinf=1e9, neginf=-1e9)
    d = np.abs(a - b)
    ch, idx = np.unravel_index(int(np.argmax(d)), d.shape)
    r['max_diff'] = float(d[ch, idx])
    r['max_at'] = [int(ch), int(idx)]
    r['max_vals'] = [float(m0[ch, idx]), float(o0[ch, idx])]
    over = np.nonzero(np.any(d > 0, axis=0))[0]
    r['first_diff'] = int(over[0]) if len(over) else -1
    over_tol = np.nonzero(np.any(d > TOL, axis=0))[0]
    r['first_over_tol'] = int(over_tol[0]) if len(over_tol) else -1
    r['last_over_tol'] = int(over_tol[-1]) if len(over_tol) else -1
    # the size of the values where they differ: subnormals only means a flush-to-zero difference
    mag = np.maximum(np.abs(a), np.abs(b))[d > 0]
    r['diff_mag_max'] = float(mag.max()) if mag.size else 0.0
    startup = min(int(0.1 * rate), r['frames'])
    r['max_diff_after_startup'] = float(d[:, startup:].max()) if r['frames'] > startup else 0.0
    r['max_diff_signal'] = float(d[:, :signal_end].max()) if signal_end > 0 else 0.0
    r['max_diff_tail'] = float(d[:, signal_end:].max()) if signal_end < r['frames'] else 0.0
    lag = best_lag(a[0], b[0], min(8192, r['frames'] // 4))
    r['lag'] = lag
    r['max_diff_lagged'] = shifted_maxdiff(a, b, lag) if lag else r['max_diff']
    if a.shape[0] == 2:
        r['max_diff_swapped'] = float(np.max(np.abs(a - b[::-1])))
    ob = float(np.dot(b.ravel(), b.ravel()))
    g = float(np.dot(a.ravel(), b.ravel()) / ob) if ob > 0 else 0.0
    r['gain'] = g
    r['gain_residual'] = float(np.max(np.abs(a - g * b))) if ob > 0 else r['max_diff']
    na, nb = np.linalg.norm(a), np.linalg.norm(b)
    r['correlation'] = float(np.dot(a.ravel(), b.ravel()) / (na * nb)) if na > 0 and nb > 0 else 0.0
    return r


# ---------------------------------------------------------------- the rules (pure)


REL_TOL = 1e-5      # WITHIN-TOLERANCE also stays under the peak times this (-100 dB): 1e-6 alone is 1 % of a 1e-4 peak
SPREAD_K = 1.5      # two hosts of a random plugin may differ by this times the plugin's own spread, no more
SPREAD_DB = 0.5     # ... and their levels (RMS) by this many dB


def ports_of(rec):
    """{'mod': (outputs, midi inputs), 'omx': (...)} from the feeder's link lines of instance 0 and 2 (None: unknown)."""
    p = rec.get('ports') or {}
    out = {}
    for host, client in (('mod', 'effect_0'), ('omx', 'effect_2')):
        line = p.get(client)
        if not line:
            return None
        f = dict(x.split('=', 1) for x in line.split() if '=' in x)
        out[host] = (int(f.get('out', 0)), int(f.get('midi', 0)))
    return out


def classify(rec):
    """(class, cause, detail) of one raw record."""
    tol = 1e-2 if SABOTAGE == 'tolerance' else TOL
    st = rec.get('status', 'ok')
    if st in ('skipped',):
        return 'SKIPPED', rec.get('cause', ''), rec.get('detail', '')
    if st.startswith('crash'):
        return 'CRASH', f"{rec.get('host', '?')} at {rec.get('phase', '?')}", rec.get('detail', '')
    if st.startswith('hang'):
        return 'HANG', f"{rec.get('host', '?')} at {rec.get('phase', '?')}", rec.get('detail', '')
    if st == 'refused':
        return 'REFUSED', f"{rec.get('host', '?')}: {rec.get('detail', '')}", ''
    if st != 'ok':
        return 'ERROR', st, rec.get('detail', '')
    m = rec['m']
    audio = 'the audio bit-identical' if m.get('equal') else 'the audio differs too'
    # the port layout first: a port one host has and the other has not is a difference whatever the audio says
    po = ports_of(rec)
    if po and po['mod'][0] != po['omx'][0]:
        return 'DIFFERS', 'output count', f"mod-host {po['mod'][0]} audio outputs, omx-clap-host {po['omx'][0]}; {audio}"
    if m['ch_mod'] != m['ch_omx']:
        return 'DIFFERS', 'output count', f"mod-host {m['ch_mod']} outputs recorded, omx-clap-host {m['ch_omx']}"
    if po and po['mod'][1] != po['omx'][1] and SABOTAGE != 'midi-blind':
        return ('DIFFERS', 'MIDI input count', f"a MIDI input through {'mod-host' if po['mod'][1] else 'omx-clap-host'} only "
                f"(mod-host {po['mod'][1]}, omx-clap-host {po['omx'][1]}): no notes sent to either; {audio}")
    if not m['self_mod'] or not m['self_omx']:
        if m.get('equal') and m.get('equal_second'):
            return ('NONDETERMINISTIC', 'its own randomness; the hosts agree instance by instance',
                    f"a per-instance seed: each host's first instance bit-identical to the other's first, the second to the "
                    f"second; self max diff {max(m['self_mod_max'], m['self_omx_max']):.3g}")
        if m['self_mod'] != m['self_omx'] and SABOTAGE != 'one-host-nondet':
            # the plugin is deterministic in one host: the other host's spread is that host's, not the plugin's
            who, other = ('omx-clap-host', 'mod-host') if m['self_mod'] else ('mod-host', 'omx-clap-host')
            return ('DIFFERS', f'nondeterministic in {who} only',
                    f"{other}'s two instances bit-identical, {who}'s differ by {max(m['self_mod_max'], m['self_omx_max']):.3g}; "
                    f"across hosts {m.get('max_diff', 0):.3g}")
        # random in both: the hosts may differ by as much as the plugin differs from itself, and no more
        spread = max(m['self_mod_max'], m['self_omx_max'])
        cross = m.get('max_diff', 0.0)
        level = None
        if m.get('rms_mod') and m.get('rms_omx'):
            import math
            level = 20 * math.log10(m['rms_mod'] / m['rms_omx'])
        lv = f"level {level:+.2f} dB" if level is not None else 'level not measured'
        if cross <= SPREAD_K * spread and (level is None or abs(level) <= SPREAD_DB):
            return ('NONDETERMINISTIC', 'its own randomness, in both hosts; the hosts within its spread',
                    f"across hosts max {cross:.3g} <= {SPREAD_K} x its own spread {spread:.3g}; {lv}")
        return ('DIFFERS', "beyond the plugin's own spread",
                f"random in both hosts, but across hosts max {cross:.3g} against its own spread {spread:.3g} "
                f"(bound {SPREAD_K} x); {lv}")
    if m.get('equal'):
        if m['peak_mod'] == 0.0 and SABOTAGE != 'silent-pass':
            return 'SILENT', 'proves nothing', 'silent through both hosts with the defaults'
        return 'IDENTICAL', '', f"peak {m['peak_mod']:.3g}"
    where = _where(m, rec.get('rate', 48000))
    if m['nonfinite_mod'] != m['nonfinite_omx']:
        return 'DIFFERS', 'NaN or Inf in one host', f"mod-host {m['nonfinite_mod']}, omx-clap-host {m['nonfinite_omx']}; {where}"
    peak = max(m['peak_mod'], m['peak_omx'])
    if m['max_diff'] < tol and m['diff_mag_max'] < SUBNORMAL:
        return 'WITHIN-TOLERANCE', 'subnormal values only (flush to zero)', f"max {m['max_diff']:.3g}; {where}"
    if m['max_diff'] < tol and m['max_diff'] <= REL_TOL * peak:
        return 'WITHIN-TOLERANCE', 'rounding', f"max {m['max_diff']:.3g}, {m['max_diff'] / peak:.2g} of the peak; {where}"
    if m.get('lag') and m['max_diff_lagged'] < tol:
        # exact after a shift: an alignment difference, which a parallel path turns into a comb filter
        return ('DIFFERS', f"offset of {m['lag']} samples, exact",
                f"omx-clap-host {'late' if m['lag'] > 0 else 'early'} by {abs(m['lag'])}; max after the shift {m['max_diff_lagged']:.3g}")
    if m['peak_omx'] == 0.0 and m['peak_mod'] > 0:
        return 'DIFFERS', 'silent through omx-clap-host', f"mod-host peak {m['peak_mod']:.3g}"
    if m['peak_mod'] == 0.0 and m['peak_omx'] > 0:
        return 'DIFFERS', 'silent through mod-host', f"omx-clap-host peak {m['peak_omx']:.3g}"
    if m.get('max_diff_swapped', 1.0) < tol:
        return 'DIFFERS', 'channels swapped', where
    if rec.get('param_diff'):
        return 'DIFFERS', 'a control starts at a different value', '; '.join(rec['param_diff'][:6]) + f'; {where}'
    if m.get('lag') and m['max_diff_lagged'] < 1e-3 * peak:
        return 'DIFFERS', f"offset of {m['lag']} samples, not exact", f"max after the shift {m['max_diff_lagged']:.3g}; {where}"
    if m['gain_residual'] < 1e-4 * peak and abs(m['gain'] - 1.0) > 1e-6 and m['gain'] > 0:
        import math
        return 'DIFFERS', 'level', f"mod-host / omx-clap-host = {20 * math.log10(m['gain']):+.3f} dB; {where}"
    if m['max_diff'] < tol:
        return 'DIFFERS', 'under 1e-6 but over -100 dB of a small peak', f"max {m['max_diff']:.3g}, peak {peak:.3g}; {where}"
    if m['max_diff_after_startup'] < tol:
        return 'DIFFERS', 'start-up only (the first 100 ms)', where
    if m['max_diff_signal'] < tol and m['max_diff_tail'] >= tol:
        return 'DIFFERS', 'tail only (after the input stops)', where
    if m['max_diff'] < 1e-3 * peak:
        return 'DIFFERS', 'small numeric drift (under -60 dB of the peak)', where
    return 'DIFFERS', 'different output', f"correlation {m.get('correlation', 0):.4f}; {where}"


def _where(m, rate):
    if 'max_at' not in m:
        return ''
    ch, idx = m['max_at']
    first = m.get('first_over_tol', -1)
    if first < 0:
        first = m.get('first_diff', -1)
    return (f"max {m['max_diff']:.3g} at ch{ch} sample {idx} ({idx / rate:.3f} s; {m['max_vals'][0]:.6g} vs "
            f"{m['max_vals'][1]:.6g}); first at sample {first} (block {first // QUANTUM}, +{first % QUANTUM})")


# ---------------------------------------------------------------- probe


def read_tsv(path):
    with open(path) as f:
        lines = [l.rstrip('\n') for l in f if l.strip()]
    head = lines[0].split('\t')
    return [dict(zip(head, l.split('\t'))) for l in lines[1:]]


def write_tsv(path, rows, cols):
    with open(path, 'w') as f:
        f.write('\t'.join(cols) + '\n')
        for r in rows:
            f.write('\t'.join(str(r.get(c, '')).replace('\t', ' ').replace('\n', ' ') for c in cols) + '\n')


def lilv_facts(uris):
    """Per URI what the coverage table does not say: presets, a latency port, sidechains, the worker, the name."""
    try:
        import lilv
    except ImportError:
        return {}
    w = lilv.World()
    w.load_all()
    ns = lambda s: w.new_uri(s)
    pset = ns('http://lv2plug.in/ns/ext/presets#Preset')
    side = ns('http://lv2plug.in/ns/lv2core#isSideChain')
    reports = ns('http://lv2plug.in/ns/lv2core#reportsLatency')
    latency = ns('http://lv2plug.in/ns/lv2core#latency')
    worker = ns('http://lv2plug.in/ns/ext/worker#schedule')
    audio = ns('http://lv2plug.in/ns/lv2core#AudioPort')
    inp = ns('http://lv2plug.in/ns/lv2core#InputPort')
    outp = ns('http://lv2plug.in/ns/lv2core#OutputPort')
    cvp = ns('http://lv2plug.in/ns/lv2core#CVPort')
    atom = ns('http://lv2plug.in/ns/ext/atom#AtomPort')
    evp = ns('http://lv2plug.in/ns/ext/event#EventPort')
    midi_ev = ns('http://lv2plug.in/ns/ext/midi#MidiEvent')
    supports = ns('http://lv2plug.in/ns/ext/atom#supports')
    instrument = ns('http://lv2plug.in/ns/lv2core#InstrumentPlugin')
    out = {}
    plugins = w.get_all_plugins()
    for u in uris:
        p = plugins.get_by_uri(ns(u)) if u else None
        if p is None:
            continue
        f = {'name': str(p.get_name())}
        try:
            f['presets'] = len(list(p.get_related(pset)))
        except Exception:
            f['presets'] = 0
        sc = lat = 0
        f.update(audio_in=0, audio_out=0, cv=0, midi_in=0)
        f['instrument'] = int(str(p.get_class().get_uri()) == str(instrument))
        f['class'] = str(p.get_class().get_label())
        for i in range(p.get_num_ports()):
            port = p.get_port_by_index(i)
            if port.is_a(audio):
                f['audio_in' if port.is_a(inp) else 'audio_out'] += 1
            if port.is_a(cvp):
                f['cv'] += 1
            if (port.is_a(atom) or port.is_a(evp)) and port.is_a(inp) and port.supports_event(midi_ev):
                f['midi_in'] += 1
            if port.is_a(audio) and port.is_a(inp) and port.has_property(side):
                sc += 1
            if port.has_property(reports) or port.get(ns('http://lv2plug.in/ns/lv2core#designation')) == latency:
                lat = 1
        f['sidechains'] = sc
        f['latency_port'] = lat
        feats = [str(x) for x in p.get_required_features()] + [str(x) for x in p.get_optional_features()]
        f['worker'] = int(str(worker) in feats)
        out[u] = f
    return out


def _int(r, k):
    try:
        return int(r.get(k, 0) or 0)
    except ValueError:
        return 0


def plan(rows, facts):
    """(the set both hosts add with the layout this runner feeds, the skipped with their cause)."""
    keep, skipped = [], []
    for r in rows:
        if r.get('bucket') != 'both':
            continue
        u = r['uri']
        f = facts.get(u, {})
        e = dict(uri=u, cls=r.get('class', ''), scope=r.get('scope', ''), audio_in=_int(r, 'audio_in'),
                 audio_out=_int(r, 'audio_out'), cv=_int(r, 'cv'), midi_in=_int(r, 'midi_in'),
                 instrument=_int(r, 'instrument') or int('instrument' in r.get('scope', '')),
                 state=int('state#interface' in r.get('ext_data', '')),
                 worker=int('worker#' in r.get('ext_data', '') + r.get('req_features', '')) or f.get('worker', 0),
                 presets=f.get('presets', int('programs#' in r.get('ext_data', ''))),
                 latency_port=f.get('latency_port', ''), sidechains=f.get('sidechains', ''), name=f.get('name', ''))
        if e['audio_in'] > 2 or (f.get('sidechains') or 0) > 0:
            e['cause'] = f"omx-clap-core#92: {e['audio_in']} audio inputs, {f.get('sidechains') or 0} sidechain"
            skipped.append(e)
        elif e['cv'] > 0:
            e['cause'] = f"omx-clap-core#92: {e['cv']} CV ports"
            skipped.append(e)
        else:
            keep.append(e)
    return keep, skipped


TAILS = ('reverb', 'delay', 'echo', 'convol')
STRATA = [  # (name, how many, the test)
    ('long tail', 6, lambda e: any(t in (e['cls'] + ' ' + e['name'] + ' ' + e['uri'].rsplit('/', 1)[-1]).lower() for t in TAILS)),
    ('worker', 6, lambda e: e['worker']),
    ('presets', 6, lambda e: e['presets'] and e['presets'] != '0'),
    ('state', 8, lambda e: e['state']),
    ('instrument', 8, lambda e: e['instrument'] or e['audio_in'] == 0),
    ('MIDI effect', 4, lambda e: e['midi_in'] and e['audio_in'] > 0),
    ('stereo in, mono out', 2, lambda e: e['audio_in'] == 2 and e['audio_out'] == 1),
    ('mono', 10, lambda e: e['audio_in'] == 1 and e['audio_out'] == 1),
    ('stereo', 10, lambda e: e['audio_in'] == 2 and e['audio_out'] == 2),
]


def stratify(keep, n, seed):
    """n plugins over STRATA, scaled to n, each stratum filled from what earlier strata did not take."""
    rnd = random.Random(seed)
    total = sum(s[1] for s in STRATA)
    chosen, taken = [], set()
    for name, k, test in STRATA:
        k = max(1, round(k * n / total))
        pool = sorted((e for e in keep if e['uri'] not in taken and test(e)), key=lambda e: e['uri'])
        rnd.shuffle(pool)
        for e in pool[:k]:
            chosen.append(dict(e, stratum=name))
            taken.add(e['uri'])
    rest = sorted((e for e in keep if e['uri'] not in taken), key=lambda e: e['uri'])
    rnd.shuffle(rest)
    while len(chosen) < n and rest:
        chosen.append(dict(rest.pop(), stratum='any'))
    return chosen[:n]


PLAN_COLS = ['uri', 'stratum', 'cls', 'audio_in', 'audio_out', 'midi_in', 'instrument', 'state', 'worker',
             'presets', 'latency_port', 'sidechains', 'name']


def coverage_of(rows, keep, skipped):
    """The denominator: every row of the coverage table by bucket, the set run, the #92 skips, and the plugins
    mod-host hosts that the adapter does not (no parity possible: the adapter refuses them)."""
    by = {}
    for r in rows:
        by[r.get('bucket', '')] = by.get(r.get('bucket', ''), 0) + 1
    modhost_only = [dict(uri=r['uri'], refusal=r.get('omx_clap_host', ''), scope=r.get('scope', ''), cls=r.get('class', ''),
                         audio_in=r.get('audio_in', ''), audio_out=r.get('audio_out', ''), midi_in=r.get('midi_in', ''))
                    for r in rows if r.get('bucket') == 'modhost-only']
    mod_hosts = by.get('both', 0) + by.get('modhost-only', 0)
    return dict(total=len(rows), buckets=by, run=len(keep), skipped_92=len(skipped), modhost_only=len(modhost_only),
                mod_host_hosts=mod_hosts, no_parity=len(modhost_only) + len(skipped)), modhost_only


MODHOST_ONLY_COLS = ['uri', 'refusal', 'scope', 'cls', 'audio_in', 'audio_out', 'midi_in']


def cmd_probe(a):
    os.makedirs(a.out, exist_ok=True)
    if a.uris:
        # no coverage table: the URIs given are taken as added by both hosts, their layout from lilv
        uris = [u for u in (open(a.uris).read().split() if os.path.exists(a.uris) else a.uris.split(','))]
        facts = lilv_facts(uris)
        rows = [dict(uri=u, bucket='both', scope='instrument' if facts.get(u, {}).get('instrument') else 'effect',
                     **{k: str(facts.get(u, {}).get(k, 0)) for k in ('audio_in', 'audio_out', 'cv', 'midi_in', 'instrument')},
                     **{'class': facts.get(u, {}).get('class', '')}) for u in uris]
    else:
        rows = read_tsv(a.coverage)
        facts = {} if a.no_lilv else lilv_facts([r['uri'] for r in rows if r.get('bucket') == 'both'])
    keep, skipped = plan(rows, facts)
    if a.full or a.uris:
        chosen = [dict(e, stratum='given' if a.uris else 'all') for e in keep]
    else:
        chosen = stratify(keep, a.sample, a.seed)
    write_tsv(os.path.join(a.out, 'plugins.tsv'), chosen, PLAN_COLS)
    write_tsv(os.path.join(a.out, 'skipped.tsv'), skipped, PLAN_COLS + ['cause'])
    if not a.uris:
        cov, mo = coverage_of(rows, keep, skipped)
        write_tsv(os.path.join(a.out, 'modhost-only.tsv'), mo, MODHOST_ONLY_COLS)
        with open(os.path.join(a.out, 'coverage.json'), 'w') as f:
            json.dump(dict(cov, table=os.path.abspath(a.coverage)), f, indent=1)
    by = {}
    for e in chosen:
        by[e['stratum']] = by.get(e['stratum'], 0) + 1
    print(f"probe: {len(keep)} plugins both hosts add in a layout fed here, {len(skipped)} skipped (#92); "
          f"chose {len(chosen)}: " + ', '.join(f'{k} {v}' for k, v in by.items()))
    return 0


# ---------------------------------------------------------------- run (inside the namespace)


class Host:
    """A fresh mod-host or omx-clap-host, its command socket; probe.py's handling of crashes and hangs."""

    def __init__(self, kind, argv0, port, logdir, tag, env):
        self.kind, self.port = kind, port
        self.log = open(os.path.join(logdir, f'{tag}.{kind}.log'), 'w+')
        self.p = subprocess.Popen([argv0, '-n', '-p', str(port)], env=env, stdout=self.log,
                                  stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        self.s = None

    def connect(self, timeout=15):
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
        try:
            self.s.sendall(line.encode() + b'\0')
            buf = b''
            dl = time.time() + timeout
            while not buf.endswith(b'\0'):
                self.s.settimeout(max(0.05, dl - time.time()))
                d = self.s.recv(4096)
                if not d:
                    return None, 'closed'
                buf += d
            return buf[:-1].decode(errors='replace'), None
        except socket.timeout:
            return None, 'timeout'
        except OSError as e:
            return None, f'oserror {e}'

    def alive(self):
        return self.p.poll() is None

    def status(self):
        rc = self.p.poll()
        return f'signal {-rc}' if rc is not None and rc < 0 else f'exit {rc}'

    def cpu_ns(self):
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

    def logtext(self):
        self.log.flush()
        self.log.seek(0)
        return self.log.read()

    def kill(self):
        try:
            os.killpg(self.p.pid, signal.SIGKILL)
        except OSError:
            pass
        try:
            self.p.wait(5)
        except Exception:
            pass
        if self.s:
            self.s.close()
        self.log.close()


NAMES = {'mod': 'mod-host', 'omx': 'omx-clap-host'}


def mem_available_gib():
    with open('/proc/meminfo') as f:
        for l in f:
            if l.startswith('MemAvailable:'):
                return int(l.split()[1]) / 1048576
    return 0.0


def start_pipewire(rt, rate, quantum):
    os.makedirs(os.path.join(rt, 'etc/pipewire/pipewire.conf.d'), exist_ok=True)
    with open(os.path.join(rt, 'etc/pipewire/pipewire.conf.d/parity.conf'), 'w') as f:
        f.write('context.properties = {\n'
                f'  default.clock.rate = {rate}\n  default.clock.allowed-rates = [ {rate} ]\n'
                f'  default.clock.quantum = {quantum}\n  default.clock.min-quantum = {quantum}\n'
                f'  default.clock.max-quantum = {quantum}\n'
                '  # no driver but the feeder\'s clock: nothing runs until it triggers a cycle\n'
                '  factory.dummy-driver = false\n  factory.freewheel-driver = false\n}\n')
    env = dict(os.environ, XDG_RUNTIME_DIR=rt, PIPEWIRE_RUNTIME_DIR=rt, XDG_CONFIG_HOME=os.path.join(rt, 'etc'))
    for k in ('DBUS_SESSION_BUS_ADDRESS', 'PIPEWIRE_REMOTE', 'JACK_DEFAULT_SERVER', 'PULSE_SERVER'):
        env.pop(k, None)
    log = open(os.path.join(rt, 'pipewire.log'), 'w')
    p = subprocess.Popen(['pipewire'], env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    for _ in range(100):
        if os.path.exists(os.path.join(rt, 'pipewire-0')):
            return p, env
        time.sleep(0.05)
    raise SystemExit('pipewire did not come up: ' + open(os.path.join(rt, 'pipewire.log')).read())


class Lines:
    """The stdout of a child, line by line with a timeout (no buffered reader between select and the fd)."""

    def __init__(self, proc):
        self.fd = proc.stdout.fileno()
        self.buf = b''
        self.eof = False

    def until(self, prefix, timeout, sink):
        """Lines into sink until one starts with prefix; False on a timeout or the end of the output."""
        import select
        dl = time.time() + timeout
        while True:
            while b'\n' in self.buf:
                line, self.buf = self.buf.split(b'\n', 1)
                text = line.decode(errors='replace')
                sink.append(text)
                if text.startswith(prefix):
                    return True
            if self.eof or time.time() >= dl:
                return False
            r, _, _ = select.select([self.fd], [], [], max(0.0, dl - time.time()))
            if not r:
                return False
            d = os.read(self.fd, 65536)
            if not d:
                self.eof = True
                if self.buf:
                    self.buf += b'\n'
            self.buf += d


def one_pass(e, rate, a, env, work, logdir, tag, arms_of):
    """One plugin at one rate with the hosts in arms_of ({'mod': [0, 1], 'omx': [2, 3]}). A raw record."""
    np = _np()
    rec = {'uri': e['uri'], 'rate': rate, 'stratum': e.get('stratum', ''), 'status': 'ok'}
    hosts = {}
    port = {'mod': 5561, 'omx': 5562}
    feeder = None
    t_start = time.time()
    tp = rec['t'] = {}
    mark = lambda k: tp.__setitem__(k, round(time.time() - t_start, 3))
    try:
        for k in arms_of:
            hosts[k] = Host(k, a.mod_host if k == 'mod' else a.omx_clap_host, port[k], logdir, tag, env)
        for k, h in hosts.items():
            if not h.connect():
                rec.update(status='crash', host=NAMES[k], phase='start', detail=h.status() + ' ' + h.logtext()[-300:])
                return rec
        mark('started')
        for k, h in hosts.items():
            for n in arms_of[k]:
                cmd = f"add {e['uri']} {n}" if k == 'mod' else f"add lv2:{e['uri']} {n}"
                t0 = time.time()
                reply, err = h.send(cmd, a.add_timeout)
                rec.setdefault('add_s', {})[k] = round(time.time() - t0, 3)
                if err:
                    st = 'crash' if not h.alive() else 'hang'
                    rec.update(status=st, host=NAMES[k], phase='add',
                               detail=(h.status() if st == 'crash' else err) + ' ' + h.logtext()[-300:])
                    return rec
                if reply != f'resp {n}':
                    rec.update(status='refused', host=NAMES[k], detail=reply)
                    return rec
        mark('added')
        clients = [f'effect_{n}' for k in arms_of for n in arms_of[k]]
        frames = int(round(a.seconds * rate / QUANTUM)) * QUANTUM
        rec_dir = tempfile.mkdtemp(dir=work, prefix='rec.')
        argv = [a.feeder] + (['-m'] if int(e.get('midi_in') or 0) else []) + [str(frames), rec_dir] + clients
        feeder = subprocess.Popen(argv, env=env, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, text=True, bufsize=1)
        lines = Lines(feeder)
        out = []
        ok = lines.until('linked ' + clients[-1], 30, out)
        rec['feeder'] = out[:]
        if not ok:
            rec.update(status='error', detail='feeder: ' + ' | '.join(out[-5:]))
            return rec
        if not out[0].startswith(f'ready rate={rate} quantum={QUANTUM}'):
            rec.update(status='error', detail='the graph runs ' + out[0])
            return rec
        mark('linked')
        ready, rec['graph'] = graph_ready(env)
        if not ready:
            rec.update(status='error', detail='the graph did not settle: ' + rec['graph'])
            return rec
        mark('settled')
        rec['ports'] = {l.split()[1]: l.split(' ', 2)[2] for l in out if l.startswith('linked ')}
        midi = {c: ' midi=1' in p for c, p in rec['ports'].items()}
        if len(set(midi.values())) > 1:
            rec['note'] = 'a MIDI input through ' + ' and '.join(sorted({NAMES['mod' if c in ('effect_0', 'effect_1') else 'omx'] for c, v in midi.items() if v})) + ' only: no notes sent'
        # the adapter holds a latency the plugin reports after its first activate for 250 ms of blocks, then asks the
        # host for a restart, which the host's control thread runs in wall time: the first warm runs past the hold, the
        # pause gives the restart its wall time with the graph stopped, the second warm runs the restart's crossfade
        first = max(a.warm_blocks, -(-int(rate * a.warm_seconds) // QUANTUM))
        steps = [('warm', f'warm {first}', a.warm_timeout), ('hold', None, 0),
                 ('warm', f'warm {a.warm_blocks}', a.warm_timeout)]
        for what, line, tmo in steps:
            if line is None:
                time.sleep(a.hold_ms / 1000.0)
                continue
            if not _say(feeder, line) or not lines.until('warm', tmo, out):
                lines.until('\0', 1, out)
                return _stuck(rec, hosts, 'warm', out)
        mark('warm')
        cpu0 = {k: h.cpu_ns() for k, h in hosts.items()}
        t0 = time.time()
        if not _say(feeder, 'go paced' if a.paced else 'go') or not lines.until('done', a.run_timeout, out):
            lines.until('\0', 1, out)
            return _stuck(rec, hosts, 'run', out)
        rec['run_wall_ms'] = round((time.time() - t0) * 1000, 1)
        mark('played')
        rec['cpu_ms'] = {k: round((h.cpu_ns() - cpu0[k]) / 1e6, 2) for k, h in hosts.items()}
        rec['done'] = out[-1]
        for k, h in hosts.items():
            if not h.alive():
                rec.update(status='crash', host=NAMES[k], phase='run', detail=h.status() + ' ' + h.logtext()[-300:])
                return rec
        logs = {k: h.logtext() for k, h in hosts.items()}
        rec['latency_log_omx'] = [l for l in logs.get('omx', '').splitlines() if ': latency ' in l][-4:]
        warm_lines = [l for l in out if l.startswith('warm ') and 'latency=' in l]
        if warm_lines:
            rec['latency_jack'] = dict(x.split(':', 1) for x in warm_lines[-1].split('latency=', 1)[1].split(';') if ':' in x)
        else:
            rec['latency_jack'] = {c: rec['ports'][c].split('latency=')[1].split()[0] for c in clients}
        feeder.wait(10)
        if len(arms_of) < 2:
            return rec
        traces = {}
        for c in clients:
            chans = []
            for ch in range(2):
                path = os.path.join(rec_dir, f'{c}.{ch}.f32')
                if os.path.exists(path):
                    chans.append(np.fromfile(path, dtype=np.float32))
            traces[c] = np.array(chans, dtype=np.float32).reshape(len(chans), frames)
        for c in clients:
            want = min(2, int(rec['ports'][c].split('out=')[1].split()[0]))
            if traces[c].shape[0] != want:
                rec.update(status='error', detail=f'{c}: {traces[c].shape[0]} recordings of {want} outputs')
                return rec
        m0, m1 = traces[clients[0]], traces[clients[1]]
        o0, o1 = traces[clients[2]], traces[clients[3]]
        if SABOTAGE == 'offset':
            # the adapter's output one sample late, in a copy: the end-to-end fixture must read it red
            o0, o1 = (np.concatenate((np.zeros((x.shape[0], 1), np.float32), x[:, :-1]), axis=1) for x in (o0, o1))
        rec['m'] = measure(m0, m1, o0, o1, rate, frames * 6 // 10)
        rec['m']['equal_second'] = bits_equal(m1, o1)
        mark('measured')
        cls = classify(rec)[0]
        if cls == 'DIFFERS' and all(h.alive() for h in hosts.values()):
            # the cause most often behind a deterministic difference: a control the two hosts start differently
            rec['param_diff'] = param_diff(hosts, e['uri'], a.lv2_path)
            cls = classify(rec)[0]
        if a.keep == 'all' or (a.keep == 'differs' and cls != 'IDENTICAL'):
            kd = os.path.join(a.out, 'keep', str(rate), _slug(e['uri']))
            os.makedirs(kd, exist_ok=True)
            for f in os.listdir(rec_dir):
                shutil.move(os.path.join(rec_dir, f), os.path.join(kd, f))
        for f in os.listdir(rec_dir):
            os.unlink(os.path.join(rec_dir, f))
        os.rmdir(rec_dir)
        return rec
    finally:
        rec['wall_s'] = round(time.time() - t_start, 2)
        if feeder and feeder.poll() is None:
            feeder.kill()
            feeder.wait(5)
        for h in hosts.values():
            h.kill()


def graph_ready(env, timeout=10.0):
    """Every link of the graph active and no node still starting: a cycle triggered before a link has agreed
    its format stalls in its first cycle, and with no driver timeout it never completes."""
    dl = time.time() + timeout
    last = ''
    while time.time() < dl:
        try:
            objs = json.loads(subprocess.run(['pw-dump'], env=env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                                             text=True, timeout=5).stdout or '[]')
        except (subprocess.TimeoutExpired, ValueError):
            objs = []
        links = [o for o in objs if o.get('type') == 'PipeWire:Interface:Link']
        states = [((o.get('info') or {}).get('state') or '') for o in links]
        last = f"{len(links)} links: " + ', '.join(f'{states.count(x)} {x}' for x in sorted(set(states)))
        if links and all(x == 'active' for x in states):
            return True, last
        time.sleep(0.02)
    return False, last


_WORLD = {}


def control_symbols(uri, lv2_path):
    """The symbols of the plugin's input control ports, from lilv (None when lilv cannot say)."""
    try:
        import lilv
    except ImportError:
        return None
    if 'w' not in _WORLD:
        os.environ['LV2_PATH'] = lv2_path
        w = lilv.World()
        w.load_all()
        _WORLD['w'] = w
    w = _WORLD['w']
    p = w.get_all_plugins().get_by_uri(w.new_uri(uri))
    if p is None:
        return None
    ctl, inp = w.new_uri('http://lv2plug.in/ns/lv2core#ControlPort'), w.new_uri('http://lv2plug.in/ns/lv2core#InputPort')
    out = []
    for i in range(p.get_num_ports()):
        port = p.get_port_by_index(i)
        if port.is_a(ctl) and port.is_a(inp):
            out.append(str(port.get_symbol()))
    return out


def param_diff(hosts, uri, lv2_path):
    """Every input control whose value mod-host's instance 0 and omx-clap-host's instance 2 read differently."""
    syms = control_symbols(uri, lv2_path)
    if syms is None:
        return None
    diff = []
    for sym in syms:
        a, _ = hosts['mod'].send(f'param_get 0 {sym}', 2)
        b, _ = hosts['omx'].send(f'param_get 2 {sym}', 2)
        # a control one host does not read (resp -103: the adapter keeps lv2:freeWheeling and the like to itself)
        # is no value to compare
        if not (a or '').startswith('resp 0 ') or not (b or '').startswith('resp 0 '):
            continue
        va, vb = a.split()[2], b.split()[2]
        if va != vb:
            diff.append(f"{sym} mod-host {va} omx-clap-host {vb}")
    return diff


def _say(feeder, line):
    try:
        feeder.stdin.write(line + '\n')
        feeder.stdin.flush()
        return True
    except OSError:
        try:
            feeder.stdin.close()
        except OSError:
            pass
        return False


def _stuck(rec, hosts, phase, out):
    for k, h in hosts.items():
        if not h.alive():
            rec.update(status='crash', host=NAMES[k], phase=phase, detail=h.status() + ' ' + h.logtext()[-300:])
            return rec
    if not any(l.startswith('linked') for l in out[-1:]) and out and ('no cycles' in out[-1] or 'did not complete' in out[-1]):
        rec.update(status='hang', host='?', phase=phase, detail='feeder: ' + ' | '.join(out[-3:]))
    elif out and not out[-1].startswith(('warm', 'linked', 'ready')):
        rec.update(status='error', detail=f'feeder at {phase}: ' + ' | '.join(out[-3:]))
    else:
        rec.update(status='hang', host='?', phase=phase, detail='feeder: ' + ' | '.join(out[-3:]))
    return rec


def _slug(u):
    return ''.join(c if c.isalnum() or c in '-_.' else '_' for c in u)[-120:]


def run_plugin(e, rate, a, env, work, logdir, n):
    rec = one_pass(e, rate, a, env, work, logdir, f'p{n:04d}', {'mod': [0, 1], 'omx': [2, 3]})
    if rec['status'] in ('hang', 'crash') and rec.get('phase') in ('warm', 'run') and rec.get('host', '?') == '?':
        # both hosts alive and the graph stuck: run each host alone to name it
        alone = {}
        for k, arms in (('mod', [0, 1]), ('omx', [2, 3])):
            r = one_pass(e, rate, a, env, work, logdir, f'p{n:04d}-{k}', {k: arms})
            alone[k] = r['status']
        stuck = [NAMES[k] for k, s in alone.items() if s != 'ok']
        rec['host'] = ' and '.join(stuck) if stuck else 'neither alone (only together)'
        rec['alone'] = alone
    return rec


def inside_run(a):
    if os.getpid() != 1:
        raise SystemExit('run: not pid 1 of a private namespace, refusing')
    subprocess.run(['ip', 'link', 'set', 'lo', 'up'], check=False)
    # a tmpfs of the namespace's own at /tmp: PipeWire's socket path must fit in 108 bytes, and the recordings of a
    # plugin live there until they are compared; nothing of it is seen outside, and it goes with the namespace
    subprocess.run(['mount', '-t', 'tmpfs', '-o', 'size=512m,mode=0700', 'lv2-parity', '/tmp'], check=True)
    # the user's own runtime dir, where the live PipeWire's socket is: a filesystem socket is not netns-scoped, so the
    # environment alone would keep a host or a plugin that hard-codes the path out. An empty tmpfs over it, seen only
    # in this namespace, and the run refused when the live socket still shows
    outer = os.environ.get('LV2_PARITY_OUTER_RUNTIME', '')
    if outer and os.path.isdir(outer):
        subprocess.run(['mount', '-t', 'tmpfs', '-o', 'size=1m,mode=0700', 'lv2-parity-run', outer], check=True)
    for sock in ('pipewire-0', 'pipewire-0-manager', 'pulse/native'):
        if outer and os.path.exists(os.path.join(outer, sock)):
            raise SystemExit(f'run: the live {sock} is still reachable at {outer}, refusing')
    plugins = read_tsv(a.plugins)
    if a.limit:
        plugins = plugins[:a.limit]
    os.makedirs(os.path.join(a.out, 'raw'), exist_ok=True)
    for rate in [int(r) for r in a.rates.split(',')]:
        rawp = os.path.join(a.out, 'raw', f'{rate}.jsonl')
        done = set()
        if os.path.exists(rawp):
            done = {json.loads(l)['uri'] for l in open(rawp) if l.strip()}
        todo = [e for e in plugins if e['uri'] not in done]
        rt = tempfile.mkdtemp(prefix=f'pw{rate}.', dir='/tmp')
        logdir = os.path.join(a.work, str(rate))
        os.makedirs(logdir, exist_ok=True)
        pw, env = start_pipewire(rt, rate, QUANTUM)
        env.update(LV2_PATH=a.lv2_path, LD_LIBRARY_PATH='/usr/lib64/pipewire-0.3/jack')
        t0 = time.time()
        with open(rawp, 'a') as raw:
            for n, e in enumerate(todo, 1):
                if mem_available_gib() < a.min_free_gib:
                    print(f'STOPPED low memory: {mem_available_gib():.1f} GiB available', flush=True)
                    pw.kill()
                    return 3
                if pw.poll() is not None:
                    pw, _ = start_pipewire(rt, rate, QUANTUM)
                try:
                    rec = run_plugin(e, rate, a, env, rt, logdir, n)
                except Exception as x:  # the runner's own fault on one plugin: recorded, the run goes on
                    import traceback
                    rec = {'uri': e['uri'], 'rate': rate, 'stratum': e.get('stratum', ''), 'status': 'error',
                           'detail': f'runner: {x!r} ' + traceback.format_exc().splitlines()[-3].strip()}
                if rec['status'] == 'ok':
                    rec.pop('feeder', None)
                raw.write(json.dumps(rec) + '\n')
                raw.flush()
                c = classify(rec)
                print(f"{rate} {n}/{len(todo)} {c[0]:<16} {e['uri']} {c[1]} ({rec['wall_s']} s)", flush=True)
        print(f'rate {rate}: {len(todo)} plugins in {time.time() - t0:.0f} s', flush=True)
        try:
            os.killpg(pw.pid, signal.SIGKILL)
        except OSError:
            pass
        pw.wait(5)
    return 0


def _version(argv):
    try:
        return subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, timeout=10).stdout.strip()
    except Exception:
        return ''


def write_meta(a):
    """What the report says about how the run was made: the hosts, their versions, the settings."""
    mp = os.path.join(a.out, 'meta.json')
    meta = json.load(open(mp)) if os.path.exists(mp) else {}
    omx = os.path.realpath(a.omx_clap_host)
    tree = os.path.dirname(omx)
    omx_v = _version(['git', '-C', tree, 'describe', '--always', '--dirty', '--tags']) or _version(['rpm', '-qf', omx])
    meta.update(date=time.strftime('%Y-%m-%d'), rates=sorted(set(meta.get('rates', [])) | {int(r) for r in a.rates.split(',')}),
                quantum=QUANTUM, seconds=a.seconds, warm_blocks=a.warm_blocks, warm_seconds=a.warm_seconds, hold_ms=a.hold_ms, paced=a.paced,
                mod_host=f"{a.mod_host} ({_version(['rpm', '-qf', os.path.realpath(a.mod_host)])})",
                omx_clap_host=f'{omx} ({omx_v})', lv2_path=a.lv2_path,
                pipewire=_version(['pipewire', '--version']).splitlines()[-1:] or [''],
                host=os.uname().nodename, out=os.path.abspath(a.out))
    with open(mp, 'w') as f:
        json.dump(meta, f, indent=1)


def cmd_run(a):
    a.plugins = a.plugins or os.path.join(a.out, 'plugins.tsv')
    a.work = a.work or os.path.join(a.out, 'work')
    os.makedirs(a.work, exist_ok=True)
    for path in (a.feeder, a.omx_clap_host, a.mod_host, a.plugins):
        if not os.path.exists(path):
            raise SystemExit(f'missing {path}')
    if os.environ.get('LV2_PARITY_INSIDE') == '1':
        return inside_run(a)
    write_meta(a)
    argv = ['unshare', '--user', '--map-root-user', '--net', '--pid', '--fork', '--mount-proc',
            sys.executable, os.path.abspath(__file__)] + sys.argv[1:]
    outer = os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}'
    return subprocess.call(argv, env=dict(os.environ, LV2_PARITY_INSIDE='1', LV2_PARITY_OUTER_RUNTIME=outer))


# ---------------------------------------------------------------- classify and report

ROW_COLS = ['rate', 'uri', 'class', 'cause', 'detail', 'note', 'stratum', 'max_diff', 'first_diff', 'lag', 'peak_mod',
            'peak_omx', 'latency_mod', 'latency_omx', 'latency_log_omx', 'cpu_ms_mod', 'cpu_ms_omx', 'wall_s',
            'add_s_mod', 'add_s_omx']


def load_raw(out):
    recs = []
    rd = os.path.join(out, 'raw')
    for f in sorted(os.listdir(rd)) if os.path.isdir(rd) else []:
        if f.endswith('.jsonl'):
            recs += [json.loads(l) for l in open(os.path.join(rd, f)) if l.strip()]
    return recs


def row_of(rec):
    cls, cause, detail = classify(rec)
    m = rec.get('m', {})
    lj = rec.get('latency_jack', {})
    return dict(rate=rec['rate'], uri=rec['uri'], stratum=rec.get('stratum', ''), **{'class': cls}, cause=cause,
                detail=detail, note=rec.get('note', ''), max_diff=m.get('max_diff', ''), first_diff=m.get('first_over_tol', ''),
                lag=m.get('lag', ''), peak_mod=m.get('peak_mod', ''), peak_omx=m.get('peak_omx', ''),
                latency_mod=lj.get('effect_0', ''), latency_omx=lj.get('effect_2', ''),
                latency_log_omx=' / '.join(l.split(': ', 1)[-1] for l in rec.get('latency_log_omx', [])),
                cpu_ms_mod=rec.get('cpu_ms', {}).get('mod', ''), cpu_ms_omx=rec.get('cpu_ms', {}).get('omx', ''),
                wall_s=rec.get('wall_s', ''), add_s_mod=rec.get('add_s', {}).get('mod', ''),
                add_s_omx=rec.get('add_s', {}).get('omx', ''))


def cmd_classify(a):
    rows = [row_of(r) for r in load_raw(a.out)]
    skipped = os.path.join(a.out, 'skipped.tsv')
    write_tsv(os.path.join(a.out, 'parity.tsv'), rows, ROW_COLS)
    counts = {}
    for r in rows:
        counts[r['class']] = counts.get(r['class'], 0) + 1
    print(f"classify: {len(rows)} measurements: " + ', '.join(f'{k} {v}' for k, v in sorted(counts.items())) +
          (f"; skipped (#92) {len(read_tsv(skipped))}" if os.path.exists(skipped) else ''))
    return 0


CLASSES = ['IDENTICAL', 'WITHIN-TOLERANCE', 'SILENT', 'DIFFERS', 'NONDETERMINISTIC', 'CRASH', 'HANG', 'REFUSED', 'ERROR']


def report_text(rows, skipped, meta):
    rates = sorted({int(r['rate']) for r in rows})
    uris = sorted({r['uri'] for r in rows})
    L = [f"# LV2 parity: mod-host against omx-clap-host's LV2 adapter, {meta.get('date', '')}", '']
    if meta:
        L += [f"Measured on {meta.get('host', '?')} by `tests/lv2_parity.py` (omx-clap-core). mod-host: {meta.get('mod_host', '?')}. "
              f"omx-clap-host: {meta.get('omx_clap_host', '?')}. {' '.join(meta.get('pipewire', ['']))}. "
              f"Quantum {meta.get('quantum')}, {meta.get('seconds')} s of input per plugin and rate.", '',
              'Method: one private PipeWire per rate in its own user, net, pid and mount namespace, whose only driver is the '
              "feeder's clock, so nothing runs until it triggers a cycle and every instance gets its first run() in the same "
              'cycle. Per plugin a fresh mod-host and a fresh omx-clap-host each add the plugin twice (default parameters, '
              f"nothing set); {meta.get('warm_seconds')} s of silence (past the adapter's latency hold of 250 ms of blocks), a pause "
              f"of {meta.get('hold_ms')} ms of wall time with the graph stopped (the host's restart for that latency, a worker's "
              f"first answer), {meta.get('warm_blocks')} blocks of silence more (the restart's fade), then "
              'noise + sine + an impulse per second on two channels for the first 60 % and silence for the tail (a fixed note '
              'sequence to a MIDI input). The cycles run back to back' + (' one quantum of wall time apart (paced).' if meta.get('paced') else
              ', as fast as the graph completes them.') + ' Compared from the first played sample: each host\'s two '
              'instances with each other, then the first instance of each host bit for bit. WITHIN-TOLERANCE: under 1e-6 '
              f'and under {REL_TOL:g} of the peak, rounding or subnormals, at no offset. An offset, even exact, is DIFFERS. '
              'NONDETERMINISTIC only when the plugin is random in both hosts and the hosts agree instance by instance, or '
              f'differ by no more than {SPREAD_K} times its own spread and {SPREAD_DB} dB in level.', '']
    L += ['## Counts per class', '', '| class | ' + ' | '.join(f'{r // 1000 if r % 1000 == 0 else r / 1000} kHz' for r in rates) + ' |',
          '|---|' + '---:|' * len(rates)]
    for c in CLASSES:
        cells = [sum(1 for r in rows if r['class'] == c and int(r['rate']) == rt) for rt in rates]
        if any(cells):
            L.append(f'| {c} | ' + ' | '.join(str(x) for x in cells) + ' |')
    L.append('| total | ' + ' | '.join(str(sum(1 for r in rows if int(r['rate']) == rt)) for rt in rates) + ' |')
    L += ['', 'IDENTICAL counts only sound compared: a plugin silent through both hosts with its defaults is SILENT, '
          'which proves nothing; a port one host has and the other lacks (an output, a MIDI input) is DIFFERS whatever '
          'the audio; a host that is random where the other is not is DIFFERS.']
    silent = [r for r in rows if r['class'] == 'SILENT']
    if silent:
        L += ['', f"SILENT ({len(silent)} measurements): " + ', '.join(sorted({r['uri'] for r in silent}))]
    cov = meta.get('coverage')
    if cov:
        b = cov['buckets']
        L += ['', '## The denominator', '',
              f"Of the {cov['total']}-row coverage table: {b.get('both', 0)} both hosts add ({cov['run']} in the set run here, "
              f"{cov['skipped_92']} skipped under omx-clap-core#92: more than two audio inputs, a sidechain or CV ports); "
              f"{b.get('modhost-only', 0)} mod-host only (the adapter refuses them: no parity possible, each a gap for "
              f"removing mod-host); {b.get('neither', 0)} neither; {b.get('adapter-only', 0)} the adapter only. "
              f"**{cov['no_parity']} of the {cov['mod_host_hosts']} plugins mod-host hosts have no parity measurement** "
              f"({cov['modhost_only']} mod-host only + {cov['skipped_92']} #92). Measured here: {len(uris)} plugins.", '']
        mo = meta.get('modhost_only_rows') or []
        if mo:
            codes = {}
            for r in mo:
                codes.setdefault(r.get('refusal') or '?', []).append(r)
            L += ['The mod-host-only plugins by the adapter\'s refusal (every one in `modhost-only.tsv`):', '',
                  '| refusal | plugins | examples |', '|---|---:|---|']
            for code, rs in sorted(codes.items(), key=lambda kv: -len(kv[1])):
                L.append(f"| {code} | {len(rs)} | " + ', '.join(r['uri'] for r in rs[:4]) + (' ...' if len(rs) > 4 else '') + ' |')
            L.append('')
    strata = sorted({r.get('stratum', '') for r in rows if r.get('stratum')})
    if len(strata) > 1:
        cls_here = [c for c in CLASSES if any(r['class'] == c for r in rows)]
        L += ['', '## Per stratum (measurements, every rate)', '', '| stratum | ' + ' | '.join(cls_here) + ' |',
              '|---|' + '---:|' * len(cls_here)]
        for st in strata:
            L.append(f'| {st} | ' + ' | '.join(str(sum(1 for r in rows if r.get('stratum') == st and r['class'] == c)) for c in cls_here) + ' |')
    if not cov:
        L += ['', f'Plugins: {len(uris)}. Skipped before the run (omx-clap-core#92, more than two audio inputs, a '
              f'sidechain or CV ports): {len(skipped)}. No coverage table: the denominator is not known.']
    def table(cls, title):
        sel = [r for r in rows if r['class'] == cls]
        if not sel:
            return
        causes = {}
        for r in sel:
            causes.setdefault(r['cause'], []).append(r)
        L.extend(['', f'## {title}', '', '| cause | measurements | plugins (rate) |', '|---|---:|---|'])
        for cause, rs in sorted(causes.items(), key=lambda kv: -len(kv[1])):
            ex = ', '.join(f"{r['uri']} ({int(r['rate']) // 1000}k)" for r in rs[:6]) + (' ...' if len(rs) > 6 else '')
            L.append(f'| {cause} | {len(rs)} | {ex} |')
        L.extend(['', '<details><summary>detail</summary>', ''])
        for r in sel:
            L.append(f"- {r['uri']} at {r['rate']}: {r['cause']}. {r['detail']}")
        L.extend(['', '</details>'])
    table('DIFFERS', 'DIFFERS, by cause')
    table('WITHIN-TOLERANCE', 'WITHIN-TOLERANCE, by reason')
    table('NONDETERMINISTIC', 'NONDETERMINISTIC')
    table('CRASH', 'CRASH')
    table('HANG', 'HANG')
    table('REFUSED', 'REFUSED this time')
    table('ERROR', 'ERROR (the runner could not measure)')
    notes = {}
    for r in rows:
        if r.get('note'):
            notes.setdefault(r['note'], []).append(r)
    if notes:
        L += ['', '## Notes', '']
        for note, rs in sorted(notes.items(), key=lambda kv: -len(kv[1])):
            cls_n = ', '.join(f'{c} {sum(1 for r in rs if r["class"] == c)}' for c in CLASSES if any(r['class'] == c for r in rs))
            L.append(f"- {note}: {len(rs)} measurements ({cls_n}): " + ', '.join(sorted({r['uri'] for r in rs})))
    lat = [r for r in rows if r.get('latency_mod', '') != r.get('latency_omx', '') or
           any(x != 'latency 0 frames' for x in (r.get('latency_log_omx') or 'latency 0 frames').split(' / '))]
    L += ['', '## Latency', '',
          'The capture latency of each host\'s first output port as the graph reports it after the links '
          '(min,max frames), and the latency omx-clap-host logs when the adapter publishes one.', '']
    if lat:
        L += ['| plugin | rate | mod-host (graph) | omx-clap-host (graph) | omx-clap-host (log) | class |', '|---|---:|---|---|---|---|']
        for r in lat:
            L.append(f"| {r['uri']} | {r['rate']} | {r['latency_mod']} | {r['latency_omx']} | {r['latency_log_omx']} | {r['class']} |")
    else:
        L.append('Both hosts reported the same graph latency for every plugin, and the adapter published none but 0.')
    cpu = [(float(r['cpu_ms_mod']), float(r['cpu_ms_omx']), r) for r in rows if r.get('cpu_ms_mod') not in ('', None) and r.get('cpu_ms_omx') not in ('', None)]
    if cpu:
        ratios = sorted(o / m for m, o, _ in cpu if m > 0.5)
        med = ratios[len(ratios) // 2] if ratios else float('nan')
        heavy = sorted(((o / m, r) for m, o, r in cpu if m > 0.5 and o / m > 1.5), key=lambda x: -x[0])
        L += ['', '## CPU', '', f"CPU time of each host process over the played window (two instances each, every thread "
              f"of the process, from /proc/<pid>/task/*/schedstat). omx-clap-host / mod-host, median over {len(ratios)} "
              f"measurements where mod-host used more than 0.5 ms: {med:.2f}."]
        if heavy:
            L += ['', 'omx-clap-host above 1.5 times mod-host:', '']
            L += [f"- {r['uri']} at {r['rate']}: {x:.2f} ({r['cpu_ms_omx']} ms against {r['cpu_ms_mod']} ms)" for x, r in heavy[:20]]
    walls = {}
    for r in rows:
        if r.get('wall_s') not in ('', None):
            walls.setdefault(int(r['rate']), []).append(float(r['wall_s']))
    if walls:
        L += ['', '## Time', '']
        for rt in sorted(walls):
            w = walls[rt]
            L.append(f"- {rt}: {len(w)} plugins in {sum(w):.0f} s, {sum(w) / len(w):.2f} s per plugin (median {sorted(w)[len(w) // 2]:.2f} s).")
        if meta.get('full_count'):
            per = {rt: sum(w) / len(w) for rt, w in walls.items()}
            # the cost per plugin is mostly the hosts' start and the plugin's add, which the rate barely moves;
            # extrapolate linearly in the rate from what was measured
            if len(per) >= 2:
                (r1, p1), (r2, p2) = sorted(per.items())[0], sorted(per.items())[-1]
                slope = (p2 - p1) / (r2 - r1)
                est = sum(p1 + slope * (rt - r1) for rt in (44100, 48000, 96000, 192000))
            else:
                est = 4 * list(per.values())[0]
            n = meta['full_count']
            L.append(f"- Full run estimate: {n} plugins at 44.1, 48, 96 and 192 kHz, {est:.1f} s per plugin over the four rates: "
                     f"{n * est / 3600:.1f} h.")
    L += ['', '## Files', '', f"In `{meta.get('out', '<out>')}` on {meta.get('host', 'the desk')}: `parity.tsv` (one row per plugin and rate: "
          'class, cause, detail, the largest difference and where, latencies, CPU), `plugins.tsv` (the set and the stratum '
          'of each), `skipped.tsv` (#92), `raw/<rate>.jsonl` (every measurement), `keep/<rate>/<plugin>/` (the recordings of '
          'what did not come out IDENTICAL), `work/` (host logs).']
    return '\n'.join(L) + '\n'


def cmd_report(a):
    p = os.path.join(a.out, 'parity.tsv')
    if not os.path.exists(p):
        cmd_classify(a)
    rows = read_tsv(p)
    sk = os.path.join(a.out, 'skipped.tsv')
    skipped = read_tsv(sk) if os.path.exists(sk) else []
    meta = {}
    mp = os.path.join(a.out, 'meta.json')
    if os.path.exists(mp):
        meta = json.load(open(mp))
    if a.full_count:
        meta['full_count'] = a.full_count
    cp = os.path.join(a.out, 'coverage.json')
    if getattr(a, 'coverage', None) and os.path.exists(a.coverage):
        # the denominator from the table itself (a run probed before probe wrote coverage.json)
        crow = read_tsv(a.coverage)
        keep, sk2 = plan(crow, lilv_facts([r['uri'] for r in crow if r.get('bucket') == 'both']))
        cov, mo = coverage_of(crow, keep, sk2)
        write_tsv(os.path.join(a.out, 'modhost-only.tsv'), mo, MODHOST_ONLY_COLS)
        with open(cp, 'w') as f:
            json.dump(dict(cov, table=os.path.abspath(a.coverage)), f, indent=1)
    if os.path.exists(cp):
        meta['coverage'] = json.load(open(cp))
        mp2 = os.path.join(a.out, 'modhost-only.tsv')
        meta['modhost_only_rows'] = read_tsv(mp2) if os.path.exists(mp2) else []
    text = report_text(rows, skipped, meta)
    with open(os.path.join(a.out, 'summary.md'), 'w') as f:
        f.write(text)
    print(f"report: {os.path.join(a.out, 'summary.md')}")
    return 0


# ---------------------------------------------------------------- self-test


def self_test():
    np = _np()
    fails, oks = [], 0

    def t(name, cond):
        nonlocal oks
        if cond:
            oks += 1
            print(f'ok   {name}')
        else:
            fails.append(name)
            print(f'FAIL {name}')

    rate, n = 48000, 48000
    rng = np.random.default_rng(7)
    sig = (rng.standard_normal((2, n)) * 0.1).astype(np.float32)
    sig[:, int(n * 0.6):] *= 0.001
    end = int(n * 0.6)

    def rec_of(m0, m1, o0, o1):
        m = measure(m0, m1, o0, o1, rate, end)
        m['equal_second'] = bits_equal(m1, o1)
        return {'uri': 'urn:fake', 'rate': rate, 'status': 'ok', 'm': m}

    def cls(m0, m1, o0, o1):
        return classify(rec_of(m0, m1, o0, o1))

    c = cls(sig, sig.copy(), sig.copy(), sig.copy())
    t('the same samples through both hosts: IDENTICAL', c[0] == 'IDENTICAL' and c[1] == '')
    z = np.zeros_like(sig)
    t('silence through both: SILENT, which proves nothing (not IDENTICAL)', cls(z, z, z, z)[:2] == ('SILENT', 'proves nothing'))
    o = sig.copy()
    o[1, 30000] += np.float32(3e-7)
    c = cls(sig, sig, o, o.copy())
    t('one sample off by 3e-7: WITHIN-TOLERANCE, rounding', c[:2] == ('WITHIN-TOLERANCE', 'rounding'))
    s, f = sig.copy(), sig.copy()
    s[0, 40000], f[0, 40000] = np.float32(1e-40), np.float32(0.0)
    c = cls(s, s, f, f.copy())
    t('a subnormal flushed to zero in one host: WITHIN-TOLERANCE, subnormal', c[0] == 'WITHIN-TOLERANCE' and 'subnormal' in c[1])
    late = np.zeros_like(sig)
    late[:, 64:] = sig[:, :-64]
    c = cls(sig, sig, late, late.copy())
    t('omx-clap-host 64 samples late, exact after the shift: DIFFERS, offset of 64 samples (never a pass)', c[:2] == ('DIFFERS', 'offset of 64 samples, exact'))
    t('... and its detail says late', 'late by 64' in c[2])
    q = (sig * np.float32(0.5)).astype(np.float32)
    c = cls(sig, sig, q, q.copy())
    t('half the level through omx-clap-host: DIFFERS, level +6.021 dB', c[:2] == ('DIFFERS', 'level') and '+6.021 dB' in c[2])
    o2 = sig.copy()
    o2[0, 100] = np.float32(0.9)
    o2[1, 101] = np.float32(-0.9)
    c = cls(sig, sig, o2, o2.copy())
    t('two samples in the first 100 ms: DIFFERS, start-up only, where named',
      c[:2] == ('DIFFERS', 'start-up only (the first 100 ms)') and 'first at sample 100 (block 0, +100)' in c[2])
    o3 = sig.copy()
    o3[:, end + 1000:] += np.float32(0.01)
    c = cls(sig, sig, o3, o3.copy())
    t('a tail that differs after the input stops: DIFFERS, tail only', c[:2] == ('DIFFERS', 'tail only (after the input stops)'))
    o4 = sig.copy()
    o4[:, 20000:] += (rng.standard_normal((2, n - 20000)) * 1e-5).astype(np.float32)
    c = cls(sig, sig, o4, o4.copy())
    t('noise at -80 dB of the peak from 0.4 s: DIFFERS, small numeric drift', c[:2] == ('DIFFERS', 'small numeric drift (under -60 dB of the peak)'))
    other = (rng.standard_normal((2, n)) * 0.1).astype(np.float32)
    c = cls(sig, sig, other, other.copy())
    t('another signal: DIFFERS, different output', c[:2] == ('DIFFERS', 'different output'))
    r = dict(rec_of(sig, sig, other, other.copy()), param_diff=['drive mod-host 0.5000 omx-clap-host 1.0000'])
    c = classify(r)
    t('another signal with a control read differently: DIFFERS, the control named', c[:2] == ('DIFFERS', 'a control starts at a different value') and c[2].startswith('drive mod-host 0.5000'))
    c = cls(sig, sig, sig[::-1].copy(), sig[::-1].copy())
    t('left and right swapped: DIFFERS, channels swapped', c[:2] == ('DIFFERS', 'channels swapped'))
    c = cls(sig, sig, z, z)
    t('silent through omx-clap-host only: DIFFERS, silent through omx-clap-host', c[:2] == ('DIFFERS', 'silent through omx-clap-host'))
    nan = sig.copy()
    nan[0, 5000] = np.float32('nan')
    c = cls(sig, sig, nan, nan.copy())
    t('a NaN through omx-clap-host only: DIFFERS, NaN or Inf', c[:2] == ('DIFFERS', 'NaN or Inf in one host'))
    c = cls(sig[:1].copy(), sig[:1].copy(), sig, sig)
    t('one output against two: DIFFERS, output count', c[:2] == ('DIFFERS', 'output count'))
    c = cls(sig, other, sig, sig)
    t("random in mod-host only: DIFFERS, nondeterministic in mod-host only", c[:2] == ('DIFFERS', 'nondeterministic in mod-host only'))
    c = cls(sig, sig, sig, other)
    t("random in omx-clap-host only (dexed): DIFFERS, nondeterministic in omx-clap-host only", c[:2] == ('DIFFERS', 'nondeterministic in omx-clap-host only'))
    c = cls(sig, other, sig, other)
    t('a per-instance seed, the hosts agreeing instance by instance: NONDETERMINISTIC, named so',
      c[0] == 'NONDETERMINISTIC' and c[1].endswith('the hosts agree instance by instance'))
    def noisy(seed, gain=1.0):
        g = np.random.default_rng(seed)
        return ((sig + (g.standard_normal(sig.shape) * 0.01).astype(np.float32)) * np.float32(gain)).astype(np.float32)
    c = cls(noisy(1), noisy(2), noisy(3), noisy(4))
    t('random in both hosts, across hosts within its own spread: NONDETERMINISTIC', c[0] == 'NONDETERMINISTIC' and 'within its spread' in c[1])
    c = cls(noisy(1), noisy(2), noisy(3, 2.0), noisy(4, 2.0))
    t("random in both hosts, omx-clap-host twice as loud: DIFFERS, beyond the plugin's own spread",
      c[:2] == ('DIFFERS', "beyond the plugin's own spread"))
    def with_ports(r, mod, omx):
        r = dict(r)
        r['ports'] = {'effect_0': f'in=2 out={mod[0]} midi={mod[1]} midi_fed=0', 'effect_2': f'in=2 out={omx[0]} midi={omx[1]} midi_fed=0'}
        return r
    c = classify(with_ports(rec_of(sig, sig, sig, sig), (2, 1), (2, 0)))
    t('a MIDI input through mod-host only, the audio bit-identical: DIFFERS, MIDI input count', c[:2] == ('DIFFERS', 'MIDI input count') and 'bit-identical' in c[2])
    c = classify(with_ports(rec_of(sig, sig, sig, sig), (4, 0), (2, 0)))
    t('four outputs against two, the first two bit-identical: DIFFERS, output count', c[:2] == ('DIFFERS', 'output count') and 'mod-host 4' in c[2])
    small = (sig * np.float32(1e-4)).astype(np.float32)
    sm = small.copy()
    sm[0, 30000] += np.float32(5e-7)
    c = cls(small, small, sm, sm.copy())
    t('5e-7 off at a peak of 4e-5: under 1e-6 but over -100 dB of the peak, DIFFERS', c[:2] == ('DIFFERS', 'under 1e-6 but over -100 dB of a small peak'))
    for st, want in (({'status': 'crash', 'host': 'mod-host', 'phase': 'add'}, ('CRASH', 'mod-host at add')),
                     ({'status': 'hang', 'host': 'omx-clap-host', 'phase': 'run'}, ('HANG', 'omx-clap-host at run')),
                     ({'status': 'refused', 'host': 'omx-clap-host', 'detail': 'resp -102 x'}, ('REFUSED', 'omx-clap-host: resp -102 x'))):
        t(f"a {st['status']} record: {want[0]}", classify(dict(st, uri='u', rate=48000))[:2] == want)

    # probe: the #92 skips and the stratified sample, on a fake coverage table
    cols = ['uri', 'omx_clap_host', 'mod_host', 'bucket', 'scope', 'class', 'instrument', 'audio_in', 'audio_out',
            'cv', 'atom_in', 'atom_out', 'midi_in', 'midi_out', 'req_features', 'ext_data']
    rows = []
    for i in range(200):
        ai, ao = [(1, 1), (2, 2), (2, 2), (0, 2), (2, 1), (4, 2), (2, 2)][i % 7]
        rows.append(dict(uri=f'urn:fake:{i}', bucket='both' if i % 10 else 'modhost-only',
                         scope='instrument' if ai == 0 else 'effect', instrument=str(int(ai == 0)),
                         audio_in=str(ai), audio_out=str(ao), cv='3' if i % 7 == 6 and i % 2 else '0',
                         midi_in=str(int(ai == 0 or i % 9 == 0)),
                         **{'class': 'Reverb Plugin' if i % 13 == 0 else 'Plugin'},
                         ext_data='state#interface' if i % 5 == 0 else ('worker#interface' if i % 11 == 0 else ''),
                         req_features=''))
    with tempfile.TemporaryDirectory() as d:
        write_tsv(os.path.join(d, 'cov.tsv'), rows, cols)
        keep, skipped = plan(read_tsv(os.path.join(d, 'cov.tsv')), {})
        t('probe keeps only bucket=both', all(int(k['uri'].split(':')[-1]) % 10 for k in keep + skipped))
        t('probe skips more than two audio inputs under #92', any('4 audio inputs' in s['cause'] for s in skipped)
          and not any(k['audio_in'] > 2 for k in keep))
        t('probe skips CV ports under #92', any('CV ports' in s['cause'] for s in skipped) and not any(k['cv'] for k in keep))
        ch = stratify(keep, 30, 1)
        t('stratified sample: 30 distinct plugins', len(ch) == 30 and len({c['uri'] for c in ch}) == 30)
        strata = {c['stratum'] for c in ch}
        t('stratified sample: instruments, mono, stereo, state and long tails are all in it',
          {'instrument', 'mono', 'stereo', 'state', 'long tail'} <= strata)
        t('stratified sample: the same seed draws the same set', [c['uri'] for c in stratify(keep, 30, 1)] == [c['uri'] for c in ch])

        # classify and report on fake raw records
        out = os.path.join(d, 'out')
        os.makedirs(os.path.join(out, 'raw'))
        recs = [dict(rec_of(sig, sig, sig, sig), uri='urn:a', rate=48000, wall_s=1.0, cpu_ms={'mod': 2.0, 'omx': 2.2},
                     latency_jack={'effect_0': '0,0', 'effect_2': '0,0'}),
                dict(rec_of(sig, sig, q, q), uri='urn:b', rate=48000, wall_s=1.2, cpu_ms={'mod': 2.0, 'omx': 4.0},
                     latency_jack={'effect_0': '64,64', 'effect_2': '0,0'}),
                {'uri': 'urn:c', 'rate': 96000, 'status': 'crash', 'host': 'omx-clap-host', 'phase': 'run', 'wall_s': 2.0},
                dict(rec_of(sig, sig, late, late), uri='urn:a', rate=96000, wall_s=1.4)]
        with open(os.path.join(out, 'raw', '48000.jsonl'), 'w') as f:
            f.writelines(json.dumps(r) + '\n' for r in recs[:2])
        with open(os.path.join(out, 'raw', '96000.jsonl'), 'w') as f:
            f.writelines(json.dumps(r) + '\n' for r in recs[2:])
        write_tsv(os.path.join(d, 'cov2.tsv'), rows, cols)
        ns = argparse.Namespace(out=out, full_count=700, coverage=os.path.join(d, 'cov2.tsv'))
        with open(os.devnull, 'w') as dn:
            so, sys.stdout = sys.stdout, dn
            try:
                cmd_classify(ns)
                cmd_report(ns)
            finally:
                sys.stdout = so
        pr = read_tsv(os.path.join(out, 'parity.tsv'))
        t('classify writes one row per measurement', len(pr) == 4)
        t('classify rows carry class and cause', [r['class'] for r in pr] == ['IDENTICAL', 'DIFFERS', 'CRASH', 'DIFFERS'])
        sm = open(os.path.join(out, 'summary.md')).read()
        t('report counts per class and rate', '| IDENTICAL | 1 | 0 |' in sm and '| CRASH | 0 | 1 |' in sm and '| total | 2 | 2 |' in sm)
        t('report names the DIFFERS cause with its plugin', '| level | 1 | urn:b (48k) |' in sm)
        t('report lists a latency the hosts disagree on', '| urn:b | 48000 | 64,64 | 0,0 |' in sm)
        t('report flags omx-clap-host CPU above 1.5x', 'urn:b at 48000: 2.00' in sm)
        t('report gives the full-run estimate', 'Full run estimate: 700 plugins' in sm)
        cov = json.load(open(os.path.join(out, 'coverage.json')))
        t('the denominator: every row of the table counted by bucket, the mod-host-only ones named',
          cov['total'] == 200 and cov['buckets'] == {'both': 180, 'modhost-only': 20} and len(read_tsv(os.path.join(out, 'modhost-only.tsv'))) == 20)
        t('report states the denominator and the plugins mod-host hosts with no parity',
          f"Of the 200-row coverage table: 180 both hosts add" in sm and f"**{cov['no_parity']} of the 200 plugins mod-host hosts have no parity measurement**" in sm
          and '20 mod-host only' in sm)

    print(f"self-test: {oks} ok, {len(fails)} failed" + (f" ({SABOTAGE})" if SABOTAGE else ''))
    return 0 if not fails else 1


def self_test_with_sabotage():
    rc = self_test()
    if SABOTAGE:
        return rc
    red = 0
    sabotages = ['lag-blind', 'tolerance', 'nondet-blind', 'silent-pass', 'one-host-nondet', 'midi-blind']
    for s in sabotages:
        p = subprocess.run([sys.executable, os.path.abspath(__file__), '--self-test'], env=dict(os.environ, LV2_PARITY_SABOTAGE=s),
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
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
    p = sub.add_parser('probe')
    src = p.add_mutually_exclusive_group(required=True)
    src.add_argument('--coverage', help='a coverage table: uri, bucket (both = both hosts add it), the layout columns')
    src.add_argument('--uris', help='a file of URIs, or a comma-separated list, taken as added by both hosts')
    p.add_argument('--out', required=True)
    g = p.add_mutually_exclusive_group()
    g.add_argument('--sample', type=int, default=60)
    g.add_argument('--full', action='store_true')
    p.add_argument('--seed', type=int, default=20261020)
    p.add_argument('--no-lilv', action='store_true')
    p = sub.add_parser('run')
    p.add_argument('--out', required=True)
    p.add_argument('--rates', default='48000,96000')
    p.add_argument('--plugins')
    p.add_argument('--limit', type=int, default=0)
    p.add_argument('--seconds', type=float, default=3.0)
    p.add_argument('--warm-blocks', type=int, default=16)
    p.add_argument('--warm-seconds', type=float, default=0.3, help='the first warm, at least this much audio')
    p.add_argument('--hold-ms', type=int, default=400)
    p.add_argument('--add-timeout', type=float, default=30)
    p.add_argument('--warm-timeout', type=float, default=20)
    p.add_argument('--run-timeout', type=float, default=120)
    p.add_argument('--paced', action='store_true', help='play one quantum of wall time per cycle')
    p.add_argument('--keep', choices=['none', 'differs', 'all'], default='differs')
    p.add_argument('--min-free-gib', type=float, default=3.0)
    p.add_argument('--work')
    p.add_argument('--feeder', default=os.path.join(HERE, 'jack_parity'))
    p.add_argument('--omx-clap-host', default=os.path.join(ROOT, 'omx-clap-host'))
    p.add_argument('--mod-host', default='/usr/bin/mod-host')
    p.add_argument('--lv2-path', default='/usr/lib64/lv2')
    for name in ('classify', 'report'):
        p = sub.add_parser(name)
        p.add_argument('--out', required=True)
        p.add_argument('--full-count', type=int, default=0)
        p.add_argument('--coverage', help='the coverage table, for the denominator (else the out dir\'s coverage.json)')
    a = ap.parse_args()
    return {'probe': cmd_probe, 'run': cmd_run, 'classify': cmd_classify, 'report': cmd_report}[a.cmd](a)


if __name__ == '__main__':
    sys.exit(main())
