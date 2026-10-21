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
  are as wide as that one's outputs and its main outputs as wide as the head's; anything else is refused, `resp -3`
  (`ERR_INVALID_OPERATION`), said on stderr. A plugin with a note input or a note output is refused as a member. A
  member's auxiliary inputs read silence and its auxiliary outputs go nowhere: what an unconnected auxiliary port of
  its own client would do.
- The head's process callback enters its own cycle gate and runs the head, then each member in order, each inside its
  own cycle gate (`omx_cycle_gate_enter`, `omx_clap_host_run_ports`, `omx_cycle_gate_leave`), handing each output
  buffer to the next member; the last member writes the head's output ports. Between them the outputs are blocks the
  host holds, `CHAIN_FRAMES_MAX` (8192) frames per channel: a longer cycle passes the head's output through the members
  dry.
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

The other paths still link every plugin in parallel.

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
