#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Command framing conformance of a mod-host protocol socket.

Every command a client sends ends with a NUL byte, and so does every answer. A conforming server answers each
command once, in order, whatever way the bytes arrive: several commands in one read, one command over many reads,
a command of exactly its buffer size. This script checks that against a running server, using one harmless
command whose answer it knows (a regular expression), and prints one line per case:

    ok   <case> <detail>
    FAIL <case> <detail>

Exit status: 0 when every case held, 1 when one failed, 2 when the server could not be reached.

The server must answer the command the same with trailing spaces added (mod-host's parser trims them): that is how
commands of an exact size are made. Run with python3 -I; the standard library only.

    socket_conformance.py --port 5555 [--command cpu_load] [--answer '^resp 0 '] [--limit 16777216]
                          [--fuzz 100000] [--seed N] [--pid PID] [--only case,case]
"""

import argparse
import os
import random
import re
import socket
import sys
import time

CASES = ["sizes", "nul-late", "many-per-write", "every-offset", "n-way", "empty", "partial-disconnect",
         "oversize", "sequential", "fuzz"]


class Conn:
    """one client connection: sends bytes, reads NUL-terminated answers"""

    def __init__(self, args):
        self.args = args
        self.s = socket.create_connection((args.host, args.port), timeout=5)
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buf = b""
        self.closed = False

    def send(self, data):
        try:
            self.s.sendall(data)
            return True
        except OSError:
            self.closed = True
            return False

    def send_pieces(self, pieces, gap):
        for i, p in enumerate(pieces):
            if i and gap:
                time.sleep(gap)
            if not self.send(p):
                return False
        return True

    def answers(self, want, wait=None):
        """up to `want` answers; stops early on timeout or when the server closes the connection"""
        wait = self.args.timeout if wait is None else wait
        end = time.time() + wait
        out = []
        while len(out) < want:
            nul = self.buf.find(b"\0")
            if nul >= 0:
                out.append(self.buf[:nul].decode("utf-8", "replace"))
                self.buf = self.buf[nul + 1:]
                continue
            left = end - time.time()
            if left <= 0 or self.closed:
                break
            self.s.settimeout(left)
            try:
                c = self.s.recv(65536)
            except socket.timeout:
                break
            except OSError:
                self.closed = True
                break
            if not c:
                self.closed = True
                break
            self.buf += c
            # acknowledge at once: a server that writes its answers one by one with Nagle on would otherwise wait for
            # our delayed ACK (40 ms) before each answer after the first
            if hasattr(socket, "TCP_QUICKACK"):
                try:
                    self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_QUICKACK, 1)
                except OSError:
                    pass
        return out

    def extra(self, wait=0.2):
        """answers nobody asked for, within `wait` seconds"""
        return self.answers(1 << 30, wait)

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


def padded(cmd, size):
    """the command, padded with trailing spaces to `size` bytes NUL included"""
    base = cmd.encode()
    assert size >= len(base) + 1
    return base + b" " * (size - len(base) - 1) + b"\0"


def norm(answer):
    # a float in an answer (a cpu load, a level) may differ between two runs of the same command
    return re.sub(r"-?\d+\.\d+(e[-+]?\d+)?", "F", answer)


class Report:
    def __init__(self):
        self.failed = []

    def check(self, case, ok, detail):
        print("%s %s %s" % ("ok  " if ok else "FAIL", case, detail), flush=True)
        if not ok:
            self.failed.append(case)


def one_command(args, payload_pieces, gap=0.002, want=1):
    """sends the pieces on a fresh connection; returns (answers, extra answers, closed)"""
    c = Conn(args)
    c.send_pieces(payload_pieces, gap)
    got = c.answers(want)
    more = c.extra(0.02) if len(got) == want and not c.closed else []
    closed = c.closed
    c.close()
    return got, more, closed


def good(args, got, want=1):
    return len(got) == want and all(re.search(args.answer, a) for a in got)


def case_sizes(args, r):
    cmd = args.command
    for size in (len(cmd) + 1, 1023, 1024, 1025, 2047, 2048, 2049, 3071, 3072, 3073, 4096, 70000):
        got, more, _ = one_command(args, [padded(cmd, size)])
        r.check("sizes", good(args, got) and not more,
                "command of %d bytes: %d answer(s) %r%s" % (size, len(got), got[:1], " plus %r" % more if more else ""))


def case_nul_late(args, r):
    # 1024 bytes with no terminator, then the terminator in a write of its own
    for size in (1024, 2048):
        p = padded(args.command, size + 1)
        got, more, _ = one_command(args, [p[:-1], b"\0"], gap=0.2)
        r.check("nul-late", good(args, got) and not more,
                "%d bytes, then the NUL alone: %d answer(s) %r" % (size, len(got), got[:1]))


def case_many_per_write(args, r):
    for n in (2, 10, 100):
        c = Conn(args)
        c.send(b"".join(padded(args.command, len(args.command) + 1 + i % 3) for i in range(n)))
        got = c.answers(n)
        c.close()
        r.check("many-per-write", good(args, got, n), "%d commands in one write: %d answer(s)" % (n, len(got)))


def case_every_offset(args, r):
    p = padded(args.command, 1500)
    bad = []
    for off in range(1, len(p)):
        got, more, _ = one_command(args, [p[:off], p[off:]])
        if not good(args, got) or more:
            bad.append((off, got[:2] + more[:1]))
    r.check("every-offset", not bad, "1500-byte command split in two at each of %d offsets: %d wrong%s"
            % (len(p) - 1, len(bad), ", first at offset %d: %r" % bad[0] if bad else ""))


def case_n_way(args, r):
    p = padded(args.command, 1500)
    bad = []
    for n in range(2, 65):
        cuts = sorted(random.Random(n).sample(range(1, len(p)), n - 1))
        pieces = [p[a:b] for a, b in zip([0] + cuts, cuts + [len(p)])]
        got, more, _ = one_command(args, pieces, gap=0.001)
        if not good(args, got) or more:
            bad.append((n, got[:2] + more[:1]))
    r.check("n-way", not bad, "1500-byte command in 2..64 writes: %d wrong%s"
            % (len(bad), ", first in %d writes: %r" % bad[0] if bad else ""))


def case_empty(args, r):
    # an empty command gets whatever it gets alone (mod-host: "not found"), and the command after it its own answer
    alone, more, closed = one_command(args, [b"\0"], gap=0)
    if closed or len(alone) > 1:
        r.check("empty", False, "an empty command alone: %d answer(s) %r, connection %s"
                % (len(alone), alone, "closed" if closed else "open"))
        return
    want = 2 * len(alone) + 1
    got, more, _ = one_command(args, [b"\0\0" + padded(args.command, len(args.command) + 1)], want=want)
    ok = len(got) == want and got[:-1] == alone * 2 and re.search(args.answer, got[-1]) is not None and not more
    r.check("empty", ok, "two empty commands (alone: %r), then one, in one write: %r" % (alone, got))


def case_partial_disconnect(args, r):
    sizes = (10, 1023, 1024, 1025, 2048, 5000)
    for i in range(args.connections // 5):
        c = Conn(args)
        size = sizes[i % len(sizes)]
        c.send(padded(args.command, max(size, len(args.command)) + 1)[:size])
        c.close()
    got, more, _ = one_command(args, [padded(args.command, len(args.command) + 1)])
    r.check("partial-disconnect", good(args, got),
            "%d clients left in the middle of a command, the next one: %d answer(s)" % (args.connections // 5, len(got)))


def case_oversize(args, r):
    if not args.limit:
        print("skip oversize: --limit 0")
        return
    got, more, closed = one_command(args, [padded(args.command, args.limit)], gap=0, want=1)
    r.check("oversize", good(args, got), "command of exactly %d bytes (the limit): %d answer(s)" % (args.limit, len(got)))
    got, more, closed = one_command(args, [padded(args.command, args.limit + 1)], gap=0, want=1)
    r.check("oversize", not got and closed,
            "command of %d bytes: %d answer(s), connection %s" % (args.limit + 1, len(got), "closed" if closed else "open"))
    got, more, _ = one_command(args, [padded(args.command, len(args.command) + 1)])
    r.check("oversize", good(args, got), "the next client: %d answer(s)" % len(got))


def rss_kb(pid):
    try:
        with open("/proc/%d/status" % pid) as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        return None


def case_sequential(args, r):
    before = rss_kb(args.pid) if args.pid else None
    bad = 0
    for i in range(args.connections):
        size = (len(args.command) + 1, 1024, 3000)[i % 3]
        got, more, _ = one_command(args, [padded(args.command, size)], want=1)
        bad += not good(args, got)
    after = rss_kb(args.pid) if args.pid else None
    grew = "" if before is None or after is None else ", server RSS %d -> %d kB" % (before, after)
    r.check("sequential", bad == 0, "%d connections one after another: %d without their answer%s"
            % (args.connections, bad, grew))


def case_fuzz(args, r):
    if not args.fuzz:
        return
    seed = args.seed if args.seed is not None else int.from_bytes(os.urandom(4), "little")
    rnd = random.Random(seed)
    print("fuzz seed %d (repeat with --seed %d)" % (seed, seed), flush=True)
    bases = [args.command, "conformance_unknown_verb", args.command + " conformance_extra_argument"]
    pads = [0, 1, 7, 500, 1000, 2040, 3000] + list(range(1010, 1026)) + list(range(2042, 2050))
    ref = {}

    def reference(base, pad):
        """the answer to the command sent alone, in one write, on a connection of its own"""
        key = (base, pad)
        if key not in ref:
            payload = padded(base, len(base) + 1 + pad)
            got, more, _ = one_command(args, [payload], gap=0)
            ref[key] = (payload, norm(got[0]) if len(got) == 1 and not more else None)
        return ref[key]

    # every reference first: a server that serves one client at a time would not answer them beside the fuzz connection
    for base in bases:
        for pad in pads:
            reference(base, pad)

    bad = 0
    examples = []
    c = Conn(args)
    for it in range(args.fuzz):
        if it and it % 20000 == 0:
            print("fuzz: %d iterations, %d wrong" % (it, bad), flush=True)
        stream = []
        for _ in range(rnd.randint(1, 12)):
            payload, answer = reference(rnd.choice(bases), rnd.choice(pads))
            if answer is not None:
                stream.append((payload, answer))
        if not stream:
            continue
        data = b"".join(p for p, _ in stream)
        ncuts = rnd.randint(0, min(8, len(data) - 1))
        cuts = sorted(rnd.sample(range(1, len(data)), ncuts))
        pieces = [data[a:b] for a, b in zip([0] + cuts, cuts + [len(data)])]
        c.send_pieces(pieces, rnd.choice((0, 0, 0, 0.0005)))
        got = [norm(a) for a in c.answers(len(stream))]
        want = [a for _, a in stream]
        if got != want:
            bad += 1
            if len(examples) < 3:
                examples.append("iteration %d: %d command(s) in %d write(s), sizes %s: expected %r, got %r"
                                % (it, len(stream), len(pieces), [len(p) for p, _ in stream], want[:3], got[:3]))
            c.close()
            c = Conn(args)
            if bad >= 50:
                print("fuzz: stopping after 50 wrong iterations")
                break
    c.close()
    dropped = sum(1 for v in ref.values() if v[1] is None)
    r.check("fuzz", bad == 0 and dropped == 0,
            "%d iterations of random chunking against each command alone (seed %d): %d wrong%s%s"
            % (it + 1, seed, bad,
               "; %d command size(s) got no answer even alone" % dropped if dropped else "",
               "".join("\n     " + e for e in examples)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--command", default="cpu_load", help="a harmless command with a known answer")
    ap.add_argument("--answer", default=r"^resp 0( |$)", help="regular expression its answer matches")
    ap.add_argument("--limit", type=int, default=16 * 1024 * 1024,
                    help="longest command the server accepts, NUL included (0: skip the oversize case)")
    ap.add_argument("--fuzz", type=int, default=100000, help="random chunking iterations (0: skip)")
    ap.add_argument("--seed", type=int)
    ap.add_argument("--connections", type=int, default=1000)
    ap.add_argument("--timeout", type=float, default=2.0, help="seconds to wait for an answer")
    ap.add_argument("--pid", type=int, help="the server's pid, to report its RSS around the sequential case")
    ap.add_argument("--only", help="comma-separated cases: " + ",".join(CASES))
    args = ap.parse_args()

    try:
        Conn(args).close()
    except OSError as e:
        print("cannot connect to %s:%d: %s" % (args.host, args.port, e))
        return 2

    only = args.only.split(",") if args.only else CASES
    r = Report()
    for name in CASES:
        if name in only:
            try:
                globals()["case_" + name.replace("-", "_")](args, r)
            except OSError as e:
                r.check(name, False, "the server stopped answering connections: %s" % e)
                break
    if r.failed:
        print("%d check(s) failed: %s" % (len(r.failed), " ".join(sorted(set(r.failed)))))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
