omx-clap-host
=============

A headless CLAP plugin host driven over a socket with mod-host's command
protocol. Each plugin instance is a jack client named `effect_<N>` with
ports `in_<k>` and `out_<k>`, exactly as mod-host lays out an LV2 plugin,
so a controller that speaks to mod-host can speak to this host unchanged.

The socket server, the line protocol and the command dispatch are
mod-host's own, linked from its `libmod-host-protocol.so.0`; this project
adds the CLAP side only.

Building
--------

    make [MOD_HOST_DIR=<mod-host checkout>] [CLAP_CFLAGS=-I<clap headers>]

The protocol is mod-host's `libmod-host-protocol.so.0`, linked as a shared
library: from `pkg-config mod-host-protocol` when it is installed
(mod-host's `make install-lib`, or the mod-host-protocol-devel package),
otherwise from `MOD_HOST_DIR`, a mod-host tree where the library is built
if missing and whose path becomes the binary's rpath. The CLAP headers
come from `pkg-config --cflags clap` (Fedora `clap-devel`) or `CLAP_CFLAGS`.

    make test CLAP_TEST_PLUGIN=<some>.clap

builds `libomx-clap-core.so.0`, checks its exports and the libraries it needs
(`tests/exports.sh`), links `tests/core_link_test.c` to the installed library
and runs it with the defaults, and runs the lifecycle test against a plugin without jack, and against
`tests/fake.clap`, a plugin built for the test that carries the port
layouts the host refuses and a passthrough with a latency, and against
`tests/fake_synth.clap`, a synth whose notes are exact to the sample.

    make test-jack CLAP_TEST_PLUGIN=<omx-delay>.clap

runs `tests/jack_e2e.sh`: the host over jack inside a PipeWire of its own
(a private user, net and pid namespace, its own runtime dir, torn down on
exit), every command over the socket and the graph read back after each
one. The PipeWire of the session that runs it is never touched.

    make test-jack-synth

runs `tests/jack_synth_e2e.sh` in the same kind of namespace: the host with
`tests/fake_synth.clap`, MIDI played into `effect_<N>:midi_in` by
`tests/jack_synth_probe`, the level that comes out of `out_<k>` measured
before the note, at two velocities and after the note off, and the layouts
the host refuses.

    make test-identity MOD_HOST=<mod-host> CLAP_TEST_PLUGIN=<omx-delay>.clap [LV2_DIR=<dir with omx-delay.lv2>]

runs `tests/clap_lv2_identity.sh` in the same kind of namespace: the LV2
twin of omx-delay through mod-host and the CLAP twin through this host,
the same parameters on both, one deterministic input fed to both from
the same cycles by `tests/jack_identity`, the outputs compared bit for
bit with `cmp`; then both bypassed, compared again and the CLAP side
compared to its input. It prints the sample counts and, on a difference,
the first differing sample.

    make test-scan CLAP_TEST_PLUGIN=<omx-delay>.clap

runs `tests/clap_scan_test`: `omx-clap-scan` against `tests/fake.clap`, a
`tests/crash.clap` whose entry point aborts, a file that is no library, a
directory walk and a path that can't be read.

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

An extra audio input (a sidechain), a main port wider than stereo, a main
pair of different widths, a second note input, a note input that reads
neither the CLAP nor the MIDI dialect, or no audio input and no note input
is refused at `add` with `resp -102` and the `hosting.*` code on stderr:
`<id>: hosting.topology.extra-inputs-fed-silence`,
`hosting.topology.wider-than-strip`, `hosting.clap.note-input`,
`hosting.topology.no-audio-input`. A sidechain is not fed silence, because a
plugin behaving on a silent sidechain is not the plugin the same core gives
in-process. An extra audio output is admitted and left unconnected: the
plugin is handed a scratch buffer for it. A plugin is admitted with the
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

The core library
----------------

The CLAP hosting core, everything that runs a plugin except the jack plumbing and
the mod-host verbs, is a shared library of its own, `libomx-clap-core.so.0`, so that
a program that hosts CLAP plugins in its own process runs the same core. omx-clap-host
and omx-clap-scan link it; a program that hosts plugins in its own process links the same file.

    make install-lib [PREFIX=/usr LIBDIR=/usr/lib64]

installs the library, `omx-clap-core.pc`, the headers under
`include/omx-clap-host/` and the export list. `pkg-config --cflags --libs
omx-clap-core` builds a program against it; `tests/core_link_test.c` is one, linked
to the `.so` alone. The library names no jack, no socket and no protocol library.

- `hosted_stage.h`, `clap_stage.h`: the RT body, inline, so the caller's own RT thread
  runs it without a call through the library. The layout of `struct omx_hosted_stage`,
  `struct omx_clap_stage` and `struct omx_clap_instance` is therefore part of the ABI.
- `clap_host.h`: the control thread's side, the exported functions, `omx_clap_host_*`.
- `clap_host_limits.h`: every number and string the core reads, generated from the
  declarations of the program that owns the numbers and committed here; never edited by hand.
- `omx_clap_ext.h`: the openmixer vendor extensions a plugin serves through `get_extension`,
  `org.openmixer.meters/1` and `org.openmixer.declaration/1`, as exact C structures. Header only:
  the library exports nothing for them.

What a host differs in is a configuration, set once per process with
`omx_clap_host_configure()` and otherwise the defaults:

| setting | defaults | omx-clap-host |
|---|---|---|
| clamp at +24 dBFS | on | off |
| non-finite scan and strike | on | off |
| warm-up before publish, restart after | on | off |
| note inputs and instruments | refused | admitted |
| `clap.preset-load` host extension | not offered | offered |
| host name, vendor, url | omx-clap-core, Pau Aliagas | omx-clap-host, Pau Aliagas |

The packages are `omx-clap-core` and `omx-clap-core-devel` (RPM), `libomx-clap-core0`
and `libomx-clap-core-dev` (deb), built from the same tag as omx-clap-host, which
depends on the library.

The version rule: a field is only ever appended to a structure and a function only
added, and that is a new minor (`0.1.0` to `0.2.0`, the soname unchanged); a field or a
function removed, moved or changed is a new soname major (`libomx-clap-core.so.1`, a new
package name). `make abi-check` compares a build with `abi/libomx-clap-core.so.0.abi`,
the baseline of the last release, with libabigail's `abidiff`, and CI runs it on every
push and before every release. A release commit records its own baseline with `make
abi-baseline` and commits `abi/`.
