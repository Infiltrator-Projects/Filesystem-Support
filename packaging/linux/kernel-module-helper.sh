#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Kernel-only native filesystem transaction and state authority.
#
# This helper owns build prerequisites, compilation, signing, replacement,
# depmod, module selection, load/unload, rollback and read-only installed-state
# queries for exactly one native filesystem module. It deliberately has no
# knowledge of desktop integration, udev presentation policy, GNOME Disks, Nemo
# or any other presentation layer.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

usage() {
    echo "usage: $0 <install|remove> <filesystem-id> <module-name>" >&2
    echo "       $0 is-installed <filesystem-id>" >&2
    exit 2
}

[[ ${EUID} -eq 0 ]] || {
    echo "kernel module helper must run as root" >&2
    exit 1
}

self_dir=$(cd "$(dirname "$0")" && pwd)
manifest="$self_dir/native-filesystems.tsv"
[[ -r "$manifest" ]] || {
    echo "native filesystem deployment manifest is missing" >&2
    exit 1
}

managed_module_for() {
    local requested=$1
    awk -F '\t' -v fs="$requested" '
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
    destination="/lib/modules/$kernel/updates/infiltrator/$module.ko"
    [[ -f "$destination" ]] && exit 0
    exit 3
fi

[[ $# -eq 3 ]] || usage
action=$1
filesystem=$2
module=$3
[[ "$action" == install || "$action" == remove ]] || usage

awk -F '\t' -v fs="$filesystem" -v mod="$module" '
    $0 !~ /^#/ && NF == 2 && $1 == fs && $2 == mod { found = 1; exit }
    END { exit found ? 0 : 1 }
' "$manifest" || {
    echo "refusing unmanaged native filesystem module: $filesystem/$module" >&2
    exit 2
}

source_fs="$self_dir/native/filesystems/$filesystem"
source_linux="$source_fs/linux"
source_platform="$self_dir/native/platform/linux_kernel"
kernel=$(uname -r)
kernel_build="/lib/modules/$kernel/build"
destination_dir="/lib/modules/$kernel/updates/infiltrator"
destination="$destination_dir/$module.ko"

cleanup_tmp=""
cleanup() {
    if [[ -n "${cleanup_tmp:-}" ]]; then
        rm -rf -- "$cleanup_tmp"
    fi
}
trap cleanup EXIT

ensure_build_environment() {
    local packages=()

    if ! command -v make >/dev/null 2>&1 || ! command -v gcc >/dev/null 2>&1; then
        packages+=(build-essential)
    fi
    if [[ ! -f "$kernel_build/Makefile" ]]; then
        packages+=("linux-headers-$kernel")
    fi

    if (( ${#packages[@]} != 0 )); then
        command -v apt-get >/dev/null 2>&1 || {
            echo "apt-get is required to install missing kernel build dependencies" >&2
            exit 1
        }
        apt-get update
        DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${packages[@]}"
    fi

    [[ -f "$kernel_build/Makefile" ]] || {
        echo "matching kernel headers are unavailable for $kernel" >&2
        exit 1
    }
}

module_loaded() {
    [[ -d "/sys/module/$module" ]]
}

preferred_module_filename() {
    modinfo -F filename "$module" 2>/dev/null | head -n1 || true
}

secure_boot_enabled() {
    if command -v mokutil >/dev/null 2>&1; then
        mokutil --sb-state 2>/dev/null | grep -qi 'SecureBoot enabled'
        return
    fi

    local variable
    variable=$(find /sys/firmware/efi/efivars -maxdepth 1 \
        -name 'SecureBoot-*' -type f -print -quit 2>/dev/null || true)
    [[ -n "$variable" ]] || return 1

    [[ "$(od -An -t u1 -j 4 -N 1 "$variable" 2>/dev/null | tr -d '[:space:]')" == "1" ]]
}

kernel_build_compiler() {
    local compile_header="$kernel_build/include/generated/compile.h"
    local compiler=""

    if [[ -r "$compile_header" ]]; then
        compiler=$(
            sed -n 's/^#define LINUX_COMPILER "\([^ ]*\).*/\1/p' \
                "$compile_header" | head -n1
        )
    fi

    if [[ -n "$compiler" ]] && command -v "$compiler" >/dev/null 2>&1; then
        command -v "$compiler"
        return 0
    fi

    command -v gcc
}

sign_for_secure_boot() {
    local image=$1

    secure_boot_enabled || return 0

    local key=/var/lib/shim-signed/mok/MOK.priv
    local cert=/var/lib/shim-signed/mok/MOK.der

    if [[ ! -r "$key" || ! -r "$cert" ]]; then
        cat >&2 <<'EOF'
Secure Boot is enabled, but no Ubuntu/Mint Machine Owner Key is available.

Create and enroll the system MOK once with:
  sudo update-secureboot-policy --new-key
  sudo update-secureboot-policy --enroll-key

Then reboot, complete MOK enrollment in the firmware/shim screen, and run
Install native again. Filesystem Support will not disable Secure Boot or
install an unsigned kernel module.
EOF
        exit 1
    fi

    if command -v mokutil >/dev/null 2>&1 &&
       mokutil --help 2>&1 | grep -q -- '--test-key'; then
        if ! mokutil --test-key "$cert" >/dev/null 2>&1; then
            cat >&2 <<'EOF'
Secure Boot is enabled and a MOK exists, but that certificate is not enrolled.

Run:
  sudo update-secureboot-policy --enroll-key

Then reboot, complete MOK enrollment, and run Install native again.
EOF
            exit 1
        fi
    fi

    if command -v kmodsign >/dev/null 2>&1; then
        kmodsign sha512 "$key" "$cert" "$image"
    elif [[ -x "$kernel_build/scripts/sign-file" ]]; then
        "$kernel_build/scripts/sign-file" sha256 "$key" "$cert" "$image"
    else
        echo "Secure Boot is enabled but no kernel-module signing tool is available." >&2
        exit 1
    fi

    local signer
    signer=$(modinfo -F signer "$image" 2>/dev/null || true)
    [[ -n "$signer" ]] || {
        echo "kernel module signing completed without a readable signer; refusing installation" >&2
        exit 1
    }
}

restore_previous_state() {
    local previous_copy=$1
    local previous_preferred=$2
    local previous_loaded=$3

    if module_loaded; then
        modprobe -r "$module" >/dev/null 2>&1 || return 1
    fi

    if [[ -n "$previous_copy" ]]; then
        install -m 0644 "$previous_copy" "$destination" || return 1
    else
        rm -f "$destination" || return 1
    fi
    depmod -a "$kernel" || return 1

    if [[ "$previous_loaded" == true ]]; then
        modprobe "$module" >/dev/null 2>&1 || return 1
        local restored_selected
        restored_selected=$(preferred_module_filename)
        [[ -d "/sys/module/$module" &&
           "$restored_selected" == "$previous_preferred" ]] || return 1
    fi

    return 0
}

fail_with_restore() {
    local reason=$1
    local previous_copy=$2
    local previous_preferred=$3
    local previous_loaded=$4

    if restore_previous_state \
        "$previous_copy" "$previous_preferred" "$previous_loaded"; then
        echo "$reason; previous module state was restored" >&2
    else
        echo "$reason and the previous module state could not be fully restored" >&2
    fi
    return 1
}

install_native() {
    [[ -f "$source_linux/Makefile" ]] || {
        echo "packaged native source is missing for $filesystem" >&2
        exit 1
    }
    [[ -f "$source_platform/vfs_compat.h" ]] || {
        echo "packaged shared Linux kernel adapter source is missing" >&2
        exit 1
    }

    ensure_build_environment

    local tmp
    local tmp_native
    tmp=$(mktemp -d)
    cleanup_tmp=$tmp
    tmp_native="$tmp/native"

    mkdir -p \
        "$tmp_native/filesystems/$filesystem" \
        "$tmp_native/platform"
    cp -a "$source_fs/core" "$tmp_native/filesystems/$filesystem/core"
    cp -a "$source_linux" "$tmp_native/filesystems/$filesystem/linux"
    cp -a "$source_platform" "$tmp_native/platform/linux_kernel"

    local build_cc
    build_cc=$(kernel_build_compiler)
    make -C "$kernel_build" \
        M="$tmp_native/filesystems/$filesystem/linux" \
        CC="$build_cc" modules

    local built="$tmp_native/filesystems/$filesystem/linux/$module.ko"
    [[ -s "$built" ]] || {
        echo "kernel build did not produce $module.ko" >&2
        exit 1
    }

    sign_for_secure_boot "$built"

    local previous_preferred=""
    local previous_copy=""
    local previous_loaded=false
    previous_preferred=$(preferred_module_filename)

    # Snapshot persistent state before touching runtime state. From the first
    # successful unload onward every failure path is required to restore both.
    if [[ -f "$destination" ]]; then
        previous_copy="$tmp/previous.ko"
        cp -a "$destination" "$previous_copy"
    fi

    if module_loaded; then
        previous_loaded=true
        if ! modprobe -r "$module"; then
            echo "$module is built-in or currently in use; refusing unsafe replacement" >&2
            exit 1
        fi
    fi

    if ! mkdir -p "$destination_dir"; then
        fail_with_restore \
            "could not prepare the native module destination" \
            "$previous_copy" "$previous_preferred" "$previous_loaded"
        return 1
    fi
    if ! install -m 0644 "$built" "$destination"; then
        fail_with_restore \
            "could not replace the native module image" \
            "$previous_copy" "$previous_preferred" "$previous_loaded"
        return 1
    fi
    if ! depmod -a "$kernel"; then
        fail_with_restore \
            "dependency metadata refresh failed after native module replacement" \
            "$previous_copy" "$previous_preferred" "$previous_loaded"
        return 1
    fi
    if ! modprobe "$module"; then
        fail_with_restore \
            "new $module module could not be loaded" \
            "$previous_copy" "$previous_preferred" "$previous_loaded"
        return 1
    fi

    local selected
    selected=$(preferred_module_filename)
    if [[ "$selected" != "$destination" || ! -d "/sys/module/$module" ]]; then
        fail_with_restore \
            "kernel selected '$selected' instead of the Infiltrator module" \
            "$previous_copy" "$previous_preferred" "$previous_loaded"
        return 1
    fi

    echo "$filesystem native module installed and loaded for kernel $kernel"
}

remove_native() {
    if [[ ! -f "$destination" ]]; then
        echo "$filesystem native module is not installed for kernel $kernel"
        return 0
    fi

    local tmp
    local previous_copy
    local previous_preferred
    local previous_loaded=false
    tmp=$(mktemp -d)
    cleanup_tmp=$tmp
    previous_copy="$tmp/previous.ko"
    cp -a "$destination" "$previous_copy"
    previous_preferred=$(preferred_module_filename)

    if module_loaded; then
        previous_loaded=true
    fi

    if [[ "$previous_loaded" == true && "$previous_preferred" == "$destination" ]]; then
        if ! modprobe -r "$module"; then
            echo "$module is currently in use; unmount dependent filesystems before removal" >&2
            exit 1
        fi
    fi

    if ! rm -f "$destination"; then
        fail_with_restore \
            "could not remove the native module image" \
            "$previous_copy" "$previous_preferred" "$previous_loaded"
        return 1
    fi
    if ! depmod -a "$kernel"; then
        fail_with_restore \
            "dependency metadata refresh failed after native module removal" \
            "$previous_copy" "$previous_preferred" "$previous_loaded"
        return 1
    fi
    rmdir "$destination_dir" 2>/dev/null || true

    echo "$filesystem native module removed for kernel $kernel"
}

case "$action" in
    install) install_native ;;
    remove) remove_native ;;
    *) usage ;;
esac
