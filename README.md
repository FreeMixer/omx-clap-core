omx-clap-host
=============

omx-clap-host runs CLAP audio plugins without a screen. You drive it over a socket with the
same text commands as mod-host, and each plugin appears in your JACK or PipeWire graph as
a client of its own. It is for controllers, mixers and stage rigs on Linux that already talk
to mod-host and want CLAP plugins as well.

- Same protocol as mod-host: `add`, `param_set`, `bypass`, `connect`, with mod-host's replies and error codes.
- Every plugin is a JACK client `effect_<N>` with ports `in_<k>` and `out_<k>`, laid out as mod-host lays out an LV2 plugin.
- Instruments work too: a note input becomes a MIDI port, `effect_<N>:midi_in`.
- Meters and latency reach your graph: gain-reduction meters come back as `output_set` lines, and the plugin's latency is published on its ports.
- `omx-clap-scan` lists what a CLAP file holds (parameters, ports, latency), as text or JSON.

LV2 plugins are loaded through the same CLAP host by an LV2 to CLAP adapter, the library
`libomx-clap-lv2` (package `omx-clap-lv2-devel`, or `libomx-clap-lv2-dev` on Debian). What works
today, and the test that proves each part, is in [docs/lv2-compatibility.md](docs/lv2-compatibility.md).

The CLAP hosting core is a library of its own, `libomx-clap-core`, for programs that host CLAP
plugins themselves: it checks a plugin before using it, runs its audio on your thread and keeps
its parameters and state. The host, the core and the LV2 adapter share one version.

Install
-------

Fedora (x86_64 and aarch64):

    sudo dnf config-manager addrepo --from-repofile=https://freemixer.github.io/rpm/freemixer.repo
    sudo dnf install omx-clap-host

Debian and Raspberry Pi OS (amd64 and arm64): add the apt line from <https://freemixer.github.io>, then

    sudo apt install omx-clap-host

Version 0.4.0 ships these packages, all from the same release:

| What | Fedora | Debian |
|---|---|---|
| The host and the scanner: `omx-clap-host`, `omx-clap-scan`, their manual pages | `omx-clap-host` | `omx-clap-host` |
| The hosting core, a shared library | `omx-clap-core` | `libomx-clap-core0` |
| Headers, pkg-config file and ABI baseline to build a program on the core | `omx-clap-core-devel` | `libomx-clap-core-dev` |
| Headers, pkg-config file and static library to run LV2 plugins in a CLAP host | `omx-clap-lv2-devel` | `libomx-clap-lv2-dev` |

For your own program, install the `-devel` / `-dev` package and build with
`pkg-config --cflags --libs omx-clap-core` (and `omx-clap-lv2` for the adapter).

Try it
------

List what a CLAP file holds (any `.clap` will do; this one is LSP's):

    omx-clap-scan /usr/lib64/clap/lsp-plugins.clap

Start the host. It prints `omx-clap-host ready!`:

    omx-clap-host -n -p 5555

In another terminal, load a limiter, set its output gain to -6 dB and read it back
(messages end in a NUL byte, which `printf` adds; a parameter is the plugin's CLAP id,
which the scan prints):

    send() { printf '%s\0' "$1" | nc -w3 127.0.0.1 5555 | tr '\0' '\n'; }
    send "add clap:/usr/lib64/clap/lsp-plugins.clap#in.lsp-plug.limiter_stereo 0"   # resp 0
    send "param_set 0 3630838676 -6"                                               # resp 0
    send "param_get 0 3630838676"                                                  # resp 0 -6.0000
    send "bypass 0 1"                                                              # resp 0

The plugin is now the JACK client `effect_0`: link your audio to its `in_<k>` ports and take it
from its `out_<k>` ports.

More: [every command, meters, bypass, port layout, latency and scanning](doc/reference.md), the
manual pages (`man omx-clap-host`, `man omx-clap-scan`) and <https://freemixer.github.io>.
Building from source: see [BUILDING.md](BUILDING.md).
