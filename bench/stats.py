#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The bench's readers, run with -I beside the private graph.

  stats.py ports <node name>          the node's audio ports, inputs then outputs, as "in <name>" / "out <name>" lines
  stats.py nodes                      "<id> <node name>" for every node
  stats.py profile <prof.log> <id>... per node id: cycles, duration (active -> finished) p50/p99/max us, scheduling
                                      latency (scheduled -> active) p50/p99/max us, the change in the node's xrun
                                      counter; and the driver's cycle end (total cycle time) p50/p99/max us and the
                                      cycles that ended past their period (overruns)
  stats.py top <pw-top -b output>     "<id> <name> <err>" from the last block pw-top printed
"""
import json
import subprocess
import sys


def dump():
    return json.loads(subprocess.run(["pw-dump"], check=True, capture_output=True, text=True).stdout)


def ports(name):
    objs = dump()
    node_ids = {o["id"] for o in objs if o.get("type") == "PipeWire:Interface:Node"
                and o.get("info", {}).get("props", {}).get("node.name") == name}
    rows = []
    for o in objs:
        if o.get("type") != "PipeWire:Interface:Port":
            continue
        p = o.get("info", {}).get("props", {})
        if p.get("node.id") not in node_ids or "audio" not in p.get("format.dsp", ""):
            continue
        rows.append((p.get("port.direction"), int(p.get("port.id", 0)), p.get("port.name")))
    for direction in ("in", "out"):
        for r in sorted(x for x in rows if x[0] == direction):
            # what pw-link names the port: the node's name and the port's
            print(direction, name + ":" + r[2])


def nodes():
    for o in dump():
        if o.get("type") == "PipeWire:Interface:Node":
            print(o["id"], o.get("info", {}).get("props", {}).get("node.name", "?"))


def pct(xs, q):
    if not xs:
        return float("nan")
    xs = sorted(xs)
    k = min(len(xs) - 1, max(0, int(round(q * (len(xs) - 1)))))
    return xs[k]


def profile(path, ids):
    want = {int(i) for i in ids}
    dur = {i: [] for i in want}
    sched = {i: [] for i in want}
    xr = {i: [] for i in want}
    end = []
    over = 0
    with open(path) as f:
        for line in f:
            fs = line.rstrip("\n").split("\t")
            if len(fs) < 4:
                continue
            try:
                end.append(float(fs[1]))
                # the cycle's end past its period: the graph did not finish in time, an xrun
                over += float(fs[1]) > float(fs[3]) > 0
            except ValueError:
                continue
            i = 4
            while i + 8 <= len(fs) and fs[i].strip():
                try:
                    nid = int(fs[i])
                except ValueError:
                    break
                if nid in want and fs[i + 6].strip() == "3":  # status 3: the node finished this cycle
                    dur[nid].append(float(fs[i + 5]))
                    sched[nid].append(float(fs[i + 4]))
                    xr[nid].append(int(fs[i + 7]))
                i += 8
    for nid in sorted(want):
        d, s, x = dur[nid], sched[nid], xr[nid]
        print("node %d cycles=%d dur_p50=%.2f dur_p99=%.2f dur_max=%.2f sched_p50=%.2f sched_p99=%.2f sched_max=%.2f "
              "xrun_delta=%d" % (nid, len(d), pct(d, .5), pct(d, .99), max(d) if d else float("nan"),
                                 pct(s, .5), pct(s, .99), max(s) if s else float("nan"),
                                 (x[-1] - x[0]) if x else -1))
    print("driver cycles=%d end_p50=%.2f end_p99=%.2f end_max=%.2f overruns=%d" %
          (len(end), pct(end, .5), pct(end, .99), max(end) if end else float("nan"), over))


def top(path):
    block = []
    with open(path) as f:
        for line in f:
            if line.startswith("S   ID"):
                block = []
                continue
            fs = line.split()
            if len(fs) >= 9 and fs[1].isdigit():
                block.append((fs[1], fs[-1], fs[8] if fs[0] in "RSICF" else "0"))
    for nid, name, err in block:
        print(nid, name, err)


if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "ports":
        ports(sys.argv[2])
    elif cmd == "nodes":
        nodes()
    elif cmd == "profile":
        profile(sys.argv[2], sys.argv[3:])
    elif cmd == "top":
        top(sys.argv[2])
