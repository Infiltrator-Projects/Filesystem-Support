#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Native filesystem transaction coordinator.
#
# Kernel-module state and desktop identification are deliberately independent
# transactions. This coordinator sequences them, but it owns neither rollback
# domain: kernel-module-helper owns kernel state and desktop-integration owns
# presentation state. A desktop failure therefore cannot undo committed kernel
# state, and a kernel failure prevents the desktop transaction from starting.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

usage() {
    echo "usage: $0 <install|remove> <filesystem-id> <module-name>" >&2
    exit 2
}

[[ $# -eq 3 ]] || usage
[[ ${EUID} -eq 0 ]] || {
    echo "native module coordinator must run as root" >&2
    exit 1
}

action=$1
filesystem=$2
module=$3
self_dir=$(cd "$(dirname "$0")" && pwd)
kernel_helper="$self_dir/kernel-module-helper"
desktop_helper="$self_dir/desktop-integration"

[[ -x "$kernel_helper" ]] || {
    echo "kernel module helper is missing; reinstall Filesystem Support" >&2
    exit 1
}

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
