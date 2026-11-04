omx-clap-host
=============

omx-clap-host hosts CLAP and LV2 audio plugins without a screen. You drive it over a socket
with the same text commands as mod-host, and each plugin appears in your JACK or PipeWire
graph as a client of its own. It is for controllers, mixers and stage rigs on Linux that
already talk to mod-host and want CLAP plugins beside their LV2 ones.

An LV2 plugin runs natively, through lilv in the host's own process, mapped by the LV2 adapter
(`libomx-clap-lv2`). The CLAP hosting core is a library of its own, `libomx-clap-core`, for
programs that host CLAP plugins themselves. The host, the core and the adapter share one version.

Install
-------

Fedora 44 (x86_64 and aarch64):

    sudo dnf install https://www.openmixer.org/rpm/openmixer-release.noarch.rpm
    sudo dnf install omx-clap-host

Debian bookworm and trixie and Raspberry Pi OS (amd64 and arm64):

    curl -fsSLO https://www.openmixer.org/deb/openmixer-release.deb && sudo apt install ./openmixer-release.deb
    sudo apt install omx-clap-host

To build on the core: `omx-clap-core-devel` (`libomx-clap-core-dev`) and
`pkg-config --cflags --libs omx-clap-core`.

Start
-----

    omx-clap-scan /usr/lib64/clap/lsp-plugins.clap      # what a CLAP file holds
    omx-clap-host -n -p 5555                            # prints: omx-clap-host ready!

In another terminal (messages end in a NUL byte):

    send() { printf '%s\0' "$1" | nc -w3 127.0.0.1 5555 | tr '\0' '\n'; }
    send "add clap:/usr/lib64/clap/lsp-plugins.clap#in.lsp-plug.limiter_stereo 0"   # resp 0
    send "add lv2:urn:zamaudio:ZamComp 1"                                          # resp 1

Each plugin is now the JACK client `effect_<N>`, with ports `in_<k>` and `out_<k>`.

Documentation
-------------

<https://www.openmixer.org/projects/omx-clap-core>, the manual pages (`man omx-clap-host`,
`man omx-clap-scan`), and in this repository: the features, the packages, what CI tests and a
longer walk-through ([docs/overview.md](docs/overview.md)); every command
([doc/reference.md](doc/reference.md)); LV2 and CLAP support, row by row
([docs/lv2-compatibility.md](docs/lv2-compatibility.md),
[docs/clap-compatibility.md](docs/clap-compatibility.md)); building from source
([BUILDING.md](BUILDING.md)).

Licence
-------

GPL-3.0-or-later. See [COPYING](COPYING).
