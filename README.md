omx-clap-host
=============

A headless CLAP plugin host driven over a socket with mod-host's command
protocol. Each plugin instance is a jack client named `effect_<N>` with
ports `in_<k>` and `out_<k>`, exactly as mod-host lays out an LV2 plugin,
so a controller that speaks to mod-host can speak to this host unchanged.

The socket server, the line protocol and the command dispatch are
mod-host's own, linked from its `libmod-host-plumbing.a`; this project
adds the CLAP side only.

Building
--------

    make MOD_HOST_DIR=<mod-host checkout> [CLAP_CFLAGS=-I<clap headers>]

`MOD_HOST_DIR` must hold a mod-host tree built with its plumbing library
(`make libmod-host-plumbing.a` there). The CLAP headers come from
`pkg-config --cflags clap` (Fedora `clap-devel`) or `CLAP_CFLAGS`.

    make test CLAP_TEST_PLUGIN=<some>.clap

runs the lifecycle test against a plugin without jack.

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
