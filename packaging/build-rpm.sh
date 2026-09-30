#!/bin/bash
# Build the RPMs of this tree from a tarball of it.
#
#   packaging/build-rpm.sh [-t <release tag>] <output dir>
#
# The version is the spec's. With -t the tag must be v<version> and the tree must be that tag's commit;
# without it the build is a snapshot: the release carries .git<short sha> and the changelog says so.
# BuildRequires must be installed: dnf builddep packaging/omx-clap-host.spec, with the protocol library.
set -euo pipefail

tag=
if [ "${1:-}" = -t ]; then tag=${2:?-t needs a tag}; shift 2; fi
out=$(readlink -m "${1:?usage: build-rpm.sh [-t <release tag>] <output dir>}")

here=$(cd "$(dirname "$0")/.." && pwd)
name=omx-clap-host
version=$(sed -n 's/^Version: *//p' "$here/packaging/$name.spec")
[ -n "$version" ] || { echo "no Version in the spec" >&2; exit 1; }

top=$(mktemp -d)
mkdir -p "$top/SOURCES" "$top/SPECS" "$out"
spec=$top/SPECS/$name.spec
cp "$here/packaging/$name.spec" "$spec"
short=$(git -C "$here" rev-parse --short HEAD)

if [ -n "$tag" ]; then
    [ "$tag" = "v$version" ] || { echo "tag $tag is not v$version, the spec's version" >&2; exit 1; }
    [ "$(git -C "$here" rev-parse "$tag^{commit}")" = "$(git -C "$here" rev-parse HEAD)" ] ||
        { echo "the tree is not at $tag" >&2; exit 1; }
else
    sed -i "s/^Release: *\([0-9][0-9]*\)%{?dist}/Release: \1.git$short%{?dist}/" "$spec"
    release=$(sed -n 's/^Release: *\(.*\)%{?dist}$/\1/p' "$spec")
    printf '* %s Pau Aliagas <linuxnow@gmail.com> - %s-%s\n- build of commit %s\n\n' \
        "$(LC_ALL=C date +'%a %b %d %Y')" "$version" "$release" "$short" > "$top/changelog"
    sed -i "/^%changelog/r $top/changelog" "$spec"
fi

git -C "$here" archive --prefix="$name-$version/" HEAD | gzip > "$top/SOURCES/$name-$version.tar.gz"
rpmbuild --define "_topdir $top" -ba "$spec"
find "$top/RPMS" "$top/SRPMS" -name '*.rpm' -exec cp {} "$out/" \;
ls "$out"/*.rpm
