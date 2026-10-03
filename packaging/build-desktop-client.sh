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
arch=$(dpkg --print-architecture)
package=infiltrator-filesystem-support-udisks
payload="$work/payload"
private="$payload/usr/lib/infiltrator-filesystem-support"
mkdir -p "$private" "$payload/usr/bin" "$payload/DEBIAN"

# A tiny private display extension replaces no distribution library. GNOME
# Disks alone loads it, and unknown/non-Amiga IDs delegate to the stock client.
cc -shared -fPIC -fvisibility=hidden -O2 -Wall -Wextra -Werror \
    $(pkg-config --cflags glib-2.0) \
    "$root/packaging/linux/udisks-amiga-names.c" \
    -o "$private/udisks-amiga-names.so" \
    $(pkg-config --libs glib-2.0) -ldl -pthread
strip --strip-unneeded "$private/udisks-amiga-names.so"
install -m 0755 "$root/packaging/linux/gnome-disks-wrapper" "$payload/usr/bin/gnome-disks"
install -m 0755 "$root/packaging/linux/desktop-preinst" "$payload/DEBIAN/preinst"
install -m 0755 "$root/packaging/linux/desktop-postrm" "$payload/DEBIAN/postrm"
mkdir -p "$work/debian"
printf 'Source: infiltrator-filesystem-support\n' > "$work/debian/control"
dependencies=$(cd "$work"; dpkg-shlibdeps -O -e"$private/udisks-amiga-names.so")
dependencies=${dependencies#shlibs:Depends=}
cat > "$payload/DEBIAN/control" <<EOF
Package: $package
Version: $version
Architecture: $arch
Maintainer: Shannon Smith
Section: utils
Priority: optional
Depends: $dependencies, libudisks2-0, gnome-disk-utility
Homepage: https://github.com/Infiltrator-Projects/Filesystem-Support
Description: Private Amiga filesystem names for GNOME Disks
 Adds OFS, FFS, SFS, SFS2 and PFS3 display names only inside GNOME Disks.
 Keeps the distribution libudisks2-0 package and library files untouched.
 The original Disks executable remains managed by gnome-disk-utility;
 removing this package restores its normal launch path.
EOF
mkdir -p "$payload/usr/share/doc/$package"
cat > "$payload/usr/share/doc/$package/copyright" <<'COPYRIGHT'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: Filesystem Support
Source: https://github.com/Infiltrator-Projects/Filesystem-Support

Files: *
Copyright: 2026 Shannon Smith
License: GPL-3+
 On Debian systems the complete GPL version 3 text is available in
 /usr/share/common-licenses/GPL-3.
COPYRIGHT
deb="$output/${package}_${version}_${arch}.deb"
dpkg-deb --root-owner-group --build "$payload" "$deb"
python3 "$root/tests/udisks_display_test.py" --package "$deb"

# Compile a separate fixture library with a private module root. This is never
# packaged, and proves every combination of per-filesystem install/removal.
cc -shared -fPIC -fvisibility=hidden -O2 -Wall -Wextra -Werror \
    -DIFS_NATIVE_MODULES_ROOT="\"$work/modules\"" \
    $(pkg-config --cflags glib-2.0) \
    "$root/packaging/linux/udisks-amiga-names.c" \
    -o "$work/udisks-amiga-names-test.so" \
    $(pkg-config --libs glib-2.0) -ldl -pthread
python3 "$root/tests/udisks_display_test.py" \
    --shim "$work/udisks-amiga-names-test.so" --modules-root "$work/modules" --matrix
printf 'Built isolated GNOME Disks names package: %s\n' "$deb"
