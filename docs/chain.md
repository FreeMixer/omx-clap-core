# The chain object of libomx-clap-core (S1, #311)

`clap_chain.h` installs beside `clap_host.h` under `include/omx-clap-host/`: an ordered list of stages that one audio
cycle runs in order, each stage's output the next one's input, in the caller's own thread. It is the mechanics of the
Stage 0 prototype (`OMX_CLAP_HOST_CHAIN` in `src/effects.c`, `docs/bench-chain-2026-10-21.md`) moved into the library,
so omx-clap-host (S2, #104), an engine's in-process path and plugin-hostd (S3) run one implementation.

The chain is generic. It names no trim, fader, strip or bus: a profile that puts stages in a fixed order is its
caller's. Stage 0 measured what a chain is for: one JACK client and 22 to 40 percent less memory per plugin. It is not
a general latency win, and nothing here claims one.

## Members

A member is one of two kinds, given in `struct omx_clap_chain_member`:

- an instance the library hosts (`struct omx_clap_instance`, opened, activated and published by the caller), run by
  `omx_clap_host_run_ports`, its latency `omx_clap_host_latency`;
- a stage of the caller's own: a `process(ctx, inputs, outputs, frames)` function, and the latency the caller states.

Every member carries:

- its channel layout: its main input and output widths. Both must equal the chain's `channels`, fixed at create
  (refused `OMX_CLAP_CHAIN_ERR_WIDTH` otherwise). A stage that changes width (mono in, stereo out) stays outside a
  chain: with one width end to end, a bypass, a removal and an insertion can never leave two neighbours that do not
  meet.
- a bypass flag. A bypassed member is not in the cycle's walk at all: no call, no copy, and no latency. The member
  before it hands its output straight to the member after it.
- its reported latency in frames.

At most `OMX_CLAP_CHAIN_MEMBERS_MAX` (64) members, the prototype's `CHAIN_MAX`; one more is refused
`OMX_CLAP_CHAIN_ERR_FULL`. A member is named by the id `omx_clap_chain_add` returns, a number never reused by that
chain.

## Latency

The chain's latency is the SUM of its non-bypassed members' latencies, as the prototype's head reported its members'
sum between its ports. It is computed again at every change that can move it (an add, a removal, a bypass, a member's
latency stated again by `omx_clap_chain_member_latency_set`, the instance members' read again by
`omx_clap_chain_refresh`), and `latency_changed(ctx, frames)` from the configuration is called, on the control thread,
each time the sum differs from the last one reported. `omx_clap_chain_report` reads it at any time, with a revision
that counts the published changes.

## The cycle

`omx_clap_chain_process(chain, inputs, outputs, frames)`, in the audio thread, one thread at a time per chain:

- the first non-bypassed member reads `inputs`, the last writes `outputs`, and between them each member writes a block
  the chain allocated at create and reads the block before it. Two blocks alternate, so no member is handed its input as
  its output; each is `channels` channels of `OMX_CLAP_CHAIN_FRAMES_MAX` (8192) frames, every channel 64-byte aligned as
  a JACK port buffer is (`OMX_CLAP_CHAIN_ALIGN`), the prototype's `CHAIN_FRAMES_MAX` and `CHAIN_ALIGN`.
- a chain of one non-bypassed member hands it `inputs` and `outputs` themselves: it runs exactly as it would alone.
- a chain with no non-bypassed member copies `inputs` to `outputs` (nothing when they are the same buffers).
- nothing in the cycle allocates, locks, waits or prints. It reads one published plan of the walk (the members, their
  order and the blocks each reads and writes) with one acquire load.

A cycle longer than `OMX_CLAP_CHAIN_FRAMES_MAX` frames PASSES: `inputs` are copied to `outputs` dry, no member is run,
and the call answers `OMX_CLAP_CHAIN_PASSED` instead of `OMX_CLAP_CHAIN_OK`, for the caller to count. The prototype
passes the same cycle dry through its members; the library has no head that owns ports, so its whole chain passes.

## Changes while the cycles run

Every change is a control-thread call (`omx_clap_chain_add`, `_remove`, `_bypass`, `_member_latency_set`,
`_refresh`), serialised by a lock the cycle never takes. A change that moves the walk builds the next plan in the
spare of two, publishes it with one release store, and waits, polling, for the cycle that may still be reading the old
plan to leave: when the call returns, no cycle reads the old plan, and the next cycle runs the new one.

`omx_clap_chain_remove(chain, id)` takes a member OUT: a member the caller removes, or one that crashed (the quarantine
itself is plugin-hostd's, S3). The chain closes around it: the member before it hands its output to the member after
it, and the cycle after the call returns is bit-identical with a chain built without that member. Once it returns the
chain never touches the member again, so the caller may close the instance or free the stage. An instance member is
stopped or restarted by the caller only after it is bypassed or removed: the chain does not gate a member's
deactivate.

## ABI

Every structure a consumer allocates (`struct omx_clap_chain_config`, `struct omx_clap_chain_member`,
`struct omx_clap_chain_report`, `struct omx_clap_chain_member_info`) carries `abi` (`OMX_CLAP_CHAIN_ABI` of the header
the caller compiled against) and `size` (its `sizeof` as the caller compiled it) as its first two fields. The library
reads and writes such a structure only when its `abi` is one the library knows and its `size` is exactly the size that
ABI gives the structure; anything else is refused `OMX_CLAP_CHAIN_ERR_ABI`, nothing read and nothing written (#301:
never a silent read of a grown structure, never a write past the caller's own). The chain itself is opaque, allocated
and freed by the library. The structures are in `abi/libomx-clap-core.so.1.layout` and the functions in
`abi/libomx-clap-core.so.1.abi`, which the `abi` CI job holds the build to; the functions are in the version node
`OMX_CLAP_CORE_0.16`.

## Proof

`tests/clap_chain_test.c` (`make test-chain`, in `make test-fake`), over the fake plugins of this tree
(`tests/fake.clap`, `tests/fake_compressor.clap`) and stages of its own, asserting values:

- a chain's output equals the same members run one after another in series, each into a buffer of its own, bit for
  bit, over blocks of several sizes, and a chain of one member equals that member alone;
- the latency is the members' sum, and is re-reported when a member's latency changes;
- a bypassed member adds nothing: the output and the latency equal a chain built without it;
- a removed member: the next cycle equals a chain built without it, bit for bit;
- a cycle longer than the blocks passes dry and says so;
- every structure of the wrong `abi` or `size` is refused with `OMX_CLAP_CHAIN_ERR_ABI`, and nothing is written to it.

`make test-chain-sabotage` breaks each guard in turn (the bypass skip, the latency sum, the removal's close, the
long-cycle pass, the ABI refusal), sees the test fail, and restores it.
