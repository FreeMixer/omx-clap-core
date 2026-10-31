# The chain object of libomx-clap-core (S1, #311)

`clap_chain.h` installs beside `clap_host.h` under `include/omx-clap-host/`: an ordered list of stages that one audio
cycle runs in order, each stage's output the next one's input, in the caller's own thread. It is the mechanics of the
Stage 0 prototype (`docs/bench-chain-2026-10-21.md`) moved into the library, so omx-clap-host (S2, #104, below), an
engine's in-process path and plugin-hostd (S3) run one implementation.

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
- nothing in the cycle allocates, locks, waits or prints. It raises a flag of its own and reads the published plan of
  the walk (the members in order) with one load, and lowers the flag when it is done.

A cycle longer than `OMX_CLAP_CHAIN_FRAMES_MAX` frames PASSES: `inputs` are copied to `outputs` dry, no member is run,
and the call answers `OMX_CLAP_CHAIN_PASSED` instead of `OMX_CLAP_CHAIN_OK`, for the caller to count. The prototype
passes the same cycle dry through its members; the library has no head that owns ports, so its whole chain passes.

## Changes while the cycles run

Every change is a control-thread call (`omx_clap_chain_add`, `_remove`, `_order`, `_bypass`, `_member_latency_set`,
`_refresh`), serialised by a lock the cycle never takes. A change that moves the walk builds the next plan in the
spare of two, publishes it with one store, and waits, polling, while the cycle's flag is up. The cycle's flag and load
and the change's store and poll are sequentially consistent, so either the cycle loads the new plan or the change sees
its flag and waits for it to leave: when the call returns, no cycle reads the old plan, and the next cycle runs the new
one.

`omx_clap_chain_remove(chain, id)` takes a member OUT: a member the caller removes, or one that crashed (the quarantine
itself is plugin-hostd's, S3). The chain closes around it: the member before it hands its output to the member after
it, and the cycle after the call returns is bit-identical with a chain built without that member. Once it returns the
chain never touches the member again, so the caller may close the instance or free the stage. An instance member is
stopped or restarted by the caller only after it is bypassed or removed: the chain does not gate a member's
deactivate.

`omx_clap_chain_order(chain, ids, n)` puts the members in a new order: `ids` names every member once, the first
first. The new order is ONE published walk, so a move never passes through a walk without the member: a removal and
an add at the new place publish two, and the cycles between them run without it (`t_move_gap` counts them: with
cycles of 16 frames spinning on a thread of their own, a third of the cycles of 2000 such moves ran without the moved
member, and none of 2000 moves by one order). A list that is not every member once is refused, `OMX_CLAP_CHAIN_ERR_ARG`
or `OMX_CLAP_CHAIN_ERR_NO_MEMBER`, and publishes nothing. The function is in the version node `OMX_CLAP_CORE_0.21`.

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
- a new order: one walk, the next cycle equals the members in series in that order, bit for bit; a list that is not
  every member once is refused and publishes nothing; a move by one order leaves no cycle without the member, where a
  removal and an add leave a counted gap;
- a cycle longer than the blocks passes dry and says so;
- every structure of the wrong `abi` or `size` is refused with `OMX_CLAP_CHAIN_ERR_ABI`, and nothing is written to it;
- the cycles on a thread of their own while the control thread bypasses, adds and removes members hundreds of times:
  every block is one whole walk, never part of the old one and part of the new.

`make test-chain` also holds the cycle's code (`pass_dry`, `omx_clap_chain_process`) to naming no allocator, sleep, lock
or printf. `make test-chain-sabotage` (`tests/chain_sabotage.sh`, in CI) breaks each guard in a copy of the tree and
sees the test red on the check that names it: a bypassed member left in the walk, a bypassed member's latency counted,
the sum replaced by the last member's, a change never told, a removal that does not publish the walk again, the long
cycle's bound one frame short, its dry copy dropped, one block for every member instead of two in turn, the ABI
refusal waved through, a new order never published, a new order published after a walk without a member, and a change that does not wait for the cycle reading the old plan, which `make
test-chain-tsan` (the test under ThreadSanitizer, in CI) reports as a race in `publish`.

## omx-clap-host runs the chain line (S2, #104)

plugin-hostd declares a chain with its `chain` verb (plugin-hostd#101, released in 0.7.0) and gives the chain one
worker. A worker that runs chains in one client, omx-clap-host for the `clap` and `lv2-clap` rows, is sent the same
line with the members it holds, in order: after the adds of its replay and before the state, the verb ledger and the
connections, and again without a member after that member's `remove`. omx-clap-host answers it:

    chain <chain> <instance> [<instance> ...]       resp 0

`<chain>` is plugin-hostd's number of the chain; the members are instances an `add` made in this host, in order.

### One client, one node

The members run in ONE JACK client: the client of the first member the line names, the chain's head. The head's main
input ports are the chain's input and its main output ports the chain's output; its note ports and auxiliary ports
stay its own. Every other member closes its own client, so it has no port and is no node of the graph: a chain of N
plugins is one node, beside the host's own client (`omx-clap-host`, the frame clock, no port). A member's note ports
go with its client too, and a line does not refuse for them (#400: a third-party LV2 effect such as Calf's declares an atom
port that supports MIDI, so it has a note input): the note verbs still reach the member through its ring, its note output
is dropped, and it gets its note ports again in a client of its own when it leaves. A member's auxiliary ports,
a side chain among them, move to the head's client as `<head client>:<member client>_aux_in_<p>_<k>` and
`_aux_out_<p>_<k>`, and the `connect` and `disconnect` verbs also take the name it has alone (`<member client>:aux_in_<p>_<k>`), which is what a replay
repeats: an unconnected side chain reads silence as it does alone, and a connected one is the member's own, bit for bit.
They go back to a client of its own when it leaves, and a link does not move with them: `plugin-hostd` replays its connections,
another controller connects again. The head keeps its own. A controller connects a chain by its head's
ports.

The head's process callback runs the library's chain (`omx_clap_chain_process`). Each member, the head too, is a stage
of the host's own (`omx_clap_chain_member.process`), not an instance member: the stage runs the instance inside its own
cycle gate exactly as a client of its own runs it (its note ring, its transport block, its gain adjustment, a restart
or a relayout shutting the cycles out), so a member's output in the chain is, bit for bit, its output in a client of
its own. Every member's audio thread is the head's; its block size follows the head's buffer-size callback.

### Per plugin

Bypass, parameters and every instance verb still name one member by its own number, and reach the instance as they do
in a client of its own. Bypass is the plugin's own (`bypass`, `:bypass`), not the chain's bypass flag: a bypassed
member keeps the latency it reports, as it does alone. Each member states its own latency to the chain
(`omx_clap_chain_member_latency_set`) whenever the instance publishes a new one, and the head's ports report the
chain's sum between them.

### What a line may name

Every member is an instance this host holds, named once and in no other chain of this host, and of the chain's width:
its main input and its main output as wide as the head's main output. Anything else answers `resp -902` with the reason on
stderr, or `resp -3` for an instance this host does not hold, and changes nothing: the members run one client each.
plugin-hostd then ends the chain and runs each member in a worker of its own, and answers its own `chain` with `resp -501`.
The protocol has no code of its own for these refusals; the reason is the
stderr line. A line of one member is a chain of one: the head in its own client, run through the chain.

### The declaration, and the refusal behind plugin-hostd (#361)

plugin-hostd's verbs file declares `chain` of omx-clap-host as `required`, and this host implements it: a line of two
or more members answers `resp 0`. `make test-host-verbs` installs plugin-hostd's package (`PLUGIN_HOSTD_DIR`) beside
this one and drives that line, so a host that refuses it, or a build without that package, goes red. A `resp -902`
the daemon logs (`refused chain <n> ...`) is one of the refusals above, whose reason this host writes to its own
stderr just before; the daemon's stderr alone does not name it. `state_load` answers one code for the whole directory;
the host's stderr names the instance and file that refused (`state_load: effect_<n> refused <file> (<code>)`).
A plugin may answer false to a state load while it processes, as most of omx-plugins do, so the core takes a running
instance out of processing for the load: its cycles pass the lane through for those blocks, with the plugin active and
its tails kept, and the next cycle runs it again.

### Changes while it runs

Every change is the library's acknowledged step (`omx_clap_chain_add`, `omx_clap_chain_remove`,
`omx_clap_chain_order`): when the call returns, no cycle runs the walk before it. No surviving member is restarted,
activated again or built again.

- A line for a chain already running whose head is the same: a member it leaves out leaves the walk first, one walk
  each, then runs in a client of its own (its ports registered again, its client activated); each of those walks runs
  the old order without the members gone so far, an order the line never named. Then the members it keeps take its
  order in ONE walk (`omx_clap_chain_order`); a member it names anew closes its client and is added at its place, one
  walk each, in the line's order. A pure move is that one walk, so it never runs a cycle without the moved member nor
  in an order between the old and the new; a line that also leaves members out runs the old order without them for a
  cycle or more before the move, and a line that adds k members publishes k walks after the order, each the line's
  order without the members still to join, so for a cycle or more each the chain runs without them: the order-bank
  flip (every change of a line in one acknowledged walk) is not what a line that removes or adds members does. A
  refusal of the order by the library (none of the host's lines is one, its checks come first) leaves the walk in
  the order it had and the line answers `resp -902`.
- A line with another head: the members it leaves out, and the new head when it is a member now, get clients of their
  own; the members it names again move straight from the old walk to the new one; the old head runs alone in its
  client again, or, when the line names it as a member, joins; the chain forms again in the new head's client.
- `remove` of a member that is not the head takes it out of the walk, the chain closing around it, then frees it.
  `remove` of the head gives every other member a client of its own and frees the head with its client; plugin-hostd's
  next line forms the chain again.

A refusal changes nothing: every check, every client of its own that a member leaving the walk goes back to, and a new
chain's library chain are made before the first change. A JACK failure part way frees no instance plugin-hostd still
counts: a client JACK cannot make for a member leaving refuses the line with nothing changed; a member named anew whose
own client JACK will not stop refuses it the same, the ones stopped before it started again; a member whose client JACK
will not activate stays in the walk, at its end, and the line answers the error. A line with another head is all or
none: every client of its own is made, every member named anew stopped and every member leaving moved into its client
(the last first) before anything else changes, and the old head, when the line names it as a member, is stopped last,
since a client JACK stops loses its connections. When JACK will not stop one or activate one, every member moved goes
back to its place in the old walk, the clients stopped run again, and the line answers the error (`resp -203`, `resp
-202`) with the chain, its order and every instance as they were. A `remove` of the head is the same: when JACK will not
make or activate a member's client of its own, the members are back in the walk and the remove answers the error.

After every change stderr says the chain as it stands: `chain <chain>: effect_<a> effect_<b> ... in effect_<a>'s
client, latency <n> frames, walk <w>`, `<w>` the walks the library has published for it (`omx_clap_chain_report`'s
revision), so a change's walks are the difference of two read-backs. The protocol has no read-back verb;
plugin-hostd's `chain_formed` event and `worker_list` are the read-back of a chain.

### A crash

A member that crashes takes the host process down, as any plugin of a shared worker does. The rest is plugin-hostd's:
the culprit is quarantined into a worker of its own (`instance_quarantined`), the chain's worker is respawned with the
others in their order, and the replay's `chain` line forms them again in one client (`chain_formed`). plugin-hostd
records a connection against the instance whose client the port names, so a chain's connections are its head's: a
quarantined member that is not the head takes none of them, and the chain rejoins connected.

### Proof

- `make test-chain-host` (`tests/chain_host_e2e.sh`, in a PipeWire of its own, the fake plugins of this tree): three
  instances are three nodes, the line makes them one (`pw-dump`), its output equals the same instances one client each
  linked in series bit for bit and differs from them in another order; the latency is the sum; a bypass and a parameter
  still reach one member, the head's and a member's bypass both; a member's removal, a member left out, added and
  moved (in one walk, the read-back's walk one more), and a new head each give the output of the chain built that way,
  bit for bit; every refusal changes nothing; a member's auxiliary ports are on the head's client, a connect by the name
  it has alone reaches them, and they leave with it (`make test-jack-fake`: the side chain and the auxiliary output carry their levels in a chain). The client's process
  callback and every function of this tree it reaches (`tests/rt_reach.py`) name no allocator, sleep, lock or printf.
- `make test-chain-host-tsan` (`tests/chain_host_tsan.sh`): the host and the core under ThreadSanitizer, 40 rounds of
  joins, a move, a member freed and added again, another head and the first one back, while the quantum changes all
  along (each change the head's buffer-size callback walking the members, each new client's thread-init callback
  naming its thread): no data race anywhere, nothing suppressed (every other report is PipeWire's `double lock` in
  `loop_locked`). No JACK thread writes an audio role. The thread-init callback names its thread with one atomic store
  into its instance's slot, `thread_named`, which the core's thread check reads with no lock (ABI 18,
  `omx_clap_host_name_slot`: the thread a slot holds is on the audio role from that store, whatever the role's
  `audio_thread` and `audio_role_held` say). The client's own instance reads its own slot; a member reads its head's
  (`chain_insert`), so a thread the head names later is the members' too. The callback takes no lock and never
  waits: under PipeWire it runs inside the client's first cycle, and that cycle runs the plugin on the audio role,
  whatever the control thread holds meanwhile. The role's own fields are the control thread's alone (the restart,
  resize and relayout give back what they saved, the core's bypass and parameter delivery take it and release it), and
  the slot is none of them, so nothing they give back covers a named thread. Round three's lock (the thread-init
  callback trying `g_chain_members` and, when the control thread held it, its client passing dry until the control
  thread applied the name) let a client lose its first cycles: two clients whose first cycles start together collide on
  the lock, and `make test-parity-lv2` read the host fixture's two instances 0.33 apart at 96 kHz on 7 runs of 20,
  every one of them with dry cycles logged (1 to 188), none on the 13 identical runs; 0 of 20 since. A head's
  buffer-size callback still tries `g_chain_members` to walk its members (a size it cannot record then waits for
  `chain_members_reconcile`). The process callback takes no lock.
- `make test-hostd-chain` (`tests/hostd_chain_e2e.sh`, plugin-hostd as the daemon and this host its CLAP worker): the
  daemon's chain of three is one node; the middle member, `tests/fault-14.clap`, crashes the worker on a parameter
  write; it is quarantined into a worker of its own and the other two rejoin in order, one node, their output bit for
  bit the two run one client each in that order; every recorder heard the source, so no identity is one of silence.
- `make test-chain-identity` (`tests/chain_identity.sh`): the chain declared by the line against the same plugins one
  client each.
- `make test-chain-host-sabotage` (`tests/chain_host_sabotage.sh`) breaks each guard of the host's chain in a copy of
  the tree and sees the test red on the check that names it: among them an allocation in a function the cycle calls
  and one in the core's `omx_clap_chain_process`, a member's auxiliary ports not made on the head's client, the verbs not finding them by the name they have alone, or their leaving not done, a move's order never
  published, a member's bypass that reaches the head alone, `g_chain_members` gone from every site and from the
  members' walk alone (each ThreadSanitizer's red), the thread-init callback's store into the slot gone and a joining
  member left on its own slot (each red on pass, `tests/fake.clap`, saying it ran off the audio role). Four sites no
  stress turns red: `chain_order`, `chain_extract` and `chain_insert` write only atomics, so a missing lock there is no
  data race ThreadSanitizer sees (the lock keeps a walk from seeing the members half shifted; `chain_insert` was red
  while it also wrote the joining member's role, green twice, 0 races, since that role reads its head's slot); and
  `chain_end`, red while JACK's walk read `chain_run` before a lock it waited on, green twice (0 races, 40 rounds each)
  since the walk reads it under the lock it only tries (the lock keeps a walk that loaded `chain_run` just before
  `chain_end` cleared it from reading the slot `chain_end` then clears, a window of a few loads). Its drills make one
  JACK call (or the library's order) fail for one instance (a leaving member's client not made, a member named anew not stopped,
  a leaving member's client not activated, the order refused; with another head a member named anew not stopped, a
  member left out not activated, the old head not stopped) and stay green: the line answers its code, no instance is
  freed, and the output is the chain the read-back names, bit for bit. Two of them stop a member named anew that is
  linked in a client of its own before the failure (a second member named anew not stopped; with another head a member
  left out not activated): the member runs again with its links, which `jack_deactivate` drops and the rollback makes
  again. One hold keeps the test green: every add holds `g_chain_members` across its client's `jack_activate` until that
  client's thread-init callback has returned, so a callback that waits on the lock fails the add.
- `make test-parity-lv2` (`tests/lv2_parity_fixture.sh`) runs the host fixture at 96 kHz `LV2_PARITY_REPEATS` times (10
  by default), a fresh pair of hosts each time, and wants it IDENTICAL to mod-host every time: a client cycle that
  passes dry where it should have run the plugin shows only on the runs where JACK's threads collide.

### What a member costs (r1)

`bench/chain-cost.sh`: N instances of `tests/fault-0.clap` (a -20 dB pad) in one omx-clap-host, one client each
against one chain, a PipeWire of its own at 48000 Hz, quantum 128, forced; three repetitions
(`docs/chain-cost-2026-10-25.csv`). r1: Intel Core Ultra 9 285K, Linux 7.2.11, PipeWire 1.6.10, a rootless container:
SCHED_OTHER, no CPU idle hold. The medians of the three:

| N | nodes own / chain | BUSY own, us (sum) | BUSY chain, us | host CPU own, % | host CPU chain, % | RSS own, KiB | RSS chain, KiB |
|---|---|---|---|---|---|---|---|
| 1 | 1 / 1 | 2.5 | 3.6 | 2.33 | 2.00 | 7000 | 7216 |
| 2 | 2 / 1 | 6.5 | 3.3 | 3.55 | 1.78 | 7904 | 7776 |
| 4 | 4 / 1 | 9.8 | 4.5 | 5.33 | 2.22 | 9420 | 9084 |
| 8 | 8 / 1 | 12.4 | 8.0 | 8.11 | 1.89 | 13400 | 11688 |
| 16 | 16 / 1 | 20.6 | 7.9 | 8.77 | 2.00 | 23052 | 18960 |

- A member of a chain adds about 0.3 us to its node's BUSY at quantum 128 ((7.9 - 3.6) / 15), against about 1.2 us
  of callback time per client of its own ((20.6 - 2.5) / 15). BUSY is pw-top's, noisy here: one repetition of the
  chain at N=16 read 35.7 us.
- The host's CPU stays near 2 % of a core whatever the chain's length; one client each it rises with the clients, to
  a median of 8.1 % at N=8 and 8.8 % at N=16 (single repetitions 3.3 to 13.1 %).
- A member costs about 780 KiB of RSS ((18960 - 7216) / 15), a client of its own about 1070 KiB.
- Not measured here: the wake-up of each node and the cycle's end, which need RT scheduling and a held idle state (the
  rig of #104's table, msi for #302's bench).
