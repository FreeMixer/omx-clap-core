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
BuildRequires: pkgconfig(mod-host-plumbing)
BuildRequires: clap-devel
BuildRequires: pipewire-jack-audio-connection-kit-devel

%description
omx-clap-host runs CLAP plugins as JACK clients and is controlled like
mod-host: the same socket, the same line protocol and the same command
replies, from mod-host's plumbing library. Each plugin instance is a JACK
client with its own ports.

%prep
%autosetup

sed -i 's,LDFLAGS += -s,LDFLAGS +=,g' Makefile

%build

%set_build_flags

%make_build

%install

%make_install PREFIX=%{_prefix}

%check

make test-fake

%files
%license COPYING
%doc README.md
%{_bindir}/omx-clap-host

%changelog
* Tue Sep 29 2026 Pau Aliagas <linuxnow@gmail.com> - 0.1.0-1
- first package
