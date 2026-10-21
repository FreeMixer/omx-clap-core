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
