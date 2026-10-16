omx-clap-host reference
=======================

Running
-------

    omx-clap-host -n -p 5555

The options are mod-host's: `-n` no fork, `-v` verbose, `-p <port>` the
command socket, `-f <port>` the feedback socket, `-V`, `-h`.

Commands
--------

    add clap:<absolute path>.clap#<plugin id> <instance_number> [client_name]
    add lv2:<uri> <instance_number> [client_name]
    remove <instance_number>
    bypass <instance_number> <bypass_value>
    param_set <instance_number> <param_id> <param_value>
    param_get <instance_number> <param_id>
    monitor_output <instance_number> <output_symbol>
    preset_load <instance_number> <preset_file>
    state_save <dir>
    state_load <dir>
    connect <origin_port> <destination_port>
    disconnect <origin_port> <destination_port>
    cpu_load
    note_drops <instance_number>
    note_out_drops <instance_number>
    help
    quit

`<param_id>` is the plugin's CLAP parameter id in decimal, or for an LV2
plugin the LV2 symbol of a control port ([LV2 plugins](#lv2-plugins));
`:bypass` reads and writes the bypass like mod-host's pseudo port. `state_save` writes one
`<dir>/effect_<N>.clapstate` per instance. Every other mod-host command
answers `resp -902`.

A parameter write is an event on the plugin's next process cycle. When no
cycle comes (a client nothing is linked to gets none from PipeWire), the
host delivers it itself through the plugin's `params.flush` on the control
thread, so `param_get`, `state_save` and the plugin always see the last
value written.

What a plugin does with its parameters by itself
------------------------------------------------

A plugin moves a parameter of its own when its user turns its own control,
loads a preset of its own or follows a learned MIDI control: it says so with
a `CLAP_EVENT_PARAM_VALUE` among its output events, and brackets its user's
hold of a control with `CLAP_EVENT_PARAM_GESTURE_BEGIN` and `_END`. The
feedback socket carries them, in the order the plugin pushed them, at the
idle rate (every 20 ms), never from the audio thread:

    param_gesture <N> <param> begin
    param_set <N> <param> <value>
    param_gesture <N> <param> end

`param_set` is mod-host's feedback line, the value as `%f`; `param_gesture`
is the shape FreeMixer/plugin-hostd#47 proposes for plugin-hostd's
vocabulary. `<param>` is what `param_set` takes: the CLAP id, or an LV2
control port's symbol. Only a parameter `param_set` takes is reported; a
read-only one is an output, `monitor_output`'s. Of the values one idle tick
finds for a parameter, the last goes out, before a gesture on it. The plugin
pushes into a ring of `OMX_CLAP_PARAM_OUT_DEPTH` (256) events between two
ticks, on the audio thread with no allocation, lock or call out; past it an
event is dropped and counted, and the next tick sends every parameter's
value as the plugin reads it then, so the controller still ends on what the
plugin holds. A value the controller sets is not echoed back.

A program that hosts plugins through the library reads the same events with
`omx_clap_host_param_events_take` (and `omx_clap_host_param_events_dropped`),
and has the rest of what CLAP's parameters offer:
`omx_clap_host_param_modulate` sends `CLAP_EVENT_PARAM_MOD` around the set
value, which stays what it was (only for a `CLAP_PARAM_IS_MODULATABLE`
parameter); `omx_clap_host_param_gesture` wraps the host's own writes in a
gesture; `omx_clap_host_params_rescanned` says which `params.rescan` flags
the plugin raised (a `CLAP_PARAM_RESCAN_INFO` changes what the rows answer at
once, a `RESCAN_ALL` while active makes the tick ask for a restart);
`params.clear` with `CLAP_PARAM_CLEAR_ALL` drops the host's read-back of the
parameter; `omx_clap_host_param_text` and `omx_clap_host_param_parse` give
the plugin's own text for a value and back.

Meters
------

A plugin's meters are output symbols, as an LV2 plugin's output ports are
to mod-host: `monitor_output <N> <symbol>` answers `resp 1` for a symbol
the plugin has and `resp 0` for any other, and from then on the feedback
socket (`-f`) carries `output_set <N> <symbol> <value>` with the value at
once and again whenever it moves, at most every 20 ms, never from the
audio thread.

The host asks each plugin for `clap.meters/1`, CLAP's meters extension
([docs/meters.md](../docs/meters.md)), first and, when it has none, for
`clap.gain-adjustment-metering/0`; it reads no other meters extension. A
meter's symbol is its name with every character outside `[A-Za-z0-9_]`
turned into `_` and a `_` before a leading digit; a meter of more than
one channel has one symbol per channel, `<symbol>_<channel>` from 0. The
standard gain adjustment is `gain_adjustment_metering`, read on the audio
thread right after the plugin's `process()`; a cycle that does not call
it, bypassed, reads 0. A plugin whose meters derive a symbol twice, or one
a parameter answers to, is refused, and the host names both.

A program that hosts the plugin in its own process through the library reads
the same `clap.meters/1` with `omx_clap_host_meter_count`,
`omx_clap_host_meter_info` and `omx_clap_host_meter_values`
([docs/meters.md](../docs/meters.md), In process).

Bypass
------

`bypass <N> 1` is the host's own: on the next cycle the output crossfades
from the plugin's output to the dry input over that one block, with gains
`1 - i/n` and `i/n`, and from then on the input passes through untouched
while the plugin idles. `bypass <N> 0` crossfades back and the plugin
runs again. The plugin's own bypass parameter, when it declares one, is
never written and is refused to `param_set`; `param_get <N> :bypass`
answers the commanded value.

Replies are mod-host's: `resp <code>`, with mod-host's error codes. A refused
`add` that has a `hosting.*` code says it after the error code, `resp -102
hosting.topology.wider-than-strip`, and on stderr as `<plugin id>: <code>`.

Port layout
-----------

A plugin is admitted with exactly one main audio output, mono or stereo,
and one main audio input of the same kind; every channel of the main pair
is one jack port. A plugin with a note input, an instrument, may have no
audio input: it then has `out_<k>` only.

A plugin with one CLAP note input port gets one jack MIDI input port on its
client, `effect_<N>:midi_in`. The host reads its jack events on the audio
thread and hands them to the plugin in the same cycle, on the frame they
arrived on, in the dialect the note port prefers:

- CLAP: a note on (`0x9n`, velocity above 0) and a note off (`0x8n`, or a
  note on with velocity 0) become `CLAP_EVENT_NOTE_ON` and
  `CLAP_EVENT_NOTE_OFF` with the key, the channel, the velocity as
  `v / 127` and a note id (below). All notes off (controller 123, and 124 to
  127, which imply it) becomes a `CLAP_EVENT_NOTE_OFF` and all sound off (120)
  a `CLAP_EVENT_NOTE_CHOKE`, both on every key of the channel (key -1). Pitch
  bend and channel or poly pressure become `CLAP_NOTE_EXPRESSION_TUNING` and
  `_PRESSURE`, unless the port also reads MIDI, which then gets them raw.
- MIDI: every channel message of one to three bytes becomes a
  `CLAP_EVENT_MIDI`. A sysex of up to 256 bytes (`OMX_CLAP_SYSEX_MAX_BYTES`) becomes a
  `CLAP_EVENT_MIDI_SYSEX`, eight to a cycle (`OMX_CLAP_SYSEX_PER_BLOCK`); a longer
  one is counted as unmappable. System real-time bytes are dropped.
- MIDI with MPE: a port that prefers `CLAP_NOTE_DIALECT_MIDI_MPE` gets every
  channel message raw, as the MIDI dialect does, and reads the MPE zone
  itself. The instance's `note_dialect` is `CLAP_NOTE_DIALECT_MIDI_MPE`.

Note ids. Every CLAP note on takes a fresh id from a pool of 256 per
instance (`OMX_CLAP_NOTE_IDS`), and its note off and every per-note
expression of it carry the same id. A note on of a key that already sounds
first ends the old note, a note off under its old id. The id is free again
when the plugin pushes `CLAP_EVENT_NOTE_END` for it, or, for a plugin that
never does, 2 seconds after its note off (`OMX_CLAP_NOTE_ID_HOLD_MS`, in
frames at the rate the instance is activated at). A note on that finds no
free id is skipped and counted, and its note off is dropped, since nothing
sounds. A note off of a key the host holds no id for goes with note id -1.
A reset of the plugin frees every id.

MPE. The MPE configuration message (RPN 6 on channel 1 or 16) sets the
lower or upper zone and its member channel count; the library's caller may
set the zone itself (`omx_clap_host_set_mpe`). In a zone, a member
channel's pitch bend, channel pressure and controller 74 become
`CLAP_NOTE_EXPRESSION_TUNING`, `_PRESSURE` and `_BRIGHTNESS` for the newest
note held on that channel, with its id, channel and key; the master
channel's reach every note. The bend range is 48 semitones on the members
and 2 on the master, MPE's defaults, until RPN 0 sets another.

A cycle takes 256 messages (`CLAP_HOST_NOTES_PER_BLOCK`), and keeps the last
128 of them (`OMX_CLAP_NOTES_RESERVED`) for releases: a note off, a note on
with velocity 0, all sound off, reset all controllers, all notes off and the
mode changes that imply it, and a sustain, sostenuto or hold pedal let go.
Every other message stops at 128 and the surplus is counted and dropped; a
release is dropped only when all 256 are taken. So 300 note ons and then one
note off in one cycle deliver the first 128 note ons and the note off, in the
order they came, and count 172 dropped. `note_drops <N>` answers
`resp 0 <dropped> <releases> <sysex> <unmappable>`: every message the cycle
had no room for, of them the releases, the sysex past a cycle's eight, and the
messages no dialect of the plugin carries, since the instance was added.
Nothing is allocated or locked on the audio thread.

A main port wider than stereo, a main pair of different widths, a second
note input, a note input that reads none of the CLAP, MIDI and MIDI MPE dialects, or
no audio input and no note input is refused at `add` with `resp -102` followed
by the `hosting.*` code, which stderr carries too: `hosting.topology.wider-than-strip`,
`hosting.clap.note-input`, `hosting.topology.no-audio-input`. An extra audio
input (a sidechain, one or two channels, up to eight) is admitted and fed
silence: the command line has no verb that binds it, so the plugin reads
zeros on it. The library's caller binds a buffer to it with
`omx_clap_host_bind_aux_input`. An extra audio output is admitted and left
unconnected: the plugin is handed a scratch buffer for it. A plugin is admitted with the
`audio-effect` or the `instrument` feature and refused with neither.

`bypass <N> 1` on an instrument fades its output to silence over one block
and then leaves the plugin idle: notes that arrive meanwhile are dropped.

Note and MIDI output. A plugin with a CLAP note output port (an
arpeggiator, a chord generator, a synth that echoes what it plays, an LV2
plugin with a MIDI output) gets one jack MIDI output port on its client,
`effect_<N>:midi_out`. What the plugin sends out on its first note output
in a cycle is written to `midi_out` in the same cycle, at the frame the
plugin gave it:

- `CLAP_EVENT_NOTE_ON` and `CLAP_EVENT_NOTE_OFF` become a MIDI note on and
  note off of their channel and key, the velocity scaled to 0..127 and
  rounded; a note on is never sent at velocity 0. `CLAP_EVENT_NOTE_CHOKE`
  becomes a note off at velocity 0. A note event with no single key or
  channel (-1) names no MIDI message.
- `CLAP_EVENT_MIDI` is sent as it is: three bytes, or two for a program
  change and a channel pressure.
- `CLAP_EVENT_NOTE_END` is the plugin telling the host a voice ended
  (above): it is never sent out.
- A note expression, a sysex, a MIDI 2 event, an event of another note
  output port and a note event with no single key or channel are counted as
  unmapped and not sent.

A time past the cycle is written on its last frame, and a time before the
previous event's on the previous event's frame, since a jack MIDI buffer
holds its events in order. The library keeps the plugin's output in a
lock-free ring of 256 events (`OMX_CLAP_NOTE_OUT_DEPTH`, a block's
`CLAP_HOST_NOTES_PER_BLOCK`), written inside `process()` and read on the
audio thread right after it; past it an event is dropped and counted.
Only a processing cycle fills it: the warm-up and a parameter flush send
nothing out. `note_out_drops <N>` answers `resp 0 <dropped> <unmapped>
<port_full>`: the events the ring had no room for, the events with no MIDI
form, and the messages the jack port had no room for, since the instance was
added. Nothing is allocated or locked on the audio thread.

A program that hosts plugins through the library reads the same events with
`omx_clap_host_notes_out`, on the audio role right after
`omx_clap_host_run` in the same cycle, or on one other thread that is the
ring's only reader: each event carries its frame in the block, the frame
since activate and its MIDI bytes. `omx_clap_note_midi` and
`omx_clap_midi_size` (`omx_clap_midi.h`) are the conversion above, the LV2
adapter's too.

LV2 plugins
-----------

    add lv2:urn:openmixer:test:host-fixture 0
    add urn:openmixer:test:host-fixture 0

An LV2 plugin is added by its URI, after `lv2:` or alone as mod-host takes it
(anything that is not `clap:` and reads as an absolute URI). The host finds its
bundle on `LV2_PATH` (unset: `~/.lv2`, `/usr/local/lib64/lv2`,
`/usr/local/lib/lv2`, `/usr/lib64/lv2`, `/usr/lib/lv2`), each directory in
order and the bundles in each in name order: the first bundle whose
`manifest.ttl` names the URI, whole (`<uri>`) or as a prefixed name of a prefix
it declares, and whose plugin list holds it. A manifest that spells the URI
any other way, relative to `@base` for instance, is not found. plugin-hostd
narrows `LV2_PATH` to the plugin's own bundle.

The plugin runs through `libomx-clap-lv2`, the LV2 to CLAP adapter, linked into
the host: one CLAP plugin with the same client, ports, latency, meters and
bypass as a CLAP plugin (`effect_<N>`, `in_<k>` and `out_<k>`, `midi_in` for a
plugin with an LV2 MIDI input). lilv is not linked: the adapter loads it on the
first LV2 add, so a host that never adds one never maps it.
[docs/lv2-compatibility.md](../docs/lv2-compatibility.md) says what the adapter
carries.

A plugin that asks for `state:makePath` makes its files in the host's state
root: `-s <dir>`, else `$XDG_STATE_HOME/omx-clap-host`, else
`~/.local/state/omx-clap-host`, made at start. Each instance has a directory
of its own there, `lv2-<pid>-<n>/`, removed with the instance; `state_save`
copies the files the plugin's state names to `lv2-<pid>-<n>.saved/`, and the
saved state names those copies by their absolute path, so they stay. A root
that is not a directory the host can write, or that cannot be made, is said
on stderr and `state:makePath` is not offered: a plugin that requires it
answers `resp -102 hosting.features.missing`, and every other LV2 plugin
still adds. plugin-hostd's workers inherit its environment, so the default
root is the daemon's user's.

What an LV2 `add` answers:

| | reply |
|---|---|
| the plugin runs | `resp <N>` |
| no bundle on the path holds the URI, or the word is no URI | `resp -101` |
| the adapter or the host refuses the plugin | `resp -102 <hosting code>`, e.g. `resp -102 hosting.features.midi-in-fed-empty`; stderr says `<uri>: <hosting code>` |
| lilv cannot be loaded, or the plugin's binary cannot | `resp -102 hosting.no-realisation` |

The codes are the adapter's (`hosting.features.*`, `hosting.topology.*`,
`hosting.no-realisation`) and the host's own (`hosting.topology.*`,
`hosting.clap.*`). A refusal names only its own `add`: the next `add` starts
with no code.

Parameters. The adapter makes each LV2 control port one CLAP parameter whose
id is the port's `lv2:index`; the host keeps the port's `lv2:symbol` beside it,
so a control port answers to either:

| LV2 | CLAP parameter | `param_set` / `param_get` | `monitor_output` |
|---|---|---|---|
| an input control port | id = `lv2:index`, automatable | by its symbol or its id | no |
| an output control port | id = `lv2:index`, read-only | `param_get` by its symbol or id; `param_set` answers `resp -103` | by its symbol: `output_set <N> <symbol> <value>` when it moves, as mod-host sends it |
| `lv2:enabled`, or a control input whose symbol is `bypass` | id = `lv2:index`, `IS_BYPASS`, held at its processing value | by its symbol or id: the host's bypass (below) | no |
| the latency port | `clap.latency`, no parameter | `resp -103` | no |
| a numeric `patch:writable` property | id = the port count plus its index in `patch:writable` order | by its id only | no |
| an audio, CV or atom port | none | `resp -103` | no |

`param_info <N> <symbol>` takes the symbol too.

Bypass. mod-host bypasses an LV2 plugin through its `lv2:enabled` port; here
that port is the host's bypass: `param_set <N> enabled 0` is `bypass <N> 1` (the
one-block crossfade to the dry input) and `param_set <N> enabled 1` is
`bypass <N> 0`; `param_get <N> enabled` answers 0 while the host bypasses the
plugin and 1 otherwise, whichever verb set it. A port whose symbol is `bypass`
is the same, the other way round. The plugin itself is never written: it is
held at its processing value.

State and presets. `state_save` writes the plugin's LV2 state, as Turtle, to
`<dir>/effect_<N>.clapstate` and `state_load` reads it back, applied by a
restart of the plugin. `preset_load <N> <preset uri>` loads an LV2 preset of
the plugin's bundle by its URI, as mod-host does, also applied by a restart; a
preset of another plugin, or one the bundle does not hold, answers `resp -104`.

`remove <N>` closes the client, so none of `effect_<N>`'s ports stays in the
graph, then the plugin and the bundle's entry.

Latency
-------

The value of the plugin's `latency` extension is published on the
instance's jack ports through a latency callback: a capture-mode query
answers on every `out_<k>` with the latency reaching `in_<k>` plus the
plugin's, a playback-mode query does the reverse on every `in_<k>`. It
is read after every activate; when the plugin reports it changed, or
asks for a restart that re-activates it, the new figure is republished
with `jack_recompute_total_latencies`. A graph reads the latency from
the ports, not from this socket.

Scanning
--------

    omx-clap-scan [--json] [<path>...]

lists what CLAP plugin files hold. A `<path>` is a `.clap` file or a
directory searched recursively for `.clap` files, in name order; with none
given the search path is `CLAP_PATH`, then `~/.clap`, `/usr/lib64/clap` and
`/usr/lib/clap`. For each plugin of each factory it prints the descriptor
(id, name, vendor, version, description, url, features) and, from an
instance that is initialised, read and destroyed without being activated:

- every parameter: id, name, module, min, max, default and the flags by
  name (`stepped`, `hidden`, `readonly`, `bypass`, `automatable`, ...);
- the audio ports of each direction: id, name, `main` or `aux`, channels;
- the note port counts;
- the latency, when the plugin answers it before activation with more than
  0 frames; otherwise the key is absent, never 0.

`--json` writes one document on stdout with the keys in a fixed order:

    {"scanner":"omx-clap-scan","version":"0.1.0","files":[
    {"path":"...","plugins":[
    {"id":"...","name":"...","vendor":"...","version":"...","description":"...",
     "url":"...","features":["audio-effect"],
     "params":[{"id":0,"name":"...","module":"","min":0,"max":1,"default":0,"flags":["stepped"]}],
     "audio_ports":{"inputs":[{"id":0,"name":"...","role":"main","channels":2}],"outputs":[...]},
     "note_ports":{"inputs":0,"outputs":0},"latency":64}
    ]}
    ]}

A bound a plugin leaves infinite is `null`. A file that can't be loaded is
`{"path":"...","error":"<reason>"}`, a plugin that can't be created or
initialised keeps its descriptor and gets an `"error"`, and a plugin that
crashes or hangs (30 s) takes only its own file's entry: each file is
scanned in a child process and the scan goes on. The exit status is 0 when
at least one path could be read, 1 when none could.


Layout pin
----------

`pin_expect <N> omx-layout/1:<sha256>` is plugin-hostd's: the daemon sends it
just before the `add` of instance `N` with the layout pin of the plugin. That
`add` computes the plugin's layout after `init()` and before `activate()`,
every parameter `params.get_info()` gives in the `omx-layout/1` serialisation of
plugin-hostd's `pin.h`, and when it differs destroys the instance and answers
`resp -510` (`PHD_ERR_PIN_LAYOUT_MISMATCH`): the plugin is never activated and
processes nothing. A pin is spent by the `add` it was sent for. A scheme the
host does not know answers `resp -508` (`PHD_ERR_PIN_ABSENT`) and pins nothing.


Strip and controls
------------------

`track_info`, `remote_pages`, `remote_page_get` and `param_info` are plugin-hostd's
(`include/plugin-hostd/protocol.h`), answered from the plugin:

- `track_info <N> <name> <color> [kind]` stores the strip's name (`""` for none),
  colour (`#RRGGBB`, or `-` for none) and kind, then calls the plugin's
  `clap.track-info` `changed`; the host's `get` answers from that copy. A word
  outside that grammar, or a name longer than 255 bytes, answers `resp -902`
  and changes nothing.
- `remote_pages <N>` answers the page count of `clap.remote-controls`, 0 for a
  plugin without it. `remote_page_get <N> <page>` answers
  `resp 0 <page_id> "<section>" "<page_name>" <s1> ... <s8>`, each slot the id
  `param_set` takes, or `-` for an empty slot and for an id `param_set` would
  refuse. A plugin's call of the host's `changed` writes
  `remote_pages_changed <N>` on the feedback socket.
- `param_info <N> <param_id>` answers what a parameter's value means:
  `resp 0 <unit> <scale> <min> <max> <default> <step> <stable_symbol>`. The
  unit, the scale, the step and the symbol come from the plugin's
  `org.openmixer.param-contract/1` (`omx_clap_ext.h`); the range and the
  default from `clap.params` when asked. The unit is a name of the contract's
  table (`none`, LV2's local names `s`, `ms`, `db`, `hz`, `semitone12TET` and
  the rest, then `dbfs`, `dbu`, `ratio`, `lufs`), the scale `linear`, `log` or
  `stepped`, and each number the shortest decimal that reads back to the same
  double (`0.10000000149011612`, `1e-7`). The host reads and checks the
  contract once, at `add`: a plugin that offers none, or whose contract breaks
  one rule (an id with no entry, two entries with one symbol, a malformed
  symbol, unit, scale or step, a fingerprint that is not the one the host
  computes, a malformed migration step), answers `resp -511`
  (`PHD_ERR_NO_PARAM_CONTRACT`) for every parameter, and the host's log names
  the broken rule once (`<plugin id>: the parameter contract is not trusted:
  R4 ...`). An id `param_set` would refuse (a read-only parameter, the
  plugin's own bypass, an unknown id, `:bypass`) answers `resp -103`. An LV2
  plugin answers `resp -511`: the adapter offers no contract.

  The same reading is libomx-clap-core's: `omx_clap_host_param_contract` says
  whether the contract holds (or which rule it broke) and
  `omx_clap_host_param_meaning` what one parameter means;
  `omx_clap_param_unit_name`, `omx_clap_param_scale_name` and
  `omx_clap_param_number` write the words and numbers above.
