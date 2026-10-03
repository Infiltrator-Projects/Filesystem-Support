#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Exercise the real APT/dpkg transaction, including the released 0.5.4 mistake.
set -euo pipefail
[[ $# -eq 1 ]]
root=$(cd "$(dirname "$0")/.." && pwd)
deb=$(realpath "$1")
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
package=infiltrator-filesystem-support-udisks
library=$(ldconfig -p | awk '/libudisks2.so.0 / && !found { print $NF; found=1 }')
library=$(realpath "$library")
test -n "$library"
stock_hash=$(sha256sum "$library" | cut -d ' ' -f1)
disks_hash=$(sha256sum /usr/bin/gnome-disks | cut -d ' ' -f1)

verify_stock() {
    test "$(dpkg-query -W -f='${Status}' libudisks2-0)" = 'install ok installed'
    dpkg-query -S "$library" | grep -Eq '^libudisks2-0(:[^: ]+)?: '
    test "$(sha256sum "$library" | cut -d ' ' -f1)" = "$stock_hash"
    python3 "$root/tests/udisks_display_test.py" --stock
    sudo apt-get check
}

install_without_removals() {
    sudo apt-get -s install --no-install-recommends "$deb" > "$work/plan"
    if grep -q '^Remv ' "$work/plan"; then cat "$work/plan"; exit 1; fi
    sudo apt-get install -y --no-remove --no-install-recommends "$deb"
    verify_stock
    test "$(sha256sum /usr/bin/gnome-disks.filesystem-support-original | cut -d ' ' -f1)" = "$disks_hash"
    LD_PRELOAD=/usr/lib/infiltrator-filesystem-support/udisks-amiga-names.so \
        python3 "$root/tests/udisks_display_test.py" --preloaded
    xvfb-run -a /usr/bin/gnome-disks --version
}

remove_and_verify() {
    sudo apt-get remove -y "$package"
    verify_stock
    test "$(sha256sum /usr/bin/gnome-disks | cut -d ' ' -f1)" = "$disks_hash"
    test -z "$(dpkg-divert --listpackage /usr/bin/gnome-disks)"
    test ! -e /usr/bin/gnome-disks.filesystem-support-original
    xvfb-run -a /usr/bin/gnome-disks --version
}

# Stock system / users who cancelled the 0.5.4 proposal.
install_without_removals
# Reinstall and upgrade the stock application while the diversion is active.
sudo apt-get install -y --no-remove --reinstall gnome-disk-utility
test "$(sha256sum /usr/bin/gnome-disks.filesystem-support-original | cut -d ' ' -f1)" = "$disks_hash"
sudo apt-get install -y --no-remove --reinstall "$deb"
remove_and_verify

# A user who already installed 0.5.4 must get the official client back without
# removing desktop consumers or asking them to repair packages manually.
gh release download v0.5.4 --repo Infiltrator-Projects/Filesystem-Support \
    --pattern 'infiltrator-filesystem-support-udisks_0.5.4_amd64.deb' --dir "$work"
old="$work/infiltrator-filesystem-support-udisks_0.5.4_amd64.deb"
test "$(sha256sum "$old" | cut -d ' ' -f1)" = \
    403ef94af6060385d4f540b07b50fbaf12bfeda094b3647ab03f75892f3d4e54
sudo apt-get install -y --no-install-recommends "$old"
test "$(dpkg-query -W -f='${Version}' "$package")" = 0.5.4
install_without_removals
remove_and_verify
sudo apt-get purge -y "$package"
verify_stock
echo 'APT stock install, 0.5.4 recovery, Disks reinstall and removal: PASS'
