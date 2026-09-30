Name: omx-clap-host
Version: 0.1.0
Release: 1%{?dist}
License: GPL-3.0-or-later
Summary: CLAP plugin host for JACK, controlled over mod-host's socket protocol
URL: https://github.com/FreeMixer/omx-clap-host

Source0: %{url}/archive/v%{version}/%{name}-%{version}.tar.gz

BuildRequires: gcc
BuildRequires: make
BuildRequires: pkgconfig
BuildRequires: pkgconfig(mod-host-protocol)
BuildRequires: pkgconfig(plugin-hostd)
BuildRequires: clap-devel
BuildRequires: pipewire-jack-audio-connection-kit-devel
Requires: omx-clap-core%{?_isa} = %{version}-%{release}

%description
omx-clap-host runs CLAP plugins as JACK clients and is controlled like
mod-host: the same socket, the same line protocol and the same command
replies, from mod-host's protocol library. Each plugin instance is a JACK
client with its own ports, and a MIDI input port for an instrument.

%package -n omx-clap-core
Summary: The CLAP hosting core of omx-clap-host, as a shared library
License: GPL-3.0-or-later

%description -n omx-clap-core
The library omx-clap-host and a program that hosts CLAP plugins in its own
process both run plugins on: the control thread that loads, judges, activates and warms a CLAP
plugin up and owns its parameters and state, and the RT body that runs its
process() on the caller's thread. It names no JACK and no socket.

%package -n omx-clap-core-devel
Summary: Headers, pkg-config file and export list of omx-clap-core
License: GPL-3.0-or-later
Requires: omx-clap-core%{?_isa} = %{version}-%{release}
Requires: clap-devel
Requires: pkgconfig

%description -n omx-clap-core-devel
The headers under include/omx-clap-host, omx-clap-core.pc, the export list
of libomx-clap-core.so.0 and the ABI baseline of the release it was built
from, for a program that hosts CLAP plugins on the library.

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
* Tue Sep 29 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.0-1
- first package: omx-clap-host, and omx-clap-core with omx-clap-core-devel, the library it is built on
