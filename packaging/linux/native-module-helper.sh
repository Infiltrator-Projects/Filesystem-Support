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
    ext2:ext2|ext3:ext3|ext4:ext4|ofs:ofs|ffs:ffs) ;;
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

install_native() {
    [[ -f "$source_linux/Makefile" ]] || {
        echo "packaged native source is missing for $filesystem" >&2
        exit 1
    }

    ensure_build_environment

    local tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT

    mkdir -p "$tmp/$filesystem"
    cp -a "$source_fs/core" "$tmp/$filesystem/core"
    cp -a "$source_linux" "$tmp/$filesystem/linux"

    make -C "$kernel_build" M="$tmp/$filesystem/linux" modules

    local built="$tmp/$filesystem/linux/$module.ko"
    [[ -s "$built" ]] || {
        echo "kernel build did not produce $module.ko" >&2
        exit 1
    }

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
