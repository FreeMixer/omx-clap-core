Name: omx-clap-host
Version:        0.4.0
Release: 1%{?dist}
License: GPL-3.0-or-later
Summary: Use CLAP plugins on a JACK or PipeWire rig, driven like mod-host
URL: https://github.com/FreeMixer/omx-clap-host

Source0: %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires: gcc
BuildRequires: make
BuildRequires: pkgconfig
BuildRequires: pkgconfig(mod-host-protocol)
BuildRequires: pkgconfig(plugin-hostd) >= 0.1.2
BuildRequires: clap-devel
BuildRequires: lv2-devel
BuildRequires: lilv-devel
BuildRequires: pipewire-jack-audio-connection-kit-devel
Requires: omx-clap-core%{?_isa} = %{version}-%{release}

%description
omx-clap-host brings CLAP plugins, the modern open plugin format, into a live
JACK or PipeWire rig. Each plugin runs as a JACK client with its own ports, and
an instrument gets a MIDI input as well. Anything that already controls
mod-host can control it over the same socket with the same commands. It comes
with omx-clap-scan, which lists what is in a CLAP bundle and survives a plugin
that crashes while loading.

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
Requires: pkgconfig

%description -n omx-clap-core-devel
What you need to write a program that hosts CLAP plugins on omx-clap-core: the
headers, the pkg-config file, and the list of functions the library promises to
keep, with the baseline each release is checked against.

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

%files -n omx-clap-core
%license COPYING
%{_libdir}/libomx-clap-core.so.0
%{_libdir}/libomx-clap-core.so.0.*

%files -n omx-clap-core-devel
%{_libdir}/libomx-clap-core.so
%{_libdir}/pkgconfig/omx-clap-core.pc
%{_includedir}/omx-clap-host/
%{_datadir}/omx-clap-core/

%files -n omx-clap-lv2-devel
%license COPYING
%{_libdir}/libomx-clap-lv2.a
%{_libdir}/pkgconfig/omx-clap-lv2.pc
%{_includedir}/omx-clap-lv2/

%changelog
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
