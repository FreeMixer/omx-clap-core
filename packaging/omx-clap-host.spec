Name: omx-clap-host
Version:        0.11.0
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
# make install puts data/ into omx-contract's contract directory with omx-contract's install script
BuildRequires: omx-contract-devel >= 2.15.0
BuildRequires: lv2-devel
BuildRequires: lilv-devel
# make test-lv2 runs the MIDI output path under AddressSanitizer
BuildRequires: libasan
BuildRequires: pipewire-jack-audio-connection-kit-devel
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
# the hosting limits and refusal codes, in omx-contract's contract directory, which that package owns
Requires: omx-contract-devel >= 2.15.0
Requires: pkgconfig

%description -n omx-clap-core-devel
What you need to write a program that hosts CLAP plugins on omx-clap-core: the
headers, the pkg-config file, the list of functions the library promises to
keep, with the baseline each release is checked against, and the hosting
limits and refusal codes as definitions in omx-contract's contract directory.

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

%make_install PREFIX=%{_prefix} LIBDIR=%{_libdir}

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

%files tests
%license COPYING
%{_libexecdir}/omx-clap-host/tests/

%files -n omx-clap-core
%license COPYING
%{_libdir}/libomx-clap-core.so.0
%{_libdir}/libomx-clap-core.so.0.*

%files -n omx-clap-core-devel
%{_libdir}/libomx-clap-core.so
%{_libdir}/pkgconfig/omx-clap-core.pc
%{_includedir}/omx-clap-host/
%{_datadir}/omx-clap-core/
%{_datadir}/omx-contract/contract.d/2/omx-clap-core/

%files -n omx-clap-lv2-devel
%license COPYING
%{_libdir}/libomx-clap-lv2.a
%{_libdir}/pkgconfig/omx-clap-lv2.pc
%{_includedir}/omx-clap-lv2/

%changelog
* Sun Oct 18 2026 Pau Aliagas <linuxnow@gmail.com> - 0.11.0-1
- The library reads an instrument's clap.voice-info at activate and answers it
  through omx_clap_host_voice_info, in the version node OMX_CLAP_CORE_0.11.
  The library ABI is 8.
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
