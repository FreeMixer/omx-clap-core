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
    help
    quit

`<param_id>` is the plugin's CLAP parameter id in decimal; `:bypass` reads
and writes the bypass like mod-host's pseudo port. `state_save` writes one
`<dir>/effect_<N>.clapstate` per instance. Every other mod-host command
answers `resp -902`.

A parameter write is an event on the plugin's next process cycle. When no
cycle comes (a client nothing is linked to gets none from PipeWire), the
host delivers it itself through the plugin's `params.flush` on the control
thread, so `param_get`, `state_save` and the plugin always see the last
value written.

Meters
------

A plugin's meters are output symbols, as an LV2 plugin's output ports are
to mod-host: `monitor_output <N> <symbol>` answers `resp 1` for a symbol
the plugin has and `resp 0` for any other, and from then on the feedback
socket (`-f`) carries `output_set <N> <symbol> <value>` with the value at
once and again whenever it moves, at most every 20 ms, never from the
audio thread.

The host asks each plugin for `org.openmixer.meters/1` (`omx_clap_ext.h`)
first and, when it has none, for `clap.gain-adjustment-metering/0`. A
meter's symbol is its name with every character outside `[A-Za-z0-9_]`
turned into `_` and a `_` before a leading digit; a meter of more than
one channel has one symbol per channel, `<symbol>_<channel>` from 0. The
standard gain adjustment is `gain_adjustment_metering`, read on the audio
thread right after the plugin's `process()`; a cycle that does not call
it, bypassed, reads 0. A plugin whose meters derive a symbol twice, or one
a parameter answers to, is refused, and the host names both.

Bypass
------

`bypass <N> 1` is the host's own: on the next cycle the output crossfades
from the plugin's output to the dry input over that one block, with gains
`1 - i/n` and `i/n`, and from then on the input passes through untouched
while the plugin idles. `bypass <N> 0` crossfades back and the plugin
runs again. The plugin's own bypass parameter, when it declares one, is
never written and is refused to `param_set`; `param_get <N> :bypass`
answers the commanded value.

Replies are mod-host's: `resp <code>`, with mod-host's error codes.

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
  `v / 127` and no note id. Nothing else is passed: a controller, pitch
  bend or program change is dropped.
- MIDI: every channel message of one to three bytes becomes a
  `CLAP_EVENT_MIDI`. System messages and sysex are dropped.

A cycle takes 256 messages (`CLAP_HOST_NOTES_PER_BLOCK`); the surplus is
counted and dropped. Nothing is allocated or locked on the audio thread.

A main port wider than stereo, a main pair of different widths, a second
note input, a note input that reads neither the CLAP nor the MIDI dialect, or
no audio input and no note input is refused at `add` with `resp -102` and the
`hosting.*` code on stderr: `hosting.topology.wider-than-strip`,
`hosting.clap.note-input`, `hosting.topology.no-audio-input`. An extra audio
input (a sidechain, one or two channels, up to eight) is admitted and fed
silence: the command line has no verb that binds it, so the plugin reads
zeros on it. The library's caller binds a buffer to it with
`omx_clap_host_bind_aux_input`. An extra audio output is admitted and left
unconnected: the plugin is handed a scratch buffer for it. A plugin is admitted with the
`audio-effect` or the `instrument` feature and refused with neither.

`bypass <N> 1` on an instrument fades its output to silence over one block
and then leaves the plugin idle: notes that arrive meanwhile are dropped.

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
- `param_info <N> <param_id>` answers `resp -511` (`PHD_ERR_NO_PARAM_CONTRACT`)
  for every parameter until the host reads `org.openmixer.param-contract/1`,
  and `resp -103` for an id `param_set` would refuse.
