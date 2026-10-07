Name: omx-clap-host
Version:        0.2.0
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

%changelog
* Sat Oct 03 2026 Pau Aliagas <linuxnow@gmail.com> - 0.2.0-1
- omx-clap-core 0.2.0: name the audio role's threads with a predicate at publish, so every worker of a split walk is the audio thread

* Thu Oct 01 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.1-1
- report plugin meters as mod-host output symbols
- check the layout pin plugin-hostd expects before activating an instance
- answer track_info, remote_pages, remote_page_get and param_info
- build against plugin-hostd 0.1.2

* Tue Sep 29 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.0-1
- first package: omx-clap-host, and omx-clap-core with omx-clap-core-devel, the library it is built on
