Building omx-clap-host
======================

make [MOD_HOST_DIR=<mod-host checkout>] [PLUGIN_HOSTD_DIR=<plugin-hostd checkout>] [CLAP_CFLAGS=-I<clap headers>] [JACK_DIR=<jack install tree>]

The protocol is mod-host's `libmod-host-protocol.so.0`, linked as a shared
library: from `pkg-config mod-host-protocol` when it is installed
(mod-host's `make install-lib`, or the mod-host-protocol-devel package),
otherwise from `MOD_HOST_DIR`, a mod-host tree where the library is built
if missing and whose path becomes the binary's rpath. The layout pin's verb,
its error codes and its serialisation are plugin-hostd's headers
`plugin-hostd/protocol.h` and `plugin-hostd/pin.h`: from `pkg-config plugin-hostd`
(the plugin-hostd-devel package) or from `PLUGIN_HOSTD_DIR/include`. The CLAP
headers come from `pkg-config --cflags clap` (Fedora `clap-devel`) or `CLAP_CFLAGS`.
JACK's headers and library come from `pkg-config jack` (the
pipewire-jack-audio-connection-kit-devel package) or, when that is absent,
from `JACK_DIR/include` and `JACK_DIR/lib`, the latter also becoming the
binary's rpath; `JACK_DIR` defaults to `../jack-dev`.

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

    make test-jack-meters

runs `tests/jack_meters_e2e.sh` in the same kind of namespace: the host with
`tests/fake_compressor.clap` fed a constant input by `tests/jack_meter_source`,
`monitor_output` on every symbol its meters derive and on some they don't,
the `output_set` lines of the feedback socket against the values the input
makes, the standard gain adjustment alone and bypassed, and the plugin whose
meters clash refused.

    make test-jack-info

runs `tests/jack_info_e2e.sh` in the same kind of namespace: `track_info`
read back byte for byte by the fake plugin through `clap.track-info`, the
names and colours that are refused, both remote pages of the fake with the
slots `param_set` would refuse written `-`, `remote_pages_changed` on the
feedback socket when the fake calls the host's `changed`, `param_info`, and
every call of the plugin on the thread that ran `init`.

    make test-jack-pin

runs `tests/jack_pin_e2e.sh` in the same kind of namespace: `pin_expect` with
the layout pin `tests/clap_layout_pin` computes for `tests/fake.clap`, the
plugin that matches loaded, activated and processing, the same binary started
with another parameter default (`FAKE_LAYOUT_DEFAULT`) refused with no
activate and no process in its `FAKE_LOG`, a pin for another instance leaving
this one alone, and a scheme the host does not know refused.

    make test-hostd-pin [PLUGIN_HOSTD=<plugin-hostd>]

runs `tests/hostd_pin_e2e.sh`: the same behind plugin-hostd with
`require_pins 1` and this host as its CLAP worker, `pin_set` with the true
layout and with another, an unpinned plugin and a wrong binary digest.

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

The audio role is what the plugin's `clap.thread-check` reads: `is_audio_thread`
answers true on the thread that holds it and false on every other, and a
`[main-thread]` host call from it is counted a violation. `omx_clap_host_publish(in, rt)`
and `omx_clap_host_set_audio_thread(in, thread)` name one thread. A caller whose walk
runs on a driver and N workers names them all with a predicate:

    int is_audio(void *ctx, pthread_t self);
    omx_clap_host_publish_role(in, driver, is_audio, ctx);
    omx_clap_host_set_audio_role(in, driver, is_audio, ctx);   /* the same on a running instance */

`is_audio_thread` then answers true on `driver`, and on any thread for which
`is_audio(ctx, self)` returns nonzero; false otherwise. The predicate runs on the
plugin's calling thread, the RT one included: it must not block, allocate or lock, and
`ctx` must stay valid until the instance is unpublished. `omx_clap_host_unpublish`
clears it; `omx_clap_host_publish` and `omx_clap_host_set_audio_thread` clear it too, so a
caller of the one-thread calls sees exactly what it saw before. While the control thread
takes the role (`omx_clap_host_take_role`), no cycle runs, and the predicate is left as
it is for the role's release. The library knows nothing of the caller's threads beyond
the predicate.

What a host differs in is a configuration, set once per process with
`omx_clap_host_configure()` and otherwise the defaults:

| setting | defaults | omx-clap-host |
|---|---|---|
| clamp at +24 dBFS | on | off |
| non-finite scan and strike | on | off |
| warm-up before publish, restart after | on | off |
| note inputs and instruments | refused | admitted |
| `clap.preset-load` host extension | not offered | offered |
| `clap.track-info` and `clap.remote-controls` host extensions | not offered | offered |
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
