omx-clap-host
=============

A headless CLAP plugin host driven over a socket with mod-host's command
protocol. Each plugin instance is a jack client named `effect_<N>` with
ports `in_<k>` and `out_<k>`, exactly as mod-host lays out an LV2 plugin,
so a controller that speaks to mod-host can speak to this host unchanged.

The socket server, the line protocol and the command dispatch are
mod-host's own, linked from its `libmod-host-plumbing.so`; this project
adds the CLAP side only.

Building
--------

    make [MOD_HOST_DIR=<mod-host checkout>] [CLAP_CFLAGS=-I<clap headers>]

The plumbing library comes from `pkg-config mod-host-plumbing` when it is
installed (mod-host's `make install`, or the mod-host-devel package).
Otherwise `MOD_HOST_DIR` must hold a mod-host tree, where
`libmod-host-plumbing.so` is built if missing; the binary then finds it
there through its rpath. The CLAP headers come from
`pkg-config --cflags clap` (Fedora `clap-devel`) or `CLAP_CFLAGS`.

    make test CLAP_TEST_PLUGIN=<some>.clap

runs the lifecycle test against a plugin without jack, and against
`tests/fake.clap`, a plugin built for the test that carries the port
layouts the host refuses and a passthrough with a latency.

    make test-jack CLAP_TEST_PLUGIN=<omx-delay>.clap

runs `tests/jack_e2e.sh`: the host over jack inside a PipeWire of its own
(a private user, net and pid namespace, its own runtime dir, torn down on
exit), every command over the socket and the graph read back after each
one. The PipeWire of the session that runs it is never touched.

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

Replies are mod-host's: `resp <code>`, with mod-host's error codes.

Port layout
-----------

A plugin is admitted with exactly one main audio input and one main audio
output, each mono or stereo; every channel of the main pair is one jack
port. A sidechain, an auxiliary port, a main port wider than stereo or a
note input is refused at `add` with `resp -102` and the reason on
stderr: `<id>: unsupported port layout: main port has 4 channels`,
`1 sidechain/aux ports`, `note input`. A sidechain is not fed silence,
because a plugin behaving on a silent sidechain is not the plugin the
same core gives in-process.

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
