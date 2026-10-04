#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# The native installer owns each driver's matching desktop files.
set -euo pipefail
system_root=/
[[ "$system_root" != / || ${EUID} -eq 0 ]] || { echo 'desktop integration requires root' >&2; exit 1; }
base=${system_root%/}
self_dir=$(cd "$(dirname "$0")" && pwd)
templates="$self_dir/desktop/templates"
active="$base/usr/lib/infiltrator-filesystem-support/desktop/active"
rules="$base/etc/udev/rules.d"
original="$base/usr/bin/gnome-disks.filesystem-support-original"
launcher="$base/usr/bin/gnome-disks"
package=infiltrator-filesystem-support
launcher_stage=
transition_rule=
kernel=$(uname -r)
mkdir -p "$base/run/lock"
exec 9>"$base/run/lock/infiltrator-filesystem-support-desktop.lock"
flock -x 9

refresh() {
    if [[ "$system_root" == / ]] && command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules || true
        udevadm trigger --subsystem-match=block --action=change || true
        udevadm settle --timeout=30 || true
    fi
}

has_active() {
    local kind
    for kind in ofs ffs sfs sfs2 pfs3; do
        [[ ! -f "$active/$kind.so" ]] || return 0
    done
    return 1
}

enable_launcher() {
    local owner
    owner=$(dpkg-divert --root="$system_root" --listpackage /usr/bin/gnome-disks)
    [[ -z "$owner" || "$owner" == "$package" ]] || {
        echo "Disks already has a diversion owned by $owner; refusing to replace it" >&2
        return 1
    }
    [[ -f "$templates/gnome-disks" ]] || return 1
    launcher_stage=$(mktemp "$base/usr/bin/.infiltrator-disks-XXXXXX")
    install -m 0755 "$templates/gnome-disks" "$launcher_stage"
    if [[ -z "$owner" ]]; then
        [[ -f "$launcher" ]] || { echo 'GNOME Disks is missing' >&2; return 1; }
        dpkg-divert --root="$system_root" --package "$package" --add --rename \
            --divert /usr/bin/gnome-disks.filesystem-support-original /usr/bin/gnome-disks
    fi
    [[ -f "$original" ]] || return 1
    mv -f "$launcher_stage" "$launcher"
    launcher_stage=
}

disable_launcher_if_empty() {
    if ! has_active && [[ $(dpkg-divert --root="$system_root" --listpackage /usr/bin/gnome-disks) == "$package" ]]; then
        [[ -f "$original" ]] || { echo 'The original Disks executable is missing' >&2; return 1; }
        rm -f "$launcher"
        dpkg-divert --root="$system_root" --package "$package" --remove --rename \
            --divert /usr/bin/gnome-disks.filesystem-support-original /usr/bin/gnome-disks
    fi
}

work=$(mktemp -d)
transaction=
kind=
cleanup() {
    local result=$?
    if [[ $result -ne 0 && -n "$transaction" ]]; then
        if [[ -f "$work/previous.so" ]]; then install -m 0644 "$work/previous.so" "$active/$kind.so"; else rm -f "$active/$kind.so"; fi
        if [[ -f "$work/previous.rules" ]]; then install -m 0644 "$work/previous.rules" "$rules/99-infiltrator-$kind.rules"; else rm -f "$rules/99-infiltrator-$kind.rules"; fi
        rm -f "$rules/59-infiltrator-$kind.rules"
        if has_active; then enable_launcher || true; else disable_launcher_if_empty || true; fi
        refresh
    fi
    [[ -z "$launcher_stage" ]] || rm -f "$launcher_stage"
    if [[ -n "$transition_rule" ]]; then rm -f "$transition_rule"; refresh; fi
    rm -rf "$work"
}
trap cleanup EXIT

begin_transaction() {
    mkdir -p "$active" "$rules"
    rm -f "$work/previous.so" "$work/previous.rules"
    [[ ! -f "$active/$kind.so" ]] || cp -a "$active/$kind.so" "$work/previous.so"
    if [[ -f "$rules/99-infiltrator-$kind.rules" ]]; then
        cp -a "$rules/99-infiltrator-$kind.rules" "$work/previous.rules"
    elif [[ -f "$rules/59-infiltrator-$kind.rules" ]]; then
        cp -a "$rules/59-infiltrator-$kind.rules" "$work/previous.rules"
    fi
    transaction=$1
}

install_kind() {
    [[ -f "$templates/$kind.so" && -f "$templates/$kind.rules" && -f "$templates/gnome-disks" ]] || {
        echo "Desktop templates are missing for $kind" >&2; exit 1;
    }
    begin_transaction install
    install -m 0644 "$templates/$kind.so" "$active/$kind.so"
    # Stock 60-persistent-storage.rules runs blkid. Our identity rule must run
    # afterwards or blkid rewrites DOS/0 and DOS/1 back to the generic 'affs'.
    rm -f "$rules/59-infiltrator-$kind.rules"
    install -m 0644 "$templates/$kind.rules" "$rules/99-infiltrator-$kind.rules"
    enable_launcher
    transaction=
    refresh
}

remove_kind() {
    begin_transaction remove
    # The matching rule clears stale identity after the native module is removed.
    refresh
    rm -f "$active/$kind.so" \
          "$rules/59-infiltrator-$kind.rules" \
          "$rules/99-infiltrator-$kind.rules"
    disable_launcher_if_empty
    transaction=
    refresh
}

[[ $# -ge 1 ]] || exit 2
action=$1
case "$action" in
    install|remove)
        [[ $# -eq 2 ]] || exit 2
        kind=$2
        case "$kind" in ofs|ffs|sfs|sfs2|pfs3) ;; *) exit 2 ;; esac
        if [[ "$action" == install ]]; then install_kind; else remove_kind; fi
        ;;
    sync)
        [[ $# -eq 1 ]] || exit 2
        # Temporarily clear identification left by the retired global rule.
        mkdir -p "$base/run/udev/rules.d"
        transition_rule="$base/run/udev/rules.d/58-infiltrator-transition.rules"
        install -m 0644 "$templates/cleanup.rules" "$transition_rule"
        for kind in ofs ffs sfs sfs2 pfs3; do
            if [[ -f "$base/lib/modules/$kernel/updates/infiltrator/$kind.ko" ]]; then install_kind; else remove_kind; fi
        done
        refresh
        rm -f "$transition_rule"
        transition_rule=
        refresh
        ;;
    purge)
        [[ $# -eq 1 ]] || exit 2
        for kind in ofs ffs sfs sfs2 pfs3; do remove_kind; done
        ;;
    *) exit 2 ;;
esac
