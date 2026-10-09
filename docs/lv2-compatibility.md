LV2 compatibility
=================

OpenMixer hosts LV2 plugins through its CLAP host. Every LV2 plugin that today's in-process host
admits (1×1 or 2×2 audio, control ports, the latency port, the worker, the default state) is meant
to run bit-identically through the adapter, `libomx-clap-lv2`. The proof of that is the per-plugin
identity run in the console, at every declared rate and quantum; until its results are published,
this page claims only what the tests of this repository prove. MIDI, transport, state and presets
are supported where the matrix below names a passing test: MIDI does, transport, state and presets do not yet. Not supported:
CV ports, non-MIDI atom messages, `state:makePath`, fixed or power-of-two block lengths. A latency
that moves under a parameter sweep is applied after a hold and a restart rather than per cycle.

Features the in-process adapter does not carry are available by running the plugin in an isolated
worker, where the matrix says so.

plugin-hostd runs every LV2 and CLAP plugin in a native host of its own format, with no
translation. A CLAP plugin can show its own window, or a panel the console draws from its CLAP
extensions. An LV2 plugin's modgui is planned in the console (until then it gets the generated panel); native LV2 desktop UIs are not hosted
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
  says unsupported in every feature row. `tests/clap_lv2_identity.sh` compares one plugin through
  mod-host and through omx-clap-host and is run on the desk, not in CI.

The tests named below:

| name | what it is |
|---|---|
| `lv2_run_test` | `lv2/tests/lv2_run_test.c`: the LV2 half of a block against the fake plugins |
| `lv2_host_test` | `lv2/tests/lv2_host_test.c`: bundles, ports, refusals and features against the fixture bundles |
| `lv2_clap_test` | `lv2/tests/lv2_clap_test.c`: the CLAP face, and libomx-clap-core's CLAP body around it |
| `lv2_link_test` | `lv2/tests/lv2_link_test.c`: a program built against the installed package |

Ports
-----

| LV2 | in-process (CLAP adapter) | isolated worker (plugin-hostd → mod-host) |
|---|---|---|
| Audio, one input and one output | supported: one mono main port each way (`lv2_clap_test` fixture, `lv2_host_test` fixture) | unsupported |
| Audio, two inputs and two outputs | supported: one stereo main port each way (`lv2_clap_test` core: the 2×2 plugin on a mono lane) | unsupported |
| Audio, any other layout (mono to stereo, more than two channels, side chains) | unsupported (stage 2); refused with the verdict's code (`lv2_host_test` and `lv2_clap_test` refusals) | unsupported |
| Port groups (`pg:Group`, `pg:mainInput`, `pg:sideChainOf`) | unsupported (stage 2) | unsupported |
| Control input ports | supported: a CLAP parameter each, id = port index, name, range and default from the TTL, written before the block's run (`lv2_clap_test` fixture) | unsupported |
| Control output ports | supported: a read-only CLAP parameter each, read back after every block (`lv2_clap_test` fixture, `lv2_host_test` fixture) | unsupported |
| Latency port (`lv2:latency`, `lv2:reportsLatency`) | supported: `clap.latency` (`lv2_clap_test` latency and fixture, `lv2_run_test` pad) | unsupported |
| A latency that changes while running | lossy: held until it has read unchanged for `latency_hold_ms`, then one restart; a change that comes back inside the hold is ignored (`lv2_clap_test` latency) | unsupported |
| The plugin's own bypass (`lv2:enabled`, a `bypass` port) | lossy: held at the value that keeps the plugin processing; the host's crossfade is the bypass. Turning the bypass off after a steady bypass resets the plugin (LV2 deactivate then activate, the worker joined before and started after) on the host's control thread, never in the audio path (`lv2_clap_test` fixture, core and worker, `lv2_host_test` fixture) | unsupported |
| CV ports | unsupported; refused with `hosting.features.cv-ports` (`lv2_host_test` refusals) | unsupported |
| Atom and event ports, MIDI (`midi:MidiEvent`) | supported: one MIDI input and one MIDI output, each a `clap.note-ports` port that offers the MIDI dialect only. Every channel message, sysex and system realtime byte arrives at its own length (a program change or channel pressure is two bytes) at its frame, and a frame past the block is held at its last. An input message past the atom buffer (at least 1024 bytes; `rsz:minimumSize` is not read yet) is dropped and counted as `midi_in_dropped`. An output message of up to three bytes, or a sysex, is passed on; anything else is not. A second MIDI port and an `ev:EventPort` are refused with `hosting.features.midi-in-fed-empty` (`lv2_clap_test` midi and refusals, `lv2_host_test` midi ports and midi run) | unsupported |
| `time:Position` | unsupported (stage 2; lossy when it comes, built from the CLAP transport) | unsupported |
| `patch:` numeric parameters | unsupported (stage 2) | unsupported |
| `patch:` file paths and strings | unsupported (stage 3) | unsupported |
| Other atom messages (an atom port that supports no MIDI) | unsupported; refused with `hosting.features.midi-in-fed-empty` (`lv2_host_test` midi ports) | unsupported |

Port properties
---------------

| LV2 | in-process (CLAP adapter) | isolated worker (plugin-hostd → mod-host) |
|---|---|---|
| `lv2:integer` | supported: `CLAP_PARAM_IS_STEPPED` (`lv2_clap_test` props) | unsupported |
| `lv2:toggled` | supported: stepped over 0..1 (`lv2_clap_test` props) | unsupported |
| `lv2:enumeration` with a label on every value | supported: `CLAP_PARAM_IS_ENUM`, the labels as the value text both ways (`lv2_clap_test` props) | unsupported |
| `lv2:enumeration` with labels on some values | lossy: stepped without `IS_ENUM`, since CLAP wants a label on every value; the labelled values still read their label (`lv2_clap_test` props) | unsupported |
| `pprops:notOnGUI` | supported: `CLAP_PARAM_IS_HIDDEN` (`lv2_clap_test` props) | unsupported |
| `pprops:trigger` | lossy: stepped; the value is not reset after the block (`lv2_clap_test` props) | unsupported |
| `pprops:logarithmic` | lossy: CLAP has no flag for it; the range is passed as it is (`lv2_clap_test` props) | unsupported |
| `pprops:rangeSteps`, `pprops:expensive`, `pprops:causesArtifacts` | unsupported | unsupported |
| No `lv2:default` | supported: the minimum is the default (`lv2_clap_test` props) | unsupported |
| Units (`units:unit`) | unsupported | unsupported |
| `lv2:inPlaceBroken` | supported: every port is the adapter's own memory, so a host that processes in place is safe (`lv2_run_test` and `lv2_clap_test` pad) | unsupported |

Features and extensions
-----------------------

| LV2 | in-process (CLAP adapter) | isolated worker (plugin-hostd → mod-host) |
|---|---|---|
| `urid:map` | supported: one table per process, answered from any thread (`lv2_host_test` worker, fixture) | unsupported |
| `urid:unmap` | supported (`lv2_host_test` worker) | unsupported |
| `options:options` | supported: `param:sampleRate`, `bufsz:minBlockLength`, `bufsz:maxBlockLength` and `bufsz:nominalBlockLength` as the activation says (`lv2_host_test` worker) | unsupported |
| `bufsz:boundedBlockLength` | supported (`lv2_host_test` worker) | unsupported |
| `bufsz:fixedBlockLength`, `bufsz:powerOf2BlockLength` | unsupported; a plugin requiring one is refused with `hosting.features.missing` (`lv2_host_test` refusals and the configure arm) | unsupported |
| `worker:schedule` and `worker:interface` | lossy: one worker thread per instance, which polls instead of being woken, so a response lands on a later block than in a host that wakes its worker (`lv2_run_test` worker, `lv2_host_test` worker, `lv2_clap_test` worker) | unsupported |
| `log:log` | lossy: answered and discarded, so a log call never blocks the audio thread (`lv2_host_test` worker) | unsupported |
| `state:loadDefaultState` | supported (`lv2_host_test` fixture: -26 dB only with the default state restored) | unsupported |
| State save and restore (`state:interface` through `clap.state`) | unsupported (stage 2) | unsupported |
| `state:mapPath` | unsupported (stage 3) | unsupported |
| `state:makePath`, `state:freePath` | unsupported | unsupported |
| Presets (`pset:Preset`) | unsupported (stage 2) | unsupported |
| `instance-access`, `data-access` | unsupported | unsupported |
| Required features outside the configured list | refused with `hosting.features.missing` before the binary is opened (`lv2_host_test` features, `lv2_clap_test` refusals) | unsupported |
| lilv loaded only when the first bundle is opened | supported: a missing lilv refuses the bundle and is retried next time (`lv2_host_test` lazy) | not applicable |

Plugin GUIs
-----------

| LV2 | in-process (CLAP adapter) | isolated worker (plugin-hostd → mod-host) |
|---|---|---|
| modgui | planned: the console draws an imported modgui once its importer and panel renderer land; until then the console shows its generated panel. Our own plugins' modgui is generated and checked (FreeMixer/omx-plugins `tools/modgui-test.sh` with `tools/modgui-gen.mjs --check`) for hosts that draw it | planned: the same console view |
| Native LV2 desktop UIs (`ui:X11UI`, `ui:GtkUI`, `ui:Qt5UI`) | not hosted yet | not hosted yet |
| No GUI | the console's panel generated from the plugin's parameters | the same |
