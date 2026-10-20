omx-clap-host reference
=======================

Running
-------

    omx-clap-host -n -p 5555

The options are mod-host's: `-n` no fork, `-v` verbose, `-p <port>` the
command socket, `-f <port>` the feedback socket, `-V`, `-h`; and this host's
own `-s <dir>`, the LV2 state root ([LV2 plugins](#lv2-plugins)), `-t`,
follow the JACK transport ([Transport](#transport)), and `-c <1|2>`, the
lane's width (2 by default): a plugin whose main ports differ from it is asked
for that many channels on each main port before it is activated, and keeps its
declared layout when it cannot offer them.

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
    audio_ports <instance_number>
    note_input <instance_number>
    note_on <instance_number> <channel> <key> <velocity>
    note_off <instance_number> <channel> <key> <velocity>
    all_notes_off <instance_number>
    set_bpm <beats_per_minute>
    set_bpb <beats_per_bar>
    transport <rolling> <beats_per_bar> <beats_per_minute>
    transport_sync <mode>
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
plugin holds, and an `end` for every gesture still open, so it is never left
holding one whose end was dropped. Begin and end go out in pairs: an end with
no open begin is not sent. A value the controller sets is not echoed back.

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

Transport
---------

Every plugin the host runs reads one transport, the host's, from the block
after a command changes it. The commands are mod-host's:

| command | what it does | reply |
|---|---|---|
| `set_bpm <bpm>` | the tempo, 20 to 280 | `resp 0`; `resp -207` out of the range |
| `set_bpb <bpb>` | the beats per bar, 1 to 16 | `resp 0`; `resp -207` out of the range |
| `transport <rolling> <bpb> <bpm>` | play (`1`) or stop (`0`), with the beats per bar and the tempo | `resp 0`; `resp -207` when the bpb or the bpm is out of the range |
| `transport_sync <mode>` | `none` | `resp 0` |
| | `link` (Ableton Link) and `midi` (MIDI clock): the host has neither | `resp -902` |

Until a tempo is given (`set_bpm` or `transport`) a plugin is handed no
transport at all, never an invented one. From then on it is handed, every
block: the tempo, the time signature (the beats per bar over 4; 4 beats per
bar until `set_bpb` or `transport` says otherwise, as mod-host starts), the
song position of the block's first frame in beats and in seconds, the bar and
its first beat, and, while rolling, `CLAP_TRANSPORT_IS_PLAYING`. The position
starts at 0 and moves only while rolling; a stop holds it, the next play goes
on from it. It is counted from the frames the graph has played since the
command that last changed it, at that command's tempo, never by adding a
block's increment to the last, so it carries no drift however long it plays.
A tempo change while rolling takes effect at the frame the command arrives
on: the beats played up to it stay as they were.

With `-t` the host follows the JACK transport instead: each block reads
`jack_transport_query`, so the plugin is playing while the JACK transport
rolls and its song position in seconds is the transport's frame over the
rate. When a JACK timebase master gives bars and beats, the plugin gets its
tempo, time signature, bar and beat; when none does, the beats are the
seconds at the tempo `set_bpm` or `transport` gave, and with no tempo given
the plugin is handed the seconds and the play state alone. `transport` then
sets the tempo and the beats per bar only: the host never starts, stops or
moves the JACK transport.

A program that hosts plugins through the library hands them a transport of its
own with `omx_clap_host_set_transport` and `omx_clap_transport_publish`. The
record has one writer, the control thread or an audio thread that publishes
it once per cycle (a console counting its song position in frames), never
both; any number of audio threads read it whole in the same cycle. Neither
side allocates or locks.

Replies are mod-host's: `resp <code>`, with mod-host's error codes. A refused
`add` that has a `hosting.*` code says it after the error code, `resp -102
hosting.topology.too-many-ports`, and on stderr as `<plugin id>: <code>`.

Port layout
-----------

A plugin is admitted with exactly one main audio output and at most one
main audio input, each of up to 64 channels and the two free to differ in
width (a mono input with a stereo output, a 5.1 pair); every channel of the
main pair is one jack port, `in_1` to `in_<n>` and `out_1` to `out_<n>`. A
plugin with a note input, an instrument, may have no audio input: it then
has `out_<k>` only.

A plugin with one CLAP note input port gets one jack MIDI input port on its
client, `effect_<N>:midi_in`. The host reads its jack events on the audio
thread and hands them to the plugin in the same cycle, on the frame they
arrived on, in the dialect the note port prefers:

- CLAP: a note on (`0x9n`, velocity above 0) and a note off (`0x8n`, or a
  note on with velocity 0) become `CLAP_EVENT_NOTE_ON` and
  `CLAP_EVENT_NOTE_OFF` with the key, the channel, the velocity as
  `v / 127` and a note id (below). What else a CLAP-dialect input receives
  depends on whether the port also declares the MIDI dialect. Declaring only
  CLAP: all sound off (controller 120) becomes `CLAP_EVENT_NOTE_CHOKE` and all
  notes off (123, and 124 to 127, which imply it) `CLAP_EVENT_NOTE_OFF`, both
  on every key of the channel (key -1); pitch bend becomes
  `CLAP_NOTE_EXPRESSION_TUNING` on the whole channel, full scale 2 semitones
  either way (`bend_semitones`); channel pressure becomes
  `CLAP_NOTE_EXPRESSION_PRESSURE` on the whole channel and poly pressure the
  same on its key, under the id of the note held there; any other controller,
  a program change and a sysex are counted as unmappable and not delivered
  (`t_channel_messages` in `tests/clap_stage_test.c`, "CLAP: ..." and
  "CLAP only: ..." checks). Also declaring MIDI: every channel message but a
  note on and a note off, pitch bend and pressure included, arrives raw as
  `CLAP_EVENT_MIDI`, all sound off and all notes off too (they still end the
  notes the host holds ids for), and a sysex as `CLAP_EVENT_MIDI_SYSEX`
  (`t_channel_messages`, "CLAP+MIDI: ..." checks).
- MIDI: every channel message of one to three bytes, notes included, becomes a
  `CLAP_EVENT_MIDI` (`t_channel_messages`, "MIDI: ..." checks). A sysex of up
  to 256 bytes (`OMX_CLAP_SYSEX_MAX_BYTES`) becomes a `CLAP_EVENT_MIDI_SYSEX`,
  eight to a cycle (`OMX_CLAP_SYSEX_PER_BLOCK`); a longer one is counted as
  unmappable and the ninth in a cycle as a dropped sysex
  (`t_channel_messages`; `t_sysex_pool_resets` shows the eight slots free
  again every cycle). A channel message longer than three bytes is counted as
  unmappable in every dialect. Every other status byte from `0xf0` up, the
  system real-time and system common messages, is dropped and not counted.
- MIDI with MPE: a port that prefers `CLAP_NOTE_DIALECT_MIDI_MPE` gets every
  channel message raw, as the MIDI dialect does, and reads the MPE zone
  itself. The instance's `note_dialect` is `CLAP_NOTE_DIALECT_MIDI_MPE`. A
  sysex goes the way it does for a CLAP input: delivered when the port also
  declares MIDI, counted as unmappable when it does not.
- MIDI 2.0: a port that prefers `CLAP_NOTE_DIALECT_MIDI2`, or declares it
  and none of CLAP, MIDI and MIDI MPE, gets every channel message as the
  MIDI 2.0 channel voice message the UMP specification translates it to, a
  `CLAP_EVENT_MIDI2` (message type 4, group 0): a note on or off with its
  velocity scaled from 7 to 16 bits, poly and channel pressure and a
  controller from 7 to 32 bits, pitch bend from 14 to 32 bits, all by the
  specification's min-centre-max scaling (0 stays 0, 64 becomes the top bit,
  127 all ones), and a program change with no bank. A note on of velocity 0
  is a note off of velocity `0x8000`, MIDI 1.0's release velocity of 64. No
  state is kept, so bank select, RPN and NRPN pass as the plain controllers
  they are. A sysex goes as it does for a CLAP input. A CLAP-dialect port
  that also declares MIDI 2.0 and not MIDI keeps its notes, bend and
  pressure as CLAP events and gets a controller and a program change as
  MIDI 2.0 rather than counted (`t_midi2` in `tests/clap_stage_test.c`;
  `synth-midi2` in `tests/fake_synth.c` plays from them).

The message types each dialect passes, as `src/clap_stage.h` documents them
(`tests/dialect_doc_check.sh` fails when the two lists differ):

Passes, CLAP only: note on, note off, all sound off, all notes off, pitch bend, channel pressure, poly pressure.
Counts, CLAP only: other controller, program change, sysex.
Passes, CLAP and MIDI: note on, note off, every other channel message, sysex.
Passes, MIDI: every channel message, sysex.
Passes, MIDI MPE: every channel message, sysex only when the port also declares MIDI.
Passes, MIDI 2.0: every channel message as MIDI 2.0, sysex only when the port also declares MIDI.
Passes, CLAP and MIDI 2.0: note on, note off, all sound off, all notes off, pitch bend, channel pressure, poly pressure, other controller and program change as MIDI 2.0.
Counts, CLAP and MIDI 2.0: sysex.

Note ids. Every CLAP note on takes a fresh id from a pool of 256 per
instance (`OMX_CLAP_NOTE_IDS`), and its note off and every per-note
expression of it carry the same id. A note on of a key that already sounds
first ends the old note, a note off under its old id. The id is free again
when the plugin pushes `CLAP_EVENT_NOTE_END` for it, or, for a plugin that
never does, 2 seconds after its note off (`OMX_CLAP_NOTE_ID_HOLD_MS`, in
frames at the rate the instance is activated at). A `CLAP_EVENT_NOTE_END`
frees its id only while the id still names the event's channel and key (-1
in either matches any): an id taken back after its hold may already name a
new note, which keeps it when the old voice's end comes late. A note on that finds no
free id is skipped and counted, and its note off is dropped, since nothing
sounds. A note off of a key the host holds no id for goes with note id -1.
A reset of the plugin frees every id.

MPE. A lower zone (master channel 1, members from channel 2 up) and an
upper zone (master channel 16, members from channel 15 down) are held
together. The MPE configuration message (RPN 6 on channel 1 or 16) sets the
lower or the upper zone and its member channel count, and touches only that
zone; a count of 0 ends it. The library's caller may set a zone itself
(`omx_clap_host_set_mpe(in, zone, members)`: `OMX_CLAP_MPE_LOWER` or
`OMX_CLAP_MPE_UPPER` with `members` 0 ends that zone only;
`OMX_CLAP_MPE_OFF` ends both).

With both zones live the members share channels 2 to 15, so the zone just
configured wins: the other zone keeps the channels left (14 minus the new
zone's members, none when the new zone has 15) and ends when none are left.
A zone cut or ended this way keeps its bend ranges only while it exists. Notes
held on a channel that leaves a zone keep their ids and sound until their note
off; the channel's expressions are no longer per note.

In a zone, a member channel's pitch bend, channel pressure and controller 74
become `CLAP_NOTE_EXPRESSION_TUNING`, `_PRESSURE` and `_BRIGHTNESS` for the
newest note held on that channel, with its id, channel and key. A master
channel's reach the notes held on the member channels of its own zone only,
one expression per held note with that note's id, channel and key; the other
zone's notes are not touched. The master's last bend, pressure and
controller 74 are kept per zone, and a note that starts in the zone later
begins with them (as expressions at its note on) until the zone's
configuration message clears them. The bend range of each zone is 48
semitones on the members and 2 on the master, MPE's defaults, until RPN 0 on
that zone's master or one of its members sets another; the zone's own
configuration message returns them to the defaults, and the other zone's stay.

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

A controller that is not a jack client plays an instrument through the
socket with plugin-hostd's note verbs. `note_input <N>` answers
`resp 0 midi_in` for an instance that takes notes and `resp 0 -` for one
that takes none. `note_on <N> <channel> <key> <velocity>`,
`note_off <N> <channel> <key> <velocity>` and `all_notes_off <N>` (controller
123 on each of the 16 channels) answer `resp 0`, `-513` for an instance that
takes no notes, and `-902` for a word out of MIDI's range (channel 0 to 15,
key 0 to 127, a note on's velocity 1 to 127, a note off's 0 to 127). The
control thread writes each message into a lock-free ring the audio thread
empties at the start of its next cycle, at its first frame and ahead of what
`midi_in` carries in it, through the same path, so the dialect rules, the
release reserve and `note_drops` hold for both. A verb the ring has no room
for (512 messages, which only a stopped graph fills) is not queued at all
and answers `-517`, plugin-hostd's code for a full note queue.

A second note input, or a note input that reads none of the CLAP, MIDI, MIDI MPE and MIDI 2.0 dialects,
is refused at `add` with `resp -102` followed
by the `hosting.*` code, which stderr carries too: `hosting.clap.note-input`.
A port of no channels (`hosting.topology.port-without-channels`),
a port of more than 64 channels (`hosting.topology.port-too-wide`), a second main port on
a side or more than 64 auxiliary ports on a side (`hosting.topology.too-many-ports`) are
refused the same way, as are a surround port whose `clap.surround` map names a channel id
past `CLAP_SURROUND_TSR` or holds other than the port's channel count, or that has no `clap.surround`
and more than `CLAP_SURROUND_TSR` + 1 channels
(`hosting.topology.channel-map-unknown`) and an ambisonic port whose channels are not
(order+1)² for any order (`hosting.topology.port-too-wide`). A plugin with no main input is a source, its output its
own: an instrument when it has a note input, a generator (an oscillator, a noise
source) when it has none; the client has `out_<k>` and no `in_<k>`. A plugin with no audio
port at all (a note tool, a controller) is a pass-through tool, the host's placement: a
stereo pair named `in (passthrough: host)` and `out (passthrough: host)` on both sides, the
strip's audio through the stage bit for bit, the plugin handed no audio buffer and run for
its notes and parameters alone; its latency applies to those, so the host reads 0 and says
so on the log, as the LV2 adapter hosts a control-only tool. A plugin with an audio input or
an auxiliary port but no main output (an analyser, every output a side chain) has nothing
the host can carry (`hosting.topology.no-audio-output`). An extra audio
input (a sidechain, of up to 64 channels, up to 64 of them) is admitted and fed
silence until its buffers are bound. The library's caller binds a buffer to it with
`omx_clap_host_bind_aux_input`, one buffer per channel. An extra audio output (of up
to 64 channels, up to 64 of them) is admitted and left unconnected: the plugin is
handed a scratch buffer per channel, and the library's caller hands it buffers of
its own with `omx_clap_host_bind_aux_output`. The descriptor's feature list
decides nothing: a plugin is admitted by what its ports declare.

omx-clap-host binds every auxiliary port to JACK: its client registers a port
per channel of each, `aux_in_<p>_<k>` and `aux_out_<p>_<k>`, `p` the port's
place among its side's auxiliary ports in the plugin's order and `k` the
channel, both from 1 (the main pair keeps `in_<k>` and `out_<k>`). Each
channel is bound to a block of the JACK buffer size, made again when that
size changes. A change of the buffer size is applied on the control thread's
next tick (the plugin's deactivate and activate are main-thread calls; JACK's
buffer-size callback only records the size), and the cycles pass dry until
then; each cycle copies an auxiliary input's JACK buffer into its
block before the plugin runs and an auxiliary output's block onto its JACK
buffer after. When the blocks cannot be made at a new size, the instance is
left inactive, passing dry, and pending: the next change of the buffer size
makes them and activates it; stderr says so in one line. A plugin that
rescans its ports in the deactivate of a buffer-size change is not activated
on its old description: it stays inactive, passing dry, stderr says so once,
and the relayout in the same tick reads its description again and activates it
at the new size. An instance its relayout left dry until it is added again
gets no blocks made. A side chain
nothing is linked to reads silence. The bound auxiliary outputs are guarded as
the main ones are (the library's, for every carrier; an output still on its
scratch block is read by no one and left alone): on every block the plugin does not run usable (not processing, bypassed,
faulted, asleep, a block longer than the bounce, an unusable block) they are
silent, never the last block again; a non-finite sample on one makes the block
unusable and strikes as on a main output; a sample past +24 dBFS is clamped
and counted. A ports rescan that leaves the auxiliary ports other than the
ones registered at `add` passes the instance dry until it is added again, as
one that changes the main ports does.

`audio_ports <N>` answers on one line: `resp 0`, then `dry` while the
instance passes dry, then a record per port registered at `add`, inputs first
and each side's main port first, separated by single spaces:

    resp 0 in:main:1:2:stereo:"in 0":in_1,in_2 in:aux:1:1:mono:"in 1":aux_in_1_1 out:main:1:2:stereo:"out 0":out_1,out_2

A record is `side:role:index:channels:kind[:layout][:assumed]:"name":jacks`:
`side` `in` or `out`, `role` `main` or `aux`, `index` the port's place among
its side's auxiliary ports from 1 (1 for the main port), `channels` its
registered width, `kind` `mono`, `stereo`, `surround`, `ambisonic` or
`unknown`, `layout` a surround port's CLAP surround ids or an ambisonic port's
ordering and normalisation joined by `+` (`0+1+2+3+9+10`, `acn+sn3d`),
`assumed` when that ambisonic configuration is the host's (the description's
`ambisonic_assumed`: an LV2 group, a CLAP plugin without `clap.ambisonic`, or
one whose `get_config` answers false), the name in double quotes with `"` and
`\` escaped as `\"` and `\\`, and the JACK ports joined by `,`. A port whose
description no longer has its registered width (a dry relayout) reads
`unknown` and `""`. `resp -3` for an instance that is not there. plugin-hostd
does not declare the verb yet (its Task 6.6, in that repository): behind it, it
is not routed to the instance's worker. `omx-clap-scan` prints the same
names, in the text form `in  aux 1 channels mono "in 1" aux_in_1_1`.

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

Voices
------

An instrument's `clap.voice-info` is read after every activate, which the
extension requires: the voices its patch uses (`voice_count`), the voices
it has allocated (`voice_capacity`, the ceiling a host prices it at), and
its flags (`CLAP_VOICE_INFO_SUPPORTS_OVERLAPPING_NOTES`: two notes on one
key at once, told apart by note id). A program that hosts the plugin
through the library reads them with `omx_clap_host_voice_info`, which
answers 1 with the three figures, or 0 when the plugin declares none: no
extension, a `get` that fails, an answer outside
`1 <= voice_count <= voice_capacity`, or an instance not yet activated.
With no declaration every figure reads 0, which is never a voice count.
The last activate's answer is kept across a deactivate. A plugin that
says its voice info changed (the host's `clap.voice-info`) is asked again
by the next `omx_clap_host_tick` while it is active.

Instrument details
------------------

The host object offers a plugin `clap.note-ports`, `clap.note-name` and
`clap.voice-info` besides the six extensions every plugin gets, and
`clap.event-registry` (below), the four of a full host (A full host,
below), and `clap.surround` and `clap.ambisonic`, sixteen in all
(`src/clap_host_extensions.h`); the two layout extensions answer their
draft ids too.

- `note-ports.supported_dialects` answers what a note input can be fed in:
  CLAP, MIDI, MIDI MPE and MIDI 2.0 (`OMX_CLAP_HOST_NOTE_DIALECTS`) when the
  configuration admits note inputs, none when it does not.
- `note-ports.rescan`: the flags are taken by the tick and read once with
  `omx_clap_host_note_ports_rescanned`. A rescan of the names
  (`CLAP_NOTE_PORTS_RESCAN_NAMES`) changes nothing the host holds. A rescan
  of every port (`CLAP_NOTE_PORTS_RESCAN_ALL`) is the plugin's to make only
  while inactive; made while active, the tick asks for a restart. Either way
  the next activate reads the note input again and chooses its dialect
  anew; ports that `add` would have refused, or a note input that came or
  went (the audio topology rests on it), refuse the activate with
  `hosting.clap.note-input` and keep the old reading.
- `note-name`: a drum machine names its keys ("Kick" on 36). The library's
  caller reads them with `omx_clap_host_note_name_count` and
  `omx_clap_host_note_name`, each with the port, key and channel it names,
  -1 for every one; a name the plugin's `get` refuses is not answered.
  `note-name.changed` reads once through `omx_clap_host_note_names_changed`.
- `voice-info.changed`: above, under Voices.
- `audio-ports.rescan`, any flags, active or not, and `surround.changed` and
  `ambisonic.changed`: a ports rescan. It is parked and reported once
  through `omx_clap_host_ports_rescanned`; nothing is restarted for it, and
  an activate never reads the description again. The caller answers it by
  re-describing the plugin, inactive, through
  `omx_clap_host_request_layout` (`n` 0 asks nothing and only reads the
  ports again), then activates it. A restart, a rate change
  (`omx_clap_host_set_rate`) or an auxiliary bind that re-activates, whose
  deactivate raised a rescan (CLAP's request_restart, deactivate, ports
  changed), does not activate the plugin: it answers
  `OMX_CLAP_RESTART_RESCANNED` with the instance inactive and the rescan
  reported, never -1, which is a failure.
  The warm-up's own deactivate is not reported; a rescan made inside
  activate is.

Tuning (`clap.tuning`, a draft) is not offered: the host has no tuning of
its own to share yet (#186).

`clap.event-registry`: the host reads no event space but CLAP's core one,
so `query` answers false for every space a plugin names, and the plugin
keeps to the core events.

A full host
-----------

What a plugin may ask of any host besides its audio and its parameters:

- `clap.timer-support`: a plugin registers up to 16 timers
  (`OMX_CLAP_TIMERS`); `omx_clap_host_tick` calls `on_timer` for each that is
  due, once per tick, then sets it a period on (from the tick's time when the
  tick ran late). A period below 1 ms is served at 1 ms. The tick's own rate
  bounds the resolution: a caller ticking every 10 ms serves a 5 ms timer
  every 10 ms.
- `clap.posix-fd-support`: up to 16 descriptors (`OMX_CLAP_FDS`), each for
  reading, writing or both; the tick polls them without waiting and calls
  `on_fd` with what each is ready for (`CLAP_POSIX_FD_ERROR` on an error or
  a hang-up).
- `clap.thread-pool`: `request_exec` inside `process()` runs every task
  before it returns, one after another on the calling audio thread, which
  the extension allows and which neither allocates nor waits. Off the audio
  role it is refused.
- `clap.tail`: the plugin's tail is read at every activate and again by the
  tick after the plugin says it changed; `omx_clap_host_tail` answers it
  (UINT32_MAX for an infinite tail).
- `clap.render`: `omx_clap_host_set_render(in, CLAP_RENDER_OFFLINE)` before a
  render that is not bound to the clock (a bounce, a freeze), and
  `CLAP_RENDER_REALTIME` after; a plugin with a hard real-time requirement is
  not asked to go offline (-1).
- `clap.state-context`: `omx_clap_host_state_save_context` and
  `omx_clap_host_state_load_context` with `CLAP_STATE_CONTEXT_FOR_PRESET`,
  `_DUPLICATE` or `_PROJECT`; a plugin without the extension saves and loads
  its plain state for each.
- `clap.audio-ports-activation`: before each activate the host sets every
  auxiliary port, input or output, active only while its buffers are bound,
  so the plugin may skip a port no one reads or writes; omx-clap-host binds
  every one to its JACK ports.
- Sleep. A plugin's process status is read: after `CLAP_PROCESS_SLEEP`, after
  `CLAP_PROCESS_CONTINUE_IF_NOT_QUIET` on a block whose input and output were
  silent, and after `CLAP_PROCESS_TAIL` once its input has been silent for its
  tail and its output is silent (never for an infinite tail), the plugin
  sleeps. Whatever it answers, it sleeps only once its output has been
  silent for 200 ms (`OMX_CLAP_SLEEP_QUIET_MS`, `in->sleep.quiet_min_frames`)
  and no note the host gave an id for is held or inside its hold after a
  note off. So an instrument that sounds on a silent lane, one whose voice
  starts silent, or a reverb still ringing, is never put to sleep. While it sleeps, a block
  whose input is exactly silent, that carries no note and no parameter
  event, and that finds the stage processing and wet, is not processed: the
  lane keeps its silence and the steady time moves on. Any other block
  wakes it, and so does its `request_process`. A stage with a side chain
  bound never sleeps. `in->sleep` counts the blocks skipped and the times it
  slept (`omx_clap_host_run`).

The tests are `tests/clap_ext_test.c` against `tests/fake_ext.clap`.

Presets
-------

A plugin file that has a preset-discovery factory
(`clap.preset-discovery-factory/2`, or its draft id) lists its presets
through `omx_clap_host_presets(binary, fn, ctx)`, or, for a linked entry or
the LV2 adapter's (`omx_clap_lv2_entry`), `omx_clap_host_entry_presets`.
Each provider of the factory is created and initialised, and each location
it declares is read: a `PLUGIN` location (presets the plugin holds itself)
through `get_metadata`, a `FILE` location that is a file the same, and one
that is a directory walked in name order, at most 16 levels deep
(`OMX_CLAP_PRESET_DEPTH`), for its files of a filetype the provider
declared; a link to a directory is not followed. The callback gets each
preset as a `struct omx_clap_preset`: the provider, the name (a file of one
preset that gives none is named by the file), the location kind and
location (the file's path, or none for `PLUGIN`), the load key (none for a
file of one preset), the flags (`CLAP_PRESET_DISCOVERY_IS_*`, the
location's when the preset sets none), the description, and the CLAP
plugin ids it applies to (an id of another format is left out). At most
65536 presets (`OMX_CLAP_PRESETS_MAX`) and 65536 files are read; a callback
that returns nonzero stops the listing. The call answers the presets
listed, or -1 for a file with no such factory. A file that hands back its
plugin factory for the preset-discovery id (SpectMorph 0.6.1 does) has
none: the host never creates a plugin through it.

`omx_clap_host_preset_load_key(in, kind, location, load_key)` loads a listed
preset through the plugin's `clap.preset-load`: a `FILE` preset by its path
and key, a `PLUGIN` preset by its key alone.

The LV2 adapter's entry offers the same factory for its bundle: one
provider, `org.openmixer.lv2.pset`, whose one location is the bundle (a
`PLUGIN` location of factory content), and each `pset:Preset` that
`lv2:appliesTo` one of the bundle's plugins a preset named by its
`rdfs:label`, its URI the load key, for the plugin whose id is its URI
(`t_preset_list` in `lv2/tests/lv2_clap_test.c`).

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
- the audio ports of each direction: id, name, `main` or `aux`, channels and
  kind (the CLAP port type: `mono`, `stereo`, `surround`, `ambisonic`, or
  `unknown` for none or another); a surround port adds its map, the CLAP
  surround ids `clap.surround` names (0 to n-1 in index order without the
  extension), and an ambisonic port its ordering (`acn`, `fuma`) and
  normalisation (`sn3d`, `n3d`, `maxn`, `sn2d`, `n2d`) from `clap.ambisonic`
  (`acn` and `sn3d` without it or when its `get_config` answers false,
  `"assumed":true` and `assumed` then, as the description's `ambisonic_assumed`
  says for an admitted plugin); then
  the JACK ports omx-clap-host registers for the port (`in_<k>`, `out_<k>`,
  `aux_in_<p>_<k>`, `aux_out_<p>_<k>`; `"jack"` in the JSON, after the name in
  the text form), and none for any port of a plugin the core refuses (the scan
  opens each plugin as omx-clap-host does, an instrument's note input and any
  main width admitted, and lists a refused one from an instance only created).
  The text form quotes the plugin's name and each port's, `"` and `\` escaped
  with a backslash and a control byte printed as a space, and is a line per port:
  `in  aux 1 channels mono "in 1" aux_in_1_1`;
- the note port counts;
- the note names (`clap.note-name`), each with its port, key and channel;
  absent when the plugin names none;
- the latency, when the plugin answers it before activation with more than
  0 frames; otherwise the key is absent, never 0;
- the presets of the file (above, under Presets), after its plugins: name,
  location kind and location, load key, provider, plugins and flags; the
  key is absent when the file has no preset-discovery factory;
- the voice info (`clap.voice-info`): the voices the patch uses, the voices
  allocated, and whether two notes may sound on one key. The extension
  answers only while active, so a plugin that offers it is activated once,
  at 48 kHz with blocks of up to 1024 frames, read and deactivated; absent
  when it offers none or answers outside `1 <= count <= capacity`.

`--json` writes one document on stdout with the keys in a fixed order:

    {"scanner":"omx-clap-scan","version":"0.1.0","files":[
    {"path":"...","plugins":[
    {"id":"...","name":"...","vendor":"...","version":"...","description":"...",
     "url":"...","features":["audio-effect"],
     "params":[{"id":0,"name":"...","module":"","min":0,"max":1,"default":0,"flags":["stepped"]}],
     "audio_ports":{"inputs":[{"id":0,"name":"...","role":"main","channels":2,"kind":"stereo","jack":["in_1","in_2"]}],"outputs":[...]},
     "note_ports":{"inputs":0,"outputs":0},
     "note_names":[{"name":"Kick","port":-1,"key":36,"channel":-1}],"latency":64,
     "voice_info":{"count":4,"capacity":8,"overlapping_notes":true}}
    ],"presets":[
    {"name":"Init","location_kind":"plugin","location":null,"load_key":"init",
     "provider":"...","plugins":["..."],"flags":["factory"]}
    ]}
    ]}

A bound a plugin leaves infinite is `null`. A file that can't be loaded is
`{"path":"...","code":"<code>","error":"<reason>"}`, the code one of `dlopen` (the loader refused
it), `entry` (no `clap_entry`, or its init or factory failed), `crash:<signal>`, `timeout`,
`unreadable` (a path that can't be read), `pipe`, `fork` or `no-result`; a plugin that can't be created or
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
  plugin answers from the contract the adapter makes of its TTL: the port's
  symbol, LV2's `units:unit` by name, `log` for `pprops:logarithmic` over a
  positive travel, `stepped` for a stepped port, and the step of
  `pprops:rangeSteps` (`param_info 0 gain_db` of the host fixture reads
  `resp 0 none linear -60 0 -20 0 gain_db`); one whose symbol is longer than
  a contract holds answers `resp -511`.

  The same reading is libomx-clap-core's: `omx_clap_host_param_contract` says
  whether the contract holds (or which rule it broke) and
  `omx_clap_host_param_meaning` what one parameter means;
  `omx_clap_param_unit_name`, `omx_clap_param_scale_name` and
  `omx_clap_param_number` write the words and numbers above.

CLAP extensions
---------------

Every extension header the installed CLAP headers carry (`clap/ext/` and
`clap/ext/draft/`), and what this host does with it. Supported: the host
offers it, or reads the plugin's, as the sections above say. Not supported:
not done yet, with the issue that tracks it. Not planned: ruled out, with the
reason. `tests/ext_table_check.sh` fails when the installed headers name one
this table does not; a row for a header an older CLAP lacks is kept.

| Header | Status | What, or why not |
|---|---|---|
| `ext/ambisonic.h` | supported | `clap.ambisonic/3` (and its draft id): read at open, `get_config` gives an ambisonic port's ordering and normalisation (ACN and SN3D without the extension); the host's `changed` (under either id) is a ports rescan, reported once by `omx_clap_host_ports_rescanned`, never a restart; `is_config_supported` is asked only by `omx_clap_host_request_layout`, with the carrier's configuration (`false`: `hosting.topology.ambisonic-config`) |
| `ext/audio-ports-activation.h` | supported | A full host above: auxiliary ports set inactive while unused |
| `ext/audio-ports-config.h` | supported | `clap.audio-ports-config`: read at open, asked only by `omx_clap_host_request_layout` with the plugin inactive: tried after `clap.configurable-audio-ports` when that is not offered, cannot apply the requests or refuses them: the first config whose main ports are the carrier's requested mains in channels and port types is selected, whatever auxiliary ports it also declares (described after), and the description read again; omx-clap-host asks for its lane's width (`-c`, 2 by default) at add and after a reported ports rescan |
| `ext/audio-ports.h` | supported | the port layout, Port layout above: every port described, `port_type` and `in_place_pair` read; omx-clap-host registers a JACK port per channel of every port, `aux_in_<p>_<k>`/`aux_out_<p>_<k>` for the auxiliary ones; the host's `rescan`, any flags, is reported once by `omx_clap_host_ports_rescanned`, never a restart (`is_rescan_flag_supported` answers false) |
| `ext/configurable-audio-ports.h` | supported | `clap.configurable-audio-ports/1` (and its draft id): read at open, asked only by `omx_clap_host_request_layout` with the plugin inactive, before `clap.audio-ports-config`: `can_apply_configuration`, then `apply_configuration`, and the description read again; a false from either falls back to `clap.audio-ports-config`, and a plugin neither offers the layout through keeps its declared one (`OMX_CLAP_LAYOUT_DECLARED`) |
| `ext/context-menu.h` | not planned | the console draws its own menus (ruled 2026-10-19, #95) |
| `ext/event-registry.h` | supported | the host offers it; no event space beyond the core one |
| `ext/gui.h` | not planned in the console | never in the console (GUI ruling 2026-10-19); native windows in the standalone omx-clap-host are #180 |
| `ext/latency.h` | supported | Latency above |
| `ext/log.h` | supported | the host offers it; the tick drains it |
| `ext/note-name.h` | supported | Instrument details above |
| `ext/note-ports.h` | supported | the plugin's ports and the host's dialects, Instrument details above |
| `ext/param-indication.h` | not supported | ruled supported (#95): the console shows mapping and automation state; #187 |
| `ext/params.h` | supported | parameters, read back, modulation and gestures |
| `ext/posix-fd-support.h` | supported | A full host above |
| `ext/preset-load.h` | supported | offered where the configuration asks; `preset_load`, `omx_clap_host_preset_load_key` |
| `ext/remote-controls.h` | supported | offered where the configuration asks |
| `ext/render.h` | supported | A full host above |
| `ext/state-context.h` | supported | A full host above |
| `ext/state.h` | supported | `state_save`, `state_load` |
| `ext/surround.h` | supported | `clap.surround/4` (and its draft id): read at open, `get_channel_map` gives a surround port's map (index order without the extension), refused `channel-map-unknown` for an unknown id or a wrong count, and without the extension for more than `CLAP_SURROUND_TSR` + 1 channels, past the ids index order can name; the host's `changed` (under either id) is a ports rescan, reported once by `omx_clap_host_ports_rescanned`, never a restart; `is_channel_mask_supported` is asked only by `omx_clap_host_request_layout`, with the mask of the carrier's map (`false`: `hosting.topology.channel-map-unknown`) |
| `ext/tail.h` | supported | A full host above, and the sleep after it |
| `ext/thread-check.h` | supported | the host offers it |
| `ext/thread-pool.h` | supported | A full host above: tasks run in turn on the audio thread |
| `ext/timer-support.h` | supported | A full host above |
| `ext/track-info.h` | supported | offered where the configuration asks |
| `ext/voice-info.h` | supported | Voices above |
| `ext/draft/background-activation.h` | not supported | a draft; not ruled |
| `ext/draft/background-progress.h` | not supported | a draft; not ruled |
| `ext/draft/background-state-context.h` | not supported | a draft; not ruled |
| `ext/draft/extensible-audio-ports.h` | not supported | #92 |
| `ext/draft/flush-events.h` | not supported | a draft; not ruled |
| `ext/draft/gain-adjustment-metering.h` | supported | read by omx-clap-host's meters (docs/meters.md) |
| `ext/draft/mini-curve-display.h` | not supported | ruled supported (#95): the console's panels draw EQ and dynamics curves; #188 |
| `ext/draft/octave-number.h` | not supported | a draft; not ruled |
| `ext/draft/param-hovered.h` | not supported | a draft; not ruled |
| `ext/draft/params-origin.h` | not supported | a draft; not ruled |
| `ext/draft/project-location.h` | not supported | a draft; not ruled |
| `ext/draft/resource-directory.h` | not supported | P3 (ruled 2026-10-19, #95) |
| `ext/draft/scratch-memory.h` | not supported | a draft; not ruled |
| `ext/draft/transport-control.h` | not supported | a draft; not ruled |
| `ext/draft/triggers.h` | not supported | a draft; not ruled |
| `ext/draft/tuning.h` | not supported | #186 |
| `ext/draft/undo.h` | not planned | the console keeps its own undo (ruled 2026-10-19, #95) |
| `ext/draft/webview.h` | not planned in the console | a plugin's own UI, as `gui` (GUI ruling 2026-10-19) |

Besides the headers: `clap.meters/1`, the console's own meters extension
(`src/clap_meters.h`, docs/meters.md), is supported, and so is the
preset-discovery factory (Presets above).

