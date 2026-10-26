omx-clap-host
=============

omx-clap-host hosts CLAP and LV2 audio plugins without a screen. You drive it over a socket
with the same text commands as mod-host, and each plugin appears in your JACK or PipeWire
graph as a client of its own. It is for controllers, mixers and stage rigs on Linux that
already talk to mod-host and want CLAP plugins beside their LV2 ones.

- Same protocol as mod-host: `add`, `param_set`, `bypass`, `connect`, with mod-host's replies and error codes.
- Every plugin is a JACK client `effect_<N>` with ports `in_<k>` and `out_<k>`, laid out as mod-host lays out an LV2 plugin.
- Instruments work too: a note input becomes a MIDI port, `effect_<N>:midi_in`, and a note output another, `effect_<N>:midi_out`.
- LV2 plugins too: `add lv2:<uri> <N>` (or the URI alone, as mod-host takes it) runs an LV2 plugin from `LV2_PATH` on the same ports, its controls addressed by their LV2 symbols.
- Tempo-synced plugins get a transport: `set_bpm`, `set_bpb` and `transport` drive the host's own, or `-t` follows the JACK transport.
- Meters and latency reach your graph: gain-reduction meters come back as `output_set` lines, and the plugin's latency is published on its ports.
- A plugin's own window, on request: `show_external_ui <N>` puts it on your display, as jalv and mod-host show one, and `hide_external_ui <N>` takes it off. On a Wayland session it is a Wayland window when the plugin has one; X11 otherwise.
- `-S` shows every plugin's window as it is added; with no display the command says so by code and the audio runs on.
- `omx-clap-scan` lists what a CLAP file holds (parameters, ports, latency), as text or JSON; `--fields` dumps every field one CLAP or LV2 plugin states, the records plugin-hostd builds its descriptors from.

An LV2 plugin runs natively: it is a real LV2 instance, loaded through lilv into the host's own
process, with no emulation and no wrapper process. What sits between it and the host is the
LV2 adapter, the library `libomx-clap-lv2` linked into omx-clap-host, and it maps the host
interface only: the plugin's ports and controls are presented as CLAP ports and parameters, so
one hosting core drives both formats. Any program can link the adapter too, from the package
`omx-clap-lv2-devel` (`libomx-clap-lv2-dev` on Debian). lilv is loaded on the first LV2 add. A
plugin the adapter cannot map is refused with its reason: `resp -102 hosting.features.missing`.
What is supported, what is not and the test behind each row is in
[docs/lv2-compatibility.md](docs/lv2-compatibility.md). plugin-hostd, which runs each plugin in a
process of its own, hosts CLAP and LV2 with omx-clap-host as the worker of both, and mod-host for
an LV2 plugin the adapter refuses.

The CLAP hosting core is a library of its own, `libomx-clap-core`, for programs that host CLAP
plugins themselves: it checks a plugin before using it, runs its audio on your thread and keeps
its parameters and state. The host, the core and the LV2 adapter share one version.

Install
-------

Fedora (x86_64 and aarch64):

    sudo dnf config-manager addrepo --from-repofile=https://freemixer.github.io/rpm/freemixer.repo
    sudo dnf install omx-clap-host

Debian bookworm and trixie and Raspberry Pi OS (amd64 and arm64):

    curl -fsSL https://freemixer.github.io/deb/freemixer.asc | sudo tee /usr/share/keyrings/freemixer.asc >/dev/null
    echo "deb [signed-by=/usr/share/keyrings/freemixer.asc] https://freemixer.github.io/deb/debian/$(. /etc/os-release; echo $VERSION_CODENAME) ./" | sudo tee /etc/apt/sources.list.d/freemixer.list
    sudo apt update && sudo apt install omx-clap-host

Version 0.22.1 ships these packages, all from the same release:

| What | Fedora | Debian |
|---|---|---|
| The host and the scanner: `omx-clap-host`, `omx-clap-scan`, their manual pages | `omx-clap-host` | `omx-clap-host` |
| The hosting core, a shared library | `omx-clap-core` | `libomx-clap-core1` |
| Headers, pkg-config file and ABI baseline to build a program on the core | `omx-clap-core-devel` | `libomx-clap-core-dev` |
| The core's definitions in omx-contract's contract directory: hosting limits and codes, the host verbs | `omx-clap-core-contract` (noarch) | `omx-clap-core-contract` (all) |
| Headers, pkg-config file and static library of the LV2 adapter, to link at build time (LV2 at run time is `omx-clap-host` with lilv) | `omx-clap-lv2-devel` | `libomx-clap-lv2-dev` |

For your own program, install the `-devel` / `-dev` package and build with
`pkg-config --cflags --libs omx-clap-core` (and `omx-clap-lv2` for the adapter).

Status
------

Version 0.22.1 is released and packaged for Fedora 44 and Debian bookworm and trixie. What
the continuous integration runs on every push, and the packages it builds:

- The plugin lifecycle and the scanner against fake plugins, including a plugin that crashes.
- The hosting core with untrusted plugins, under AddressSanitizer and UndefinedBehaviorSanitizer.
- The LV2 adapter against fixture bundles, validated with `lv2_validate`.
- An LV2 plugin through the host in a JACK graph: `add lv2:<uri>`, refusals with their codes,
  parameters by symbol, `lv2:enabled` as the bypass, state, presets and remove; and its output bit
  for bit against mod-host and against the adapter in process.
- The host in a JACK graph, over PipeWire's JACK and over JACK2's `jackd` (x86_64): port layout,
  latency, MIDI input for instruments, meters.
- The exported symbols of `libomx-clap-core` against a versioned baseline.
- RPMs (Fedora 44, x86_64 and aarch64) and DEBs (bookworm and trixie, amd64 and arm64), each
  installed in a clean root or container.

Almost all of LV2 is supported: [docs/lv2-compatibility.md](docs/lv2-compatibility.md) lists
each feature with the test behind it, and the few that are not supported, each with its reason.
Every CLAP extension, what the host does with it and the test behind each claim, is in
[docs/clap-compatibility.md](docs/clap-compatibility.md).

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
from its `out_<k>` ports. On a desktop, or over the VNC display a Zynthian serves, a plugin that
has its own window shows it (CLAP plugins with Wayland or X11 windows; LV2 desktop UIs are not
shown yet):

    send "show_external_ui 0"                                                      # resp 0
    send "hide_external_ui 0"                                                      # resp 0

The show tries, in order: a Wayland floating window, when `WAYLAND_DISPLAY` names a compositor
and the plugin offers one; an X11 window embedded in one of the host's, on `DISPLAY`; an X11
floating window. The first the plugin offers whose display answers is taken.

An LV2 plugin is added by its URI, from the bundles on `LV2_PATH`, and its controls are its port
symbols, as with mod-host. Its audio ports are read by the LV2 port groups, so a mono-in
stereo-out reverb has `in_1` and `out_1`, `out_2`; a plugin the LV2 adapter cannot map answers
`resp -102` and the code that says why:

    send "add lv2:urn:zamaudio:ZamComp 1"                                          # resp 1
    send "param_set 1 thr -20"                                                     # resp 0
    send "param_get 1 thr"                                                         # resp 0 -20.0000
    send "add lv2:http://moddevices.com/plugins/caps/Plate 2"                       # resp 2

More: [every command, meters, bypass, port layout, latency and scanning](doc/reference.md), the
manual pages (`man omx-clap-host`, `man omx-clap-scan`) and <https://freemixer.github.io>.
Building from source: see [BUILDING.md](BUILDING.md).
