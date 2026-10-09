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

The message ring
----------------

`omx_msgring.h`, installed with the library's headers, is the variable-length
single-producer single-consumer ring between a plugin's RT thread and a non-RT
thread of its host: a 4-byte size, then the payload, contiguous; a full ring refuses
a record and never waits. Header only, so it changes nothing in the ABI. `make
test-msgring` runs its closed-form test, and `make test-fake` includes it. The LV2
adapter's worker uses the same ring.

The LV2 adapter
---------------

    make libomx-clap-lv2.a

builds `libomx-clap-lv2.a`, the library that presents one LV2 plugin as one CLAP
plugin, from `lv2/`. It needs the LV2 headers and lilv's (`lv2-devel` and
`lilv-devel` on Fedora, `lv2-dev` and `liblilv-dev` on Debian), found through
`pkg-config lv2 lilv-0` or `LV2_CFLAGS`. lilv is never linked: the adapter dlopens
`liblilv-0.so.0` when the first bundle is opened, so a program that never opens one
never needs it.

`lv2/core` is the LV2 host, with no CLAP in it: the bundle and its ports, the
refusals, the features the plugin is given, the worker thread and the LV2 half of a
block. `lv2/clap` is the shim that makes it a CLAP plugin; its header,
`lv2/clap/omx_clap_lv2.h`, is the whole public surface:

    omx_clap_lv2_configure(&cfg, why);           /* once per process: the features to provide, the numbers */
    entry = omx_clap_lv2_entry("/usr/lib64/lv2/some.lv2", why);
    /* entry->init, entry->get_factory(CLAP_PLUGIN_FACTORY_ID), factory->create_plugin(..., "<LV2 URI>") */
    omx_clap_lv2_entry_release(entry);

A plugin the adapter cannot run is refused by `create_plugin`, which writes the
reason (the verdict's code, such as `hosting.features.cv-ports`) to the host's
`clap.log`. `make install` installs the archive, `omx-clap-lv2/omx_clap_lv2.h` and
`omx-clap-lv2.pc`; the packages are `omx-clap-lv2-devel` (RPM) and
`libomx-clap-lv2-dev` (deb).

    make test-lv2

builds the fixture bundles into `build/lv2/` and runs the adapter's tests:
`lv2/tests/lv2_run_test` (the LV2 half of a block against fake plugins),
`lv2/tests/lv2_host_test` (bundles, ports, refusals, features and the worker),
`lv2/tests/lv2_clap_test` (the CLAP face, and libomx-clap-core's body around it),
`lv2/tests/lv2_link_test` (a program built against the installed files through
pkg-config alone) and, when `lv2_validate` is installed, the fixture bundles against
the LV2 specifications. `make lv2-cost` prints what the adapter costs per `process()`
call at 96 and 192 kHz, quantum 128. What each test proves is listed in
[docs/lv2-compatibility.md](docs/lv2-compatibility.md).

The version
-----------

The file `VERSION` holds the one version of the whole tree: the programs, `libomx-clap-core`
(its file is `libomx-clap-core.so.<major>.<minor>.<patch>`) and `libomx-clap-lv2`, with the
`.pc` files and the manual pages. The Makefile reads it and hands it to the compiler, so no
source file spells it. A release is: put the
version in `VERSION`, the spec's `Version` and a new top entry of `debian/changelog`, run `make
abi-baseline`, and tag `v<version>`. `make version-check` and the version job in CI refuse a tree
whose files disagree. There is no changelog file: git history is the changelog.
