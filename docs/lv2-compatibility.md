LV2 compatibility
=================

OpenMixer hosts LV2 plugins through its CLAP host. Every LV2 plugin that today's in-process host
admits (1×1 or 2×2 audio, control ports, the latency port, the worker, the default state) is meant
to run bit-identically through the adapter, `libomx-clap-lv2`. The proof of that is the per-plugin
identity run in the console, at every declared rate and quantum; until its results are published,
this page claims only what the tests of this repository prove. MIDI, transport, state and presets
are supported where the matrix below names a passing test. Not supported:
CV ports, atom messages other than MIDI and the numeric `patch:` properties, fixed or power-of-two
block lengths. A latency
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
| `lv2_host_test` | `lv2/tests/lv2_host_test.c`: bundles, ports, refusals and features against the fixture bundles, and the feature options as the feature probe sees them through the console's instances |
| `lv2_features_test` | `lv2/tests/lv2_features_test.c`: the feature provider of a host of its own (`omx_lv2_features.h`), each option as the feature probe sees it, no lilv |
| `lv2_clap_test` | `lv2/tests/lv2_clap_test.c`: the CLAP face, and libomx-clap-core's CLAP body around it |
| `lv2_link_test` | `lv2/tests/lv2_link_test.c`: a program built against the installed package |
| `lv2_patch_test` | `lv2/tests/lv2_patch_test.c`: the patch ports of the LV2 half, against the patch fixture bundle |

Ports
-----

| LV2 | in-process (CLAP adapter) | isolated worker (plugin-hostd → mod-host) |
|---|---|---|
| Audio, one input and one output | supported: one mono main port each way (`lv2_clap_test` fixture, `lv2_host_test` fixture) | unsupported |
| Audio, two inputs and two outputs | supported: one stereo main port each way (`lv2_clap_test` core: the 2×2 plugin on a mono lane) | unsupported |
| Audio, side chains (`lv2:isSideChain`, or `pg:sideChainOf` on a port or group) | supported: one CLAP input per side chain, of one or two channels, named by its group's label or its port's name, silent until the host binds a buffer to it (`lv2_clap_test` side chain, `lv2_host_test` side chain) | unsupported |
| Audio, any other layout (mono to stereo, more than two channels, an input that is neither main nor a side chain) | unsupported (stage 2); refused with the verdict's code (`lv2_host_test` and `lv2_clap_test` refusals) | unsupported |
| Port groups (`pg:mainInput`, `pg:group`, `pg:sideChainOf`) | supported for the main input and side chains: the members of `pg:mainInput` are the main pair, the members of one `pg:group` one CLAP input (`lv2_host_test` side chain). Output groups and other groups are read as ungrouped ports (unsupported) | unsupported |
| Control input ports | supported: a CLAP parameter each, id = port index, name, range and default from the TTL, written before the block's run (`lv2_clap_test` fixture) | unsupported |
| Control output ports | supported: a read-only CLAP parameter each, read back after every block (`lv2_clap_test` fixture, `lv2_host_test` fixture) | unsupported |
| Latency port (`lv2:latency`, `lv2:reportsLatency`) | supported: `clap.latency` (`lv2_clap_test` latency and fixture, `lv2_run_test` pad) | unsupported |
| A latency that changes while running | lossy: held until it has read unchanged for `latency_hold_ms`, then one restart; a change that comes back inside the hold is ignored (`lv2_clap_test` latency) | unsupported |
| The plugin's own bypass (`lv2:enabled`, a `bypass` port) | lossy: held at the value that keeps the plugin processing; the host's crossfade is the bypass. Turning the bypass off after a steady bypass resets the plugin (LV2 deactivate then activate, the worker joined before and started after) on the host's control thread, never in the audio path (`lv2_clap_test` fixture, core and worker, `lv2_host_test` fixture) | unsupported |
| CV ports | unsupported; refused with `hosting.features.cv-ports` (`lv2_host_test` refusals) | unsupported |
| Atom and event ports, MIDI (`midi:MidiEvent`) | supported: one MIDI input and one MIDI output, each a `clap.note-ports` port that offers the MIDI dialect only. Every channel message, sysex and system realtime byte arrives at its own length (a program change or channel pressure is two bytes) at its frame, and a frame past the block is held at its last. An input message past the atom buffer (at least 1024 bytes; `rsz:minimumSize` is not read yet) is dropped and counted as `midi_in_dropped`. An output message of up to three bytes, or a sysex, is passed on; anything else is not. A second MIDI port, a port that takes both MIDI and `patch:Message`, and an `ev:EventPort` are refused with `hosting.features.midi-in-fed-empty` (`lv2_clap_test` midi and refusals, `lv2_host_test` midi ports and midi run, `lv2_patch_test` MIDI twin) | unsupported |
| `time:Position` input | lossy: forged at frame 0 only when the host's transport changes (its fields but the frame); an unchanged transport forges nothing, no transport an empty sequence. Named by `lv2_clap_test` time: every key and value, the change rule, a withdrawal and a tempo-only transport (sabotaged: an always-forge, a dropped playing flag and a dropped bar start each go red) | unsupported |
| `patch:` numeric parameters | supported: a `patch:Set` forged before each block's run, `patch:Set` and `patch:Put` read back after it, the ids above the port indices (`lv2_patch_test`); the CLAP face lists them as parameters, a write lands before the block and the plugin's own value comes back as an output parameter event (`lv2_clap_test` patch face) | unsupported |
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
| `bufsz:fixedBlockLength`, `bufsz:powerOf2BlockLength` | unsupported by default; a plugin requiring one is refused with `hosting.features.missing` (`lv2_host_test` refusals and the configure arm). A host that pins its block declares them through the feature options' block declarations (`lv2_host_test` options, `lv2_features_test` block) | unsupported |
| `worker:schedule` and `worker:interface` | lossy: one worker thread per instance, which polls instead of being woken, so a response lands on a later block than in a host that wakes its worker (`lv2_run_test` worker, `lv2_host_test` worker, `lv2_clap_test` worker) | unsupported |
| `log:log` | lossy: answered and discarded by default, so a log call never blocks the audio thread (`lv2_host_test` worker). The feature options route a line to a host sink or capture it into a buffer, off the audio role only (`lv2_host_test` options, `lv2_features_test` log) | unsupported |
| `state:loadDefaultState` | supported (`lv2_host_test` fixture: -26 dB only with the default state restored) | unsupported |
| State save and restore (`state:interface` through `clap.state`) | supported: the state as LV2 Turtle, read after an activate and restored before the first one or by the restart a load while active asks for (`lv2_clap_test` state). Lossy: a load while active is applied by a restart, never by a call during `process()`; the bypass is not in the state. A state longer than 1 MiB is refused | unsupported |
| `state:mapPath` | unsupported (stage 3); a plugin that requires it is refused with `hosting.features.missing` (`lv2_clap_test` refusals) | unsupported |
| `state:makePath` | supported when the configured list names it and the configuration names a state root (`state_root`, an absolute directory the configure creates): each instance makes its paths in a directory of its own, `<state_root>/lv2-<pid>-<n>/` with an `n` no directory of the root holds yet, created on the first request with the directories the path names. Every directory of a path is made and opened without following a symbolic link. A request is answered NULL, counted in `make_path_refused`, and its code written to clap.log: `hosting.state.path-outside` for an empty or absolute path or one with a `..` component, even one such as `a/../b` that would come back inside; `hosting.state.path-symlink` when a component, the last one included, is a symbolic link; `hosting.state.path-too-long` for a path of `PATH_MAX` bytes or more, or a component longer than `NAME_MAX`; `hosting.state.path-on-audio` for a request on the audio thread; `hosting.state.path-unmakeable` when a directory cannot be made. A state save (`clap.state`) copies each file of that directory the plugin's state names to `<state_root>/lv2-<pid>-<n>.saved/`, and the state names the copy by its absolute path, as lilv writes a path; a load hands the plugin that path. The instance's directory is removed with the instance, and an instance never saved leaves nothing under the root. The copies a save made stay, since the saved state names them: nothing in the adapter removes them, so the root grows with every save that names a changed file. The host removes them when it deletes the state. `omx_clap_lv2_saved_dir` names a plugin's copy directory, and `omx_clap_lv2_saved_drop` removes one, refusing with `hosting.state.not-a-copy` any path that is not a `lv2-*.saved` directory directly under the state root. A saved state is valid only in the same directory tree: it names its copies by their absolute path under this state root, so it cannot be loaded on another machine, or under another root, without them at the same path (`lv2_clap_test` make-path). Without a state root it is not offered, and a plugin that requires it is refused with `hosting.features.missing` (`lv2_clap_test` make-path). The feature options do not withhold it | unsupported |
| `state:freePath` | not offered: a plugin frees a path it was handed with `free()`, as the specification says when the feature is absent | unsupported |
| Presets (`pset:Preset`) | supported: `clap.preset-load` with the preset's URI as the load key, held and applied as a state load is (`lv2_clap_test` preset); a preset that applies to another plugin is refused. The preset-discovery factory is not offered | unsupported |
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
