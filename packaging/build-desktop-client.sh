#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

[[ $# -eq 1 ]] || { echo "usage: $0 <output-directory>" >&2; exit 2; }
root=$(cd "$(dirname "$0")/.." && pwd)
version=$(tr -d '[:space:]' < "$root/VERSION")
output=$(realpath -m "$1")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
mkdir -p "$output"
cd "$work"

# Use the configured distribution's source and its Debian build rules. Do not
# copy a client library built for a different UDisks version or architecture.
apt-get source udisks2
mapfile -t sources < <(find "$work" -mindepth 1 -maxdepth 1 -type d -name 'udisks2-*')
[[ ${#sources[@]} -eq 1 ]] || { echo "Expected one UDisks source tree" >&2; exit 1; }
source=${sources[0]}
patch -d "$source" -p1 --forward --fuzz=0 < "$root/packaging/linux/udisks2-amiga-display.patch"
(cd "$source"; DEB_BUILD_OPTIONS="${DEB_BUILD_OPTIONS:-nocheck}" dpkg-buildpackage -b -uc -us)

arch=$(dpkg --print-architecture)
mapfile -t libraries < <(find "$work" -maxdepth 1 -type f -name "libudisks2-0_*_${arch}.deb")
[[ ${#libraries[@]} -eq 1 ]] || { echo "Expected one UDisks client package" >&2; exit 1; }
stock=${libraries[0]}
base_version=$(dpkg-deb -f "$stock" Version)
package=infiltrator-filesystem-support-udisks
payload="$work/payload"
dpkg-deb -R "$stock" "$payload"

python3 - "$payload/DEBIAN/control" "$package" "$version" "$base_version" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
package, version, base_version = sys.argv[2:]
fields = {}
current = None
for line in path.read_text().splitlines():
    if line[:1].isspace() and current:
        fields[current] += "\n" + line
    elif ":" in line:
        current, value = line.split(":", 1)
        fields[current] = value.lstrip()
fields["Package"] = package
fields["Version"] = version
fields["Source"] = f"udisks2 ({base_version})"
fields["Maintainer"] = "Shannon Smith"
fields["Homepage"] = "https://github.com/Infiltrator-Projects/Filesystem-Support"
for key, value in (("Provides", f"libudisks2-0 (= {base_version})"),
                   ("Conflicts", "libudisks2-0"), ("Replaces", "libudisks2-0")):
    fields[key] = ", ".join(part for part in (fields.get(key, ""), value) if part)
fields["Description"] = (
    "UDisks client with native Amiga filesystem display names\n"
    " Distribution-built UDisks client library with OFS, FFS, SFS, SFS2 and\n"
    " PFS3 names for GNOME Disks and other UDisks clients. The library ABI,\n"
    " detection and mount behaviour remain those of the distribution."
)
path.write_text("\n".join(f"{key}: {value}" for key, value in fields.items()) + "\n")
PY

# Debian's dependency metadata must point at the package which owns the library.
for metadata in shlibs symbols; do
    if [[ -f "$payload/DEBIAN/$metadata" ]]; then
        sed -i "s/libudisks2-0/$package/g" "$payload/DEBIAN/$metadata"
    fi
done
if [[ -f "$payload/DEBIAN/shlibs" ]]; then
    sed -i "s/(>= [^)]*)/(>= $version)/g" "$payload/DEBIAN/shlibs"
fi
if [[ -f "$payload/DEBIAN/symbols" ]]; then
    python3 - "$payload/DEBIAN/symbols" "$version" <<'PY'
from pathlib import Path
import sys
path = Path(sys.argv[1])
lines = []
for line in path.read_text().splitlines():
    # The new package first provides every stock export at its own initial
    # version, not at the upstream library's numerically unrelated version.
    if line.startswith(" "):
        fields = line.split()
        if len(fields) >= 2:
            fields[1] = sys.argv[2]
            line = " " + " ".join(fields)
    lines.append(line)
path.write_text("\n".join(lines) + "\n")
PY
fi

deb="$output/${package}_${version}_${arch}.deb"
dpkg-deb --root-owner-group --build "$payload" "$deb"
python3 "$root/tests/udisks_display_test.py" --package "$deb"
printf 'Built %s against distribution libudisks2-0 %s\n' "$deb" "$base_version"
