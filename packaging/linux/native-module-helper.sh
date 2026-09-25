#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
set -euo pipefail

usage() {
    echo "usage: $0 <install|remove> <filesystem-id> <module-name>" >&2
    exit 2
}

[[ $# -eq 3 ]] || usage
[[ ${EUID} -eq 0 ]] || {
    echo "native module helper must run as root" >&2
    exit 1
}

action=$1
filesystem=$2
module=$3

case "$filesystem:$module" in
    ext2:ext2|ext3:ext3|ext4:ext4|ofs:ofs|ffs:ffs|sfs:sfs) ;;
    *)
        echo "refusing unmanaged native filesystem module: $filesystem/$module" >&2
        exit 2
        ;;
esac

self_dir=$(cd "$(dirname "$0")" && pwd)
source_fs="$self_dir/native/filesystems/$filesystem"
source_linux="$source_fs/linux"
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
    variable=$(find /sys/firmware/efi/efivars -maxdepth 1         -name 'SecureBoot-*' -type f -print -quit 2>/dev/null || true)
    [[ -n "$variable" ]] || return 1

    [[ "$(od -An -t u1 -j 4 -N 1 "$variable" 2>/dev/null | tr -d '[:space:]')" == "1" ]]
}

kernel_build_compiler() {
    local compile_header="$kernel_build/include/generated/compile.h"
    local compiler=""

    if [[ -r "$compile_header" ]]; then
        compiler=$(
            sed -n 's/^#define LINUX_COMPILER "\([^ ]*\).*/\1/p'                 "$compile_header" | head -n1
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

install_native() {
    [[ -f "$source_linux/Makefile" ]] || {
        echo "packaged native source is missing for $filesystem" >&2
        exit 1
    }

    ensure_build_environment

    local tmp
    tmp=$(mktemp -d)
    cleanup_tmp=$tmp

    mkdir -p "$tmp/$filesystem"
    cp -a "$source_fs/core" "$tmp/$filesystem/core"
    cp -a "$source_linux" "$tmp/$filesystem/linux"

    local build_cc
    build_cc=$(kernel_build_compiler)
    make -C "$kernel_build" M="$tmp/$filesystem/linux" CC="$build_cc" modules

    local built="$tmp/$filesystem/linux/$module.ko"
    [[ -s "$built" ]] || {
        echo "kernel build did not produce $module.ko" >&2
        exit 1
    }

    sign_for_secure_boot "$built"

    local previous_preferred=""
    local previous_copy=""
    previous_preferred=$(preferred_module_filename)

    if module_loaded; then
        if ! modprobe -r "$module"; then
            echo "$module is built-in or currently in use; refusing unsafe replacement" >&2
            exit 1
        fi
    fi

    mkdir -p "$destination_dir"
    if [[ -f "$destination" ]]; then
        previous_copy="$tmp/previous.ko"
        cp -a "$destination" "$previous_copy"
    fi

    install -m 0644 "$built" "$destination"
    depmod -a "$kernel"

    if ! modprobe "$module"; then
        if [[ -n "$previous_copy" ]]; then
            install -m 0644 "$previous_copy" "$destination"
        else
            rm -f "$destination"
        fi
        depmod -a "$kernel"

        if [[ -n "$previous_preferred" &&
              "$previous_preferred" != "(builtin)" &&
              "$previous_preferred" != "$destination" ]]; then
            modprobe "$module" >/dev/null 2>&1 || true
        fi

        echo "new $module module could not be loaded; installation was rolled back" >&2
        exit 1
    fi

    local selected
    selected=$(preferred_module_filename)
    if [[ "$selected" != "$destination" || ! -d "/sys/module/$module" ]]; then
        modprobe -r "$module" >/dev/null 2>&1 || true
        if [[ -n "$previous_copy" ]]; then
            install -m 0644 "$previous_copy" "$destination"
        else
            rm -f "$destination"
        fi
        depmod -a "$kernel"
        if [[ -n "$previous_preferred" &&
              "$previous_preferred" != "(builtin)" &&
              "$previous_preferred" != "$destination" ]]; then
            modprobe "$module" >/dev/null 2>&1 || true
        fi
        echo "kernel selected '$selected' instead of the Infiltrator module; rolled back" >&2
        exit 1
    fi

    echo "$filesystem native module installed and loaded for kernel $kernel"
}

remove_native() {
    if [[ ! -f "$destination" ]]; then
        echo "$filesystem native module is not installed for kernel $kernel"
        exit 0
    fi

    local selected
    selected=$(preferred_module_filename)

    if module_loaded && [[ "$selected" == "$destination" ]]; then
        if ! modprobe -r "$module"; then
            echo "$module is currently in use; unmount dependent filesystems before removal" >&2
            exit 1
        fi
    fi

    rm -f "$destination"
    depmod -a "$kernel"
    rmdir "$destination_dir" 2>/dev/null || true

    echo "$filesystem native module removed for kernel $kernel"
}

case "$action" in
    install) install_native ;;
    remove) remove_native ;;
    *) usage ;;
esac
