#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Disposable CI host: verify the current Filesystems package install/reinstall/purge lifecycle.
set -euo pipefail
[[ $# -eq 1 ]]
root=$(cd "$(dirname "$0")/.." && pwd)
deb=$(realpath "$1")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
main_package=filesystems
library=$(ldconfig -p | awk '/libudisks2.so.0 / && !found { print $NF; found=1 }')
library=$(realpath "$library")
stock_hash=$(sha256sum "$library" | cut -d ' ' -f1)
disks_hash=$(sha256sum /usr/bin/gnome-disks | cut -d ' ' -f1)
dpkg-deb -x "$deb" "$work/new"

verify_common() {
    test "$(dpkg-query -W -f='${Status}' libudisks2-0)" = 'install ok installed'
    dpkg-query -S "$library" | grep -Eq '^libudisks2-0(:[^: ]+)?: '
    test "$(sha256sum "$library" | cut -d ' ' -f1)" = "$stock_hash"
    test ! -e /usr/lib/filesystems/udisks-amiga-names.so
    test ! -e /usr/lib/udev/rules.d/59-infiltrator-filesystems.rules
    test -z "$(find /etc/udev/rules.d -maxdepth 1 \( -name '59-infiltrator-*.rules' -o -name '99-infiltrator-*.rules' \) -print)"
    sudo apt-get check
}

verify_installed() {
    verify_common

    # Native orchestration is deliberately split into a thin coordinator and
    # a kernel-only transaction helper. Both must exist in the real package.
    test -x /usr/lib/filesystems/native-module-helper
    test -x /usr/lib/filesystems/kernel-module-helper

    # Shared launcher infrastructure is package-scoped. It remains installed
    # even when no Amiga filesystem is active, and is inert until a matching
    # per-filesystem plugin is activated.
    test "$(dpkg-divert --listpackage /usr/bin/gnome-disks)" = "$main_package"
    test -f /usr/bin/gnome-disks.filesystems-original
    test -x /usr/bin/gnome-disks
    test "$(sha256sum /usr/bin/gnome-disks.filesystems-original | cut -d ' ' -f1)" = "$disks_hash"

    python3 "$root/tests/udisks_display_test.py" --stock
    xvfb-run -a /usr/bin/gnome-disks --help
}

verify_stock() {
    verify_common
    test "$(sha256sum /usr/bin/gnome-disks | cut -d ' ' -f1)" = "$disks_hash"
    test -z "$(dpkg-divert --listpackage /usr/bin/gnome-disks)"
    test ! -e /usr/bin/gnome-disks.filesystems-original
    python3 "$root/tests/udisks_display_test.py" --stock
    xvfb-run -a /usr/bin/gnome-disks --help
}

install_main() {
    sudo apt-get -s install --no-install-recommends "$deb" > "$work/plan"
    if awk '$1 == "Remv" { bad=1 } END { exit !bad }' "$work/plan"; then
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

echo 'Filesystems package install, reinstall, stock library preservation and purge: PASS'
