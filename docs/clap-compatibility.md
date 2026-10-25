CLAP compatibility
==================

What of CLAP 1.2 the host supports, extension by extension, and the test behind each claim. The
LV2 counterpart is [lv2-compatibility.md](lv2-compatibility.md); the reference for the API is
`doc/reference.md`, whose table of CLAP extensions says the same per header and this page per id.

The hosting core, `libomx-clap-core`, holds every rule below: the host object it hands a plugin,
the extensions it asks of the plugin, and what it does with each. omx-clap-host is a program on
that core, and so is the console's in-process stage. Where the three differ, [Surfaces](#surfaces)
says so; everywhere else a row holds for all of them.

How to read the tables
----------------------

- One row per id: every id the installed CLAP headers spell, stable and draft (a draft id the host
  answers or asks for is a row of its own), then the factories. `tests/clap_doc_check.sh` fails when
  the headers spell an id this page has no row for. The headers are the installed `clap-devel`: 1.2.10 on msi, 1.2.7 in CI, which spells `clap.transport-control/1` where 1.2.10 spells `/2` (both have rows). On msi they are 63
  ids, 58 of extensions under `ext/` and `ext/draft/` and 5 of factories; the 9 rows of
  [Ids outside the headers](#ids-outside-the-headers) come beside them. A runner with another set
  is held to the ids it finds; the rows stay.
- **host-side**: the extension of the host object, what a plugin gets when it calls the host's
  `get_extension` with the id. **plugin-side**: the plugin's extension, what the host asks the
  plugin for and does with it. "—" where CLAP defines no such half.
- **supported**: a test in CI exercises it and passes; the cell names the test (`tests/<file>`,
  `tests/<file>:<function>`, or `make <target>`). The jack tests (`make test-jack-*`) run in CI
  where the runner grants a private user namespace, and say SKIPPED where it does not.
- **lossy**: it works, but not as the CLAP header describes; the cell says how.
- **offered, untested** (host-side) and **read, untested** (plugin-side): the code does it and no
  test exercises it yet. That is not supported.
- **refused**: the plugin is not opened, with the hosting code named.
- **out of scope**: ruled out, with the ruling. The console draws every panel itself, so a
  plugin's own window and every extension that only serves one is out of scope there; the
  standalone omx-clap-host alone shows a plugin's window, on request (`clap.gui`).
- **unsupported**: not done; the issue that plans it, when one does.
- **unruled (#207)**: a draft nobody has ruled on yet. #207 asks for the ruling.
- **asked on msi**: how many of the hosted plugins on the desk asked the host for the id and were
  not given it ([Measured](#measured)). The probe counts only the asks the host does not answer,
  so an offered id reads "offered" there, and "—" means no plugin asked.

An id the host does not offer answers NULL, and the host's log says `extension not offered: <id>`
once per instance and id (`tests/clap_host_test.c:unoffered_checks`, `tests/clap_unoffered_test.c`):
a plugin must cope with a host that lacks it, as CLAP requires.

The tests named below:

| name | what it is |
|---|---|
| `tests/clap_host_test.c` | the core against `tests/fake.clap`, `tests/fake_synth.clap` and `tests/fake_mover.clap`, no jack (`make test-fake`); each function named is one group of its checks |
| `tests/clap_ext_test.c` | the host extensions of a full host against `tests/fake_ext.clap`: timers, descriptors, the thread pool, render, the tail, sleep, state-context, audio-ports-activation (`make test-fake`) |
| `tests/clap_core_test.c` | the core over plugins built into the test: the audio role, the side chain, the meters, the transport (`make test-fake`) |
| `tests/clap_unoffered_test.c` | the log of ids the host did not offer, from the audio role and the control thread (`make test-fake`) |
| `tests/clap_scan_test.c` | omx-clap-scan against the fakes, `tests/fake_panel.clap`'s published panel among them, and a plugin that crashes (`make test-fake`) |
| `tests/param_contract_test.c` | the rules of `org.openmixer.param-contract/1` (`tests/param_contract_test.c`, in `make test-fake`) |
| `tests/panel_abi_test.c` | `org.openmixer.panel-description/1` as `src/omx_clap_ext.h` states it, against omx-plugins-devel's `omx_panel_description.h` (`make test-panel-abi`) |
| `tests/jack_info_e2e.sh` | omx-clap-host over jack: `track_info`, the remote pages, `param_info` (`make test-jack-info`) |
| `tests/jack_meters_e2e.sh` | omx-clap-host over jack: the meters as `output_set` lines (`make test-jack-meters`) |
| `tests/jack_window_e2e.sh` | omx-clap-host over jack and Xvfb: a plugin's own window over `clap.gui` and X11, embedded and floating, its timer and X descriptor on the tick, the refusals by code (`make test-jack-window`) |
| `tests/jack_window_wl_e2e.sh` | omx-clap-host over jack, a headless weston and Xvfb: a plugin's own window over `clap.gui` with Wayland first, mapped by weston, X11 for a plugin with no Wayland window, no-display with neither or for a Wayland-only plugin on an X11 session, and the host as `X11=0` builds it, no xcb linked, showing the Wayland window (`make test-jack-window-wl`) |
| `tests/jack_params_e2e.sh` | omx-clap-host over jack: a plugin's own parameter changes, gestures and rescans (`make test-jack-params`) |
| `lv2/tests/lv2_clap_test.c` | the LV2 adapter's CLAP face, which serves a subset of the plugin-side extensions for an LV2 plugin (`make test-lv2-clap`) |

Host-side and plugin-side extensions
------------------------------------

The host object offers seventeen extensions to every plugin (`CLAP_HOST_EXTENSIONS_INIT` in
`src/clap_host_extensions.h`), the draft ids of `clap.surround` and `clap.ambisonic` with them, and
`clap.preset-load`, `clap.track-info` and `clap.remote-controls` where the caller's configuration
asks (`struct omx_clap_host_config`; omx-clap-host asks for all three). Of the plugin, it asks at
open for the extensions `instance_create` in `src/clap_host.c` reads, and later for
`clap.voice-info`, `clap.note-name`, `clap.audio-ports-activation`, `clap.state-context`,
`clap.render`, `clap.meters/1` and `org.openmixer.param-contract/1` where it needs them.

### Stable

| extension | host-side | plugin-side | asked on msi | note |
|---|---|---|---|---|
| `clap.audio-ports` | supported: `rescan`, any flags, active or not, is parked and reported once by `omx_clap_host_ports_rescanned`, never a restart; `is_rescan_flag_supported` answers false (`tests/clap_host_test.c:rescan_checks`, `tests/clap_core_test.c:t_rescan_across_warmup`) | supported: every port described, its channels, `port_type` and `in_place_pair`; a main pair of any width up to 64 channels, auxiliary ports beside it on both sides (`tests/clap_host_test.c:t_layout_description`, `tests/clap_host_test.c:t_run_ports`, `tests/clap_core_test.c:t_side_chain`) | offered | refused: a port of no channel `hosting.topology.port-without-channels`, a port past 64 channels `hosting.topology.port-too-wide`, two main ports or too many auxiliary ones `hosting.topology.too-many-ports` (`tests/clap_host_test.c:t_layout_description`), no main output with an audio input or an auxiliary port `hosting.topology.no-audio-output` (`tests/clap_host_test.c:t_request_layout`), while no audio port at all is a pass-through tool on the host's placement (`tests/clap_host_test.c:tool_checks`); a main port wider than the caller's lane is refused at activate, `hosting.topology.wider-than-strip` (`tests/clap_host_test.c:synth_checks`). The LV2 adapter serves it too (`lv2/tests/lv2_clap_test.c:t_layouts`) |
| `clap.audio-ports-activation/2` | — | supported: before each activate every auxiliary port, input or output, is set active only while its buffers are bound (tested on an auxiliary output: `tests/clap_ext_test.c`) | — | omx-clap-host binds every auxiliary port to its JACK ports |
| `clap.audio-ports-activation/draft-2` | — | read, untested: asked when the plugin lacks the final id, and used the same way | — | |
| `clap.audio-ports-config` | supported: `rescan` (the plugin's configuration list changed) is reported as a ports rescan (`omx_clap_host_ports_rescanned`) while the plugin is inactive, when the list is what the next `omx_clap_host_request_layout` chooses from, and dropped while it is active, where the configuration in use stands and the list is read again at the next ask; never a restart (`tests/clap_host_test.c:t_request_layout`, #263) | supported: asked only by `omx_clap_host_request_layout`, the plugin inactive, after `clap.configurable-audio-ports` fails or is absent: the first config whose main ports match the caller's requests in channels and port type is selected (`tests/clap_host_test.c:t_request_layout`) | 65 | omx-clap-host asks for its lane's width (`-c`, 2 by default) at `add` and after a reported rescan |
| `clap.audio-ports-config-info/1` | — | unsupported: not asked; the host reads the config's ports from the description after selecting it | — | |
| `clap.audio-ports-config-info/draft-0` | — | unsupported: not asked | — | |
| `clap.configurable-audio-ports/1` | — | supported: asked only by `omx_clap_host_request_layout`, the plugin inactive, before `clap.audio-ports-config`: `can_apply_configuration`, then `apply_configuration`, and the description read again (`tests/clap_host_test.c:t_request_layout`) | — | |
| `clap.configurable-audio-ports.draft1` | — | read, untested: asked when the plugin lacks the final id | — | |
| `clap.surround/4` | supported: `changed` is a ports rescan, reported once, never a restart (`tests/clap_host_test.c:t_surround_ambisonic`) | supported: `get_channel_map` gives a surround port's map; `is_channel_mask_supported` asked by `omx_clap_host_request_layout` with the mask of the caller's map (`tests/clap_host_test.c:t_surround_ambisonic`, `tests/clap_host_test.c:t_request_layout`) | offered | refused `hosting.topology.channel-map-unknown`: a map naming an id past `CLAP_SURROUND_TSR` or of the wrong count, a surround port of more than `CLAP_SURROUND_TSR` + 1 channels without the extension (index order is assumed below that), a mask the plugin refuses (`tests/clap_host_test.c:t_surround_ambisonic`, `tests/clap_host_test.c:t_request_layout`). The LV2 adapter serves it for a typed surround group (`lv2/tests/lv2_clap_test.c:t_layouts`) |
| `clap.surround.draft/4` | supported: answered as the final id, the same object (`tests/clap_host_test.c:t_surround_ambisonic`) | read, untested: asked when the plugin lacks the final id | offered, not counted | |
| `clap.ambisonic/3` | supported: `changed` is a ports rescan, reported once, never a restart (`tests/clap_host_test.c:t_surround_ambisonic`) | supported: `get_config` gives an ambisonic port's ordering and normalisation, ACN and SN3D assumed without it or when it answers false; `is_config_supported` asked by `omx_clap_host_request_layout` (`tests/clap_host_test.c:t_surround_ambisonic`, `tests/clap_host_test.c:t_request_layout`) | offered | refused: a configuration the plugin does not support `hosting.topology.ambisonic-config` (`tests/clap_host_test.c:t_request_layout`); a channel count that is no (order+1)² `hosting.topology.port-too-wide` (`tests/clap_host_test.c:t_surround_ambisonic`) |
| `clap.ambisonic.draft/3` | supported: answered as the final id, the same object (`tests/clap_host_test.c:t_surround_ambisonic`) | read, untested: asked when the plugin lacks the final id | offered, not counted | |
| `clap.context-menu/1` | out of scope: the console draws its own menus (ruled 2026-10-19, #95) | out of scope: the same ruling | 62 | |
| `clap.context-menu.draft/0` | out of scope: the same ruling | out of scope: the same ruling | 63 | |
| `clap.event-registry` | offered, untested: `query` answers false for every space, the host reads no event space but CLAP's core one | — | offered | |
| `clap.gui` | supported in omx-clap-host, offered in every build (Wayland needs no library of the host's, X11 needs xcb): `request_resize` resizes the host's window, `request_show` and `request_hide` show and hide it, `closed` hides a floating window or forgets a destroyed one, `resize_hints_changed` reads `can_resize` again (`make test-jack-window`, `make test-jack-window-wl`); the library offers the caller's object, `omx_clap_host_offer_gui`, and none by default (`tests/clap_ext_test.c`, `tests/clap_host_test.c`). Out of scope in the console, which draws every panel from the plugin's parameters (GUI ruling 2026-10-19) | supported in omx-clap-host, on request (`show_external_ui`, `-S`), the first API the plugin offers whose display answers: Wayland floating, the plugin's own toplevel (`create("wayland", true)`, `suggest_title`, `show`, `hide`, `destroy`) (`make test-jack-window-wl`); then X11 embedded, in a window of the host's titled with the plugin's name (`create`, `set_scale` 1.0, `can_resize`, `get_size`, `set_parent`, `show`, `hide`, `destroy`; a resize of the host's window is `adjust_size` then `set_size`); then X11 floating (`suggest_title`), also for a plugin that offers X11 embedded with no `set_parent` (`make test-jack-window`). A build without xcb tries Wayland alone. Out of scope in the console | offered in omx-clap-host | refused by code, `resp -404`: no display that answers for any API the plugin offers `hosting.window.no-display`, neither a Wayland window nor an X11 one the build shows `hosting.window.unsupported`, the plugin's call answered false `hosting.window.refused` (`make test-jack-window` and `make test-jack-window-wl` for the first two) |
| `clap.latency` | supported: `changed`, then the restart the plugin asks for, runs on the control thread (`tests/clap_host_test.c:fake_plugin_checks`) | supported: read at every activate (`tests/clap_host_test.c:fake_plugin_checks`) | offered | omx-clap-host publishes it on the plugin's JACK ports. The LV2 adapter serves it (`lv2/tests/lv2_clap_test.c:t_latency_hold`) |
| `clap.log` | supported: the plugin's lines reach the host's log, one per tick through `omx_clap_host_log_take`, a plugin that logs every tick sharing it with the host's own lines (`tests/clap_host_test.c:unoffered_starve`) | — | offered | |
| `clap.note-name` | supported: `changed` read once through `omx_clap_host_note_names_changed` (`tests/clap_host_test.c:synth_host_details_checks`) | supported: each name with its port, key and channel (`tests/clap_host_test.c:synth_host_details_checks`) | offered | omx-clap-scan lists them (`doc/reference.md`, Scanning) |
| `clap.note-ports` | supported: `supported_dialects` answers CLAP, MIDI, MIDI MPE and MIDI 2.0 where the configuration admits note inputs; `rescan` taken by the tick, a full rescan while active asks for a restart (`tests/clap_host_test.c:synth_host_details_checks`) | supported: one note input admitted and its dialect chosen, note outputs read (`tests/clap_host_test.c:fake_plugin_checks`, `tests/clap_host_test.c:synth_checks`, `tests/clap_host_test.c:synth_out_all`) | offered | refused `hosting.clap.note-input`: note ports the configuration does not admit (`tests/clap_host_test.c:synth_checks`). The LV2 adapter serves it for an instrument (`lv2/tests/lv2_clap_test.c:t_instrument`) |
| `clap.param-indication/4` | — | unsupported: ruled supported (#95), the console shows mapping and automation state; #187 | — | |
| `clap.param-indication.draft/4` | — | unsupported: the same, #187 | — | |
| `clap.params` | supported: `rescan` read once through `omx_clap_host_params_rescanned`, `clear` with `CLAP_PARAM_CLEAR_ALL` drops the host's read-back of the parameter (`tests/clap_host_test.c:mover_checks`); `request_flush` is taken and nothing reads it yet | supported: the parameters read, written, read back, the plugin's own changes and gestures, modulation (`tests/clap_host_test.c:mover_checks`, `tests/clap_core_test.c:t_roster_whole_or_refused`, `make test-jack-params`) | offered | the LV2 adapter serves it (`lv2/tests/lv2_clap_test.c:t_fixture`) |
| `clap.posix-fd-support` | supported: a descriptor registered, polled by the tick without waiting (`tests/clap_ext_test.c`); the limit of 16 (`OMX_CLAP_FDS`) is untested | supported: `on_fd` with what each is ready for (`tests/clap_ext_test.c`) | offered | in the host since #90 |
| `clap.preset-load/2` | offered, untested: offered where the configuration asks (`tests/clap_host_test.c:configuration_checks` sees it offered); its `on_error` writes the host's log and `loaded` is taken, and no test calls either | supported: `from_location` for a listed preset, a `FILE` one by path and key, a `PLUGIN` one by key (`tests/clap_host_test.c:synth_presets_checks`) | offered | omx-clap-host's `preset_load` verb. The LV2 adapter serves it (`lv2/tests/lv2_clap_test.c:t_preset`) |
| `clap.preset-load.draft/2` | offered, untested: answered as the final id where the configuration asks | read, untested: asked when the plugin lacks the final id | offered | |
| `clap.remote-controls/2` | supported: `changed` reaches the caller, in omx-clap-host as `remote_pages_changed` on the feedback socket (`make test-jack-info`) | supported: the page count and each page's slots (`tests/jack_info_e2e.sh`) | offered | offered and read only where the configuration asks |
| `clap.remote-controls.draft/2` | offered, untested: answered as the final id where the configuration asks | read, untested: asked when the plugin lacks the final id | offered | |
| `clap.render` | — | supported: `omx_clap_host_set_render`, offline or realtime; a plugin with a hard real-time need is not sent offline (`tests/clap_ext_test.c`) | — | in the host since #90; omx-clap-host never asks for offline (no verb) |
| `clap.state` | offered, untested: `mark_dirty` is taken and kept in the instance; nothing in this repository reads it yet | supported: `omx_clap_host_state_save` (`tests/clap_ext_test.c`), and a save then a load through omx-clap-host's `state_save` and `state_load`, on the LV2 adapter's `clap.state` (`make test-jack-lv2`) | offered | omx-clap-host's `state_save` and `state_load`. The LV2 adapter serves it (`lv2/tests/lv2_clap_test.c:t_state`) |
| `clap.state-context/2` | — | supported: save and load for preset, duplicate and project (`tests/clap_ext_test.c`); a plugin without it gets its plain state for each, which no test exercises | — | in the host since #90 |
| `clap.tail` | supported: `changed`, read again by the next tick (`tests/clap_ext_test.c`) | supported: read at every activate, and the sleep after a tail (`tests/clap_ext_test.c`) | offered | in the host since #90 |
| `clap.thread-check` | supported: `is_audio_thread` is true on the audio role alone, a split of a driver and workers included (`tests/clap_host_test.c:role_checks`, `tests/clap_core_test.c:t_audio_role_on_split`); `is_main_thread`, true on the thread that opened the instance off the audio role, is untested | — | offered | |
| `clap.thread-pool` | supported: `request_exec` inside `process()` runs every task in turn on the calling audio thread before it returns; refused off the audio role and from inside a task (`tests/clap_ext_test.c`) | supported: `exec` called for each task (`tests/clap_ext_test.c`) | offered | in the host since #90 |
| `clap.timer-support` | supported: a timer registered, called once per tick when due (`tests/clap_ext_test.c`); the limit of 16 (`OMX_CLAP_TIMERS`) is untested | supported: `on_timer` (`tests/clap_ext_test.c`) | offered | in the host since #90 |
| `clap.track-info/1` | supported: `get` answers the strip's name, colour and kind as the caller set them (`make test-jack-info`) | supported: `changed` called on each set (`tests/jack_info_e2e.sh`) | offered | offered and read only where the configuration asks |
| `clap.track-info.draft/1` | offered, untested: answered as the final id where the configuration asks | read, untested: asked when the plugin lacks the final id | offered | |
| `clap.voice-info` | supported: `changed`, asked again by the next tick while active (`tests/clap_host_test.c:synth_host_details_checks`) | supported: read after every activate, `omx_clap_host_voice_info` (`tests/clap_host_test.c:synth_voice_checks`) | offered | omx-clap-scan reads it from one activation (`doc/reference.md`, Scanning) |

### Draft

| extension | host-side | plugin-side | asked on msi | note |
|---|---|---|---|---|
| `clap.background-activation/1` | — | unruled (#207) | — | |
| `clap.background-progress/1` | unruled (#207) | — | 3 | |
| `clap.background-state-context/1` | — | unruled (#207) | — | |
| `clap.extensible-audio-ports/1` | — | unsupported: #92 | — | |
| `clap.flush-events/1` | unruled (#207) | unruled (#207) | 3 | |
| `clap.gain-adjustment-metering/0` | — | supported in omx-clap-host only: read on the audio thread after `process()` when the plugin has no `clap.meters/1` (`make test-jack-meters`) | — | not read in process ([Surfaces](#surfaces), `docs/meters.md`) |
| `clap.mini-curve-display/3` | unsupported: ruled supported (#95), the console's panels draw EQ and dynamics curves; #188 | unsupported: #188 | 33 | |
| `clap.octave-number/1` | — | unruled (#207) | — | |
| `clap.param-hovered/1` | unruled (#207) | — | 3 | |
| `clap.params-origin/1` | unruled (#207) | unruled (#207) | 3 | |
| `clap.project-location/2` | — | unruled (#207) | — | |
| `clap.resource-directory/1` | unsupported: ruled P3 (2026-10-19, #95) | unsupported: the same | 62 | |
| `clap.scratch-memory/1` | unruled: not on #207's list; plugin-qualify#116 wants it as a resource-profile term | — | 33 | |
| `clap.transport-control/1` | unruled (#207) | — | — | the spelling of `clap-devel` 1.2.7 (CI's); 1.2.10 spells `/2` |
| `clap.transport-control/2` | unruled (#207) | — | 3 | |
| `clap.triggers/1` | unruled (#207) | unruled (#207) | — | |
| `clap.tuning/2` | unsupported: the host has no tuning of its own to share yet, #186 | unsupported: #186 | — | |
| `clap.undo/4` | out of scope: the console keeps its own undo (ruled 2026-10-19, #95) | — | 35 | |
| `clap.undo_context/4` | — | out of scope: the same ruling | — | |
| `clap.undo_delta/4` | — | out of scope: the same ruling | — | |
| `clap.webview/3` | out of scope: a plugin's own UI, as `clap.gui` (GUI ruling 2026-10-19) | out of scope: the same | 33 | |

### Factories

A factory is what a plugin file's entry hands out by id; the host asks the entry, never the plugin.

| factory | host-side | plugin-side | asked on msi | note |
|---|---|---|---|---|
| `clap.plugin-factory` | — | supported: every plugin is opened through it (`tests/clap_host_test.c`, `tests/clap_scan_test.c`) | — | the LV2 adapter's entry serves one per bundle (`lv2/tests/lv2_clap_test.c:t_entry`) |
| `clap.preset-discovery-factory/2` | — | supported: every provider created, each location read, `PLUGIN` locations through `get_metadata`, `FILE` ones walked (`tests/clap_host_test.c:synth_presets_checks`) | — | the LV2 adapter's entry serves one for the bundle's presets (`lv2/tests/lv2_clap_test.c:t_preset_list`) |
| `clap.preset-discovery-factory/draft-2` | — | read, untested: asked when the entry lacks the final id | — | |
| `clap.plugin-invalidation-factory/1` | — | unruled (#207) | — | |
| `clap.plugin-state-converter-factory/1` | — | unruled (#207) | — | |

### Ids outside the headers

The console's own extensions, and the ids the desk's plugins asked for that CLAP 1.2.10's headers no
longer spell (older draft ids). The check does not require these rows; they are kept for what they
say.

| id | host-side | plugin-side | asked on msi | note |
|---|---|---|---|---|
| `clap.meters/1` | — | supported: every meter declared, read whole at open or not at all; values read off the audio thread (`tests/clap_core_test.c:t_meters`, `make test-jack-meters`) | — | the draft proposed to the CLAP SDK, declared in `src/clap_meters.h` until an SDK ships it (`docs/meters.md`) |
| `org.openmixer.param-contract/1` | — | supported: read and checked at open and after each `params.rescan`, trusted whole or not at all, the broken rule named (`tests/param_contract_test.c`, `tests/clap_host_test.c:contract_checks`, `tests/clap_host_test.c:mover_checks`, `make test-jack-info`) | — | the vendor extension of `src/omx_clap_ext.h`; the LV2 adapter makes one from the TTL (`lv2/tests/lv2_clap_test.c:t_props`) |
| `org.openmixer.resource-hints.draft/0` | — | supported: read once at create, whole or refused by code (`hosting.resource-hints.unknown-hint`, `hosting.resource-hints.wrong-unit`); a hint whose bit is clear reads as unknown, never zero (`tests/resource_hints_test.c`, `make test-resource-hints-sabotage`) | — | omx-contract's RESOURCE_HINTS, rendered as `omxcontract/omx_resource_hints.h` (3.1.0); the LV2 adapter makes one from the TTL's `omxres:` properties (`lv2/tests/lv2_clap_test.c:t_hints`) |
| `org.openmixer.panel-description/1` | — | supported by omx-clap-scan, which reads it and prints the whole description, family, fingerprint and its five tables, in `--json` (`panel`, null when the plugin serves none) and in `--fields` (`tests/clap_scan_test.c:panel_checks`); omx-clap-host and the core do not ask for it | — | omx-plugins' `include/omx_panel_description.h` is the ABI; `src/omx_clap_ext.h` states its layout under other names, held to it by `make test-panel-abi` |
| `org.openmixer.declaration/1` | — | unsupported: declared in `src/omx_clap_ext.h` for a plugin to serve; nothing in this repository asks for it | — | |
| `clap.track-info.draft/0` | unsupported: an id older than the headers' | — | 2 | |
| `clap.voice-info.draft/0` | unsupported: an id older than the headers' | — | 2 | |
| `clap.resource-directory.draft/0` | unsupported: an id older than the headers' | — | 1 | |
| `clap.file-reference.draft/0` | unsupported: a draft no header spells | — | 2 | |
| `clap.quick-controls.draft/0` | unsupported: the old name of remote-controls | — | 2 | |
| `com.idlefoundry.native-window-occlusion-unreliable/1` | out of scope: a vendor id about a plugin's own window | — | 1 | |

Surfaces
--------

Three programs host CLAP plugins on this core, and they differ only where the core leaves a choice
to its caller, or where a program adds something of its own. A row is here when they differ.

- **omx-clap-host**: this repository's program; what this column says is read from `src/effects.c`
  and `src/main.c` and the tests named.
- **plugin-hostd's CLAP worker**: plugin-hostd runs omx-clap-host as its CLAP worker
  (`BUILDING.md`, `make test-hostd-pin`), so the omx-clap-host column holds for it; its protocol's
  own verbs (`track_info`, `remote_pages`, `remote_page_get`, `param_info`, `pin_expect`) are
  answered by this host (`doc/reference.md`, Strip and controls). Anything else is not stated here.
- **in process (the console's stage)**: a program that links `libomx-clap-core` and runs its own
  audio thread through `clap_stage.h`, as the console does. This column says what the library does
  with its defaults (`omx_clap_host_config_default`); the console's own configuration is not stated
  in this repository. Such a caller allocates `struct omx_clap_host_config` itself, so a build of it
  older than the library it links against (its package floor has no ceiling, #301) has a smaller
  struct than the one the library's headers now declare: `omx_clap_host_config_default` always
  stamps its own current size, past the end of that caller's own allocation. `omx_clap_host_config_
  default_sized(config, sizeof(*config))`, added in 0.15.0, is the call such a caller should make
  instead: it never writes past `sizeof(*config)` as the CALLER compiled it, filling only that much
  and saying so in `size`, and refuses whole (no byte moved) a size too short to carry a host's name.

| what | omx-clap-host | plugin-hostd's CLAP worker | in process (the console's stage) |
|---|---|---|---|
| `clap.preset-load`, `clap.track-info`, `clap.remote-controls` host-side | offered, all three (`src/effects.c`); supported as the rows above say (`make test-jack-info`) | as omx-clap-host; `track_info`, `remote_pages` and `remote_page_get` are its verbs | off by default (`tests/core_link_test.c` holds the default of `preset_load`); the caller turns each on |
| note inputs (`clap.note-ports`) | admitted: an instrument's note input is `effect_<N>:midi_in` (`make test-jack-synth`) | as omx-clap-host | refused by default with `hosting.clap.note-input` (`tests/core_link_test.c` holds the default); the caller admits them |
| main port wider than two channels | carried: a JACK port per channel, any width (`lane_channels` 0) | as omx-clap-host | refused at activate by default, `hosting.topology.wider-than-strip` (`lane_channels` 2, held by `tests/core_link_test.c`); the caller sets its lane's width |
| the plugin's meters | `clap.meters/1`, else `clap.gain-adjustment-metering/0`, as `output_set` lines (`make test-jack-meters`) | as omx-clap-host | `clap.meters/1` only, read by the caller off the audio thread (`tests/clap_core_test.c:t_meters`) |
| `clap.render` | never asked for offline: no verb sets it | as omx-clap-host | `omx_clap_host_set_render` for the caller (`tests/clap_ext_test.c`) |
| `clap.state-context` | not used: `state_save` and `state_load` save and load the plain state | as omx-clap-host | `omx_clap_host_state_save_context` and `_load_context` for the caller (`tests/clap_ext_test.c`) |
| auxiliary ports (`clap.audio-ports-activation`) | each bound to its JACK ports, `aux_in_<p>_<k>` and `aux_out_<p>_<k>` (`make test-jack-fake`) | as omx-clap-host | active while the caller binds buffers to it (`tests/clap_ext_test.c`) |
| `org.openmixer.param-contract/1` | answers `param_info` (`make test-jack-info`) | as omx-clap-host; `param_info` is its verb | `omx_clap_host_param_contract` and `omx_clap_host_param_meaning` (`tests/clap_host_test.c:contract_checks`) |
| `org.openmixer.resource-hints.draft/0` | read, nothing answers it on the socket | as omx-clap-host | `omx_clap_host_resource_hints`, each hint's word, unit and figure by `omx_clap_resource_hint_word`, `_unit` and `_figure` (`tests/resource_hints_test.c`) |
| `clap.gui` | the plugin's own window on request, over Wayland first, then X11: `show_external_ui`, `hide_external_ui`, `-S` (`make test-jack-window-wl`, `make test-jack-window`) | never asked: plugin-hostd has no window verb | never offered: the console draws every panel; the library offers only what the caller hands `omx_clap_host_offer_gui` |

Measured
--------

A desk measurement, not a CI result: `make clap-coverage` added every CLAP plugin installed on msi,
one at a time, to this tree's omx-clap-host in a private namespace with a PipeWire of its own, and
read the answer and the host's log of ids it did not offer. Run on 2026-10-20 with the host built
on this change over main `a5d0dba`; re-run it to measure again, from a fresh output directory (the
probe resumes from the last one).

**Hosted 433 of 433 plugins** in 234 files: none refused, none crashed, and every file scanned.
The extensions are counted for the hosted plugins only: a refused plugin ends before it asks.

Nothing is refused. The two that the `5991b9c` run refused `hosting.topology.no-audio-output`,
`com.madskjeldgaard.birdhouse` and `studio.kx.distrho.ildaeil#midi`, are note tools with no audio
port at all: a note input, a note output. They are hosted as pass-through tools now, the host's
placement (#270): the strip's audio through the stage untouched, the plugin run for its notes alone.
That code is written for a plugin with an audio input or an auxiliary port and no main output (an
analyser, every output a side chain), of which the desk has none. The four nebula bundles
(`nebula-cluster.clap/libnebula_cluster.so` and the de-esser, delay and stereo delay beside it),
which no earlier run could scan, are listed by their one object and hosted (#259). The installed set
changed since the `07d70a1` run: QPitch, NoiseRF and ChowKick were removed from msi, so their crash
and scan-failure rows are not measured here, not fixed.

The ids the hosted plugins asked for most and were not given:

| asked for | plugins | where it stands |
|---|---|---|
| `clap.gui` | 317 | supported in omx-clap-host since #180, on request, Wayland first since #329 (`make test-jack-window`, `make test-jack-window-wl`); out of scope in the console |
| `clap.audio-ports-config` (host-side) | 70 | supported since #263 (`tests/clap_host_test.c:t_request_layout`); the next run counts 0 |
| `clap.context-menu.draft/0`, `clap.context-menu/1` | 68, 67 | out of scope |
| `clap.resource-directory/1` | 67 | unsupported, P3 |
| `clap.undo/4` | 38 | out of scope |
| `clap.mini-curve-display/3` | 36 | unsupported, #188 |
| `clap.scratch-memory/1` | 36 | unruled |
| `clap.webview/3` | 36 | out of scope |

Gaps
----

In the order #196 gives them, with what the desk measured and where each stands today:

1. **`clap.thread-pool`**: in the host, host-side and plugin-side (#90, `tests/clap_ext_test.c`).
   The probe counts only ids the host does not offer, so no plugin is counted for it.
2. **`clap.tail`**: in the host (#90): read at activate and on `changed`, and the sleep after it.
   Not counted, as above.
3. **`clap.render`**: in the library (#90), offline and realtime; omx-clap-host has no verb that
   asks for offline. A plugin-side extension, which the probe does not count.
4. **`clap.timer-support`** and **`clap.posix-fd-support`**: in the host (#90). Both are offered,
   and `instance_create` asks the plugin for both; the control thread's tick
   (`omx_clap_host_tick`, every idle pass of omx-clap-host) calls `on_timer` for each timer due and
   `on_fd` for each descriptor ready. No plugin is refused for lacking either.
5. **`clap.note-name`**: in the host, host-side and plugin-side (`tests/clap_host_test.c:synth_host_details_checks`).
6. **`clap.scratch-memory/1`**: not offered; 36 hosted plugins asked. Unruled, and not on #207's
   list; plugin-qualify#116 wants it as a resource-profile term.
7. **`clap.audio-ports-config`, `clap.surround`, `clap.ambisonic`**: the #92 work carries the
   plugin-side of all three and the host-side of surround and ambisonic; #263 carries the host-side
   of `clap.audio-ports-config` (its `rescan`, reported as a ports rescan while the plugin is
   inactive), which 69 hosted plugins asked for, the largest ask after `clap.gui`.

What the probe found beyond those, each filed: #257 (23 plugins refused on their feature list
alone; admitted by ports now), #258 (QPitch aborted at `add` and took the host with it; no longer
installed, so not measured), #259 (a bundle directory whose shared object has another name listed no
plugin; listed by that object now).
