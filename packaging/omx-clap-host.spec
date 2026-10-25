Name: omx-clap-host
Version:        0.21.0
Release: 1%{?dist}
License: GPL-3.0-or-later
Summary: Use CLAP plugins on a JACK or PipeWire rig, driven like mod-host
URL: https://github.com/FreeMixer/omx-clap-core

Source0: %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires: gcc
BuildRequires: make
BuildRequires: pkgconfig
BuildRequires: pkgconfig(mod-host-protocol)
BuildRequires: pkgconfig(plugin-hostd) >= 0.4.0
BuildRequires: clap-devel
BuildRequires: omx-dsp-devel >= 0.18.0
# make install puts data/ into omx-contract's contract directory with omx-contract's install script, and the sources
# the codes under contract.d/3 (3.0.0, the hosting codes moved here)
BuildRequires: omx-contract-devel >= 3.1.0
BuildRequires: lv2-devel
BuildRequires: lilv-devel
# make test-lv2 runs the MIDI output path under AddressSanitizer
BuildRequires: libasan
# make test-fake reads the sources for hand-typed hosting codes with perl (tests/hosting-codes-check.sh)
BuildRequires: perl-interpreter
BuildRequires: pipewire-jack-audio-connection-kit-devel
# a plugin's own window (show_external_ui, -S) over X11; the package then requires the libxcb it links
BuildRequires: pkgconfig(xcb)
BuildRequires: systemd-rpm-macros
Requires: omx-clap-core%{?_isa} = %{version}-%{release}
# LV2 plugins run through the adapter linked into the host, which loads lilv on the first LV2 add
Recommends: lilv-libs

%description
omx-clap-host brings CLAP plugins, the modern open plugin format, into a live
JACK or PipeWire rig. Each plugin runs as a JACK client with its own ports, and
an instrument gets a MIDI input as well. Anything that already controls
mod-host can control it over the same socket with the same commands. LV2
plugins run in it too, by their URI, through its LV2 to CLAP adapter. It comes
with omx-clap-scan, which lists what is in a CLAP bundle and survives a plugin
that crashes while loading.

%package tests
Summary: Fixtures to test a program that drives omx-clap-host
Requires: %{name}%{?_isa} = %{version}-%{release}

%description tests
Plugins and a signal source for testing a program that runs omx-clap-host as
its CLAP worker: a plugin that reads the track information the host passes
it, a compressor that reports meters, and a JACK client that plays a constant
level. They are installed in %{_libexecdir}/omx-clap-host/tests, away from the
directories CLAP hosts scan, and are of no use to anyone else.
socket_conformance.py, beside them, checks how any server of the mod-host
socket protocol splits what it receives into commands.

%package -n omx-clap-core
Summary: The engine that runs CLAP plugins for omx-clap-host, as a library
License: GPL-3.0-or-later

%description -n omx-clap-core
The library that loads and runs CLAP plugins: it checks a plugin before using
it, activates and warms it up, keeps its parameters and saved state, and runs
its audio on the thread of the program that hosts it. It knows nothing about
JACK or sockets, so a program of your own can host CLAP plugins on it as well.

%package -n omx-clap-core-devel
Summary: Build a program that hosts CLAP plugins on omx-clap-core
License: GPL-3.0-or-later
Requires: omx-clap-core%{?_isa} = %{version}-%{release}
Requires: clap-devel
# hosted_stage.h includes omx-dsp's headers
Requires: omx-dsp-devel >= 0.18.0
# the hosting limits and codes, in omx-contract's contract directory, which that package owns (3.0.0), and
# omxcontract/omx_resource_hints.h, which clap_host.h includes (3.1.0)
Requires: omx-contract-devel >= 3.1.0
Requires: pkgconfig

%description -n omx-clap-core-devel
What you need to write a program that hosts CLAP plugins on omx-clap-core: the
headers, the pkg-config file, the list of functions the library promises to
keep, with the baseline each release is checked against, and the hosting
limits as definitions in omx-contract's contract directory.

%package -n omx-clap-lv2-devel
Summary: Run LV2 plugins in a CLAP host, as a library to link
License: GPL-3.0-or-later
Requires: omx-clap-core-devel%{?_isa} = %{version}-%{release}
Requires: clap-devel
Requires: pkgconfig

%description -n omx-clap-lv2-devel
libomx-clap-lv2 lets a program that hosts CLAP plugins run LV2 plugins too:
it presents one LV2 plugin as one CLAP plugin, with its controls as CLAP
parameters, its latency, its own background worker and its default
settings. It is a static library with its header and pkg-config file, and
it loads lilv when the first LV2 bundle is opened, so a program that never
opens one never needs lilv.

%prep
%autosetup

sed -i 's,LDFLAGS += -s,LDFLAGS +=,g' Makefile

%build

%set_build_flags

# the programs find the library where the package puts it, not in the build tree
%make_build RPATH=

%install

%make_install PREFIX=%{_prefix} LIBDIR=%{_libdir} UDEVDIR=%{_udevrulesdir}

%check

make test-fake
make test-lv2

%files
%license COPYING
%doc README.md
%{_bindir}/omx-clap-host
%{_bindir}/omx-clap-scan
%{_mandir}/man1/omx-clap-host.1*
%{_mandir}/man1/omx-clap-scan.1*
%{_udevrulesdir}/60-omx-clap-host.rules

%files tests
%license COPYING
%{_libexecdir}/omx-clap-host/tests/

%files -n omx-clap-core
%license COPYING
%{_libdir}/libomx-clap-core.so.1
%{_libdir}/libomx-clap-core.so.%{version}
%{_datadir}/omx-clap-core/ts/

%files -n omx-clap-core-devel
%{_libdir}/libomx-clap-core.so
%{_libdir}/pkgconfig/omx-clap-core.pc
%{_includedir}/omx-clap-host/
%{_datadir}/omx-clap-core/omx-clap-core.map
%{_datadir}/omx-clap-core/libomx-clap-core.so.*.abi
%{_datadir}/omx-contract/contract.d/3/omx-clap-core/

%files -n omx-clap-lv2-devel
%license COPYING
%{_libdir}/libomx-clap-lv2.a
%{_libdir}/pkgconfig/omx-clap-lv2.pc
%{_includedir}/omx-clap-lv2/

%changelog
* Sun Oct 25 2026 Pau Aliagas <linuxnow@gmail.com> - 0.21.0-1
- omx-clap-host runs plugin-hostd's chain line in one JACK client: chain
  <chain> <instance> ... makes the instances, in order, one chain in the
  first one's client through the library's chain, one node of the graph,
  each member running as it does in a client of its own (bypass, parameters
  and latency per plugin, the latency summed between the head's ports). A
  line for a running chain changes it in place, a member left out getting a
  client of its own back, a move in one walk; remove of a member closes the
  chain around it. A member with a note or an auxiliary port is refused.
  OMX_CLAP_HOST_CHAIN, the Stage 0 prototype, is gone. make test-chain-host,
  make test-chain-host-tsan, make test-hostd-chain (the crash drill behind
  plugin-hostd 0.7.0) and make test-chain-host-sabotage prove it (#104).
- libomx-clap-core: omx_clap_chain_order puts a chain's members in a new
  order as one published walk, so a move never runs a cycle without the
  moved member (OMX_CLAP_CORE_0.21, one function added) (#104).

* Sun Oct 25 2026 Pau Aliagas <linuxnow@gmail.com> - 0.20.1-1
- The fuzz harnesses read each input by its own grammar, never by the
  reader they test: the preset file's JSON, the state body's base64 and
  lv2_locate.h's naming rule, so a reader and writer that share a mistake,
  or a reader that refuses everything, is found. Every check of fuzz/
  records the one-line bug it catches and the seed that shows it, and make
  fuzz-records proves each alone (42 records and the 12 patches of
  fuzz/sabotage/); the library does not change (#345).

* Sun Oct 25 2026 Pau Aliagas <linuxnow@gmail.com> - 0.20.0-1
- omx-clap-scan prints the panel a plugin publishes through
  org.openmixer.panel-description/1: --json carries it as "panel" (family,
  fingerprint, sections, controls, roles, displays, placementDefaults), null
  when the plugin serves none, and --fields as the scopes panel, section:<i>,
  control:<i>, role:<i>, display:<i> and placement_default:<i>. Nothing is
  filled in: an empty table is empty. omx_clap_ext.h states the extension's
  structures, held to omx-plugins' omx_panel_description.h by make
  test-panel-abi; no export changes (#337).

* Sun Oct 25 2026 Pau Aliagas <linuxnow@gmail.com> - 0.19.1-1
- lv2_locate reads a manifest in work linear in its size: each declaration
  of a namespace the plugin URI starts with read the whole text again, so a
  4 MiB manifest repeating one took 10.7 s per lookup. Past
  LV2_LOCATE_PREFIXES_MAX (16) such declarations the bundle is a candidate
  the caller opens to know; tests/lv2_locate_test.c counts the readings
  (16 for 1024 declarations). libFuzzer harnesses for the preset file, its
  state body and the manifest (fuzz/, make fuzz, fuzz-run, fuzz-sabotage,
  fuzz-coverage, fuzz-static), run in CI on every push and pull request
  (#326).

* Sat Oct 24 2026 Pau Aliagas <linuxnow@gmail.com> - 0.19.0-1
- show_external_ui tries a Wayland window first: on a session whose
  WAYLAND_DISPLAY names a compositor that answers, a plugin that offers a
  Wayland window opens its own floating toplevel there (create("wayland",
  true), show); X11 embedded, then X11 floating, follow for a plugin with no
  Wayland window and for X11 sessions. hide_external_ui and remove work the
  same on it. The host links no Wayland library and keeps no Wayland
  connection, so a build without xcb (X11=0) shows Wayland windows too, and
  hosting.window.not-built, which nothing answers any more, is gone from the
  window codes; no export changes. A plugin that offers X11 embedded but
  has no set_parent is shown floating when it offers that, where it was
  hosting.window.unsupported (#329).

* Sat Oct 24 2026 Pau Aliagas <linuxnow@gmail.com> - 0.18.0-1
- omx-clap-host shows a plugin's own window, over clap.gui and X11, on
  request: show_external_ui <N> as mod-host takes it, hide_external_ui <N>,
  and -S (--show-ui) for every plugin at its add. The plugin's window is set
  into a window of the host's titled with the plugin's name, or floats on its
  own when that is all it offers; its timers and descriptors run on the host's
  tick. With no display, or a plugin with no X11 window, the command answers
  resp -404 and a code (hosting.window.no-display, unsupported, refused,
  not-built) and the audio runs on. libxcb is linked when found at build time.
  omx_clap_host_offer_gui, in the version node OMX_CLAP_CORE_0.18, hands a
  plugin the caller's clap.gui; the library draws nothing (#180).
* Sat Oct 24 2026 Pau Aliagas <linuxnow@gmail.com> - 0.17.0-1
- omx_clap_host_resource_hints reads the resource hints a plugin declares about
  itself (org.openmixer.resource-hints.draft/0, omx-contract 3.1.0's
  RESOURCE_HINTS), asked once at create: whole or refused by code
  (hosting.resource-hints.unknown-hint, wrong-unit, out-of-range), a hint whose
  bit is clear unknown, never zero. omx_clap_resource_hint_word, _unit and
  _figure give each hint in the contract's words and units. The LV2 adapter
  reads the same hints from the TTL's omxres properties and offers them through
  the draft; omx_clap_lv2_resource_hints_why names a refusal. omx-clap-scan
  --json writes them. ABI 17; the calls are in the version node
  OMX_CLAP_CORE_0.17. omx-contract 3.1.0 or newer (#323).
* Thu Oct 22 2026 Pau Aliagas <linuxnow@gmail.com> - 0.16.0-1
- clap_chain.h, a new header of libomx-clap-core: the chain, an ordered list of
  stages one audio cycle runs in order, each an instance the library hosts or a
  stage of the caller's own, each with a bypass flag (a bypassed member is not
  run, copied or counted), its latency and its channel layout; the chain reports
  the sum of its non-bypassed members' latencies and tells every change of it.
  omx_clap_chain_remove takes a member out and the next cycle equals a chain built
  without it. The cycle allocates, locks and waits for nothing; a cycle longer
  than OMX_CLAP_CHAIN_FRAMES_MAX (8192) passes dry and says so. Every structure a
  consumer allocates carries its ABI and size first and a mismatch is refused,
  OMX_CLAP_CHAIN_ERR_ABI. The functions are in the version node
  OMX_CLAP_CORE_0.16 (docs/chain.md, #311).
* Thu Oct 22 2026 Pau Aliagas <linuxnow@gmail.com> - 0.15.0-1
- omx_clap_host_config_default_sized(config, caller_size) bounds the defaults' stamp to caller_size,
  never to the library's own current sizeof: a consumer whose struct is smaller, because it was built
  against an older header and not rebuilt since, is filled only that far and never overrun past its own
  allocation. caller_size too short to carry even `version` is refused whole, not a byte moved.
  omx_clap_host_config_default, unchanged, still stamps its own current size and cannot make this
  promise; prefer the sized call. The call is in the version node OMX_CLAP_CORE_0.15 (#301).
- The hosting codes' TypeScript modules (omx-clap-core-hosting.js,
  omx-clap-core-vocabulary.js and their .d.ts) move from omx-clap-core-devel
  to omx-clap-core, at the same path, so a program that only reads them at
  run time, such as plugin-qualify, needs no development package.
  pkg-config --variable=tsdir omx-clap-core still names that path.
* Tue Oct 20 2026 Pau Aliagas <linuxnow@gmail.com> - 0.14.0-1
- libomx-clap-core holds /dev/cpu_dma_latency while a hosted plugin is active,
  at 10 us by default (C1 only), 0 for no idle state, or off: one descriptor
  per process, opened at the first activate and closed at the last, so a
  crash restores the machine's default. A device that cannot be opened is
  reported by code, hosting.idle.hold-refused, declared in this tree's
  data/hosting.json, and the host runs on. omx-clap-host takes --idle-hold
  and an idle_hold command; its packages ship a udev rule giving the audio
  group the device. The calls are in the version node OMX_CLAP_CORE_0.14.
  The library ABI is 16.
- The hosting codes are this tree's: omx-contract's 16 data/hosting files
  moved here byte for byte, installed under contract.d/3/omx-clap-core/hosting/
  and rendered as omx_clap_core_vocabulary.h and the omx-clap-core-vocabulary
  TypeScript module, the OMX_HOSTING_ names unchanged. The six retired codes
  are removed (72 codes are left) and featuresMissingAtLoad is featuresMissing.
  omx-clap-scan --fields clap:<path>#<id> | lv2:<uri> writes one plugin's
  field dump for plugin-hostd's format tables, an LV2 plugin read from the
  adapter's lilv world with its binary never opened. omx-contract 3.0.0 or
  newer.
* Tue Oct 20 2026 Pau Aliagas <linuxnow@gmail.com> - 0.13.0-1
- Every audio port a plugin declares is described, main and auxiliary, up to
  64 a side and 64 channels a port, with the refusal codes of omx-contract for
  what the bus cannot carry; the main pair may differ in width and the carrier
  runs any width (run_ports); an LV2 plugin's main and auxiliary ports are
  told apart by the port-group rules; surround and ambisonic ports are read
  through the CLAP extensions, a rescan reported and never a restart;
  request_layout asks a plugin for the carrier's layout; omx-clap-host binds a
  JACK port per auxiliary channel. The calls, with bind_aux_input in its per-
  channel-buffer shape and bind_aux_output, are in the version node
  OMX_CLAP_CORE_0.13.
- The LV2 adapter reads an ev:EventPort MIDI port as it reads the atom MIDI
  port, hosts a control-only plugin or a MIDI sink as a pass-through tool, and
  counts a CV port against the auxiliary limit of 64 a side and nothing else:
  the cap of 8 is gone and hosting.features.cv-ports is no longer raised.
- A CLAP plugin is admitted by what its ports declare, as the LV2 adapter
  decides; the descriptor's feature list refuses nothing and hosting.clap.not-
  audio-effect is no longer raised.
- tests/clap_coverage.py measures which installed CLAP plugins the host hosts
  and which extensions they ask for and are not offered; docs/clap-
  compatibility.md names every CLAP id the host offers or reads and the test
  behind it.
- The library ABI is 14. omx-contract 2.19.0 or newer.
* Tue Oct 20 2026 Pau Aliagas <linuxnow@gmail.com> - 0.12.0-1
- The host offers a plugin clap.note-ports, clap.note-name and
  clap.voice-info: supported_dialects answers CLAP, MIDI, MIDI MPE and
  MIDI 2.0; a rescan of every note port while active asks for a restart,
  and the next activate reads the note input again; note names read through
  omx_clap_host_note_name_count and omx_clap_host_note_name; a voice count
  the plugin says changed is asked again by the tick. A note port that
  speaks MIDI 2.0 gets every channel message as its MIDI 2.0 channel voice
  message, CLAP_EVENT_MIDI2, scaled by the UMP specification's rule.
  omx-clap-scan lists a plugin's note names and its voice info.
- The host is a fuller CLAP host: timer-support, posix-fd-support (called
  by the tick), thread-pool (every task runs before request_exec returns,
  and a request made from inside exec is refused), tail, render
  (omx_clap_host_set_render), state-context (omx_clap_host_state_save_context
  and _load_context) and port activation. A plugin that answers SLEEP, or
  that is silent for its tail, is not processed until a transport change, a
  note, a sysex, a parameter event or its own request wakes it; a sleep
  never takes a sounding voice. A plugin file's presets are listed through
  its preset-discovery factory (omx_clap_host_presets), and an LV2
  adapter's entry lists the bundle's pset:Preset.
- The LV2 adapter admits more of the specification, each with the test that
  runs it: bufsz:fixedBlockLength where the host declares
  OMX_LV2_BLOCK_FIXED (a smaller JACK block re-activates the plugin, under a
  lock that serialises the lilv world); lv2:inPlaceBroken as a required
  feature; one atom port that carries MIDI and patch; a second and later
  MIDI input or output (a later input is fed empty); a MIDI tool with no
  audio port (passed through); a generator with no input of any kind, as a
  source; state:mapPath and freePath, state:threadSafeRestore,
  options:interface, units, log and stepped scale points, trigger ports
  reset after a block, a worker woken when it has work, the transport at
  every block, and param_info read from the plugin's TTL. The refusal
  hosting.topology.no-audio-input is no longer raised: a plugin with no
  input is admitted as a source, and one with only control ports keeps
  hosting.topology.no-audio-output.
- An LV2 plugin is hosted as mod-host hosts it, checked plugin by plugin
  against mod-host by the parity runner: a plugin that reports its latency
  only after its first run is no longer reset by the restart that follows (the
  LV2 instance stays active unless the rate, the block lengths or a pending
  state load need a full activate; the core's warm-up still resets it);
  control output ports start at their default, else their minimum, as mod-host
  starts them (an equalizer that reads its own output port was 18 dB loud);
  and a default state is restored only for a plugin that has a real
  state:state (an empty state wiped a synth's modulation matrix).
- tests/lv2_parity.py compares every LV2 plugin that mod-host and this
  adapter both host, from the same cycles of one input, and classifies each
  (identical, within tolerance, differs, nondeterministic, silent, crash).
  It is a test, not a part of the packages.
- The calls are in the version node OMX_CLAP_CORE_0.12. The library ABI
  is 11.
* Mon Oct 19 2026 Pau Aliagas <linuxnow@gmail.com> - 0.11.0-1
- An instrument's clap.voice-info is read at activate and answered through
  omx_clap_host_voice_info, in the version node OMX_CLAP_CORE_0.11. The
  library ABI is 8.
- A note input holds a lower and an upper MPE zone together; a master
  channel's expressions reach only the notes of its own zone, and a note that
  starts later begins with its master's last values. omx_clap_host_set_mpe
  sets one zone (members 0 ends it; OFF ends both). The library ABI is 9.
- The LV2 adapter frees the side-chain names of a plugin it refuses to open.
- The reference says what the code does: the message types each note dialect
  passes, and that there is no plugin window. A test compares those lines with
  clap_stage.h.
- The hosting headers are tested value by value against what omx-contract
  reads, and OMX_CLAP_PARAM_SCALES against omx-contract's PARAM_SCALES.
- The oracle tests take their rates from OMX_ORACLE_FULL_RATES_*, so the
  package requires omx-contract 2.16.0 or newer.
- Steadier tests: the transport over jack reads beats and seconds in one
  process(), and the LV2 worker test holds the worker inside work(). The stage
  bench builds and runs in CI.
* Sat Oct 17 2026 Pau Aliagas <linuxnow@gmail.com> - 0.10.0-1
- A CLAP note input takes note ids, an MPE zone and note end; a note port that
  prefers MIDI_MPE gets raw MIDI. The library ABI is 6.
- A plugin's note output reaches effect_<N>:midi_out, through a lock-free ring
  the audio role reads, and omx_clap_midi.h installs the MIDI form of a CLAP
  note. The library ABI is 7.
- omx-clap-host hands every plugin a transport: set_bpm, set_bpb, transport
  and -t, the beat and bar worked out from the frames played, never by adding
  increments.
- A plugin's own parameter events, modulation, gestures, rescan and text reach
  the host, and omx-clap-host answers param_info and reports param_set and
  param_gesture on the feedback socket.
- omx-clap-host takes a state root, so state:makePath is offered to plugins.
- The LV2 adapter hosts an atom control input that takes no MIDI.
- The LV2 worker's ring is the declared 16384 bytes, where the host handed it
  4096.
- The hosting limits and refusal codes are declared in data/hosting.json, and
  the headers clap_host_limits.h and hosted_stage_limits.h and a TypeScript
  module are rendered from it. They install into omx-contract's contract
  directory.
- The package requires omx-contract 2.15.0 or newer, which owns that
  directory.
* Fri Oct 16 2026 Pau Aliagas <linuxnow@gmail.com> - 0.9.0-1
- omx-clap-host runs LV2 plugins: `add lv2:<uri>` finds the bundle on
  LV2_PATH and loads it through the LV2 adapter, and a refusal answers with
  its code.
- The LV2 adapter tells a host the code of its last refusal and the LV2
  symbol of a parameter.
- LV2 CV ports work, each one a mono CLAP audio port, up to 8 each way.
- LV2 atom messages work at every rate; a reported text that fills its slot
  with no terminator is dropped instead of overrunning it. Patch parameter
  ids come in a different order than in earlier versions.
- A plugin's makePath requests stay in a directory of its own under the
  configured state root; a path that would leave it, a symbolic link or a
  request on the audio thread is refused with a code.
- A crash ends omx-clap-host at once, without writing a core dump;
  OMX_CLAP_HOST_CORE_DUMPS=1 keeps the dump for debugging a plugin.
- A burst of notes can no longer swallow the note-off that ends them: the
  last half of each block's note room is kept for note-offs and
  all-notes-off, and omx-clap-host reports what it dropped with
  note_drops <N>.

* Thu Oct 15 2026 Pau Aliagas <linuxnow@gmail.com> - 0.8.0-1
- A new header, omx_clap_raw.h, opens a CLAP plugin the raw way (dlopen,
  clap_entry, factory, a null host) for programs that do not use the core.
- The LV2 adapter takes the feature options a host applies, and installs
  omx_lv2_features.h, the one provider of the features an LV2 plugin asks for.
- The omx-dsp dependency is a floor (0.18.0) with no upper bound.

* Thu Oct 15 2026 Pau Aliagas <linuxnow@gmail.com> - 0.7.0-1
- The host's whole transport (position, bar, time signature, loop) reaches the
  plugins, and the LV2 adapter forges it as time:Position.
- A plugin with auxiliary inputs (side chains) can have them: they are fed
  silence until the host binds a pair, and LV2 port groups become CLAP inputs.
- A program hosting in process reads a plugin's clap.meters/1 through the
  library: omx_clap_host_meter_count, _info and _values.
- LV2 MIDI ports carry MIDI through clap.note-ports.
- The new library functions are in the OMX_CLAP_CORE_0.7 version node.

* Wed Oct 14 2026 Pau Aliagas <linuxnow@gmail.com> - 0.6.0-1
- LV2 numeric patch properties appear as CLAP parameters, and a value the plugin
  emits on its own is kept.
- The headers install clap_meters.h with the other core headers.
- A new package, omx-clap-host-tests, carries the worker's test fixtures for a
  program that tests itself against the worker.

* Wed Oct 14 2026 Pau Aliagas <linuxnow@gmail.com> - 0.5.0-1
- LV2 plugins keep their state: the adapter saves and restores it through
  clap.state and loads a preset through clap.preset-load. A state longer than
  1 MiB is refused on load.
- A plugin activated with blocks larger than the ones it then runs at is
  warmed up at the activated size.
- An LV2 plugin whose worker thread cannot start again after a reset stays
  unusable until it is activated again, instead of running without its worker.

* Mon Oct 12 2026 Pau Aliagas <linuxnow@gmail.com> - 0.4.2-1
- clap_host_limits.h carries CLAP_HOST_LATENCY_HOLD_MS, the hold before a hosted
  stage's new latency takes effect.

* Mon Oct 12 2026 Pau Aliagas <linuxnow@gmail.com> - 0.4.1-1
- A plugin is now prepared for playing at the block size your host runs it at.
  Before, the short warm-up after a plugin was activated used blocks of at
  most 128 frames, so a plugin that counts in frames, such as a decimator, a
  smoother or a sample-and-hold, started playing in a slightly different state
  than it would on a host running its own block size. The warm-up now uses the
  size the host activated for, and those plugins sound the same from the first
  block.

* Thu Oct 08 2026 Pau Aliagas <linuxnow@gmail.com> - 0.4.0-1
- New development package, omx-clap-lv2-devel (libomx-clap-lv2-dev on Debian):
  a library that lets a CLAP host run LV2 plugins. An LV2 plugin with one or
  two audio channels in and out is presented as a CLAP plugin, with its
  controls as parameters, its reported latency, its default settings and its
  background worker.
- A plugin the library cannot run yet (MIDI or other event ports, CV ports,
  other channel layouts, a feature it does not provide) is refused when it is
  created, with a reason the host can log.
- The CLAP host, its scanner, libomx-clap-core and the LV2 library now share
  one version, 0.4.0, so the packages you install together always match. The
  core library itself is unchanged from 0.3.1: same interface, same soname,
  libomx-clap-core.so.0.

* Thu Oct 08 2026 Pau Aliagas <linuxnow@gmail.com> - 0.3.1-1
- A plugin may have at most 2048 parameters. One that reports more is served
  its first 2048, and the host's log says so once, when the plugin is opened;
  the library no longer sizes its tables or its scans from whatever count the
  plugin gives. Its layout cannot be pinned.
- A plugin whose parameter table cannot be allocated is refused when it is
  opened, instead of being opened with the table missing.
- A parameter name the plugin did not terminate is read no further than its
  own buffer.

* Wed Oct 07 2026 Pau Aliagas <linuxnow@gmail.com> - 0.3.0-1
- A plugin whose parameter list has a hole is refused whole, never served
  short; the whole list comes in one call.
- A host can give an instance its tempo: the plugin reads it from the
  transport of every block.
- The list of host extensions has a header of its own,
  `clap_host_extensions.h`.
- The qualifier's fault plugins come with the tests (`make fixtures`).
- A plugin re-engaged after a bypass is reset on the control thread, never in
  the audio path.
- The worker message ring an in-process plugin shares with its worker thread
  ships as `omx_msgring.h`.
- The library's ABI number is 2: a program built against the 0.2 headers keeps
  working, and one built against these is refused by an older library.

* Sat Oct 03 2026 Pau Aliagas <linuxnow@gmail.com> - 0.2.0-1
- The library names the threads that carry audio with a test the host
  supplies, so every worker of a split processing chain counts as an audio
  thread.

* Thu Oct 01 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.1-1
- Plugin meters are reported the way mod-host reports its output meters.
- Before a plugin is activated, the host checks the layout that plugin-hostd
  expects.
- Answers the new plugin information commands: track_info, remote_pages,
  remote_page_get and param_info.
- Built against plugin-hostd 0.1.2.

* Tue Sep 29 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.0-1
- First package: the CLAP host, and the library it is built on with its
  development files.
