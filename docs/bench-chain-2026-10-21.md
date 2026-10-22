# A chain of plugins in one JACK client, Stage 0, 2026-10-21

The gate of #302: N plugins in series in ONE JACK client of omx-clap-host, against the same N plugins in N clients
linked in series, bit for bit and by the bench's numbers. A prototype in `src/effects.c` and `bench/` alone: the
library's API, `CORE_HEADERS` and `abi/` do not change. A pass releases S1, the library's chain.

## What the prototype is

`OMX_CLAP_HOST_CHAIN=<k0>,<k1>,...` in omx-clap-host's environment names ONE chain of instance numbers, in order.
Every instance it does not name is added as before, one JACK client each.

- `k0` is the head: an instance added as before, with its client `effect_<k0>`, its ports and its process callback.
- `k1`, `k2`, ... are members. A member's add opens no JACK client and registers no port: its cycles run inside the
  head's. It is added only when the instance before it in the list is in the chain already (its tail), its main inputs
  are as wide as that one's outputs and its main outputs as wide as the head's; anything else is refused,
  `resp -902` (`ERR_INVALID_OPERATION`), said on stderr. A plugin with a note input or a note output is refused as a
  member. A member's auxiliary inputs read silence and its auxiliary outputs go nowhere: what an unconnected auxiliary
  port of its own client would do.
- The head's process callback enters its own cycle gate and runs the head, then each member in order, each inside its
  own cycle gate (`omx_cycle_gate_enter`, `omx_clap_host_run_ports`, `omx_cycle_gate_leave`), handing each output
  buffer to the next member; the last member writes the head's output ports. Between them the outputs are blocks the
  host holds, `CHAIN_FRAMES_MAX` (8192) frames per channel, 64-byte aligned as a port buffer is: a longer cycle passes
  the head's output through the members dry.
- Every member's audio thread is the head's (`omx_clap_host_set_audio_thread`, at its add and in the head's thread-init
  callback); a member's block size follows the head's buffer-size callback, its transport the head's frame clock.
- The head's ports report the sum of every member's `omx_clap_host_latency` on top of its own (the latency callback),
  recomputed when any member's latency changes.
- Bypass, parameters, meters, pins, presets and state stay per instance, by the member's own number.
- `remove <member>` unlinks it inside a gate of the chain's own around the members' walk (the cycle under way leaves
  first, the cycles in between pass the head's output dry through the members), then frees it; `remove <head>` closes
  the client and frees every member with it.

## What the bench adds

- `clap-series`: the CLAP plugins of `bench/plugins.tsv` in one omx-clap-host, one client each, linked in series
  (the probe's noise into the first, the last into the recorder): the arm the chain is judged against.
- `chain-clap`: the same plugins, the same host, `OMX_CLAP_HOST_CHAIN` naming them: one client.
- `adapter-series` and `chain-adapter`: the same two for the LV2 rows through libomx-clap-lv2 (the eight of #104's
  bench).
- `PLUGIN_NAMES` picks the rows by name (N=4: `omx-eq8 omx-comp omx-deesser omx-delay`).
- The latency pad stays its own client in every arm, so its reading is the graph's, not the chain's.

The other paths still link every plugin in parallel. A container that refuses a `/proc` of its own (podman's masked
`/proc`: `unshare --mount-proc` fails with "mount too revealing") runs each run, and `tests/chain_identity.sh`, in a
user and network namespace as a session of its own, which its cleanup ends, instead of a pid namespace.

## Bit identity

`tests/chain_identity.sh` (the `tests/clap_lv2_identity.sh` pattern, `tests/jack_identity` feeding and recording):
one omx-clap-host with the plugins in N clients in series, one with them in a chain, both fed the same
deterministic noise and recorded from the same cycles, left and right; every pair `cmp`-equal or the first
differing sample reported, and the latency each arm's last output port reports equal. A control pass runs two
N-client arms against each other first: a plugin that is not deterministic between two instances of itself shows
there, not as the chain's difference.

## The bar (operator's ruling, 2026-10-21)

The chain passes when it is bit-identical with one client per plugin and makes nothing worse (cycle-end p99,
overruns, xruns at every hold); any measurable cycle-end saving at hold 10 passes.

## Where it ran

The cluster's r1 pod (`omx-worker`, Fedora 44, 4 CPUs), as user 1000 with no root: the build's packages (jack and
pipewire-jack devel, omx-dsp-devel 0.24.1, omx-contract-devel 3.0.0, mod-host-protocol-devel, plugin-hostd-devel
0.6.0, omx-plugins 0.12.0 CLAP and LV2) unpacked from their RPMs into a tree of their own (`PLUGIN_ROOT`), the stock
plugins (lsp-plugins, Dragonfly, ZamCompX2, caps) the image's. PipeWire 1.6.9, its dummy driver.

- `/dev/cpu_dma_latency` does not exist in the pod: no hold can be held there (the brief's NOT VERIFIED: it cannot).
  The host asks for 10 us, the shipped default, says it cannot open the device and runs with none, so every run is
  in fact unheld; off and 0 would be the same runs again and were not made. The hold arms need a machine with the
  device (the desk, or a pod given it).
- `ulimit -r` is 0: PipeWire's data loop and every client thread run SCHED_OTHER, not FIFO. The figures are larger
  and noisier than a realtime graph's, the same for both arms of a pair.

## Bit identity: the results

`tests/chain_identity.sh`, 48000 Hz, quantum 1024 (the private graph's own), 96000 samples per side:

| plugins | kind | pins | control (series against series) | chain against series | latency series / chain |
|---|---|---|---|---|---|
| this tree's fixture: passthrough, sidechain, passthrough | CLAP | no | identical | identical | 192 / 192 |
| omx-eq8 omx-comp omx-deesser omx-delay (N=4) | CLAP | no | identical | identical | 0 / 0 |
| omx comp, delay, deesser, eq8, reverb, lsp compressor (6) | CLAP | no | identical | identical | 0 / 0 |
| the 7 CLAP rows (the 6 and Dragonfly Room) | CLAP | no | differs at sample 0, -29.1 dB | differs at sample 0, -31.0 dB | 0 / 0 |
| omx comp, delay, deesser, eq8, reverb, ZamCompX2, caps CompressX2 (7) | LV2 | yes | identical | identical | 0 / 0 |
| the 8 LV2 rows (the 7 and caps PlateX2) | LV2 | yes | differs at sample 0, -24.5 dB | differs at sample 0, -26.8 dB | 0 / 0 |

- Dragonfly Room (CLAP) and caps PlateX2 (LV2) are not deterministic between two instances of themselves: paired
  with omx-eq8, two one-client-per-plugin arms differ at -30.2 dB and -31.1 dB, while the chain matched the series in
  those same runs. With either in the set no arm can be judged by `cmp`; the chain's difference is the control's size.
  Every other plugin of the table is bit-identical through the chain.
- lsp compressor, paired with omx-eq8, three runs: the chain differed from the series by one ulp (-142 dB) in the
  first, its blocks then 16-byte aligned; in the two after, the blocks 64-byte aligned, the chain was identical and
  the control differed by the same one ulp. Two instances of the plugin round apart now and then, chain or not; the
  six-plugin set above was identical in both comparisons.
- The latency the chain reports is the members' sum: the fixture's three passthroughs report 64 frames each, the
  chain's output 192 192, as the series' last output.
- Sabotaged: the members skipped, omx N=4 differs at sample 0 (-0.0674 against -0.2036); the latency sum dropped,
  the chain reports 64 64 against 192 192.

## require_pins: does a chain need a union-narrowed world?

Yes. plugin-hostd narrows a worker's `LV2_PATH` to the plugin's own bundle ([doc/reference.md](../doc/reference.md)),
and an LV2 plugin is found on that path at its add. A chain's members are added in the head's process, with the head's
`LV2_PATH`: in a world of the first plugin's bundle alone (a directory holding that bundle only) the chain's second
member answers `resp -101`, not found. A chain of LV2 plugins needs its worker's `LV2_PATH` to be the union of its
members' bundles (one directory holding those bundles alone, as the test makes it), and nothing wider.

The pins themselves need nothing new: `pin_expect` names an instance, and a member's add checks its own layout as any
add does. With `CHAIN_PINS=1` every member of the 7- and 8-plugin LV2 chains refused a wrong pin (`resp -510`,
`PHD_ERR_PIN_LAYOUT_MISMATCH`) and took its right one; the pinned chain was bit-identical with the pinned series.

## The bench: the results

The raw rows are in [bench-chain-2026-10-21/](bench-chain-2026-10-21/) (`runs.csv`, `plugins.csv`, `machine.txt`;
`machine.txt`'s package lines query rpm, which knows nothing of the unpacked tree: omx-plugins 0.12.0).

```
PLUGIN_ROOT=<the unpacked tree> make bench BENCH_PATHS="clap-series chain-clap adapter-series chain-adapter" \
    BENCH_RATES="96000 48000" BENCH_QUANTA="64 128" BENCH_REPS=3 BENCH_SECONDS=30 BENCH_CRASH=0 BENCH_IDLE_HOLD=10 \
    BENCH_OUT=build/bench/results-chain
PLUGIN_ROOT=... PLUGIN_NAMES="omx-eq8 omx-comp omx-deesser omx-delay" make bench BENCH_PATHS="clap-series chain-clap" ...
python3 -I bench/summary.py build/bench/results-chain
```

The arms interleaved inside each repetition. The node's load average was 2.9 before and 3.1 after on 4 threads: the
pod shares its machine, and the interleaving lands that load on both arms. Hold 10 asked, no device (above).

Series / chain, median of 3 repetitions of 30 s; change = (chain - series) / series:

| rate | quantum | plugins | cycle end p50 us | change | cycle end p99 us | change | overruns | probe late | xruns probe/driver | sched p50 us | host CPU % | RSS MiB |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 96000 | 64 | 7 CLAP | 336.8 / 187.6 | -44.3% | 776.0 / 570.5 | -26.5% | 7390 / 241 | 1509 / 162 | 0/0 / 0/0 | 7.2 / 7.6 | 20.8 / 12.4 | 42.4 / 34.7 |
| 96000 | 128 | 7 CLAP | 461.3 / 261.1 | -43.4% | 1256.7 / 693.7 | -44.8% | 172 / 71 | 79 / 48 | 0/0 / 0/0 | 8.4 / 8.9 | 16.6 / 10.0 | 42.5 / 34.4 |
| 48000 | 64 | 7 CLAP | 452.7 / 257.1 | -43.2% | 1181.8 / 679.2 | -42.5% | 113 / 86 | 65 / 47 | 0/0 / 0/0 | 10.0 / 11.6 | 14.6 / 8.6 | 41.9 / 33.9 |
| 48000 | 128 | 7 CLAP | 687.4 / 345.5 | -49.7% | 1557.3 / 857.5 | -44.9% | 23 / 4 | 23 / 20 | 0/0 / 0/0 | 14.0 / 20.3 | 11.7 / 7.8 | 42.0 / 34.3 |
| 96000 | 64 | 8 LV2 | 391.2 / 171.1 | -56.3% | 799.7 / 545.3 | -31.8% | 9222 / 98 | 1711 / 92 | 0/0 / 0/0 | 8.1 / 8.7 | 22.1 / 10.4 | 29.4 / 20.2 |
| 96000 | 128 | 8 LV2 | 491.4 / 223.0 | -54.6% | 1204.4 / 649.1 | -46.1% | 102 / 18 | 55 / 11 | 0/0 / 0/0 | 9.6 / 10.0 | 16.4 / 9.6 | 29.4 / 20.2 |
| 48000 | 64 | 8 LV2 | 457.7 / 217.6 | -52.5% | 1146.0 / 631.5 | -44.9% | 44 / 14 | 35 / 15 | 0/0 / 0/0 | 9.8 / 10.3 | 14.1 / 8.0 | 29.2 / 19.9 |
| 48000 | 128 | 8 LV2 | 583.3 / 282.5 | -51.6% | 1402.0 / 721.6 | -48.5% | 8 / 3 | 6 / 4 | 0/0 / 0/0 | 10.3 / 17.6 | 8.1 / 5.3 | 29.3 / 20.1 |

- Cycle end p50 falls 43-50% for the 7 CLAP plugins and 52-56% for the 8 LV2 ones, p99 27-49%. Rep by rep the
  ranges do not overlap: the chain's worst repetition is below the series' best, p50 and p99, in all eight rows.
- Overruns and late probe cycles fall in every row by the median; two rows overlap rep by rep (7 CLAP 96000/128: the
  chain's worst 100 against the series' best 69; 8 LV2 48000/128: 7 against 4). No xrun in any run, either arm.
- Cycle end max is the profiler's ceiling in both arms (933, 1867, 3733 us: 1.4 periods), the same.
- sched p50 is a node's wake-up to its start, the median over the arm's nodes: the chain's one node waits as long as
  the series' first or longer (14.0 against 20.3 us at 48000/128 CLAP) and then runs the whole series without another
  wake-up; it is no cost the cycle end does not already count.
- The host's CPU falls by a third or more (20.8 % to 12.4 % of a core at 96000/64 CLAP), its RSS by 8-9 MiB: N-1 JACK
  clients fewer, their threads and buffers.
- The pad's latency is 0..0 in every chain run. Three series runs at 96000/64 read 0..64, one cycle late at least
  once (7 CLAP rep 1, 8 LV2 reps 1 and 2): the rows whose series overran thousands of times.

N=4, omx-eq8 omx-comp omx-deesser omx-delay (CLAP), the same way ([bench-chain-2026-10-21/n4/](bench-chain-2026-10-21/n4/)):

| rate | quantum | cycle end p50 us | change | cycle end p99 us | change | overruns | probe late | xruns probe/driver | sched p50 us | host CPU % | RSS MiB |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 96000 | 64 | 239.2 / 166.1 | -30.6% | 685.6 / 541.5 | -21.0% | 1175 / 98 | 438 / 87 | 0/0 / 0/0 | 9.2 / 10.2 | 11.8 / 7.9 | 15.8 / 12.1 |
| 96000 | 128 | 272.9 / 173.4 | -36.5% | 852.7 / 599.8 | -29.7% | 17 / 9 | 15 / 11 | 0/0 / 0/0 | 9.8 / 8.1 | 7.4 / 4.5 | 15.6 / 12.2 |
| 48000 | 64 | 262.9 / 184.0 | -30.0% | 860.0 / 605.0 | -29.6% | 11 / 12 | 15 / 10 | 0/0 / 0/0 | 8.8 / 9.0 | 7.4 / 3.8 | 15.6 / 12.1 |
| 48000 | 128 | 444.2 / 273.4 | -38.5% | 1019.0 / 678.2 | -33.4% | 4 / 5 | 6 / 4 | 0/0 / 0/0 | 15.5 / 16.3 | 5.7 / 3.6 | 15.6 / 12.2 |

- Cycle end p50 falls 30-39%, p99 21-33%; rep by rep the chain's worst is below the series' best in every row.
- Overruns: two rows have the chain's median one above the series' (48000/64: 12 against 11, the repetitions 14 8 12
  against 11 15 10; 48000/128: 5 against 4, the repetitions 5 5 2 against 4 7 3), out of about 22500 and 11250
  cycles in the window. The repetitions overlap whole; at 96000 the chain overruns less (98 against 1175 at quantum
  64, 9 against 17 at 128).

## Verdict against the bar

- Bit identity: held. Every plugin of the table that is deterministic between two instances of itself (5 omx CLAP,
  lsp compressor, 7 LV2 with their pins) is bit-identical through the chain, and the latency is the members' sum.
  Dragonfly Room and caps PlateX2 differ between any two instances; the chain differs from the series by as much as
  the series from itself.
- Cycle end at hold 10 (asked; the pod has no device): p50 -30% to -56%, p99 -21% to -49%, in all twelve rows.
- Nothing worse: p99 and xruns, in no row. Overruns, by the median, in two of the twelve rows, by one overrun each,
  within the spread of the repetitions (N=4 at 48000, above); in the other ten the chain overruns less, at 96000/64
  12 to 94 times less.
- Not measured: the holds off and 0, and the hold itself (no `/dev/cpu_dma_latency` in the pod), and a realtime
  graph (`ulimit -r` 0). #105's question, whether the saving is the idle-state exit, is open until the same bench runs
  on the desk at off, 10 and 0.

The question for the operator: do two N=4 rows whose median overruns are one higher (12 against 11, 5 against 4),
within the spread of three repetitions, count as "worse" under the bar? If they do, the gate needs those two rows run
again with more repetitions, or on the desk under FIFO, before S1. If they do not, the chain passes: bit-identical, a
saving in every row, and nothing worse.
