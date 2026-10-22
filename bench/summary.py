#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarise a bench results directory as Markdown tables: each figure the median of the repetitions, with the
spread (min..max over the repetitions) beside it.

  summary.py <results dir>
"""
import csv
import statistics
import sys
from collections import defaultdict

PATHS = ["modhost", "clap", "adapter", "inproc-clap", "inproc-lv2", "hostd", "jalv", "clap-series", "chain-clap",
         "adapter-series", "chain-adapter"]
# the chain's arms (#302): one client per plugin in series, and the same plugins in one client
CHAIN_PAIRS = [("clap-series", "chain-clap"), ("adapter-series", "chain-adapter")]
HOLDS = ["default", "off", "1000", "10", "0"]


def pidx(path):
    return PATHS.index(path) if path in PATHS else len(PATHS)


def hidx(hold):
    return HOLDS.index(hold) if hold in HOLDS else len(HOLDS)


def num(x):
    try:
        v = float(x)
        return None if v != v else v
    except (TypeError, ValueError):
        return None


def med(xs):
    xs = [x for x in xs if x is not None]
    return statistics.median(xs) if xs else None


def spread(xs, fmt="%.1f"):
    xs = [x for x in xs if x is not None]
    if not xs:
        return "NA"
    m = statistics.median(xs)
    if len(xs) == 1 or min(xs) == max(xs):
        return fmt % m
    return (fmt + " (" + fmt + "..." + fmt + ")") % (m, min(xs), max(xs))


def load(path):
    with open(path) as f:
        return list(csv.DictReader(f))


def idle_hold_table(runs, power):
    """The idle-state arm (#105, #296): per rate, quantum and path, one column group per hold the runs carry; the
    device's own answer and the whole run's mean package power beside the cycle end."""
    holds = sorted({r.get("idle_hold", "default") for r in runs}, key=hidx)
    if len(holds) < 2:
        return
    print("### The CPU idle hold: the whole graph per hold\n")
    print("Hold = what /dev/cpu_dma_latency was held at during the run (default: the library's own 10 us; off: "
          "nothing, the machine's resting policy); device = its own answer with every plugin active, the kernel's "
          "limit across every requester; RSS = the host's processes with every plugin loaded; watts = the mean "
          "package power over the run's measured window where an energy counter was readable. Cycle end as above. "
          "Median of the repetitions (min...max).\n")
    print("| rate | quantum | path | hold | device us | cycle end p50 us | cycle end p99 us | overruns | late | "
          "host CPU % | RSS loaded MiB | watts |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|")
    groups = defaultdict(list)
    for r in runs:
        groups[(int(r["rate"]), int(r["quantum"]), r["path"], r.get("idle_hold", "default"))].append(r)
    pg = defaultdict(list)
    for p in power:
        pg[(int(p["rate"]), int(p["quantum"]), p["path"], p.get("idle_hold", "default"))].append(p)
    for key in sorted(groups, key=lambda k: (k[0], k[1], pidx(k[2]), hidx(k[3]))):
        g = groups[key]
        watts = spread([num(p["watts"]) for p in pg.get(key, [])], "%.1f") if pg.get(key) else "-"
        print("| %d | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            key[0], key[1], key[2], key[3],
            "/".join(sorted({r.get("cpu_latency_us", "-") for r in g})),
            spread([num(r["cycle_end_p50_us"]) for r in g]),
            spread([num(r["cycle_end_p99_us"]) for r in g]),
            spread([num(r["overruns"]) for r in g], "%.0f"),
            spread([num(r["probe_late"]) for r in g], "%.0f"),
            spread([num(r["host_cpu_pct"]) for r in g]),
            spread([num(r["rss_loaded_kb"]) / 1024 for r in g if num(r["rss_loaded_kb"]) is not None]),
            watts))
    print()


def chain_table(runs, plugins):
    """The chain against one client per plugin in series (#302), per rate, quantum and hold: the medians of the
    repetitions side by side, the change of the cycle end, and whether p99, overruns and xruns got worse."""
    pairs = [(a, b) for (a, b) in CHAIN_PAIRS if any(r["path"] == b for r in runs)]
    if not pairs:
        return
    print("### The chain: one client per plugin in series against one client for all of them\n")
    print("Series / chain, the median of the repetitions; change = (chain - series) / series. Worse = the chain's "
          "median cycle end p99, overruns, probe late cycles or xruns above the series'. sched p50 = a node's wake-up to "
          "its start, the median over the arm's nodes (the chain has one, its head) and repetitions.\n")
    print("| rate | quantum | hold | arms | plugins | cycle end p50 us | change | cycle end p99 us | change | "
          "cycle end max us | overruns | late | xruns (probe/driver) | sched p50 us | host CPU % | RSS MiB | pad latency | "
          "worse |")
    print("|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|")
    g = defaultdict(list)
    for r in runs:
        g[(int(r["rate"]), int(r["quantum"]), r.get("idle_hold", "default"), r["path"])].append(r)

    pg = defaultdict(list)
    for p in plugins:
        pg[(int(p["rate"]), int(p["quantum"]), p.get("idle_hold", "default"), p["path"])].append(num(p["sched_p50_us"]))

    def m(rs, k):
        if k == "sched_p50_us":
            return med(pg.get(rs, []))
        return med([num(r[k]) for r in rs])

    def f(v, fmt="%.1f"):
        return "NA" if v is None else fmt % v

    def pct(a, b):
        return "NA" if a is None or b is None or a == 0 else "%+.1f%%" % (100 * (b - a) / a)

    keys = sorted({(k[0], k[1], k[2]) for k in g}, key=lambda k: (-k[0], k[1], hidx(k[2])))
    for a, b in pairs:
        for (rate, q, hold) in keys:
            sa, sb = g.get((rate, q, hold, a)), g.get((rate, q, hold, b))
            if not sa or not sb:
                continue
            worse = [k for k in ("cycle_end_p99_us", "overruns", "probe_late", "probe_xruns", "driver_xruns")
                     if (m(sb, k) or 0) > (m(sa, k) or 0)]
            print("| %d | %d | %s | %s / %s | %s | %s / %s | %s | %s / %s | %s | %s / %s | %s / %s | %s / %s | "
                  "%s/%s / %s/%s | %s / %s | %s / %s | %s / %s | %s / %s | %s |" % (
                      rate, q, hold, a, b, sa[0]["plugins"],
                      f(m(sa, "cycle_end_p50_us")), f(m(sb, "cycle_end_p50_us")),
                      pct(m(sa, "cycle_end_p50_us"), m(sb, "cycle_end_p50_us")),
                      f(m(sa, "cycle_end_p99_us")), f(m(sb, "cycle_end_p99_us")),
                      pct(m(sa, "cycle_end_p99_us"), m(sb, "cycle_end_p99_us")),
                      f(m(sa, "cycle_end_max_us"), "%.0f"), f(m(sb, "cycle_end_max_us"), "%.0f"),
                      f(m(sa, "overruns"), "%.0f"), f(m(sb, "overruns"), "%.0f"),
                      f(m(sa, "probe_late"), "%.0f"), f(m(sb, "probe_late"), "%.0f"),
                      f(m(sa, "probe_xruns"), "%.0f"), f(m(sa, "driver_xruns"), "%.0f"),
                      f(m(sb, "probe_xruns"), "%.0f"), f(m(sb, "driver_xruns"), "%.0f"),
                      f(m((rate, q, hold, a), "sched_p50_us")), f(m((rate, q, hold, b), "sched_p50_us")),
                      f(m(sa, "host_cpu_pct")), f(m(sb, "host_cpu_pct")),
                      f(m(sa, "rss_loaded_kb") and m(sa, "rss_loaded_kb") / 1024),
                      f(m(sb, "rss_loaded_kb") and m(sb, "rss_loaded_kb") / 1024),
                      f(m(sa, "pad_lat_max"), "%.0f"), f(m(sb, "pad_lat_max"), "%.0f"),
                      ", ".join(worse) or "no"))
    print()


def main(d):
    runs = load(d + "/runs.csv")
    plugins = load(d + "/plugins.csv")
    try:
        power = load(d + "/power.csv")
    except FileNotFoundError:
        power = []

    idle_hold_table(runs, power)
    chain_table(runs, plugins)

    print("### Per run: the whole graph\n")
    print("Cycle end = the driver's time from wake-up to the last node finished (pw-profiler), median of the run; "
          "overruns = cycles that ended past their period; late = probe cycles more than 1.5 periods after the one "
          "before; host CPU = the host process's CPU over the window, % of one core. Median of the repetitions "
          "(min...max).\n")
    print("| rate | quantum | path | plugins | cycle end p50 us | cycle end p99 us | overruns | late | host CPU % | "
          "RSS loaded MiB | pad latency frames |")
    print("|---|---|---|---|---|---|---|---|---|---|---|")
    groups = defaultdict(list)
    for r in runs:
        groups[(int(r["rate"]), int(r["quantum"]), r["path"])].append(r)
    for (rate, q, path) in sorted(groups, key=lambda k: (k[0], k[1], pidx(k[2]))):
        g = groups[(rate, q, path)]
        lat = sorted({(r["pad_lat_min"], r["pad_lat_max"]) for r in g})
        print("| %d | %d | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            rate, q, path, g[0]["plugins"],
            spread([num(r["cycle_end_p50_us"]) for r in g]),
            spread([num(r["cycle_end_p99_us"]) for r in g]),
            spread([num(r["overruns"]) for r in g], "%.0f"),
            spread([num(r["probe_late"]) for r in g], "%.0f"),
            spread([num(r["host_cpu_pct"]) for r in g]),
            spread([num(r["rss_loaded_kb"]) / 1024 for r in g]),
            ",".join("%s..%s" % l for l in lat)))

    print("\n### Per plugin: the node's own time per cycle (p50 / p99 / max, us), 48 kHz and 96 kHz at quantum 128\n")
    print("The node's duration (pw-profiler: active -> finished) is the plugin plus everything its host does in the "
          "node's cycle. For the in-process paths the inline column is omx_clap_host_run alone: what the plugin costs "
          "an engine that runs it in its own lane, with no node of its own. Median of the repetitions.\n")
    pg = defaultdict(list)
    for p in plugins:
        pg[(int(p["rate"]), int(p["quantum"]), p["plugin"], p["path"])].append(p)
    names = []
    for p in plugins:
        if p["plugin"] not in names:
            names.append(p["plugin"])
    for rate in (48000, 96000):
        print("| %d / 128 | modhost | clap | adapter | inproc-clap node | inproc-clap inline | inproc-lv2 node | "
              "inproc-lv2 inline |" % rate)
        print("|---|---|---|---|---|---|---|---|")
        for n in names:
            cells = []
            for path in PATHS:
                g = pg.get((rate, 128, n, path))
                # a chain member is no node of its own: no node time
                if g and med([num(x["dur_p50_us"]) for x in g]) is None:
                    g = None
                if not g:
                    cells.append("-")
                    if path.startswith("inproc"):
                        cells.append("-")
                    continue
                cells.append("%.1f / %.1f / %.0f" % (med([num(x["dur_p50_us"]) for x in g]),
                                                     med([num(x["dur_p99_us"]) for x in g]),
                                                     med([num(x["dur_max_us"]) for x in g])))
                if path.startswith("inproc"):
                    cells.append("%.1f / %.1f / %.0f" % (med([num(x["run_p50_us"]) for x in g]),
                                                         med([num(x["run_p99_us"]) for x in g]),
                                                         med([num(x["run_max_us"]) for x in g])))
            print("| %s | %s |" % (n, " | ".join(cells)))
        print()

    print("### Per path, all plugins, all configurations: median node time p50 (us), and start-up\n")
    print("| path | node p50 us (median over plugins and configs) | inline p50 us | add ms (median) | add -> first sound "
          "ms (median) | host ready ms (median) | RSS empty MiB | RSS per plugin MiB |")
    print("|---|---|---|---|---|---|---|---|")
    for path in PATHS:
        ps = [p for p in plugins if p["path"] == path]
        rs = [r for r in runs if r["path"] == path]
        if not ps:
            continue
        # a path with no host before its first add (jalv) rests at 0 RSS: a real figure, not an absent one
        per_plugin = [(num(r["rss_loaded_kb"]) - num(r["rss_empty_kb"])) / 1024 / (int(r["plugins"]) + 1)
                      for r in rs if num(r["rss_loaded_kb"]) is not None and num(r["rss_empty_kb"]) is not None]
        inline = med([num(p["run_p50_us"]) for p in ps])
        print("| %s | %s | %s | %s | %s | %s | %s | %s |" % (
            path, spread([num(p["dur_p50_us"]) for p in ps]), "%.1f" % inline if inline is not None else "-",
            spread([num(p["add_ms"]) for p in ps]), spread([num(p["first_audio_ms"]) for p in ps]),
            spread([num(r["host_ready_ms"]) for r in rs]), spread([num(r["rss_empty_kb"]) / 1024 for r in rs]),
            spread(per_plugin, "%.2f")))

    try:
        crash = load(d + "/crash.csv")
    except FileNotFoundError:
        return
    print("\n### Crash drill (48 kHz / 128), median of the repetitions (min...max)\n")
    print("| path | event | host died | exit ms | graph alive | probe max gap us | late cycles | victim silent ms | "
          "neighbour silent ms | respawn -> sound ms |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    cg = defaultdict(list)
    for c in crash:
        if c.get("event") in ("kill", "crash"):
            cg[(c["path"], c["event"])].append(c)
    for (path, ev) in sorted(cg, key=lambda k: (pidx(k[0]), k[1] != "kill")):
        g = cg[(path, ev)]
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            path, ev, "/".join(sorted({c["host_died"] for c in g})),
            spread([num(c["host_exit_ms"]) for c in g]),
            "/".join(sorted({c["pipewire_alive"] for c in g})),
            spread([num(c["probe_max_gap_us"]) for c in g], "%.0f"),
            spread([num(c["probe_late"]) for c in g], "%.0f"),
            spread([num(c["victim_silent_ms"]) for c in g]),
            spread([num(c["neighbour_silent_ms"]) for c in g]),
            spread([num(c["respawn_to_sound_ms"]) for c in g])))


if __name__ == "__main__":
    main(sys.argv[1])
