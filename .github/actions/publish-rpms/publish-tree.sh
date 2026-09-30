#!/bin/bash
# Add RPMs to a dnf tree laid out as <tree>/fedora/<releasever>/<arch>/ (sources in SRPMS), sign what is
# unsigned, regenerate the metadata of every directory that holds packages and sign it.
#
#   publish-tree.sh <tree> <rpm dir> [<gpg key id>]     no key id: an unsigned tree, for inspection only
#
# A package the tree already holds under the same file name is kept whatever the new build's bytes: a
# published NEVRA is immutable and a rebuild that wants in bumps its Release. The tree is shared with
# other projects, so nothing already in it is ever removed.
set -euo pipefail

tree=$(readlink -m "${1:?usage: publish-tree.sh <tree> <rpm dir> [<gpg key id>]}")
src=${2:?usage: publish-tree.sh <tree> <rpm dir> [<gpg key id>]}
key=${3:-}

signed() {
    local sigs
    sigs=$(rpm -qp --qf '%{SIGPGP:pgpsig}|%{SIGGPG:pgpsig}|%{RSAHEADER:pgpsig}|%{DSAHEADER:pgpsig}' "$1" 2>/dev/null) || return 1
    sigs=$(printf '%s' "$sigs" | sed -e 's/(none)//g' -e 's/|//g' -e 's/[[:space:]]//g')
    [ -n "$sigs" ]
}

mapfile -t found < <(find "$src" -name '*.rpm' | sort)
[ "${#found[@]}" -gt 0 ] || { echo "no rpm under $src" >&2; exit 1; }

declare -A dirs=()
for f in "${found[@]}"; do
    IFS='|' read -r is_src arch rel < <(rpm -qp --qf '%{SOURCEPACKAGE}|%{ARCH}|%{RELEASE}\n' "$f")
    [ "$is_src" = 1 ] && archdir=SRPMS || archdir=$arch
    releasever=$(printf '%s\n' "$rel" | sed -n 's/.*\.fc\([0-9][0-9]*\).*/\1/p')
    [ -n "$releasever" ] || { echo "$f has no .fcNN dist tag" >&2; exit 1; }
    dest=$tree/fedora/$releasever/$archdir
    mkdir -p "$dest"
    dirs[$dest]=1
    if [ -f "$dest/$(basename "$f")" ]; then
        echo "kept   $(basename "$f") (already published; a changed build needs a new Release)"
    else
        install -m 0644 "$f" "$dest/"
        echo "placed $(basename "$f") in fedora/$releasever/$archdir"
    fi
done

if [ -n "$key" ]; then
    for dest in "${!dirs[@]}"; do
        for f in "$dest"/*.rpm; do
            signed "$f" || rpmsign --define "_gpg_name $key" --addsign "$f" >/dev/null
            signed "$f" || { echo "unsigned after rpmsign: $f" >&2; exit 1; }
        done
    done
fi

# every directory that holds packages, not only the ones this run placed into: the metadata of the
# pulled tree was discarded, and a directory another project filled would be left bare
while IFS= read -r d; do dirs[$d]=1; done < <(find "$tree" -name '*.rpm' -printf '%h\n' | sort -u)
for dest in "${!dirs[@]}"; do
    if [ -d "$dest/repodata" ]; then createrepo_c --update --quiet "$dest"; else createrepo_c --quiet "$dest"; fi
    rm -f "$dest/repodata/repomd.xml.asc"
    if [ -n "$key" ]; then
        gpg --batch --yes --armor --detach-sign --local-user "$key" \
            --output "$dest/repodata/repomd.xml.asc" "$dest/repodata/repomd.xml"
    fi
    echo "repodata ${dest#"$tree"/}"
done

if [ -n "$key" ]; then
    gpg --armor --export "$key" > "$tree/RPM-GPG-KEY-freemixer"
    [ -s "$tree/RPM-GPG-KEY-freemixer" ] || { echo "exporting $key gave an empty key file" >&2; exit 1; }
fi
