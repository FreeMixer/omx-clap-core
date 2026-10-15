# Hosting paths bench, 2026-10-15

The first measurement for #82: what it costs to host the same plugins through mod-host, omx-clap-host, omx-clap-host
with libomx-clap-lv2, and in-process the way the engine hosts them. The raw rows are in
[bench-hosting-paths-2026-10-15/](bench-hosting-paths-2026-10-15/): `runs.csv` (one row per run), `plugins.csv` (one
row per plugin per run), `crash.csv`, `refusals.csv` and `machine.txt`.

## How to run it

```
make bench                                   # every path, 48 and 96 kHz, quantum 64/128/256/512, 3 x 60 s, then the crash drill
make bench BENCH_RATES=48000 BENCH_QUANTA=128 BENCH_REPS=1 BENCH_SECONDS=10   # a quick look
python3 -I bench/summary.py build/bench/results                               # the tables below
```

Each run is a PipeWire of its own in a private user, network, pid and mount namespace (the scaffolding of
`tests/clap_lv2_identity.sh`), driven by PipeWire's dummy driver; nothing touches a session graph. It needs `unshare`
to work (a privileged rootless podman container does, `podman run --privileged`), mod-host, pipewire, pipewire-utils,
the JACK headers (pipewire-jack-audio-connection-kit-devel), libpipewire headers, lilv, the CLAP headers, and the
plugins named in `bench/plugins.tsv`. This run used `localhost/openmixer-worker:big-2026-10-15` plus
pipewire-jack-audio-connection-kit-devel, on r1.

## The paths

| path | what runs the plugin |
|---|---|
| `modhost` | mod-host 0.10.6 (distro package), the LV2 build, one JACK client per plugin |
| `clap` | omx-clap-host built from this tree, the CLAP build, one JACK client per plugin |
| `adapter` | the same omx-clap-host, the LV2 build through libomx-clap-lv2 |
| `inproc-clap` | the CLAP build in a native PipeWire filter's own process callback through libomx-clap-core |
| `inproc-lv2` | the LV2 build the same way, opened through libomx-clap-lv2 |

**Path 3 is a stand-in.** omx-clap-host cannot load an LV2 plugin today (#96: it only takes `clap:<file>#<id>`, and
nothing in it calls libomx-clap-lv2). The bench gives it `build/bench/omx-lv2-bridge.clap`, a bench-only `.clap`
(`bench/lv2_bridge.c`) that links libomx-clap-lv2 and hands the LV2 plugins of the bundles in `OMX_BENCH_LV2_BUNDLES`
over as CLAP plugins. So `adapter` measures the adapter's code inside an unmodified omx-clap-host process, loaded
through a `.clap` instead of linked in; it is what path 3 would cost, not a measurement of a path that exists. The
first `add` also pays for the bridge loading lilv and its bundles (7 to 18 ms here with a warm page cache).

**Path 4 is not the engine.** `bench/inproc.c` makes the calls the engine's in-process tier makes
(`omx_clap_host_open` / `_open_entry`, `_activate`, `_publish`, `omx_clap_host_run` on the PipeWire data thread, the
core's default configuration: clamp, non-finite scan and warm-up on), but it gives each plugin a PipeWire filter node
of its own, where the engine runs a lane's plugins inline in its own node. Two figures are reported for it: the node's
time, comparable with the other paths' nodes, and the inline time, `omx_clap_host_run` alone, which is what a plugin
adds to an engine cycle.

## The plugins

From `bench/plugins.tsv`: omx-plugins 0.7.0 (comp, delay, deesser, eq8, reverb) in both builds; lsp Compressor Stereo
1.2.29 and Dragonfly Room Reverb 3.2.10 as CLAP; ZamCompX2, caps PlateX2 and caps CompressX2 as LV2. The LV2 builds of
lsp, Dragonfly and x42 fil4 were the first choice and are left out because libomx-clap-lv2 refuses all three
(`hosting.features.midi-in-fed-empty`, an atom port that is neither MIDI nor patch-only; reported on #84); calf's
Compressor is left out because it carries a MIDI input, which the core's default configuration (the engine's) refuses
(`hosting.clap.note-input`). mod-host hosts all of them. So the CLAP paths run 7 plugins, the LV2 paths 8, and the
cross-format comparison is the five omx-plugins twins.

Every plugin of a path is racked in ONE host (one process), each fed the same stereo white noise at -20 dBFS and
recorded; one more instance, a pad (`tests/fault-0.clap`, or the stereo pad of the adapter's fake bundle), carries a
unit impulse four times a second; a direct link carries the same impulse as the reference.

## What is measured

- **Node time per cycle**: pw-profiler's "active -> finished" for each plugin's node, every cycle of the 60 s window,
  p50 / p99 / max. It is the plugin plus what its host does around it in the node's cycle (port buffers, events, the
  core's stage).
- **Inline time** (in-process only): `omx_clap_host_run` timed with CLOCK_MONOTONIC on the data thread.
- **Cycle end**: the driver's time from wake-up to the last node finished; with every plugin on its own node and its
  own data thread this is mostly wake-up chains, so it is reported per path, not per plugin.
- **Xruns**: pw-top's ERR column for the driver counted one in 120 runs and the probe's JACK xrun callback none, so
  both are kept in the CSV but not used; **overruns**, the
  profiler's cycles whose end passed their period; **late**, the probe's cycles that came more than 1.5 periods after
  the one before (a missed cycle as a client hears it).
- **Added latency**: the impulse through the pad against the direct link, in frames.
- **Memory**: VmRSS of the host process with no plugin and with every plugin racked.
- **Start-up**: host spawn to ready; `add` to its answer; `add` to the first non-silent block at the recorder (this
  includes the three `pw-link` calls, about 60 ms, the same for every path).
- **Crash drill** (`bench/crash.sh`, 48 kHz / 128, three times per path): the host SIGKILLed while it runs omx-comp
  (for in-process the host process is the engine), then respawned; and, on the CLAP paths, `tests/fault-14.clap` made
  to abort() beside a healthy omx-comp in the same host.

## Machine

r1: Intel Core Ultra 9 285K, 24 threads, Fedora 44 kernel 7.2.9, cpufreq governor `powersave` (intel_pstate),
PipeWire 1.6.9, in a rootless privileged podman container. The data threads ran SCHED_FIFO 55 (rtprio limit 70 in the
container). r1 is a shared build machine: its load average was 2.8 to 3.3 through the run (k3s, CI, other agents), and
nothing was pinned or isolated. omx-clap-host and the bench tools were built from this branch (main a06b7bf plus the
bench).

## Results

Generated by `python3 -I bench/summary.py` from the CSVs beside this file. Every figure is the median of three
60-second runs, the spread across the three in brackets. The CLAP paths and the LV2 paths run different sets (7 and 8
plugins, Dragonfly Room's CLAP the heaviest of all), so a whole-graph figure compares paths of the same format only:
`modhost`, `adapter` and `inproc-lv2` run the same eight; `clap` and `inproc-clap` the same seven.

### Per run: the whole graph

Cycle end = the driver's time from wake-up to the last node finished (pw-profiler), median of the run; overruns = cycles that ended past their period; late = probe cycles more than 1.5 periods after the one before; host CPU = the host process's CPU over the window, % of one core. Median of the repetitions (min...max).

| rate | quantum | path | plugins | cycle end p50 us | cycle end p99 us | overruns | late | host CPU % | RSS loaded MiB | pad latency frames |
|---|---|---|---|---|---|---|---|---|---|---|
| 48000 | 64 | modhost | 8 | 220.4 (188.8...232.2) | 610.9 (578.5...617.3) | 13 (13...55) | 13 (12...29) | 14.7 (11.2...15.9) | 40.1 (39.8...40.2) | 0..0 |
| 48000 | 64 | clap | 7 | 286.1 (229.9...296.3) | 754.7 (680.7...764.5) | 38 (30...45) | 28 (23...34) | 15.7 (12.1...16.6) | 40.3 (39.7...41.2) | 0..0 |
| 48000 | 64 | adapter | 8 | 279.6 (254.5...284.5) | 716.2 (715.9...752.1) | 13 (12...18) | 9 (6...10) | 15.3 (13.1...16.2) | 26.5 (26.4...26.5) | 0..0 |
| 48000 | 64 | inproc-clap | 7 | 288.5 (237.7...300.7) | 758.9 (704.8...767.5) | 18 (16...23) | 23 (16...31) | 16.5 (12.3...16.8) | 38.4 (38.4...38.5) | 0..0 |
| 48000 | 64 | inproc-lv2 | 8 | 278.5 (264.9...294.3) | 708.8 (684.7...745.4) | 19 (19...25) | 13 (9...15) | 16.1 (15.4...16.3) | 22.8 (22.7...22.8) | 0..0 |
| 48000 | 128 | modhost | 8 | 250.2 (181.5...286.9) | 719.4 (617.9...728.4) | 3 (3...3) | 2 (1...3) | 8.2 (6.6...9.0) | 40.0 (39.8...40.3) | 0..0 |
| 48000 | 128 | clap | 7 | 287.5 (268.5...287.9) | 759.9 (736.2...808.5) | 5 (4...5) | 3 (3...3) | 9.1 (8.7...9.2) | 40.1 (40.0...40.2) | 0..0 |
| 48000 | 128 | adapter | 8 | 293.3 (242.4...378.8) | 772.7 (718.6...775.9) | 3 (2...5) | 2 (1...2) | 10.0 (7.7...11.1) | 26.5 (26.4...26.7) | 0..0 |
| 48000 | 128 | inproc-clap | 7 | 337.5 (271.5...338.2) | 788.5 (702.0...846.1) | 2 (1...8) | 3 (1...5) | 8.5 (6.6...11.0) | 38.3 (38.3...38.3) | 0..0 |
| 48000 | 128 | inproc-lv2 | 8 | 265.5 (203.6...338.9) | 692.5 (461.7...829.1) | 4 (4...22) | 4 (3...12) | 7.9 (6.5...10.8) | 22.8 (22.7...22.8) | 0..0 |
| 48000 | 256 | modhost | 8 | 309.3 (244.7...388.6) | 820.9 (801.6...877.5) | 0 (0...1) | 0 (0...0) | 5.8 (4.9...6.9) | 40.1 (40.0...40.1) | 0..0 |
| 48000 | 256 | clap | 7 | 397.9 (337.3...405.4) | 897.0 (866.8...990.5) | 0 (0...1) | 1 (0...1) | 7.6 (5.7...7.7) | 40.0 (39.9...40.0) | 0..0 |
| 48000 | 256 | adapter | 8 | 338.2 (234.4...474.9) | 841.5 (704.5...1002.2) | 0 (0...0) | 0 (0...0) | 6.1 (4.3...10.2) | 26.5 (26.5...26.6) | 0..0 |
| 48000 | 256 | inproc-clap | 7 | 484.1 (364.3...677.9) | 937.7 (865.5...1247.0) | 0 (0...1) | 0 (0...4) | 9.1 (6.1...15.9) | 38.4 (38.3...38.5) | 0..0 |
| 48000 | 256 | inproc-lv2 | 8 | 436.7 (367.0...437.0) | 876.7 (864.3...903.6) | 0 (0...9) | 0 (0...5) | 9.3 (7.3...9.6) | 22.7 (22.6...23.0) | 0..0 |
| 48000 | 512 | modhost | 8 | 607.1 (497.4...619.2) | 1196.5 (1051.8...1254.7) | 0 (0...0) | 0 (0...0) | 6.9 (5.3...7.5) | 39.8 (39.8...40.3) | 0..0 |
| 48000 | 512 | clap | 7 | 903.0 (669.6...1024.8) | 2037.1 (1644.7...2425.5) | 0 (0...0) | 0 (0...0) | 11.2 (8.1...12.7) | 40.2 (40.1...40.2) | 0..0 |
| 48000 | 512 | adapter | 8 | 756.1 (502.0...781.9) | 1223.5 (944.8...1265.8) | 0 (0...0) | 0 (0...0) | 11.2 (7.5...11.5) | 26.6 (26.5...26.6) | 0..0 |
| 48000 | 512 | inproc-clap | 7 | 984.8 (647.6...991.1) | 2091.6 (1082.5...2492.1) | 0 (0...0) | 0 (0...0) | 12.9 (7.7...13.1) | 38.5 (38.3...38.7) | 0..0 |
| 48000 | 512 | inproc-lv2 | 8 | 635.4 (595.2...792.4) | 1292.6 (956.0...1385.4) | 0 (0...0) | 0 (0...0) | 9.2 (7.6...11.8) | 22.9 (22.7...22.9) | 0..0 |
| 96000 | 64 | modhost | 8 | 156.3 (134.2...196.7) | 391.7 (353.6...518.1) | 98 (63...216) | 137 (80...314) | 19.9 (18.4...28.4) | 40.2 (40.1...40.3) | 0..0 |
| 96000 | 64 | clap | 7 | 220.8 (185.6...238.0) | 645.8 (397.4...668.1) | 759 (173...1222) | 498 (126...627) | 27.5 (27.4...28.9) | 40.3 (40.2...40.4) | 0..0 |
| 96000 | 64 | adapter | 8 | 224.5 (187.6...243.8) | 589.7 (451.0...619.3) | 332 (86...510) | 355 (100...506) | 28.2 (26.4...30.1) | 26.8 (26.6...26.9) | 0..0 |
| 96000 | 64 | inproc-clap | 7 | 234.8 (202.0...253.8) | 636.3 (533.5...649.5) | 602 (189...766) | 674 (337...720) | 28.9 (24.8...29.0) | 38.6 (38.4...38.9) | 0..0 |
| 96000 | 64 | inproc-lv2 | 8 | 233.0 (206.9...241.5) | 588.2 (528.0...611.4) | 310 (114...463) | 420 (326...549) | 28.0 (23.7...30.0) | 23.2 (23.0...23.3) | 0..0 |
| 96000 | 128 | modhost | 8 | 275.4 (272.4...281.9) | 748.1 (731.6...760.5) | 20 (12...21) | 23 (15...24) | 23.7 (23.2...23.9) | 40.4 (40.3...40.4) | 0..0 |
| 96000 | 128 | clap | 7 | 447.0 (437.8...458.2) | 926.9 (925.4...928.0) | 230 (207...257) | 136 (129...144) | 32.4 (31.3...34.0) | 40.3 (40.2...40.4) | 0..0 |
| 96000 | 128 | adapter | 8 | 403.0 (386.2...416.0) | 844.6 (843.4...852.4) | 17 (14...18) | 11 (8...12) | 31.4 (28.6...32.3) | 26.7 (26.5...26.7) | 0..0 |
| 96000 | 128 | inproc-clap | 7 | 446.2 (422.3...485.3) | 915.4 (899.9...917.4) | 109 (98...114) | 139 (121...144) | 35.4 (30.5...36.2) | 38.8 (38.7...38.9) | 0..0 |
| 96000 | 128 | inproc-lv2 | 8 | 436.9 (429.2...437.3) | 863.4 (862.8...869.4) | 18 (16...20) | 19 (14...20) | 33.5 (33.0...35.5) | 22.9 (22.9...23.0) | 0..0 |
| 96000 | 256 | modhost | 8 | 454.9 (396.8...454.9) | 942.4 (912.9...965.7) | 7 (5...11) | 4 (2...7) | 18.4 (16.4...18.7) | 40.2 (40.1...40.5) | 0..0 |
| 96000 | 256 | clap | 7 | 591.8 (548.0...608.5) | 1059.8 (1037.9...1095.1) | 20 (11...22) | 78 (52...83) | 24.2 (22.7...27.5) | 40.5 (40.5...40.6) | 0..0 |
| 96000 | 256 | adapter | 8 | 563.8 (527.8...577.3) | 949.0 (947.5...955.9) | 5 (4...5) | 2 (2...2) | 26.1 (23.8...28.4) | 26.7 (26.7...26.7) | 0..0 |
| 96000 | 256 | inproc-clap | 7 | 617.4 (603.0...677.2) | 1097.5 (1091.3...1097.6) | 11 (4...14) | 70 (65...73) | 30.2 (27.4...32.2) | 38.8 (38.6...38.9) | 0..0 |
| 96000 | 256 | inproc-lv2 | 8 | 508.1 (501.3...593.2) | 970.7 (968.7...976.4) | 6 (5...7) | 2 (1...3) | 25.0 (23.9...31.6) | 22.9 (22.9...22.9) | 0..0 |
| 96000 | 512 | modhost | 8 | 538.2 (500.3...612.0) | 1111.1 (1087.6...1145.0) | 2 (2...3) | 0 (0...0) | 12.8 (12.1...14.3) | 40.2 (40.0...40.2) | 0..0 |
| 96000 | 512 | clap | 7 | 829.2 (760.5...829.8) | 1962.9 (1756.3...1972.0) | 1 (0...1) | 4 (3...4) | 19.6 (17.9...20.4) | 40.3 (40.3...40.4) | 0..0 |
| 96000 | 512 | adapter | 8 | 698.8 (697.0...711.1) | 1106.3 (1106.1...1117.8) | 0 (0...0) | 0 (0...0) | 20.1 (20.0...20.3) | 27.0 (26.8...27.1) | 0..0 |
| 96000 | 512 | inproc-clap | 7 | 844.1 (844.0...884.8) | 1867.3 (1780.5...2079.7) | 3 (0...5) | 5 (3...6) | 21.2 (20.8...23.1) | 38.8 (38.8...39.0) | 0..0 |
| 96000 | 512 | inproc-lv2 | 8 | 711.1 (698.5...730.0) | 1137.2 (1134.6...1137.6) | 1 (0...1) | 0 (0...0) | 21.4 (20.8...22.7) | 23.0 (23.0...23.2) | 0..0 |

### Per plugin: the node's own time per cycle (p50 / p99 / max, us), 48 kHz and 96 kHz at quantum 128

The node's duration (pw-profiler: active -> finished) is the plugin plus everything its host does in the node's cycle. For the in-process paths the inline column is omx_clap_host_run alone: what the plugin costs an engine that runs it in its own lane, with no node of its own. Median of the repetitions.

| 48000 / 128 | modhost | clap | adapter | inproc-clap node | inproc-clap inline | inproc-lv2 node | inproc-lv2 inline |
|---|---|---|---|---|---|---|---|
| omx-comp | 3.6 / 19.5 / 46 | 4.3 / 29.3 / 63 | 5.1 / 17.9 / 52 | 6.0 / 20.5 / 67 | 3.5 / 15.7 / 44 | 6.8 / 18.9 / 72 | 4.2 / 16.9 / 59 |
| omx-delay | 2.1 / 9.2 / 25 | 2.1 / 10.8 / 44 | 2.9 / 13.6 / 37 | 4.8 / 18.3 / 59 | 2.0 / 9.3 / 52 | 5.2 / 18.5 / 59 | 2.5 / 10.4 / 49 |
| omx-deesser | 4.5 / 22.9 / 53 | 4.5 / 25.9 / 54 | 5.1 / 29.5 / 59 | 6.9 / 29.3 / 69 | 4.5 / 22.6 / 46 | 7.4 / 26.9 / 74 | 5.0 / 20.9 / 51 |
| omx-eq8 | 2.1 / 10.1 / 23 | 1.9 / 8.8 / 34 | 2.6 / 12.1 / 38 | 4.4 / 16.7 / 41 | 1.8 / 8.1 / 24 | 4.9 / 18.2 / 46 | 2.2 / 9.9 / 23 |
| omx-reverb | 5.7 / 28.9 / 59 | 5.9 / 24.4 / 55 | 6.7 / 32.6 / 67 | 7.9 / 29.8 / 78 | 5.5 / 23.6 / 69 | 8.8 / 30.4 / 90 | 6.5 / 23.9 / 68 |
| zam-compx2 | 3.9 / 20.2 / 46 | - | 4.4 / 22.2 / 56 | - | - | 6.6 / 24.4 / 68 | 3.9 / 17.1 / 46 |
| caps-platex2 | 5.7 / 29.1 / 61 | - | 6.3 / 26.6 / 93 | - | - | 8.4 / 31.0 / 88 | 5.8 / 23.4 / 68 |
| caps-compressx2 | 9.8 / 57.3 / 118 | - | 10.3 / 58.0 / 119 | - | - | 12.9 / 62.3 / 138 | 9.9 / 54.8 / 115 |
| lsp-comp | - | 6.6 / 28.5 / 67 | - | 8.9 / 33.3 / 87 | 6.5 / 25.8 / 71 | - | - |
| dragonfly-room | - | 28.4 / 140.0 / 1462 | - | 30.2 / 135.7 / 1454 | 27.9 / 128.4 / 1448 | - | - |

| 96000 / 128 | modhost | clap | adapter | inproc-clap node | inproc-clap inline | inproc-lv2 node | inproc-lv2 inline |
|---|---|---|---|---|---|---|---|
| omx-comp | 8.5 / 24.6 / 55 | 8.6 / 29.9 / 79 | 6.4 / 27.2 / 59 | 11.0 / 42.9 / 142 | 8.3 / 32.1 / 127 | 9.6 / 40.2 / 80 | 7.7 / 31.1 / 64 |
| omx-delay | 3.9 / 11.5 / 47 | 4.3 / 11.5 / 46 | 5.0 / 13.9 / 39 | 9.4 / 22.6 / 68 | 3.8 / 10.3 / 40 | 10.3 / 24.7 / 80 | 4.8 / 12.4 / 53 |
| omx-deesser | 10.4 / 30.9 / 120 | 10.0 / 29.5 / 82 | 11.3 / 32.1 / 74 | 15.5 / 39.0 / 73 | 10.6 / 28.9 / 53 | 16.7 / 42.8 / 104 | 11.8 / 31.4 / 80 |
| omx-eq8 | 3.5 / 11.5 / 36 | 4.0 / 10.0 / 36 | 4.4 / 13.0 / 33 | 9.3 / 19.7 / 70 | 3.8 / 8.8 / 45 | 9.0 / 22.9 / 52 | 4.5 / 11.7 / 46 |
| omx-reverb | 11.8 / 33.5 / 71 | 11.0 / 31.3 / 86 | 11.5 / 34.2 / 86 | 15.5 / 40.5 / 101 | 11.2 / 30.1 / 78 | 16.5 / 45.1 / 97 | 12.2 / 33.2 / 64 |
| zam-compx2 | 8.3 / 23.3 / 52 | - | 9.5 / 24.6 / 51 | - | - | 14.6 / 35.2 / 78 | 9.1 / 23.4 / 54 |
| caps-platex2 | 13.5 / 31.2 / 92 | - | 14.9 / 31.0 / 84 | - | - | 19.7 / 41.2 / 119 | 13.8 / 30.9 / 94 |
| caps-compressx2 | 24.2 / 59.7 / 120 | - | 25.3 / 58.8 / 118 | - | - | 30.5 / 68.6 / 167 | 24.6 / 58.9 / 142 |
| lsp-comp | - | 14.2 / 32.5 / 69 | - | 19.3 / 41.6 / 106 | 13.8 / 31.1 / 69 | - | - |
| dragonfly-room | - | 64.3 / 151.2 / 1296 | - | 70.2 / 158.5 / 1351 | 63.2 / 151.1 / 2375 | - | - |

### Per path, all plugins, all configurations: median node time p50 (us), and start-up

| path | node p50 us (median over plugins and configs) | inline p50 us | add ms (median) | add -> first sound ms (median) | host ready ms (median) | RSS empty MiB | RSS per plugin MiB |
|---|---|---|---|---|---|---|---|
| modhost | 7.3 | - | 4.2 | 68.6 | 24.4 | 19.4 | 2.31 |
| clap | 8.3 | - | 4.5 | 68.2 | 13.5 | 5.8 | 4.30 |
| adapter | 7.5 | - | 3.7 | 67.2 | 13.7 | 5.8 | 2.31 |
| inproc-clap | 13.5 | 8.3 | 13.2 | 75.9 | 2.6 | 3.3 | 4.41 |
| inproc-lv2 | 13.1 | 8.4 | 12.6 | 74.9 | 2.5 | 3.3 | 2.17 |

### Crash drill (48 kHz / 128), median of the repetitions (min...max)

| path | event | host died | exit ms | graph alive | probe max gap us | late cycles | victim silent ms | neighbour silent ms | respawn -> sound ms |
|---|---|---|---|---|---|---|---|---|---|
| modhost | kill | yes | 4.9 (3.7...7.8) | yes | 3075 (3048...3531) | 0 (0...0) | 2.5 (2.5...3.0) | NA | 101.4 (89.9...101.6) |
| clap | kill | yes | 3.5 (3.2...3.6) | yes | 3236 (3154...4379) | 0 (0...1) | 1.1 (0.8...1.4) | NA | 75.4 (75.2...78.9) |
| clap | crash | yes | 175.0 (164.6...176.9) | yes | 3456 (3091...4558) | 0 (0...1) | 174.6 (164.1...174.9) | 176.5 (165.9...176.5) | NA |
| adapter | kill | yes | 4.9 (3.6...5.1) | yes | 3071 (3030...3113) | 0 (0...0) | 1.3 (1.2...1.6) | NA | 75.5 (75.4...77.8) |
| inproc-clap | kill | yes | 3.9 (3.8...4.4) | yes | 3034 (3027...3224) | 0 (0...0) | 1.2 (1.0...1.3) | NA | 73.5 (71.6...73.5) |
| inproc-clap | crash | yes | 154.3 (150.1...161.3) | yes | 3264 (3060...3658) | 0 (0...0) | 152.7 (149.2...160.0) | 152.7 (150.1...161.1) | NA |
| inproc-lv2 | kill | yes | 3.1 (3.1...3.7) | yes | 3117 (3083...3121) | 0 (0...0) | 1.5 (1.4...1.6) | NA | 71.8 (68.9...73.3) |

## What the numbers say

- **A plugin costs the same on every path.** For the same plugin at the same rate and quantum, the node's p50 on
  mod-host, omx-clap-host and the adapter lie within about 1 us of each other and of the in-process inline time
  (omx-eq8 at 48 kHz / 128: 2.1, 1.9, 2.6 and 1.8 to 2.2 us; caps CompressX2: 9.8, 10.3 and 9.9 us). The p99 and max
  move by tens of microseconds between repetitions of the same configuration, which is the machine, not the path.
- **The bench's in-process node costs 2 to 5 us more than the others' nodes** at p50, while its inline time matches
  them: that is the price of `bench/inproc.c`'s own PipeWire filter per plugin (four DSP port buffers, two copies) on
  top of the core's stage with its guards on, not of in-process hosting. An engine that runs the plugin inline in its
  lane pays the inline figure and no node at all.
- **No path adds latency.** The impulse through the pad arrived in the cycle it left on every path, rate and quantum
  (0 frames against a direct link that also read 0), every impulse of every run (about 240 a run). The "+1 quantum" for
  mod-host in the hosting-path spec does not hold for a JACK client in a PipeWire graph: pipewire-jack clients are
  scheduled synchronously in the cycle.
- **The whole graph**: on the same eight LV2 plugins mod-host had the lowest cycle end in all 8 configurations and
  the fewest overruns at 96 kHz / 64 (98 against 310 to 332), with the adapter and in-process LV2 close to each other.
  Where it comes from is not shown here: the nodes themselves cost the same, so it is in the wake-up chain between
  nodes (every plugin is a node with its own data thread on all five paths), which an engine running plugins inline
  does not have. Host process CPU follows the same order (96 kHz / 128: mod-host 23.7 %, adapter 31.4 %, in-process
  LV2 33.5 % of one core). At 96 kHz / 64 and 96 kHz / 128 every path overran on this machine with these sets; the
  CLAP set's overruns at 96 kHz / 128 track Dragonfly Room's maxima (1.3 to 1.5 ms against a 1.33 ms period).
- **Memory**: on the same eight plugins, 40 MiB resident for mod-host (19 MiB before any plugin: it loads the lilv
  world of every installed bundle), 26.5 MiB for omx-clap-host with the adapter (5.8 MiB empty), 22.8 MiB for the
  in-process host (3.3 MiB empty). About 2.2 to 2.3 MiB per LV2 plugin on all three; the CLAP set's 4.3 MiB per
  plugin is lsp's one large binary.
- **Start-up**: a host is ready in 2.5 ms (in-process), 13.5 ms (omx-clap-host) or 24 ms (mod-host); an `add` answers
  in 4 ms out of process and 13 ms in process (the core's default warm-up, which omx-clap-host turns off). The first
  non-silent block came 67 to 76 ms after the `add` on every path, most of it the bench's own three `pw-link` calls.
- **Crash**: SIGKILL of the host silences its plugins within 1 to 3 ms, the graph keeps cycling (no late cycle, the
  probe's longest gap 3.0 to 4.4 ms against a 2.67 ms period) and a respawned host sounds again 70 to 100 ms later.
  A plugin that abort()s takes its host down 150 to 175 ms after the write that triggers it (the abort and its core
  dump), and every other plugin in that host with it; the graph survived on both CLAP paths. In process, the host
  process is the engine: the same crash there is the console's audio gone until the engine restarts. One smoke run
  before the measured runs saw a 163 ms graph stall while omx-clap-host was dying; the nine measured crash runs did
  not reproduce it, so it is noted and not claimed.

## Limits

- r1 is a shared build machine under a load of about 3, with the `powersave` governor, no CPU isolation and no
  pinning; tails (p99, max) and whole-graph figures carry its noise, and the spread columns show how much. The rig
  (msi) was not used and these are not the rig's numbers.
- The graph is driven by PipeWire's dummy driver in a container, with no device; a real driver wakes differently.
- Path 3 is a stand-in (the adapter loaded through a bench-only `.clap` under omx-clap-host, #96); path 4 is a
  PipeWire filter per plugin, not the engine's lane.
- The plugins are fed white noise at -20 dBFS; a plugin whose cost depends on its input (a gate, a limiter in
  reduction) would read differently on programme material.
- Three stock LV2 plugins the adapter refuses and one with a MIDI input are not in the LV2 set (see The plugins), so
  "the adapter is comparable to mod-host" holds for the plugins it admits.

## Verdict

On the plugins every path can host, the paths cost the same per plugin: the plugin's own process() dominates, the
host's wrapper adds about a microsecond, and no path adds latency, so the "+1 quantum" assigned to mod-host is not a
cost to plan around. mod-host is not the expensive path: on the same eight LV2 plugins it had the lowest graph cycle
time and process CPU, and its price is memory (about 14 MiB more at start than omx-clap-host) and a slower start.
libomx-clap-lv2 through omx-clap-host is comparable to mod-host per plugin and lighter in memory, but omx-clap-host
cannot load LV2 yet (#96) and the adapter refuses common LV2 plugins with non-MIDI atom ports (#84), so mod-host cannot
be retired today. In-process costs the plugin's inline time and nothing else, and saves the per-plugin node, but a
crash there takes the console's audio with it, where out of process the graph lives and a respawn sounds within
0.1 s. Recommendation for plugin-hostd's default worker: omx-clap-host, out of process, for CLAP plugins now and for
LV2 plugins once #96 lands and the adapter admits them; mod-host stays as the LV2 fallback for what the adapter
refuses until #84 closes; in-process only for plugins the qualifier has admitted. Re-measure on the rig (or an
isolated, `performance`-governed machine) before treating the whole-graph differences as more than this machine's.
