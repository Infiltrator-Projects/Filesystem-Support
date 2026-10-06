#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Per-filesystem desktop identity activation.
#
# Shared GNOME Disks/Nemo launcher diversions belong to the package lifecycle
# (sync/unprepare), not to an individual filesystem install/remove transaction.
# A filesystem operation therefore changes only its own active plugin and udev
# rule and cannot tear down another filesystem's shared desktop infrastructure.
set -euo pipefail
system_root=/
[[ "$system_root" != / || ${EUID} -eq 0 ]] || { echo 'desktop integration requires root' >&2; exit 1; }
base=${system_root%/}
self_dir=$(cd "$(dirname "$0")" && pwd)
templates="$self_dir/desktop/templates"
active="$base/usr/lib/infiltrator-filesystem-support/desktop/active"
rules="$base/etc/udev/rules.d"
manifest="$base/usr/lib/infiltrator-filesystem-support/native-filesystems.tsv"
disks_original="$base/usr/bin/gnome-disks.filesystem-support-original"
disks_launcher="$base/usr/bin/gnome-disks"
nemo_original="$base/usr/bin/nemo.filesystem-support-original"
nemo_launcher="$base/usr/bin/nemo"
package=infiltrator-filesystem-support
kernel=$(uname -r)
[[ -r "$manifest" ]] || { echo 'native filesystem deployment manifest is missing' >&2; exit 1; }
mkdir -p "$base/run/lock"
exec 9>"$base/run/lock/infiltrator-filesystem-support-desktop.lock"
flock -x 9

desktop_filesystems() {
    awk -F '\t' '$0 !~ /^#/ && NF == 4 && $3 == "amiga" { print $1 }' "$manifest"
}

is_desktop_filesystem() {
    local requested=$1
    awk -F '\t' -v fs="$requested" '
        $0 !~ /^#/ && NF == 4 && $1 == fs && $3 == "amiga" { found = 1; exit }
        END { exit found ? 0 : 1 }
    ' "$manifest"
}

refresh() {
    if [[ "$system_root" == / ]] && command -v udevadm >/dev/null 2>&1; then
        udevadm control --reload-rules || true
        udevadm trigger --subsystem-match=block --action=change || true
        udevadm settle --timeout=30 || true
    fi
}

has_active() {
    local kind
    while IFS= read -r kind; do
        [[ ! -f "$active/$kind.so" ]] || return 0
    done < <(desktop_filesystems)
    return 1
}

enable_disks_launcher() {
    local owner stage
    owner=$(dpkg-divert --root="$system_root" --listpackage /usr/bin/gnome-disks)
    [[ -z "$owner" || "$owner" == "$package" ]] || {
        echo "Disks already has a diversion owned by $owner; refusing to replace it" >&2
        return 1
    }
    [[ -f "$templates/gnome-disks" ]] || return 1
    stage=$(mktemp "$base/usr/bin/.infiltrator-disks-XXXXXX")
    trap 'rm -f -- "$stage"' RETURN
    install -m 0755 "$templates/gnome-disks" "$stage"
    if [[ -z "$owner" ]]; then
        [[ -f "$disks_launcher" ]] || { echo 'GNOME Disks is missing' >&2; return 1; }
        dpkg-divert --root="$system_root" --package "$package" --add --rename \
            --divert /usr/bin/gnome-disks.filesystem-support-original /usr/bin/gnome-disks
    fi
    [[ -f "$disks_original" ]] || return 1
    mv -f "$stage" "$disks_launcher"
    stage=
    trap - RETURN
}

enable_nemo_launcher() {
    local owner stage
    owner=$(dpkg-divert --root="$system_root" --listpackage /usr/bin/nemo)
    if [[ -z "$owner" && ! -e "$nemo_launcher" ]]; then
        return 0
    fi
    [[ -z "$owner" || "$owner" == "$package" ]] || {
        echo "Nemo already has a diversion owned by $owner; refusing to replace it" >&2
        return 1
    }
    [[ -f "$templates/nemo" ]] || return 1
    stage=$(mktemp "$base/usr/bin/.infiltrator-nemo-XXXXXX")
    trap 'rm -f -- "$stage"' RETURN
    install -m 0755 "$templates/nemo" "$stage"
    if [[ -z "$owner" ]]; then
        dpkg-divert --root="$system_root" --package "$package" --add --rename \
            --divert /usr/bin/nemo.filesystem-support-original /usr/bin/nemo
    fi
    [[ -f "$nemo_original" ]] || return 1
    mv -f "$stage" "$nemo_launcher"
    stage=
    trap - RETURN
}

enable_launchers() {
    enable_disks_launcher
    enable_nemo_launcher
}

disable_launchers() {
    if has_active; then
        echo 'refusing to remove shared desktop launchers while native desktop filesystems are active' >&2
        return 1
    fi

    if [[ $(dpkg-divert --root="$system_root" --listpackage /usr/bin/gnome-disks) == "$package" ]]; then
        [[ -f "$disks_original" ]] || { echo 'The original Disks executable is missing' >&2; return 1; }
        rm -f "$disks_launcher"
        dpkg-divert --root="$system_root" --package "$package" --remove --rename \
            --divert /usr/bin/gnome-disks.filesystem-support-original /usr/bin/gnome-disks
    fi
    if [[ $(dpkg-divert --root="$system_root" --listpackage /usr/bin/nemo) == "$package" ]]; then
        [[ -f "$nemo_original" ]] || { echo 'The original Nemo executable is missing' >&2; return 1; }
        rm -f "$nemo_launcher"
        dpkg-divert --root="$system_root" --package "$package" --remove --rename \
            --divert /usr/bin/nemo.filesystem-support-original /usr/bin/nemo
    fi
}

shared_launchers_ready() {
    local owner
    owner=$(dpkg-divert --root="$system_root" --listpackage /usr/bin/gnome-disks)
    [[ "$owner" == "$package" && -x "$disks_launcher" && -f "$disks_original" ]] || {
        echo 'Filesystem Support package desktop launcher state is not prepared; reinstall or reconfigure the package' >&2
        return 1
    }

    owner=$(dpkg-divert --root="$system_root" --listpackage /usr/bin/nemo)
    if [[ -e "$nemo_launcher" || -e "$nemo_original" || -n "$owner" ]]; then
        [[ "$owner" == "$package" && -x "$nemo_launcher" && -f "$nemo_original" ]] || {
            echo 'Filesystem Support Nemo launcher state is inconsistent; reinstall or reconfigure the package' >&2
            return 1
        }
    fi
}

snapshot_kind() {
    local kind=$1
    local work=$2
    mkdir -p "$active" "$rules"
    [[ ! -f "$active/$kind.so" ]] || cp -a "$active/$kind.so" "$work/previous.so"
    if [[ -f "$rules/99-infiltrator-$kind.rules" ]]; then
        cp -a "$rules/99-infiltrator-$kind.rules" "$work/previous.rules"
    elif [[ -f "$rules/59-infiltrator-$kind.rules" ]]; then
        cp -a "$rules/59-infiltrator-$kind.rules" "$work/previous.rules"
    fi
}

restore_kind() {
    local kind=$1
    local work=$2
    if [[ -f "$work/previous.so" ]]; then
        install -m 0644 "$work/previous.so" "$active/$kind.so"
    else
        rm -f "$active/$kind.so"
    fi
    if [[ -f "$work/previous.rules" ]]; then
        install -m 0644 "$work/previous.rules" "$rules/99-infiltrator-$kind.rules"
    else
        rm -f "$rules/99-infiltrator-$kind.rules"
    fi
    rm -f "$rules/59-infiltrator-$kind.rules"
}

run_kind_transaction() {
    local action=$1
    local kind=$2

    (
        set -eEuo pipefail
        local work committed result
        work=$(mktemp -d)
        committed=0
        snapshot_kind "$kind" "$work"

        cleanup_transaction() {
            result=$?
            trap - EXIT
            if [[ $result -ne 0 && $committed -eq 0 ]]; then
                restore_kind "$kind" "$work"
                refresh
            fi
            rm -rf "$work"
            exit "$result"
        }
        trap cleanup_transaction EXIT

        case "$action" in
            install)
                shared_launchers_ready
                install -m 0644 "$templates/$kind.so" "$active/$kind.so"
                # Stock 60-persistent-storage.rules runs blkid. Our identity
                # rule must run afterwards or blkid rewrites DOS/0 and DOS/1
                # back to the generic 'affs'.
                rm -f "$rules/59-infiltrator-$kind.rules"
                install -m 0644 "$templates/$kind.rules" "$rules/99-infiltrator-$kind.rules"
                refresh
                ;;
            remove)
                # Refresh while the matching rule still exists so stale
                # identity is cleared before the rule itself disappears.
                refresh
                rm -f "$active/$kind.so" \
                      "$rules/59-infiltrator-$kind.rules" \
                      "$rules/99-infiltrator-$kind.rules"
                refresh
                ;;
            *)
                echo "invalid desktop transaction action: $action" >&2
                exit 2
                ;;
        esac

        committed=1
        trap - EXIT
        rm -rf "$work"
    )
}

install_kind() {
    local kind=$1
    [[ -f "$templates/$kind.so" && -f "$templates/$kind.rules" ]] || {
        echo "Desktop templates are missing for $kind" >&2
        return 1
    }
    run_kind_transaction install "$kind"
}

remove_kind() {
    local kind=$1
    run_kind_transaction remove "$kind"
}

[[ $# -ge 1 ]] || exit 2
action=$1
case "$action" in
    install|remove)
        [[ $# -eq 2 ]] || exit 2
        kind=$2
        is_desktop_filesystem "$kind" || exit 2
        if [[ "$action" == install ]]; then
            install_kind "$kind"
        else
            remove_kind "$kind"
        fi
        ;;
    sync)
        [[ $# -eq 1 ]] || exit 2

        # Package-level shared launcher setup happens once here. Individual
        # filesystem transactions below never create or remove diversions.
        enable_launchers

        # Temporarily clear identification left by the retired global rule.
        mkdir -p "$base/run/udev/rules.d"
        transition_rule="$base/run/udev/rules.d/58-infiltrator-transition.rules"
        cleanup_transition() {
            local result=$?
            trap - EXIT
            rm -f "$transition_rule"
            refresh
            exit "$result"
        }
        trap cleanup_transition EXIT
        install -m 0644 "$templates/cleanup.rules" "$transition_rule"
        while IFS= read -r kind; do
            if [[ -f "$base/lib/modules/$kernel/updates/infiltrator/$kind.ko" ]]; then
                install_kind "$kind"
            else
                remove_kind "$kind"
            fi
        done < <(desktop_filesystems)
        rm -f "$transition_rule"
        trap - EXIT
        refresh
        ;;
    purge)
        [[ $# -eq 1 ]] || exit 2
        while IFS= read -r kind; do
            remove_kind "$kind"
        done < <(desktop_filesystems)
        ;;
    unprepare)
        [[ $# -eq 1 ]] || exit 2
        disable_launchers
        ;;
    *) exit 2 ;;
esac
