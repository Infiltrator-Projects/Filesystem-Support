#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Disposable CI host: verify actual APT migration from each retired package class.
set -euo pipefail
[[ $# -eq 1 ]]
root=$(cd "$(dirname "$0")/.." && pwd)
deb=$(realpath "$1")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
old_package=infiltrator-filesystem-support-udisks
main_package=infiltrator-filesystem-support
library=$(ldconfig -p | awk '/libudisks2.so.0 / && !found { print $NF; found=1 }')
library=$(realpath "$library")
stock_version=$(dpkg-query -W -f='${Version}' libudisks2-0)
stock_hash=$(sha256sum "$library" | cut -d ' ' -f1)
disks_hash=$(sha256sum /usr/bin/gnome-disks | cut -d ' ' -f1)
cp "$library" "$work/stock-library"
dpkg-deb -x "$deb" "$work/new"

verify_common() {
    test "$(dpkg-query -W -f='${Status}' libudisks2-0)" = 'install ok installed'
    test "$(dpkg-query -W -f='${Status}' "$old_package" 2>/dev/null || true)" != 'install ok installed'
    dpkg-query -S "$library" | grep -Eq '^libudisks2-0(:[^: ]+)?: '
    test "$(sha256sum "$library" | cut -d ' ' -f1)" = "$stock_hash"
    test ! -e /usr/lib/infiltrator-filesystem-support/udisks-amiga-names.so
    test ! -e /usr/lib/udev/rules.d/59-infiltrator-filesystems.rules
    test -z "$(find /etc/udev/rules.d -maxdepth 1 \( -name '59-infiltrator-*.rules' -o -name '99-infiltrator-*.rules' \) -print)"
    sudo apt-get check
}

verify_installed() {
    verify_common

    # Shared launcher infrastructure is package-scoped. It remains installed
    # even when no Amiga filesystem is active, and is inert until a matching
    # per-filesystem plugin is activated.
    test "$(dpkg-divert --listpackage /usr/bin/gnome-disks)" = "$main_package"
    test -f /usr/bin/gnome-disks.filesystem-support-original
    test -x /usr/bin/gnome-disks
    test "$(sha256sum /usr/bin/gnome-disks.filesystem-support-original | cut -d ' ' -f1)" = "$disks_hash"

    python3 "$root/tests/udisks_display_test.py" --stock
    xvfb-run -a /usr/bin/gnome-disks --help
}

verify_stock() {
    verify_common
    test "$(sha256sum /usr/bin/gnome-disks | cut -d ' ' -f1)" = "$disks_hash"
    test -z "$(dpkg-divert --listpackage /usr/bin/gnome-disks)"
    test ! -e /usr/bin/gnome-disks.filesystem-support-original
    python3 "$root/tests/udisks_display_test.py" --stock
    xvfb-run -a /usr/bin/gnome-disks --help
}

install_main() {
    sudo apt-get -s install --no-install-recommends "$deb" > "$work/plan"
    if awk '$1 == "Remv" && $2 != "infiltrator-filesystem-support-udisks" { bad=1 } END { exit !bad }' "$work/plan"; then
        cat "$work/plan"
        exit 1
    fi
    sudo apt-get install -y --no-install-recommends "$deb"
    verify_installed
}

python3 "$root/tests/udisks_display_test.py" --package "$deb"
install_main
sudo bash "$root/tests/desktop_mount_options_test.sh" "$root"
verify_installed
sudo apt-get install -y --no-remove --reinstall gnome-disk-utility
sudo apt-get install -y --no-remove --reinstall "$deb"
verify_installed
sudo apt-get purge -y "$main_package"
verify_stock

# These temporary fixtures reproduce installed package ownership/dependencies.
# No legacy binary is kept in source, published, or retained as a build artifact.
for version in 0.5.4 0.5.5 0.5.6; do
    payload="$work/legacy-$version"
    mkdir -p "$payload/DEBIAN"
    cat > "$payload/DEBIAN/control" <<EOF
Package: $old_package
Version: $version
Architecture: amd64
Maintainer: Shannon Smith
Description: Temporary installed-state migration fixture
EOF
    if [[ "$version" == 0.5.4 ]]; then
        cat >> "$payload/DEBIAN/control" <<EOF
Provides: libudisks2-0 (= $stock_version)
Conflicts: libudisks2-0
Replaces: libudisks2-0
EOF
        mkdir -p "$payload$(dirname "$library")"
        cp "$work/stock-library" "$payload$library"
        printf '\0' >> "$payload$library"
        if [[ $(basename "$library") != libudisks2.so.0 ]]; then
            ln -s "$(basename "$library")" "$payload$(dirname "$library")/libudisks2.so.0"
        fi
    else
        printf 'Depends: libudisks2-0, gnome-disk-utility\n' >> "$payload/DEBIAN/control"
        mkdir -p "$payload/usr/bin" "$payload/usr/lib/infiltrator-filesystem-support"
        cp "$work/new/usr/lib/infiltrator-filesystem-support/desktop/templates/ofs.so" \
            "$payload/usr/lib/infiltrator-filesystem-support/udisks-amiga-names.so"
        cat > "$payload/usr/bin/gnome-disks" <<'SCRIPT'
#!/bin/sh
export LD_PRELOAD=/usr/lib/infiltrator-filesystem-support/udisks-amiga-names.so
exec /usr/bin/gnome-disks.filesystem-support-original "$@"
SCRIPT
        cat > "$payload/DEBIAN/preinst" <<'SCRIPT'
#!/bin/sh
set -e
dpkg-divert --package infiltrator-filesystem-support-udisks --add --rename \
    --divert /usr/bin/gnome-disks.filesystem-support-original /usr/bin/gnome-disks
SCRIPT
        cat > "$payload/DEBIAN/postrm" <<'SCRIPT'
#!/bin/sh
set -e
case "$1" in remove|purge|abort-install)
    if [ "$(dpkg-divert --listpackage /usr/bin/gnome-disks)" = infiltrator-filesystem-support-udisks ]; then
        dpkg-divert --package infiltrator-filesystem-support-udisks --remove --rename \
            --divert /usr/bin/gnome-disks.filesystem-support-original /usr/bin/gnome-disks
    fi
    ;;
esac
SCRIPT
        chmod 755 "$payload/usr/bin/gnome-disks" "$payload/DEBIAN/preinst" "$payload/DEBIAN/postrm"
    fi
    dpkg-deb --root-owner-group --build "$payload" "$work/legacy-desktop-$version.deb"
    main="$work/legacy-main-$version"
    mkdir -p "$main/DEBIAN"
    cat > "$main/DEBIAN/control" <<EOF
Package: $main_package
Version: $version
Architecture: amd64
Maintainer: Shannon Smith
Depends: $old_package (>= $version)
Description: Temporary main-package dependency migration fixture
EOF
    dpkg-deb --root-owner-group --build "$main" "$work/legacy-main-$version.deb"
    sudo apt-get install -y --no-install-recommends "$work/legacy-desktop-$version.deb" "$work/legacy-main-$version.deb"
    test "$(dpkg-query -W -f='${Version}' "$old_package")" = "$version"
    sudo install -m 0644 "$work/legacy-desktop-$version.deb" \
        "/var/cache/apt/archives/${old_package}_${version}_amd64.deb"
    install_main
    test ! -e "/var/cache/apt/archives/${old_package}_${version}_amd64.deb"
    sudo apt-get purge -y "$main_package" "$old_package"
    verify_stock
    echo "Legacy $version dependency/ownership migration: PASS"
done
echo 'Single-package stock install, package-scoped desktop launchers, legacy retirement, library preservation and purge: PASS'
