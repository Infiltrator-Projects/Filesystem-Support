#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Native filesystem transaction coordinator and read-only state facade.
#
# Kernel-module state and desktop identification are deliberately independent
# transactions. This coordinator sequences mutations, but it owns neither
# rollback domain: kernel-module-helper owns kernel state and desktop-integration
# owns presentation state. It also exposes the native layer's installed-state
# query so presentation recovery never has to reconstruct module identity from a
# filesystem ID or read the native deployment manifest itself.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

usage() {
    echo "usage: $0 <install|remove> <filesystem-id> <module-name>" >&2
    echo "       $0 is-installed <filesystem-id>" >&2
    exit 2
}

[[ ${EUID} -eq 0 ]] || {
    echo "native module coordinator must run as root" >&2
    exit 1
}

self_dir=$(cd "$(dirname "$0")" && pwd)
kernel_helper="$self_dir/kernel-module-helper"
desktop_helper="$self_dir/desktop-integration"
manifest="$self_dir/native-filesystems.tsv"

[[ -x "$kernel_helper" ]] || {
    echo "kernel module helper is missing; reinstall Filesystem Support" >&2
    exit 1
}
[[ -r "$manifest" ]] || {
    echo "native filesystem deployment manifest is missing" >&2
    exit 1
}

managed_module_for() {
    local filesystem=$1
    awk -F '\t' -v fs="$filesystem" '
        $0 !~ /^#/ && NF == 2 && $1 == fs { print $2; found = 1; exit }
        END { if (!found) exit 1 }
    ' "$manifest"
}

if [[ $# -eq 2 && $1 == is-installed ]]; then
    filesystem=$2
    if ! module=$(managed_module_for "$filesystem"); then
        echo "refusing unmanaged native filesystem: $filesystem" >&2
        exit 2
    fi

    kernel=$(uname -r)
    if [[ -f "/lib/modules/$kernel/updates/infiltrator/$module.ko" ]]; then
        exit 0
    fi
    exit 3
fi

[[ $# -eq 3 ]] || usage
action=$1
filesystem=$2
module=$3
[[ "$action" == install || "$action" == remove ]] || usage

# Commit the kernel transaction first. No desktop operation is attempted if
# this fails, and this coordinator contains no kernel rollback behaviour.
"$kernel_helper" "$action" "$filesystem" "$module"

# Desktop integration is optional and independently authoritative about which
# filesystems it manages. Absence of the desktop helper does not invalidate a
# successful kernel transaction.
[[ -x "$desktop_helper" ]] || exit 0

set +e
"$desktop_helper" is-managed "$filesystem"
managed_status=$?
set -e

case "$managed_status" in
    0)
        ;;
    3)
        exit 0
        ;;
    *)
        echo "$filesystem kernel module transaction succeeded, but desktop integration policy could not be queried; kernel state remains committed." >&2
        exit 1
        ;;
esac

if ! "$desktop_helper" "$action" "$filesystem"; then
    echo "$filesystem kernel module transaction succeeded, but desktop integration failed; kernel state remains committed." >&2
    exit 1
fi
