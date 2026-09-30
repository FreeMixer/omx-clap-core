#!/bin/bash
# Build the mod-host RPMs, with mod-host-protocol-devel, from the audinux spec of linuxnow/fedora-spec into $1.
# Runs as root in a Fedora container. SPEC_REF is a commit of linuxnow/fedora-spec.
set -euo pipefail

out=$(readlink -f "${1:?usage: build.sh <output dir>}")
ref=${SPEC_REF:?SPEC_REF is not set}
top=$(mktemp -d)
mkdir -p "$top/SOURCES" "$top/SPECS" "$out"

base=https://raw.githubusercontent.com/linuxnow/fedora-spec/$ref/moddevices
for f in mod-host.spec mod-host.service $(curl -fsSL "$base/mod-host.spec" | sed -n 's/^Patch[0-9]*: *//p'); do
    curl -fsSL "$base/$f" -o "$top/SOURCES/$f"
done
mv "$top/SOURCES/mod-host.spec" "$top/SPECS/"

# pipewire's jack first: the builddep would pull jack-audio-connection-kit-devel, which conflicts with it
dnf install -y --setopt=install_weak_deps=False rpm-build rpmdevtools dnf5-plugins gcc make patch curl pipewire-jack-audio-connection-kit-devel
spectool -g -C "$top/SOURCES" "$top/SPECS/mod-host.spec"
dnf builddep -y --setopt=install_weak_deps=False "$top/SPECS/mod-host.spec"
rpmbuild --define "_topdir $top" -bb "$top/SPECS/mod-host.spec"

find "$top/RPMS" -name '*.rpm' -exec cp {} "$out/" \;
ls "$out"/mod-host-[0-9]*.rpm "$out"/mod-host-protocol-devel-*.rpm
