LV2 compatibility
=================

OpenMixer hosts LV2 plugins through its CLAP host. Every LV2 plugin that today's in-process host
admits (1×1 or 2×2 audio, control ports, the latency port, the worker, the default state) is meant
to run bit-identically through the adapter, `libomx-clap-lv2`. The proof of that is the per-plugin
identity run in the console, at every declared rate and quantum; until its results are published,
this page claims only what the tests of this repository prove. MIDI, transport, state and presets
are supported where the matrix below names a passing test; each atom message type is either carried or refused with
its reason (the atom rows below). A fixed block length is supported where the host declares it (omx-clap-host
does, under JACK; the console does not). Not supported: power-of-two
block lengths. Refused on purpose: the two features only a plugin's own UI needs (`instance-access`, `data-access`). A latency
that moves under a parameter sweep is applied after a hold and a restart rather than per cycle.

Features the in-process adapter does not carry are available by running the plugin in an isolated
worker, where the matrix says so.

omx-clap-host runs LV2 plugins through the same adapter, linked into it: `add lv2:<uri>`, or the
URI alone, finds the bundle on `LV2_PATH` and presents the plugin as one CLAP plugin on the ports a
CLAP plugin gets. [Through omx-clap-host](#through-omx-clap-host) says what that path does and the
test behind each row.

plugin-hostd runs every LV2 and CLAP plugin in a native host of its own format, with no
translation. A CLAP plugin has no window of its own: no code in omx-clap-core, plugin-hostd or the engine asks
a plugin for `clap.gui`, and the console draws a panel from the plugin's parameters. An LV2 plugin's modgui is planned in the console (until then it gets the generated panel); native LV2 desktop UIs are not hosted
yet. A plugin with no GUI gets the console's panel generated from its parameters.

How to read the matrix
----------------------

- **in-process (CLAP adapter)** is `libomx-clap-lv2`, the library this repository builds: an LV2
  plugin presented as a CLAP plugin to the console's engine or to omx-clap-host.
- **isolated worker (plugin-hostd → mod-host)** is the plugin run by mod-host in a process of its
  own.
- A cell says **supported** only when a test exercises the feature and passes in CI, and the cell
  names it. The tests are this repository's unless the cell names another one. Run them with
  `make test-lv2`.
- **lossy** means the feature works, but not exactly as the LV2 specification describes it; the
  cell says how it differs.
- **unsupported** means no test proves it yet. Where the adapter's plan adds it, the cell says at
  which stage.
- No mod-host feature is proven by a test of this repository yet, so the isolated-worker column
  says unsupported in every feature row. `tests/clap_lv2_identity.sh` runs one plugin through
  mod-host as the reference of the omx-clap-host rows below; it proves the output, not the features.

The tests named below:

| name | what it is |
|---|---|
| `lv2_run_test` | `lv2/tests/lv2_run_test.c`: the LV2 half of a block against the fake plugins |
| `lv2_host_test` | `lv2/tests/lv2_host_test.c`: bundles, ports, refusals and features against the fixture bundles, and the feature options as the feature probe sees them through the console's instances |
| `lv2_features_test` | `lv2/tests/lv2_features_test.c`: the feature provider of a host of its own (`omx_lv2_features.h`), each option as the feature probe sees it, no lilv |
| `lv2_clap_test` | `lv2/tests/lv2_clap_test.c`: the CLAP face, and libomx-clap-core's CLAP body around it |
| `lv2_link_test` | `lv2/tests/lv2_link_test.c`: a program built against the installed package |
| `lv2_patch_test` | `lv2/tests/lv2_patch_test.c`: the patch ports of the LV2 half, against the patch fixture bundle |
| `jack_lv2_e2e` | `tests/jack_lv2_e2e.sh` (`make test-jack-lv2`): omx-clap-host over jack in a PipeWire of its own, the fixture bundles on `LV2_PATH`, every value read back from the plugin's own meter port |
| `identity` | `tests/clap_lv2_identity.sh`: one plugin from the same input through mod-host, omx-clap-host's `lv2:` path and the adapter in process, compared bit for bit, then bypassed and compared with the input. `make test-identity-lv2` runs the fixture in CI; `make test-identity` runs omx-delay from the omx-plugins packages four ways, its CLAP twin through omx-clap-host the fourth, on the desk |
| `parity` | `tests/lv2_parity.py` (`tests/lv2_parity.sh` on the desk): every installed LV2 plugin that mod-host and omx-clap-host both add, through both from the same cycles of one input (a private PipeWire whose only driver is the feeder's clock, `tests/jack_parity.c`), default parameters, two instances per host; each plugin classed IDENTICAL (bit for bit), WITHIN-TOLERANCE (under 1e-6 and under 1e-5 of the peak, at no offset, the reason named), SILENT (silent through both: proves nothing), DIFFERS (the largest difference, where, a cause: a port one host lacks, an output or MIDI input count, an offset, randomness in one host only, an instance silent, a random plugin beyond its own spread), NONDETERMINISTIC (random in both hosts, which agree instance by instance or stay within its spread), CRASH or HANG (which host), with both hosts' latency and CPU time, and the coverage table's plugins with no parity named. `make test-parity-self` runs its rules on fake recordings and breaks each once, seen red; `make test-parity-lv2` runs the fixture bundles end to end at 48 and 96 kHz, every one IDENTICAL, and again with the adapter's recordings one sample late, which must read red |
| `lv2_locate_test` | `tests/lv2_locate_test.c` (`make test-lv2-locate`): which bundle on `LV2_PATH` the host opens for a URI |

Ports
-----

| LV2 | in-process (CLAP adapter) | isolated worker (plugin-hostd → mod-host) |
|---|---|---|
| Audio, one input and one output | supported: one mono main port each way (`lv2_clap_test` fixture, `lv2_host_test` fixture) | unsupported |
| Audio, two inputs and two outputs | supported: one stereo main port each way (`lv2_clap_test` core: the 2×2 plugin on a mono lane) | unsupported |
| Audio, side chains (`lv2:isSideChain`, or `pg:sideChainOf` on a port or group) | supported: one CLAP input per side chain, of one or two channels, named by its group's label or its port's name, silent until the host binds a buffer to it (`lv2_clap_test` side chain, `lv2_host_test` side chain) | unsupported |
| An instrument: a MIDI input, one or two audio outputs, no audio input | supported as a source: no CLAP audio input, a mono or stereo main output, the MIDI input its note input (below), played through `effect_<N>:midi_in` in omx-clap-host (`lv2_host_test` midi ports, `lv2_clap_test` instrument, `jack_lv2_e2e`) | unsupported |
| A generator: one or two audio outputs and no input of any kind (an oscillator, a noise or tone source) | supported as a source: no CLAP audio input, no note port, a mono or stereo main output of its own; `effect_<N>` has `out_<k>` and no input port, and a probe hears it (`lv2_host_test` sources, `lv2_clap_test` generator, `jack_lv2_e2e`) | unsupported: no test runs a source through mod-host yet |
| A MIDI source: a MIDI output and no input of any kind (an arpeggio or sequence generator) | supported as a pass-through tool: the strip's pair passes through untouched (`passthrough: host`), its notes on `effect_<N>:midi_out` (`lv2_host_test` sources, `lv2_clap_test` generator, `jack_lv2_e2e`) | unsupported: no test runs a source through mod-host yet |
| A control port alone: no audio port and no MIDI port either way | unsupported: nothing to host, refused `hosting.topology.no-audio-output` (`lv2_host_test` and `lv2_clap_test` refusals, `jack_lv2_e2e`) | unsupported |
| Audio, any other layout (mono to stereo, more than two channels, an input that is neither main nor a side chain) | unsupported (stage 2); refused with the verdict's code (`lv2_host_test` and `lv2_clap_test` refusals) | unsupported |
| Port groups (`pg:mainInput`, `pg:group`, `pg:sideChainOf`) | supported for the main input and side chains: the members of `pg:mainInput` are the main pair, the members of one `pg:group` one CLAP input (`lv2_host_test` side chain). Output groups and other groups are read as ungrouped ports, on purpose: such a group is a port of more than two channels, which the strip's topology does not take yet (#92) | unsupported |
| Control input ports | supported: a CLAP parameter each, id = port index, name, range and default from the TTL, written before the block's run (`lv2_clap_test` fixture) | unsupported |
| Control output ports | supported: a read-only CLAP parameter each, read back after every block, starting at the port's `lv2:default`, else its `lv2:minimum`, as mod-host starts it, for a plugin that reads its own output (`lv2_clap_test` fixture and props, `lv2_host_test` fixture) | unsupported |
| Latency port (`lv2:latency`, `lv2:reportsLatency`) | supported: `clap.latency` (`lv2_clap_test` latency and fixture, `lv2_run_test` pad) | unsupported |
| A latency that changes while running | lossy: held until it has read unchanged for `latency_hold_ms`, then one restart; a change that comes back inside the hold is ignored. The restart is CLAP's alone: the LV2 instance stays active and is not reset, so the plugin's history runs on through it, unless the rate, the block lengths the plugin was last told (a shorter block told through `options:interface` included) or a state load held for that activate change (`lv2_clap_test` latency, state; `lv2_host_test` keep). libomx-clap-core's restart after its warm-up stays a full LV2 deactivate and activate (`lv2_clap_test` core) | unsupported |
| The plugin's own bypass (`lv2:enabled`, a `bypass` port) | lossy: held at the value that keeps the plugin processing; the host's crossfade is the bypass. Turning the bypass off after a steady bypass resets the plugin (LV2 deactivate then activate, the worker joined before and started after) on the host's control thread, never in the audio path (`lv2_clap_test` fixture, core and worker, `lv2_host_test` fixture) | unsupported |
| CV ports (`lv2:CVPort`) | supported: each CV input one mono CLAP audio input after the side chains, each CV output one mono CLAP audio output after the main one, not main, named by the port's `lv2:name`; every frame of a block is passed both ways, bit-identical with the plugin run directly (`lv2_clap_test` cv, `lv2_host_test` cv). An input the host hands no buffer reads silence. More than 8 CV inputs or 8 CV outputs are refused with `hosting.features.cv-ports` (`lv2_host_test` cv) | unsupported |
| Atom and event ports, MIDI (`midi:MidiEvent`) | supported: one MIDI input and one MIDI output, each a `clap.note-ports` port that prefers the MIDI dialect (the input also takes CLAP note events, below; the output offers MIDI only). Every channel message, sysex and system realtime byte arrives at its own length (a program change or channel pressure is two bytes) at its frame, and a frame past the block is held at its last. An input message past the atom buffer (at least 1024 bytes; `rsz:minimumSize` is not read yet) is dropped and counted as `midi_in_dropped`. An output message of up to three bytes, or a sysex, is passed on; anything else is not. omx-clap-host writes the output's messages of up to three bytes to `effect_<N>:midi_out` at their frame (`jack_notes_out_e2e`); a sysex is counted there and not sent (`lv2_clap_test` midi and refusals, `lv2_host_test` midi ports and midi run). A second atom MIDI port each way is fed an empty sequence or drained, the first in index order the note path's (`lv2_host_test` midi ports, `lv2_clap_test` two-in and two-out), and a port that takes both MIDI and `patch:Message` is the patch port too (`lv2_clap_test` midi+patch, `lv2_patch_test` MIDI twin). An `ev:EventPort` (the event extension's port, atom's predecessor) that supports `midi:MidiEvent` is read the same way, input and output, over an `LV2_Event_Buffer` of the atom MIDI buffer's bytes, `uri-map` and `event` offered for it: a CLAP note on and a raw MIDI message reach the plugin at their frames and what it writes comes back at its frames, a message past the buffer is dropped and counted as `midi_in_dropped`, and a second event port each way is fed an empty buffer or offered one and never read (`lv2_host_test` event and event run, `lv2_clap_test` event). An event is at most 65516 bytes (its padded length counts in 16 bits): a longer input message is dropped and counted, and the output is read only inside the buffer the plugin was given, in frames, up to its event count, from the data the host handed it (`lv2_host_test` event run and event big buffer). A table whose `midi:MidiEvent` id is past 16 bits refuses an `ev:EventPort` plugin with `hosting.no-realisation` before it is instantiated (`lv2_host_test` event wide table). An `ev:EventPort` that supports no `midi:MidiEvent` is refused with `hosting.features.midi-in-fed-empty` (`lv2_host_test` event) | unsupported |
| `time:Position` on the MIDI input (a MIDI input that also supports `time:Position`) | supported: the same object as on a `time:Position` input, by the same rule, at frame 0 of the MIDI input's sequence, before the block's MIDI messages (`lv2_clap_test` atom midi-time) | unsupported |
| CLAP note events into the MIDI input | supported: the MIDI input's `clap.note-ports` port offers the CLAP dialect beside MIDI (MIDI preferred). `CLAP_EVENT_NOTE_ON` is a note on and `CLAP_EVENT_NOTE_OFF` a note off of the same channel and key, the velocity scaled to 0..127 and rounded (a note on never at 0, which MIDI reads as a note off). Lossy: `CLAP_EVENT_NOTE_CHOKE` is a note off of velocity 0, since MIDI has no choke; an event with a wildcard key or channel, a note expression, and a note on another port are not carried, since a MIDI message names one key on one channel (`lv2_clap_test` atom midi-time) | unsupported |
| `time:Position` input (the first sequence input that supports `time:Position` and not `patch:Message`, whatever else it lists, such as `atom:Sequence`) | supported: forged at frame 0 of every block while the transport plays, so the plugin reads each block's frame and beat; while stopped only when the transport changes, an unchanged one forging nothing; no transport, an empty sequence. `time:frame` is the frame count the host's blocks have run (CLAP's steady time), not a song position in frames, which CLAP's transport does not carry. Named by `lv2_clap_test` time: every key and value, the rule, a withdrawal and a tempo-only transport | unsupported |
| `patch:` numeric parameters | supported: a `patch:Set` forged before each block's run, `patch:Set` and `patch:Put` read back after it, the ids above the port indices, given in the order of the properties' URIs so that a plugin's ids are the same in every instance (`lv2_patch_test`, `lv2_host_test` atom ports). Earlier versions gave them in lilv's order: for a plugin with two or more such properties the ids may differ, so a session saved with an earlier version may map those parameters differently; the CLAP face lists them as parameters, a write lands before the block and the plugin's own value comes back as an output parameter event (`lv2_clap_test` patch face) | unsupported |
| `patch:` messages written as `atom:Blank`, and `patch:Put` with a `patch:body` | supported: an output `patch:Set` or `patch:Put` is read whether its object is an `atom:Object` or an `atom:Blank` (the older name of the same atom, which older plugins forge), and every numeric or text property of a `patch:Put`'s `patch:body` is read as a `patch:Set` of it is (`lv2_clap_test` atom text) | unsupported |
| `patch:` file paths and strings (`rdfs:range` `atom:Path` or `atom:String`) | supported through the plugin's own patch port: CLAP has no text parameter, so the adapter's `omx_clap_lv2_patch_text_*` functions list them, write one (a `patch:Set` at frame 0 of the next block) and read back the value the plugin last reported in a `patch:Set` or `patch:Put`. A value is at most 4095 bytes, and with its NUL at most the patch port's buffer less 80 bytes (the buffer is the configured `atom_buffer_bytes`, or the port's larger `rsz:minimumSize`): a longer one is refused with `hosting.patch.too-large`; a property the plugin does not list with `hosting.patch.not-writable`; a write while the previous one waits for its block with `hosting.patch.busy`. A value the plugin reports longer than 4095 bytes before its NUL, or with no NUL within 4096 bytes, is dropped and counted in `patch_dropped`; one of exactly 4095 bytes is read whole. A new LV2 instance (an activate at another rate) is sent the host's last value of each again (`lv2_clap_test` atom text) | unsupported |
| Other messages on the patch output (an object that is not a `patch:Set` or `patch:Put`, a property of another type) | ignored: CLAP has no counterpart for them (`lv2_clap_test` atom text) | unsupported |
| An atom output that is neither MIDI nor the patch output (a notify port for the plugin's own GUI) | supported, discarded: up to 4 such outputs, each offered a buffer of its own every block; what the plugin writes there has no CLAP counterpart and the adapter hosts no LV2 GUI, so it is not passed on. A fifth is refused with `hosting.features.midi-in-fed-empty` (`lv2_clap_test` atom text, `lv2_host_test` atom ports) | unsupported |
| The patch input when it lists more than `patch:Message` (LSP's `in_ui`: `atom:Sequence`, `patch:Message`, `time:Position`; dragonfly's `atom:String`, `patch:Message`) | supported: the first sequence input that supports `patch:Message` is the patch input, whatever else it lists; when it also supports `time:Position`, the block's transport object (the same object, by the same change rule, as on a `time:Position` input) is at frame 0 ahead of the block's `patch:Set` messages, and the buffer has room for it (`lv2_host_test` atom ui, `lv2_clap_test` atom ui) | unsupported |
| An atom input that is no MIDI input, no patch input and no time port (a GUI's control port: `atom:supports` naming nothing, or the plugin's own message types) | supported, fed empty: up to 4 such inputs, each a sequence buffer of its own (the larger of the atom buffer and its `rsz:minimumSize`), an empty sequence at every block. The LV2 atom rules let a host send nothing to a port; the adapter hosts no LV2 GUI, so nothing is sent. It is never presented as a MIDI port: no `clap.note-ports` port. An atom input that is no sequence (an `atom:bufferType` other than `atom:Sequence`), or a fifth such input, is refused with `hosting.features.atom-in-unfed`, not with the MIDI code (`lv2_host_test` atom ui and refusals, `lv2_clap_test` atom ui and refusals) | unsupported |
| The sequence unit of the inputs the adapter forges | `atom:frameTime`, as the LV2 atom specification names it: earlier versions wrote `time:frame` (a `time:Position` key) in the patch ports' sequence header; a plugin that checks the unit read those as not in frames (`lv2_host_test` atom ui) | unsupported |

### Why a CV port is an audio port

An LV2 CV port carries one float per frame, as an audio port does. The adapter presents it as a CLAP audio port, not
as a parameter-modulation input or output, because only an audio port keeps it exact: CLAP's parameter modulation
(`CLAP_EVENT_PARAM_MOD`) is an event that holds one value from its time on, so a signal that changes on every frame would
be quantised to the events a block carries, and CLAP has no modulation output at all, so a CV output would have nowhere
to go. CLAP has no CV port type either (its draft `clap.cv` extension is not in CLAP 1.2), so the port's type is
`CLAP_PORT_MONO` and its name says what it is. A host that runs the plugin through libomx-clap-core sees a CV input as an
auxiliary input it may bind (`omx_clap_host_bind_aux_input`) and a CV output as an auxiliary output, left unconnected.

Port properties
---------------

| LV2 | in-process (CLAP adapter) | isolated worker (plugin-hostd → mod-host) |
|---|---|---|
| `lv2:integer` | supported: `CLAP_PARAM_IS_STEPPED` (`lv2_clap_test` props) | unsupported |
| `lv2:toggled` | supported: stepped over 0..1 (`lv2_clap_test` props) | unsupported |
| `lv2:enumeration` with a label on every value | supported: `CLAP_PARAM_IS_ENUM`, the labels as the value text both ways (`lv2_clap_test` props) | unsupported |
| `lv2:enumeration` with labels on some values | lossy: stepped without `IS_ENUM`, since CLAP wants a label on every value; the labelled values still read their label (`lv2_clap_test` props) | unsupported |
| `pprops:notOnGUI` | supported: `CLAP_PARAM_IS_HIDDEN` (`lv2_clap_test` props) | unsupported |
| `pprops:trigger` | supported: stepped; set for one block, then back at its default before the next, the host told by a `CLAP_EVENT_PARAM_VALUE` (`lv2_clap_test` props) | unsupported |
| `pprops:logarithmic` | supported: CLAP has no flag for it, so the face states it in its `org.openmixer.param-contract/1`: a log scale over a positive travel (`lv2_clap_test` props, through the core's `omx_clap_host_param_meaning`) | unsupported |
| `pprops:rangeSteps` | supported: the contract's step, `(max - min) / (steps - 1)` (`lv2_clap_test` props) | unsupported |
| `pprops:expensive`, `pprops:causesArtifacts` | not passed, on purpose: CLAP has no counterpart, and they only tell a host what to avoid automating, which the console leaves to its user | unsupported |
| No `lv2:default` | supported: the minimum is the default (`lv2_clap_test` props) | unsupported |
| Units (`units:unit`) | supported for LV2's 24 units: the contract's unit, the console's code of the same name (`units:hz` is `hz`) (`lv2_clap_test` props). A unit the plugin defines in its own TTL reads as none | unsupported |
| No audio output: a MIDI tool (a MIDI output, no audio port) or an analyser (audio inputs, no output) | supported, the host's placement: the strip's audio passes through the stage bit for bit with no latency, named `passthrough: host` on the main ports and on clap.log; the tool's MIDI goes through the note path and the analyser's control outputs read as before (`lv2_clap_test` passthrough, `jack_lv2_e2e` the tool). A plugin with neither an audio output, a MIDI output nor an audio input is refused `hosting.topology.no-audio-output` | unsupported |
| More than one MIDI input or output port | supported: the first of each, in port index order, is the note path's; a later MIDI input is fed an empty sequence every block and a later MIDI output is offered a buffer and drained, up to 16 of each (carla midijoin, midisplit; `lv2_clap_test` two-in and two-out) | unsupported |
| `lv2:inPlaceBroken` | supported: every port is the adapter's own memory, so a host that processes in place is safe (`lv2_run_test` and `lv2_clap_test` pad). A plugin that lists it as a required feature is admitted, whatever the feature options (`lv2_run_test` rack: the in-place fake requires it; `lv2_clap_test` inplace; `lv2_features_test`) | unsupported |

Features and extensions
-----------------------

| LV2 | in-process (CLAP adapter) | isolated worker (plugin-hostd → mod-host) |
|---|---|---|
| `urid:map` | supported: one table per process, answered from any thread (`lv2_host_test` worker, fixture) | unsupported |
| `urid:unmap` | supported (`lv2_host_test` worker) | unsupported |
| `options:options` | supported: `param:sampleRate`, `bufsz:minBlockLength`, `bufsz:maxBlockLength` and `bufsz:nominalBlockLength` as the activation says (`lv2_host_test` worker) | unsupported |
| `options:interface` | supported: a shorter block that fits the buffers is told through the plugin's `set()`, the plugin kept; a longer block, a new rate, or a plugin with no interface is instantiated again (`lv2_clap_test` live) | unsupported |
| `bufsz:boundedBlockLength` | supported (`lv2_host_test` worker) | unsupported |
| `bufsz:fixedBlockLength` | supported where the host declares `OMX_LV2_BLOCK_FIXED`: omx-clap-host does, since under JACK every cycle is the buffer size and any change of it re-activates; the plugin is then told one length, and a cycle of another length is refused and counted apart from the plugin's faults (`lv2_clap_test` fixed, `jack_lv2_e2e` the shrink and the growth). Not declared, a plugin requiring it is refused `hosting.features.missing` (`lv2_host_test` refusals) | unsupported |
| `bufsz:powerOf2BlockLength` | unsupported by default; a plugin requiring it is refused with `hosting.features.missing` (`lv2_host_test` refusals and the configure arm). A host that pins its block to a power of two declares it through the feature options' block declarations (`lv2_host_test` options, `lv2_features_test` block) | unsupported |
| `worker:schedule` and `worker:interface` | supported: one worker thread per instance, woken by `schedule_work` (a semaphore post, which neither allocates nor waits), so a request is worked at once and its response lands in the next block; `worker_poll_us` is only the longest it waits unwoken (`lv2_run_test` worker woken, `lv2_run_test` worker, `lv2_host_test` worker, `lv2_clap_test` worker) | unsupported |
| `log:log` | lossy: answered and discarded by default, so a log call never blocks the audio thread (`lv2_host_test` worker). The feature options route a line to a host sink or capture it into a buffer, off the audio role only (`lv2_host_test` options, `lv2_features_test` log) | unsupported |
| `state:loadDefaultState` | supported: the plugin's `state:state` restored at each instantiation; a plugin whose description has none is restored nothing (`lv2_host_test` fixture: -26 dB only with the default state restored; `lv2_clap_test` live) | unsupported |
| State save and restore (`state:interface` through `clap.state`) | supported: the state as LV2 Turtle, read after an activate and restored before the first one or by the restart a load while active asks for (`lv2_clap_test` state). A load while active is applied by a restart, unless the plugin declares `state:threadSafeRestore`: then its `restore()` is called at once beside `process()`, handed the worker's schedule, and no restart is asked for (`lv2_clap_test` live). Lossy: the bypass is not in the state. A state longer than 1 MiB is refused | unsupported |
| `state:mapPath` | supported: offered at instantiate; a path under the state root is abstracted relative to it, any other kept whole, and made absolute again the same way (`lv2_clap_test` map-path: a file inside the root and one outside, each read back by a second instance). `save()` and `restore()` get lilv's own, which copies a saved file and names it by the copy (`lv2_clap_test` make-path) | unsupported |
| `state:threadSafeRestore` | supported: read from the plugin's features; see State save and restore | unsupported |
| `state:makePath` | supported when the configured list names it and the configuration names a state root (`state_root`, an absolute directory the configure creates): each instance makes its paths in a directory of its own, `<state_root>/lv2-<pid>-<n>/` with an `n` no directory of the root holds yet, created on the first request with the directories the path names. Every directory of a path is made and opened without following a symbolic link. A request is answered NULL, counted in `make_path_refused`, and its code written to clap.log: `hosting.state.path-outside` for an empty or absolute path or one with a `..` component, even one such as `a/../b` that would come back inside; `hosting.state.path-symlink` when a component, the last one included, is a symbolic link; `hosting.state.path-too-long` for a path of `PATH_MAX` bytes or more, or a component longer than `NAME_MAX`; `hosting.state.path-on-audio` for a request on the audio thread; `hosting.state.path-unmakeable` when a directory cannot be made. A state save (`clap.state`) copies each file of that directory the plugin's state names to `<state_root>/lv2-<pid>-<n>.saved/`, and the state names the copy by its absolute path, as lilv writes a path; a load hands the plugin that path. The instance's directory is removed with the instance, and an instance never saved leaves nothing under the root. The copies a save made stay, since the saved state names them: nothing in the adapter removes them, so the root grows with every save that names a changed file. The host removes them when it deletes the state. `omx_clap_lv2_saved_dir` names a plugin's copy directory, and `omx_clap_lv2_saved_drop` removes one, refusing with `hosting.state.not-a-copy` any path that is not a `lv2-*.saved` directory directly under the state root. A saved state is valid only in the same directory tree: it names its copies by their absolute path under this state root, so it cannot be loaded on another machine, or under another root, without them at the same path (`lv2_clap_test` make-path). Without a state root it is not offered, and a plugin that requires it is refused with `hosting.features.missing` (`lv2_clap_test` make-path). The feature options do not withhold it | unsupported |
| `state:freePath` | supported: offered at instantiate, frees a path the host's path features handed out (`lv2_clap_test` map-path) | unsupported |
| `uri-map` and `event` (the event extension's features, deprecated upstream) | supported: offered at instantiate from the configured list, last in the array; `uri-map` answers `urid:map`'s id under the event map and under another (`lv2_host_test` options), except that under the event map an id past 16 bits, which no event type holds, is 0, uri-map's "no mapping" (`lv2_host_test` event wide table); `event`'s `lv2_event_ref` and `lv2_event_unref` answer 0, since no event the adapter writes is reference-counted (`lv2_host_test` options); a list without `uri-map` refuses a plugin requiring it with `hosting.features.missing` (`lv2_host_test` without uri-map) | unsupported |
| Presets (`pset:Preset`) | supported: `clap.preset-load` with the preset's URI as the load key, held and applied as a state load is (`lv2_clap_test` preset); a preset that applies to another plugin is refused. Listed through the preset-discovery factory the entry offers: one provider, the bundle a `PLUGIN` location, each `pset:Preset` by its `rdfs:label` with its URI as the load key, for the plugin it applies to (`lv2_clap_test` preset list) | unsupported |
| `instance-access`, `data-access` | refused, on purpose: only a plugin's own UI needs them, and the console never shows one (GUI ruling 2026-10-19) | unsupported |
| Required features outside the configured list | refused with `hosting.features.missing` before the binary is opened (`lv2_host_test` features, `lv2_clap_test` refusals) | unsupported |
| lilv loaded only when the first bundle is opened | supported: a missing lilv refuses the bundle and is retried next time (`lv2_host_test` lazy) | not applicable |

Through omx-clap-host
---------------------

| What | omx-clap-host |
|---|---|
| `add lv2:<uri> <N>`, and the URI alone as mod-host takes it | supported: the bundle on `LV2_PATH` whose manifest names the URI, whole or as a prefixed name, and whose plugin list holds it; the client `effect_<N>` with `in_<k>`/`out_<k>`, and `midi_in` for a plugin with an LV2 MIDI input, as for a CLAP plugin (`jack_lv2_e2e`, `lv2_locate_test`) |
| A URI no bundle holds | `resp -101` (`jack_lv2_e2e`) |
| A plugin the adapter refuses | `resp -102 <code>`, the code also on stderr: `hosting.topology.wider-than-strip`, `hosting.features.midi-in-fed-empty` (among its causes an `ev:EventPort` that supports no `midi:MidiEvent`; one that does is read, above), `hosting.features.atom-in-unfed`, `hosting.features.missing`, `hosting.topology.no-audio-output` (no audio output, no MIDI output and no audio input: nothing to host), and `hosting.no-realisation` for a plugin whose binary cannot be loaded (`jack_lv2_e2e`); the adapter keeps the code for the host, `omx_clap_lv2_last_refusal` (`lv2_clap_test` refusals) |
| The default state (`state:loadDefaultState`) | supported (`jack_lv2_e2e`: -26 dB with the -6 dB of the default state) |
| Control input ports | supported: `param_set` and `param_get` by the port's `lv2:symbol` or by its CLAP id, the port index; `param_info` by either (`jack_lv2_e2e`, `lv2_clap_test` fixture: `omx_clap_lv2_param_symbol`) |
| Control output ports | supported: `param_get` by symbol; `monitor_output <N> <symbol>` sends `output_set` when the value moves (`jack_lv2_e2e`) |
| `lv2:enabled` | supported: the host's bypass, its one-block crossfade; `param_set <N> enabled 0` bypasses, `1` does not, `param_get <N> enabled` reads the host's bypass whichever verb set it (`jack_lv2_e2e`, `identity` bypass) |
| State save and restore | supported: `state_save` writes the LV2 state as Turtle to `effect_<N>.clapstate`, `state_load` restores it (`jack_lv2_e2e`) |
| `state:makePath` | supported: the host's state root is `-s <dir>`, else `$XDG_STATE_HOME/omx-clap-host`, else `~/.local/state/omx-clap-host`, made at start; each instance makes its paths in `<root>/lv2-<pid>-<n>/`, removed with the instance, and a save copies the files its state names to `<root>/lv2-<pid>-<n>.saved/`, which stays (`jack_lv2_e2e`: the path fixture saves its level to `takes/level.txt`, a load reads it back as its gain, `remove` takes the instance's directory and leaves the copy). A root that is not a directory the host can write, or cannot be made, is said on stderr and makePath is not offered: a plugin that requires it answers `resp -102 hosting.features.missing`, and every other LV2 plugin still adds (`jack_lv2_e2e`, `-s` under a file). Nothing in omx-clap-host removes the copies |
| Presets by URI | supported: `preset_load <N> <preset uri>`; a preset of another plugin or one the bundle does not hold answers `resp -104` (`jack_lv2_e2e`) |
| `remove` | supported: no port of `effect_<N>` stays in the graph (`jack_lv2_e2e`, `identity`) |
| Output bit-identical to mod-host and to the adapter in process | supported for the fixture in CI and for omx-delay on the desk, wet and bypassed (`identity`); omx-delay's LV2 build is bit-identical with its CLAP twin through omx-clap-host as well |
| lilv | not linked: the adapter loads it on the first LV2 add (`make test-jack-lv2` reads the binary's `NEEDED` entries; CI's clean-root install checks `ldd`) |
| A latency the plugin reports after its first activate | lossy, as in process: held for 250 ms (`CLAP_HOST_LATENCY_HOLD_MS`), then the plugin is restarted, its LV2 instance kept active and not reset, and the stage crossfades through the restart (`identity` waits for it before it plays) |

Plugin GUIs
-----------

| LV2 | in-process (CLAP adapter) | isolated worker (plugin-hostd → mod-host) |
|---|---|---|
| modgui | planned: the console draws an imported modgui once its importer and panel renderer land; until then the console shows its generated panel. Our own plugins' modgui is generated and checked (FreeMixer/omx-plugins `tools/modgui-test.sh` with `tools/modgui-gen.mjs --check`) for hosts that draw it | planned: the same console view |
| Native LV2 desktop UIs (`ui:X11UI`, `ui:GtkUI`, `ui:Qt5UI`) | not hosted yet | not hosted yet |
| No GUI | the console's panel generated from the plugin's parameters | the same |
