#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Self-test of tests/socket_conformance.py's readiness wait (#479), against stand-in daemons.

  slow     answers nothing for 5 s after start, then conforms: must pass (the 2 s bound is not paid for start-up)
  dropper  conforms, but closes the first client that connects after one left in mid-command: must fail partial-disconnect
  dead     accepts and never answers: must exit 2 naming the socket, after --ready-timeout

Run with python3 -I; the standard library only."""

import os
import socket
import subprocess
import sys
import threading
import time

here = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(here, "socket_conformance.py")


class StandIn:
    def __init__(self, mode):
        self.mode = mode
        self.ready_at = time.time() + (5 if mode == "slow" else 0)
        self.poisoned = False
        self.lock = threading.Lock()
        self.srv = socket.socket()
        self.srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.srv.bind(("127.0.0.1", 0))
        self.srv.listen(256)
        self.port = self.srv.getsockname()[1]
        threading.Thread(target=self.accept, daemon=True).start()

    def accept(self):
        while True:
            try:
                c, _ = self.srv.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()

    def serve(self, c):
        with self.lock:
            drop = self.mode == "dropper" and self.poisoned
            if drop:
                self.poisoned = False
        if drop:
            c.close()
            return
        buf = b""
        try:
            while True:
                d = c.recv(65536)
                if not d:
                    break
                buf += d
                while b"\0" in buf:
                    _, buf = buf.split(b"\0", 1)
                    if self.mode == "dead":
                        continue
                    left = self.ready_at - time.time()
                    if left > 0:
                        time.sleep(left)
                    c.sendall(b"resp 0\0")
        except OSError:
            pass
        if buf.strip(b" ") and self.mode == "dropper":
            self.poisoned = True
        c.close()


def run(mode, *extra):
    s = StandIn(mode)
    t = time.time()
    p = subprocess.run([sys.executable, "-I", SCRIPT, "--port", str(s.port), "--fuzz", "0", "--limit", "0",
                        "--only", "sizes,partial-disconnect"] + list(extra), capture_output=True, text=True)
    s.srv.close()
    return p.returncode, p.stdout, time.time() - t


def expect(name, ok, detail):
    print("%s %s %s" % ("ok  " if ok else "FAIL", name, detail), flush=True)
    return ok


def main():
    good = True
    rc, out, dt = run("slow")
    good &= expect("slow-start", rc == 0 and dt >= 5, "5 s to the first answer, 2 s bound: exit %d after %.1f s\n%s" % (rc, dt, out if rc else ""))
    rc, out, _ = run("dropper")
    good &= expect("drops-next-client", rc == 1 and "FAIL partial-disconnect" in out, "exit %d" % rc)
    rc, out, dt = run("dead", "--ready-timeout", "1")
    good &= expect("never-answers", rc == 2 and "127.0.0.1:" in out and dt < 5, "exit %d after %.1f s: %s" % (rc, dt, out.strip()))
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
