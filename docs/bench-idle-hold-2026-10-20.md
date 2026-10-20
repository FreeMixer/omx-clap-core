# The CPU idle hold bench, 2026-10-20

What #296 buys, measured: the same plugins through mod-host, omx-clap-host (CLAP and the LV2 adapter), the
in-process tier, plugin-hostd and jalv, with `/dev/cpu_dma_latency` held at nothing, at 10 us (C1 only, the shipped
default) and at 0 (no idle state), on the desk (msi). It is the idle-state arm the October 15 bench
([bench-hosting-paths-2026-10-15.md](bench-hosting-paths-2026-10-15.md), follow-up of the 16th) promised for #105,
run on the library's own hold instead of one held from outside. The raw rows are in
[bench-idle-hold-2026-10-20/](bench-idle-hold-2026-10-20/): `runs.csv` (one row per run, with the hold and the
device's answer), `plugins.csv` (one row per plugin per run), `power.csv` (one row per run with an energy counter),
`machine.txt`.

## How to run it

```
make bench BENCH_IDLE_HOLD=10 BENCH_RATES="96000 48000" BENCH_QUANTA=128 BENCH_REPS=3 BENCH_SECONDS=30 BENCH_CRASH=0 \
    BENCH_PATHS="modhost clap adapter inproc-clap inproc-lv2 hostd jalv" \
    BENCH_ENERGY_CMD="sudo -n cat /sys/class/powercap/intel-rapl:0/energy_uj"      # one hold; off and 0 the same way
python3 -I bench/summary.py build/bench/results                                   # opens with the per-hold table
```

This run interleaved the three holds per repetition (`BENCH_REP_FROM`), so a drift of the machine over the hour
does not line up with one hold. The crash drill was left out. The jalv rows come from a separate pass of the same
harness (below).

## What each path holds, and how

| path | what holds the device |
|---|---|
| `clap`, `adapter` | omx-clap-host, `--idle-hold=<off\|us>`: the library's own hold |
| `inproc-clap`, `inproc-lv2` | `bench/inproc.c` through `omx_clap_host_configure` (`OMX_BENCH_IDLE_HOLD`): the same library call the engine's in-process tier makes |
| `hostd` | plugin-hostd 0.6.0 with its LV2 worker omx-clap-host (this tree's binary, the adapter, every plugin in one pool worker), the option passed by a wrapper the daemon spawns; the daemon itself links no library |
| `modhost`, `jalv` | nothing of their own: the bench keeps the same request open on the device for the run (a python holder), as an operator's playbook would |

`hold off` is the machine at rest: tuned's throughput-performance profile holds the device at 4000 us, which refuses
no state (C3 exits in 1048 us). The `device us` column is what the device read with every plugin active, the
kernel's limit across every requester: 10 and 0 there are the hold itself, seen from outside the process.

## The machine

msi: Intel Core Ultra 9 275HX (8 performance + 16 efficiency cores, 24 threads), Fedora 44 kernel 7.2.9, governor
performance, intel_idle with the menu governor, states POLL, C1_ACPI (1 us), C2_ACPI (127 us), C3_ACPI (1048 us), all
enabled, nothing on the kernel command line about idle. PipeWire 1.6.9. The device carries the package's udev rule
(`root:audio 0660`) and the bench user is in `audio`. Watts are the package's RAPL counter (`intel-rapl:0`), sampled
four times a second from outside the runs' namespace and integrated over each run's 30 s measured window alone. **The
live console (96 kHz / 512) runs on the same box and is inside every watt figure**; only the differences between
holds mean anything, not the level. Each run is a PipeWire of its own in a private namespace; the hold is a kernel
aggregate, so the live console's cores were held too during the 10 and 0 arms.

## The whole graph per hold

Cycle end = the driver's time from wake-up to the last node finished (pw-profiler); overruns = cycles that ended past
their period; late = probe cycles more than 1.5 periods after the one before; host CPU = the host's processes over
the window, per cent of one core (the daemon plus its worker for plugin-hostd, the sum over the jalv processes);
RSS = the same processes with every plugin loaded. The CLAP rows carry seven plugins, the LV2 rows eight
(`bench/plugins.tsv`).

#### 96 kHz / 128 (period 1333 us), median of three repetitions, cycle end with its min...max

| path | hold | device us | cycle end p50 us | cycle end p99 us | overruns / 30 s | late probe cycles | host CPU % | RSS loaded MiB | package W |
|---|---|---|---|---|---|---|---|---|---|
| mod-host | off | 4000 | 235 (235...237) | 675 (662...725) | 19 | 49 | 20.7 | 41.0 | 19.4 |
| mod-host | 10 | 10 | 74 (73...75) | 116 (114...119) | 0 | 2 | 8.6 | 41.0 | 18.6 |
| mod-host | 0 | 0 | 27 (26...29) | 47 (42...114) | 1 | 5 | 3.7 | 40.9 | 79.3 |
| omx-clap-host CLAP | off | 4000 | 324 (298...345) | 720 (718...751) | 14 | 31 | 16.5 | 42.6 | 17.9 |
| omx-clap-host CLAP | 10 | 10 | 103 (101...103) | 183 (174...264) | 0 | 9 | 10.4 | 42.4 | 20.1 |
| omx-clap-host CLAP | 0 | 0 | 41 (40...42) | 103 (64...191) | 0 | 3 | 4.4 | 42.5 | 79.6 |
| omx-clap-host adapter | off | 4000 | 355 (291...375) | 756 (704...774) | 26 | 44 | 15.8 | 28.8 | 18.0 |
| omx-clap-host adapter | 10 | 10 | 72 (72...78) | 120 (111...121) | 0 | 5 | 8.6 | 28.8 | 19.2 |
| omx-clap-host adapter | 0 | 0 | 26 (25...28) | 42 (41...65) | 0 | 5 | 3.8 | 28.7 | 79.7 |
| in-process CLAP | off | 4000 | 369 (364...411) | 797 (759...847) | 39 | 52 | 18.8 | 40.5 | 17.8 |
| in-process CLAP | 10 | 10 | 101 (99...105) | 186 (184...197) | 0 | 22 | 10.9 | 40.9 | 19.5 |
| in-process CLAP | 0 | 0 | 43 (42...44) | 80 (67...151) | 1 | 2 | 4.8 | 40.6 | 79.6 |
| in-process LV2 | off | 4000 | 392 (372...403) | 768 (712...769) | 17 | 36 | 18.2 | 24.6 | 17.6 |
| in-process LV2 | 10 | 10 | 72 (70...73) | 127 (116...130) | 1 | 34 | 9.6 | 24.6 | 19.4 |
| in-process LV2 | 0 | 0 | 26 (26...27) | 44 (41...67) | 0 | 18 | 3.8 | 24.6 | 79.6 |
| plugin-hostd | off | 4000 | 224 (218...227) | 644 (562...698) | 3 | 24 | 26.1 | 91.4 | 18.1 |
| plugin-hostd | 10 | 10 | 82 (81...84) | 139 (133...146) | 1 | 15 | 16.5 | 91.5 | 19.5 |
| plugin-hostd | 0 | 0 | 27 (26...27) | 140 (85...149) | 0 | 21 | 7.2 | 91.6 | 79.8 |
| jalv (headless) | off | 4000 | 341 (317...349) | 816 (731...964) | 23 | 63 | 16.6 | 109.4 | 21.1 |
| jalv (headless) | 10 | 10 | 72 (67...73) | 121 (114...124) | 0 | 18 | 8.2 | 109.3 | 20.7 |
| jalv (headless) | 0 | 0 | 26 (25...26) | 50 (43...129) | 0 | 10 | 3.4 | 109.4 | 78.9 |
| jalv (ZamCompX2 GUI up) | off | 4000 | 359 (350...361) | 757 (716...845) | 28 | 41 | 16.6 | 203.0 | 17.9 |
| jalv (ZamCompX2 GUI up) | 10 | 10 | 68 (67...70) | 131 (128...170) | 0 | 38 | 8.1 | 203.1 | 21.5 |
| jalv (ZamCompX2 GUI up) | 0 | 0 | 26 (25...26) | 85 (41...106) | 1 | 22 | 3.5 | 203.1 | 79.7 |

#### 48 kHz / 128 (period 2666 us), median of three repetitions, cycle end with its min...max

| path | hold | device us | cycle end p50 us | cycle end p99 us | overruns / 30 s | late probe cycles | host CPU % | RSS loaded MiB | package W |
|---|---|---|---|---|---|---|---|---|---|
| mod-host | off | 4000 | 322 (276...403) | 826 (808...904) | 0 | 1 | 13.1 | 40.8 | 15.6 |
| mod-host | 10 | 10 | 85 (83...86) | 121 (121...129) | 0 | 0 | 4.9 | 40.8 | 17.2 |
| mod-host | 0 | 0 | 28 (28...28) | 64 (46...88) | 0 | 0 | 2.0 | 40.8 | 79.8 |
| omx-clap-host CLAP | off | 4000 | 434 (426...446) | 878 (825...957) | 0 | 0 | 12.1 | 42.2 | 15.4 |
| omx-clap-host CLAP | 10 | 10 | 117 (114...118) | 196 (185...209) | 0 | 0 | 6.0 | 42.1 | 18.8 |
| omx-clap-host CLAP | 0 | 0 | 46 (45...47) | 87 (79...191) | 0 | 0 | 2.9 | 42.1 | 79.8 |
| omx-clap-host adapter | off | 4000 | 415 (402...447) | 950 (801...952) | 0 | 1 | 11.6 | 28.4 | 16.6 |
| omx-clap-host adapter | 10 | 10 | 83 (81...83) | 130 (127...133) | 0 | 0 | 5.1 | 28.4 | 18.5 |
| omx-clap-host adapter | 0 | 0 | 26 (26...28) | 116 (53...143) | 0 | 0 | 1.9 | 28.5 | 79.8 |
| in-process CLAP | off | 4000 | 439 (397...442) | 809 (799...1095) | 0 | 1 | 11.8 | 40.5 | 15.4 |
| in-process CLAP | 10 | 10 | 114 (111...114) | 224 (191...280) | 0 | 0 | 6.1 | 40.6 | 18.7 |
| in-process CLAP | 0 | 0 | 49 (44...50) | 82 (74...118) | 0 | 0 | 2.7 | 40.5 | 79.8 |
| in-process LV2 | off | 4000 | 434 (427...436) | 881 (809...919) | 0 | 1 | 12.8 | 24.4 | 16.6 |
| in-process LV2 | 10 | 10 | 80 (78...82) | 148 (119...168) | 0 | 0 | 5.4 | 24.4 | 19.3 |
| in-process LV2 | 0 | 0 | 27 (26...27) | 48 (45...50) | 0 | 0 | 2.0 | 24.4 | 79.8 |
| plugin-hostd | off | 4000 | 243 (239...257) | 710 (647...762) | 0 | 0 | 20.4 | 91.4 | 16.2 |
| plugin-hostd | 10 | 10 | 87 (86...89) | 177 (141...193) | 0 | 0 | 10.2 | 91.3 | 19.9 |
| plugin-hostd | 0 | 0 | 28 (27...29) | 79 (76...88) | 0 | 0 | 4.6 | 91.4 | 79.8 |
| jalv (headless) | off | 4000 | 427 (407...441) | 894 (808...913) | 0 | 2 | 11.4 | 109.2 | 18.2 |
| jalv (headless) | 10 | 10 | 76 (76...78) | 131 (123...217) | 0 | 0 | 4.6 | 109.5 | 19.8 |
| jalv (headless) | 0 | 0 | 26 (26...26) | 54 (54...72) | 0 | 0 | 1.8 | 109.5 | 79.5 |
| jalv (ZamCompX2 GUI up) | off | 4000 | 420 (416...432) | 869 (818...886) | 0 | 1 | 11.2 | 203.1 | 16.9 |
| jalv (ZamCompX2 GUI up) | 10 | 10 | 76 (75...78) | 120 (119...139) | 0 | 0 | 4.6 | 203.1 | 18.4 |
| jalv (ZamCompX2 GUI up) | 0 | 0 | 27 (26...27) | 83 (47...133) | 0 | 0 | 1.8 | 202.9 | 79.9 |

### What it says

- **The hold at 10 us cuts the cycle end three to five times on every path, and mod-host's lead is gone.** With the
  hold off, mod-host and plugin-hostd end their cycles at 225 to 235 us where omx-clap-host and the in-process tier
  take 311 to 392 (96 kHz). At 10 the six paths sit between 72 and 103 us; at 0 between 26 and 43. The operator's
  reading of the October bench holds: mod-host's advantage was the cores its extra clients kept awake, and once no
  core sleeps past C1 the hosts are equal within one plugin set. The two CLAP rows stay about 30 us above the LV2
  rows at every hold; that is the CLAP paths' own cost, not an idle effect.
- **Overruns go with it.** 14 to 39 per 30 s with the hold off at 96 kHz / 128, 0 to 1 at 10.
- **10 us costs one to two and a half watts; 0 costs sixty.** The hold at 10 reads 18.6 to 20.1 W against 17.6 to
  19.4 at rest (mod-host's two are within its repetitions' spread, the others clear of it). The hold at 0 refuses
  even C1, so all 24 threads poll between cycles: 79 to 80 W, four times the package power, for a further 45 to 60
  us of cycle end. That settles the default as the issue has it: 10 shipped, 0 only as an explicit choice.
- **Host CPU is clock time, so it falls as cycles shorten.** It is not the host working less.
- **Memory does not move with the hold**, as one open descriptor should not. Between paths: in-process LV2 25 MiB,
  the adapter 29, mod-host and the CLAP rows 41 to 43 (the CLAP bundles are the larger binaries of the same
  plugins), plugin-hostd 91 (the daemon and its one worker).
- 48 kHz tells the same story with higher numbers: 243 to 439 us at rest, 80 to 118 at 10, 26 to 49 at 0.

## Per plugin: the node's own time per cycle, by hold

The node's duration (pw-profiler, active to finished) is the plugin plus what its host does inside the node's cycle.
#### 96 kHz / 128: node time p50 us per plugin, hold off / 10 / 0 (median of three repetitions)

| plugin | mod-host | clap | adapter | in-proc CLAP | in-proc LV2 | plugin-hostd | jalv | jalv + GUI |
|---|---|---|---|---|---|---|---|---|
| omx-comp | 3.8 / 2.7 / 2.4 | 4.6 / 5.4 / 2.7 | 3.1 / 4.1 / 2.4 | 3.9 / 3.5 / 3.5 | 3.3 / 3.3 / 3.2 | 5.5 / 5.6 / 2.6 | 2.6 / 2.4 / 2.2 | 2.5 / 2.6 / 2.3 |
| omx-delay | 2.2 / 1.2 / 0.9 | 2.8 / 1.3 / 0.8 | 3.4 / 1.5 / 0.9 | 5.0 / 3.7 / 1.6 | 5.6 / 3.9 / 1.8 | 3.1 / 2.3 / 1.0 | 4.0 / 2.0 / 0.8 | 3.8 / 1.6 / 0.8 |
| omx-deesser | 4.6 / 5.7 / 3.3 | 6.1 / 6.7 / 3.6 | 5.5 / 5.8 / 3.4 | 8.2 / 8.2 / 3.9 | 7.6 / 7.6 / 3.4 | 6.1 / 6.2 / 3.4 | 5.5 / 5.7 / 3.2 | 5.3 / 5.7 / 2.6 |
| omx-eq8 | 1.7 / 1.2 / 0.6 | 1.9 / 1.1 / 0.5 | 2.4 / 1.5 / 0.6 | 4.6 / 2.7 / 1.5 | 5.4 / 3.1 / 1.4 | 2.5 / 2.0 / 0.7 | 2.3 / 1.4 / 0.5 | 2.3 / 1.3 / 0.4 |
| omx-reverb | 5.8 / 7.7 / 3.7 | 5.7 / 7.3 / 3.2 | 6.3 / 7.7 / 3.5 | 8.2 / 8.5 / 4.5 | 8.5 / 8.8 / 4.0 | 8.0 / 8.6 / 3.9 | 6.5 / 7.0 / 3.2 | 6.2 / 7.0 / 3.3 |
| zam-compx2 | 3.8 / 4.6 / 2.4 | - | 4.4 / 5.0 / 2.5 | - | 6.7 / 6.6 / 3.1 | 5.2 / 5.5 / 2.7 | 4.1 / 4.6 / 2.4 | 4.0 / 4.4 / 2.5 |
| caps-platex2 | 5.7 / 9.4 / 4.4 | - | 6.2 / 9.7 / 4.3 | - | 8.8 / 11.2 / 5.2 | 7.1 / 9.8 / 4.6 | 6.0 / 8.8 / 4.3 | 5.9 / 8.6 / 4.3 |
| caps-compressx2 | 9.8 / 16.8 / 8.5 | - | 10.2 / 16.8 / 8.6 | - | 12.8 / 19.0 / 9.7 | 11.6 / 17.4 / 7.9 | 10.0 / 16.7 / 8.4 | 9.9 / 16.4 / 8.4 |
| lsp-comp | - | 5.7 / 4.8 / 2.0 | - | 8.3 / 6.0 / 2.8 | - | - | - | - |
| dragonfly-room | - | 28.0 / 49.6 / 25.0 | - | 31.5 / 51.1 / 26.7 | - | - | - | - |

#### 48 kHz / 128: node time p50 us per plugin, hold off / 10 / 0 (median of three repetitions)

| plugin | mod-host | clap | adapter | in-proc CLAP | in-proc LV2 | plugin-hostd | jalv | jalv + GUI |
|---|---|---|---|---|---|---|---|---|
| omx-comp | 5.9 / 5.3 / 2.6 | 6.7 / 6.1 / 2.9 | 6.5 / 5.3 / 2.5 | 7.6 / 7.8 / 3.8 | 6.8 / 6.8 / 3.3 | 5.5 / 6.7 / 2.6 | 3.9 / 4.8 / 2.3 | 4.8 / 4.8 / 2.4 |
| omx-delay | 2.3 / 1.3 / 1.0 | 2.9 / 1.6 / 0.9 | 3.7 / 1.7 / 1.0 | 5.2 / 4.7 / 1.9 | 5.6 / 5.5 / 1.9 | 3.2 / 3.1 / 1.1 | 2.8 / 2.2 / 0.8 | 4.2 / 2.9 / 0.8 |
| omx-deesser | 5.1 / 5.7 / 3.3 | 6.6 / 6.8 / 3.5 | 6.2 / 6.0 / 3.4 | 8.9 / 8.3 / 4.4 | 8.5 / 7.6 / 3.4 | 6.0 / 6.2 / 3.1 | 5.3 / 5.7 / 2.4 | 8.8 / 5.6 / 2.4 |
| omx-eq8 | 2.2 / 1.6 / 0.6 | 2.8 / 1.4 / 0.5 | 3.2 / 1.8 / 0.6 | 5.0 / 2.9 / 1.3 | 6.1 / 3.6 / 1.4 | 2.9 / 2.4 / 0.7 | 2.6 / 1.5 / 0.5 | 2.9 / 1.5 / 0.5 |
| omx-reverb | 6.5 / 8.4 / 4.1 | 6.8 / 8.8 / 3.6 | 7.2 / 8.8 / 3.8 | 9.1 / 9.1 / 4.8 | 9.6 / 9.4 / 4.1 | 7.9 / 9.3 / 4.1 | 6.8 / 7.0 / 3.5 | 9.0 / 7.5 / 3.7 |
| zam-compx2 | 4.1 / 5.1 / 2.5 | - | 5.0 / 5.5 / 2.6 | - | 7.5 / 7.2 / 3.2 | 5.2 / 5.9 / 2.7 | 4.7 / 5.0 / 2.6 | 5.3 / 5.1 / 2.7 |
| caps-platex2 | 5.6 / 10.0 / 4.7 | - | 6.3 / 10.4 / 4.5 | - | 11.0 / 11.3 / 5.3 | 6.9 / 10.6 / 4.6 | 8.0 / 9.7 / 4.3 | 9.8 / 9.6 / 4.3 |
| caps-compressx2 | 10.6 / 18.4 / 9.1 | - | 11.3 / 18.8 / 9.2 | - | 14.4 / 19.9 / 10.1 | 11.8 / 18.6 / 8.3 | 11.2 / 18.2 / 8.7 | 12.0 / 18.0 / 8.9 |
| lsp-comp | - | 7.2 / 6.8 / 2.6 | - | 9.4 / 7.5 / 3.5 | - | - | - | - |
| dragonfly-room | - | 27.5 / 53.7 / 27.0 | - | 35.2 / 53.6 / 29.3 | - | - | - | - |

- **Across hosts, at one hold, a plugin costs the same within one or two microseconds**: mod-host, omx-clap-host and
  plugin-hostd's worker are indistinguishable per node, as the October bench found. The in-process rows read one to
  three microseconds higher on the light plugins: the engine-side per-block guards the in-process tier runs (clamp,
  non-finite scan) and the socket hosts do not.
- **The hold's effect per node is not monotonic.** From rest to 10 the light plugins get faster, but the heavy ones
  get slower, on every host alike: caps-compressx2 from about 10 to 17 us, dragonfly-room from 28 to 50, omx-reverb
  from 6 to 8. At 0 everything is fastest. The whole-graph figure still falls at 10 because the idle-state exit it
  removes is hundreds of microseconds. The cause is not in this data; the placement section below is the check made
  for it.

### Where the real-time threads ran, and at what clock

Two checks for the heavy-plugin slowdown at 10, one adapter run per hold at 96 kHz / 128 each, sampled from outside
the namespace twice a second over the run (`machine.txt` carries the figures):

- **Core placement is not it.** This CPU has 8 performance cores (CPUs 0 to 7) and 16 efficiency cores (8 to 23), a
  1.7 times difference on floating-point work, which is the size of the effect. The host's SCHED_FIFO threads
  (one per JACK client) sat on performance cores 65 % of the samples with the hold off, 77 % at 10 and 61 % at 0:
  the hold at 10 has the most performance-core time and the slowest heavy nodes.
- **The clocks are not it.** The uncore held 3800 MHz under every hold (`intel_uncore_frequency`, its maximum).
  The cores read 3.8 to 4.6 GHz at rest and at 10, 4.4 to 4.9 under polling; nothing near a 1.7 times gap between
  rest and 10.

So a reverb or a look-ahead compressor runs slower per cycle with every core halted in C1 than with the cores
sleeping deeper or polling, on every host, and this run does not say why. It is written up as an open observation,
not a reading of the hold; the whole-graph gain at 10 stands over it by an order of magnitude.

## jalv, as Zynthian runs a layer

jalv 1.8.0, one `jalv -i -n effect_<k> <uri>` process per plugin, eight processes, the LV2 build of the same set, the
device held for them by the bench as for mod-host. Two variants, three repetitions each, after the main run:
headless, and with `-s`, which shows a plugin's UI where it has one. Of the eight only ZamCompX2 has one (an X11 DPF
UI with `ui:showInterface`, the one kind a console jalv can show), so the second variant is seven headless layers
and one with its window up. The window went to a virtual X server (`xvfb-run`), where the DPF UI renders in software
(33 threads in that process against 9 headless): an upper bound of what the GUI costs, not what it costs on the
desk's own display. The rows are in the tables above as `jalv (headless)` and `jalv (ZamCompX2 GUI up)`.

- **Eight processes cost the same cycle as one.** With the hold at 10, jalv ends the cycle at 72 us against 72 to 74
  for mod-host and the adapter in one process; at 0, 26 against 26 to 27. The process count is not in the cycle.
- **It is in memory and a little in power**: 109 MiB for eight jalv processes against 29 to 41 for one process
  holding the same plugins, and about one to two watts more, within three repetitions' spread.
- **jalv's probe cycles arrive late more often** at every hold (10 to 63 late cycles per 30 s against 0 to 49 for
  the others, 18 against 2 to 5 at the shipped 10). Its own node times per plugin are the same as everyone's.
- **The GUI changes the cycle by nothing** (68 against 72 us at 10, within the spread) and costs 94 MiB of RSS and
  about a watt at 10. In the main run the first repetition of every jalv arm had that window up on the desk's
  display; those rows were discarded and jalv was measured again as above.

## Caveats

- One box, one plugin set, 30 s windows, three repetitions. The live console shares the box and the watts.
- The first repetition of plugin-hostd was taken before the bench counted the daemon's worker, so its host CPU and
  RSS in that repetition are the daemon's alone; the medians above are of the later two, and the spread's low end is
  that row.
- The in-process rows are `bench/inproc.c`, not the engine: the same library calls, none of the engine's lane. The
  live console's node links the installed library and was not rebuilt for this; its tier gets the hold when it is.
