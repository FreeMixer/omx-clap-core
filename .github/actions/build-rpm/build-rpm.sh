#!/bin/bash
# Build the RPMs of a tree from a tarball of it.
#
#   build-rpm.sh [-C <tree>] [-t <release tag>] <output dir>
#
# The spec is the tree's only packaging/*.spec and the version is its own. With -t the tag must be
# v<version> and the tree must be that tag's commit; without it the build is a snapshot: the release
# carries .git<short sha> and the changelog says so. BuildRequires must be installed.
set -euo pipefail

tree=.
tag=
while getopts C:t: opt; do
    case $opt in
        C) tree=$OPTARG ;;
        t) tag=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
out=$(readlink -m "${1:?usage: build-rpm.sh [-C <tree>] [-t <release tag>] <output dir>}")
tree=$(readlink -f "$tree")

specs=("$tree"/packaging/*.spec)
[ "${#specs[@]}" -eq 1 ] && [ -f "${specs[0]}" ] || { echo "$tree/packaging holds no single spec" >&2; exit 1; }
name=$(basename "${specs[0]}" .spec)
version=$(sed -n 's/^Version: *//p' "${specs[0]}")
[ -n "$version" ] || { echo "no Version in the spec" >&2; exit 1; }

top=$(mktemp -d)
mkdir -p "$top/SOURCES" "$top/SPECS" "$out"
spec=$top/SPECS/$name.spec
cp "${specs[0]}" "$spec"
short=$(git -C "$tree" rev-parse --short HEAD)

if [ -n "$tag" ]; then
    [ "$tag" = "v$version" ] || { echo "tag $tag is not v$version, the spec's version" >&2; exit 1; }
    [ "$(git -C "$tree" rev-parse "$tag^{commit}")" = "$(git -C "$tree" rev-parse HEAD)" ] ||
        { echo "the tree is not at $tag" >&2; exit 1; }
else
    sed -i "s/^Release: *\([0-9][0-9]*\)%{?dist}/Release: \1.git$short%{?dist}/" "$spec"
    release=$(sed -n 's/^Release: *\(.*\)%{?dist}$/\1/p' "$spec")
    printf '* %s Pau Aliagas <linuxnow@gmail.com> - %s-%s\n- build of commit %s\n\n' \
        "$(LC_ALL=C date +'%a %b %d %Y')" "$version" "$release" "$short" > "$top/changelog"
    sed -i "/^%changelog/r $top/changelog" "$spec"
fi

git -C "$tree" archive --prefix="$name-$version/" HEAD | gzip > "$top/SOURCES/$name-$version.tar.gz"
rpmbuild --define "_topdir $top" -ba "$spec"
find "$top/RPMS" "$top/SRPMS" -name '*.rpm' -exec cp {} "$out/" \;
ls "$out"/*.rpm
