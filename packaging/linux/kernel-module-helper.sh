#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Kernel-only native filesystem transaction and state authority.
#
# This helper owns build prerequisites, compilation, signing, replacement,
# depmod, module selection, load/unload, rollback and read-only installed-state
# queries for native filesystem modules. It deliberately has no knowledge of
# desktop integration, udev presentation policy, GNOME Disks, Nemo or any other
# presentation layer.
set -euo pipefail
PATH=/usr/sbin:/usr/bin:/sbin:/bin
export PATH

usage() {
    echo "usage: $0 <install|remove> <filesystem-id> <module-name>" >&2
    echo "       $0 <is-installed|status> <filesystem-id>" >&2
    echo "       $0 remove-all" >&2
    exit 2
}

self_dir=$(cd "$(dirname "$0")" && pwd)
manifest="$self_dir/native-filesystems.tsv"
[[ -r "$manifest" ]] || {
    echo "native filesystem deployment manifest is missing" >&2
    exit 1
}

kernel_lock_path=/run/lock/infiltrator-filesystem-support-kernel.lock
acquire_kernel_mutation_lock() {
    # Native filesystem modules are separate products, but depmod metadata and
    # kernel module selection are shared host state. One transaction therefore
    # owns that state at a time. remove-all invokes this helper recursively, so
    # descendants inherit the already-held descriptor instead of deadlocking on
    # the same lock while preserving one aggregate removal transaction.
    if [[ ${FILESYSTEM_SUPPORT_KERNEL_LOCK_FD:-} == 9 ]] &&
       [[ $(readlink /proc/$$/fd/9 2>/dev/null || true) == "$kernel_lock_path" ]]; then
        return 0
    fi

    mkdir -p /run/lock
    exec 9>"$kernel_lock_path"
    flock -x 9
    export FILESYSTEM_SUPPORT_KERNEL_LOCK_FD=9
}

managed_module_for() {
    local requested=$1
    awk -F '\t' -v fs="$requested" '
        $0 !~ /^#/ && NF == 2 && $1 == fs { print $2; found = 1; exit }
        END { if (!found) exit 1 }
    ' "$manifest"
}

uses_shared_linux_kernel_adapter() {
    local requested=$1
    local source_fs="$self_dir/native/filesystems/$requested"

    # The shared adapter is a build input only for modules that include it.
    # Derive that relationship from the filesystem source itself instead of
    # maintaining a second consumer list beside the actual include graph.
    grep -Rqs \
        --include='*.c' --include='*.h' \
        'platform/linux_kernel/vfs_compat[.]h' \
        "$source_fs/core" "$source_fs/linux"
}

native_source_digest_for() {
    local requested=$1
    local source_fs="$self_dir/native/filesystems/$requested"
    local source_platform="$self_dir/native/platform/linux_kernel"

    [[ -d "$source_fs/core" && -d "$source_fs/linux" &&
       -f "$source_fs/linux/Makefile" ]] || {
        echo "packaged native source is incomplete for $requested" >&2
        return 1
    }

    if uses_shared_linux_kernel_adapter "$requested" &&
       [[ ! -f "$source_platform/vfs_compat.h" ]]; then
        echo "packaged shared Linux kernel adapter source is missing for $requested" >&2
        return 1
    fi

    # Hash only actual native build inputs. README/Kconfig/reference material is
    # intentionally excluded because the out-of-tree module build consumes the
    # Linux Makefile plus C/header sources. The shared compatibility header is
    # included only for filesystems whose source includes it. This prevents an
    # unrelated documentation or adapter change from making a current module
    # appear stale and forcing a needless unload/rebuild.
    (
        cd "$self_dir"
        find \
            "native/filesystems/$requested/core" \
            "native/filesystems/$requested/linux" \
            -type f \
            \( -name 'Makefile' -o -name '*.c' -o -name '*.h' \) \
            -print0
        if uses_shared_linux_kernel_adapter "$requested"; then
            printf '%s\0' "native/platform/linux_kernel/vfs_compat.h"
        fi
    ) | LC_ALL=C sort -z | xargs -0 -r sha256sum | sha256sum | awk '{print $1}'
}

preferred_module_filename_for() {
    local requested_module=$1
    modinfo -F filename "$requested_module" 2>/dev/null | head -n1 || true
}

native_status() {
    local requested=$1
    local requested_module
    local kernel
    local destination
    local stamp
    local recorded_digest=""
    local current_digest
    local freshness=stale
    local selection=unselected

    if ! requested_module=$(managed_module_for "$requested"); then
        echo "refusing unmanaged native filesystem: $requested" >&2
        return 2
    fi

    kernel=$(uname -r)
    destination="/lib/modules/$kernel/updates/infiltrator/$requested_module.ko"
    stamp="$destination.source-sha256"

    if [[ ! -f "$destination" ]]; then
        printf 'absent\tstale\tunselected\n'
        return 0
    fi

    current_digest=$(native_source_digest_for "$requested") || return 1
    if [[ -r "$stamp" ]]; then
        read -r recorded_digest < "$stamp" || true
    fi
    if [[ -n "$recorded_digest" && "$recorded_digest" == "$current_digest" ]]; then
        freshness=current
    fi

    if [[ $(preferred_module_filename_for "$requested_module") == "$destination" ]]; then
        selection=selected
    fi

    printf 'installed\t%s\t%s\n' "$freshness" "$selection"
}

remove_stale_kernel_directories() {
    local current_kernel
    local directory
    local release
    local staging
    current_kernel=$(uname -r)

    for directory in /lib/modules/*/updates/infiltrator; do
        [[ -d "$directory" ]] || continue
        release=${directory#/lib/modules/}
        release=${release%/updates/infiltrator}

        if [[ "$release" == "$current_kernel" ]]; then
            rmdir "$directory" 2>/dev/null || true
            continue
        fi

        # This directory is owned exclusively by Filesystem Support. Rename it
        # as one unit so stale-kernel cleanup has one persistent rollback point
        # instead of reimplementing per-filesystem membership outside the
        # canonical manifest.
        staging="${directory}.filesystem-support-removing.$$"
        [[ ! -e "$staging" ]] || {
            echo "stale native-module cleanup staging path already exists: $staging" >&2
            return 1
        }
        mv "$directory" "$staging"
        if depmod -a "$release"; then
            rm -rf -- "$staging"
            continue
        fi

        mv "$staging" "$directory"
        depmod -a "$release" >/dev/null 2>&1 || true
        echo "dependency metadata refresh failed while removing native modules for stale kernel $release; previous state was restored" >&2
        return 1
    done
}

# Read-only queries intentionally precede the privilege gate. The desktop and
# manager need observed state, not mutation rights, and forcing them to elevate
# would either cause authentication prompts during scans or encourage policy
# duplication outside this authority.
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

if [[ $# -eq 2 && $1 == status ]]; then
    native_status "$2"
    exit $?
fi

[[ ${EUID} -eq 0 ]] || {
    echo "kernel module helper mutations must run as root" >&2
    exit 1
}

if [[ $# -eq 1 && $1 == remove-all ]]; then
    acquire_kernel_mutation_lock
    while IFS=$'\t' read -r filesystem module extra; do
        [[ -n "${filesystem:-}" ]] || continue
        [[ "$filesystem" == \#* ]] && continue
        [[ -n "${module:-}" && -z "${extra:-}" ]] || {
            echo "invalid native filesystem deployment manifest row" >&2
            exit 1
        }
        "$0" remove "$filesystem" "$module"
    done < "$manifest"

    remove_stale_kernel_directories
    echo "all Filesystem Support native modules removed"
    exit 0
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

acquire_kernel_mutation_lock

source_fs="$self_dir/native/filesystems/$filesystem"
source_linux="$source_fs/linux"
source_platform="$self_dir/native/platform/linux_kernel"
kernel=$(uname -r)
kernel_build="/lib/modules/$kernel/build"
destination_dir="/lib/modules/$kernel/updates/infiltrator"
destination="$destination_dir/$module.ko"
source_stamp="$destination.source-sha256"

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
    preferred_module_filename_for "$module"
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

secure_boot_signing_identity() {
    # Debian's DKMS tooling owns the normal distribution MOK at /var/lib/dkms.
    # Ubuntu/Mint derivatives retain a different shim-signed convention. Treat
    # both as external signing identities and prefer the Debian baseline first;
    # Filesystem Support must not create a competing private-key authority.
    local candidate
    for candidate in \
        '/var/lib/dkms/mok.key|/var/lib/dkms/mok.pub' \
        '/var/lib/shim-signed/mok/MOK.priv|/var/lib/shim-signed/mok/MOK.der'; do
        local key=${candidate%%|*}
        local cert=${candidate#*|}
        if [[ -r "$key" && -r "$cert" ]]; then
            printf '%s\t%s\n' "$key" "$cert"
            return 0
        fi
    done
    return 1
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

    local identity
    local key
    local cert
    if ! identity=$(secure_boot_signing_identity); then
        cat >&2 <<'EOF'
Secure Boot is enabled, but no supported enrolled-signing identity is available.

On Debian, configure/enroll the normal DKMS Machine Owner Key, for example:
  sudo apt install dkms
  sudo dkms generate_mok
  sudo mokutil --import /var/lib/dkms/mok.pub

Then reboot, complete MOK enrollment in the firmware/shim screen, and run
Install native again.

Ubuntu/Mint's /var/lib/shim-signed/mok/MOK.priv + MOK.der identity is also
accepted when present. Filesystem Support will not disable Secure Boot, create
a second signing-key authority, or install an unsigned kernel module.
EOF
        exit 1
    fi
    IFS=$'\t' read -r key cert <<<"$identity"

    if command -v mokutil >/dev/null 2>&1 &&
       mokutil --help 2>&1 | grep -q -- '--test-key'; then
        if ! mokutil --test-key "$cert" >/dev/null 2>&1; then
            cat >&2 <<EOF
Secure Boot is enabled and a signing identity exists at:
  $cert

That certificate is not enrolled. Import it with:
  sudo mokutil --import $cert

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
    local previous_stamp_copy=$2
    local previous_preferred=$3
    local previous_loaded=$4
    local restore_runtime=$5

    if [[ "$restore_runtime" == true ]] && module_loaded; then
        modprobe -r "$module" >/dev/null 2>&1 || return 1
    fi

    if [[ -n "$previous_copy" ]]; then
        install -m 0644 "$previous_copy" "$destination" || return 1
    else
        rm -f "$destination" || return 1
    fi
    if [[ -n "$previous_stamp_copy" ]]; then
        install -m 0644 "$previous_stamp_copy" "$source_stamp" || return 1
    else
        rm -f "$source_stamp" || return 1
    fi
    depmod -a "$kernel" || return 1

    if [[ "$restore_runtime" == true && "$previous_loaded" == true ]]; then
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
    local previous_stamp_copy=$3
    local previous_preferred=$4
    local previous_loaded=$5
    local restore_runtime=$6

    if restore_previous_state \
        "$previous_copy" "$previous_stamp_copy" "$previous_preferred" \
        "$previous_loaded" "$restore_runtime"; then
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

    local source_digest
    source_digest=$(native_source_digest_for "$filesystem") || exit 1

    ensure_build_environment

    local tmp
    local tmp_native
    tmp=$(mktemp -d)
    cleanup_tmp=$tmp
    tmp_native="$tmp/native"

    mkdir -p "$tmp_native/filesystems/$filesystem"
    cp -a "$source_fs/core" "$tmp_native/filesystems/$filesystem/core"
    cp -a "$source_linux" "$tmp_native/filesystems/$filesystem/linux"
    if uses_shared_linux_kernel_adapter "$filesystem"; then
        mkdir -p "$tmp_native/platform"
        cp -a "$source_platform" "$tmp_native/platform/linux_kernel"
    fi

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
    local previous_stamp_copy=""
    local previous_loaded=false
    previous_preferred=$(preferred_module_filename)

    # Snapshot persistent state before touching runtime state. From the first
    # successful unload onward every failure path is required to restore both
    # the module image and the exact source-provenance stamp that described it.
    if [[ -f "$destination" ]]; then
        previous_copy="$tmp/previous.ko"
        cp -a "$destination" "$previous_copy"
    fi
    if [[ -f "$source_stamp" ]]; then
        previous_stamp_copy="$tmp/previous.source-sha256"
        cp -a "$source_stamp" "$previous_stamp_copy"
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
            "$previous_copy" "$previous_stamp_copy" "$previous_preferred" \
            "$previous_loaded" true
        return 1
    fi
    if ! install -m 0644 "$built" "$destination"; then
        fail_with_restore \
            "could not replace the native module image" \
            "$previous_copy" "$previous_stamp_copy" "$previous_preferred" \
            "$previous_loaded" true
        return 1
    fi
    printf '%s\n' "$source_digest" > "$tmp/current.source-sha256"
    if ! install -m 0644 "$tmp/current.source-sha256" "$source_stamp"; then
        fail_with_restore \
            "could not record native module source provenance" \
            "$previous_copy" "$previous_stamp_copy" "$previous_preferred" \
            "$previous_loaded" true
        return 1
    fi
    if ! depmod -a "$kernel"; then
        fail_with_restore \
            "dependency metadata refresh failed after native module replacement" \
            "$previous_copy" "$previous_stamp_copy" "$previous_preferred" \
            "$previous_loaded" true
        return 1
    fi
    if ! modprobe "$module"; then
        fail_with_restore \
            "new $module module could not be loaded" \
            "$previous_copy" "$previous_stamp_copy" "$previous_preferred" \
            "$previous_loaded" true
        return 1
    fi

    local selected
    selected=$(preferred_module_filename)
    if [[ "$selected" != "$destination" || ! -d "/sys/module/$module" ]]; then
        fail_with_restore \
            "kernel selected '$selected' instead of the Infiltrator module" \
            "$previous_copy" "$previous_stamp_copy" "$previous_preferred" \
            "$previous_loaded" true
        return 1
    fi

    echo "$filesystem native module installed and loaded for kernel $kernel"
}

remove_native() {
    if [[ ! -f "$destination" ]]; then
        rm -f "$source_stamp"
        echo "$filesystem native module is not installed for kernel $kernel"
        return 0
    fi

    local tmp
    local previous_copy
    local previous_stamp_copy=""
    local previous_preferred
    local previous_loaded=false
    local runtime_changed=false
    tmp=$(mktemp -d)
    cleanup_tmp=$tmp
    previous_copy="$tmp/previous.ko"
    cp -a "$destination" "$previous_copy"
    if [[ -f "$source_stamp" ]]; then
        previous_stamp_copy="$tmp/previous.source-sha256"
        cp -a "$source_stamp" "$previous_stamp_copy"
    fi
    previous_preferred=$(preferred_module_filename)

    if module_loaded; then
        previous_loaded=true
    fi

    if [[ "$previous_loaded" == true && "$previous_preferred" == "$destination" ]]; then
        if ! modprobe -r "$module"; then
            echo "$module is currently in use; unmount dependent filesystems before removal" >&2
            exit 1
        fi
        runtime_changed=true
    fi

    if ! rm -f "$destination" "$source_stamp"; then
        fail_with_restore \
            "could not remove the native module image and provenance" \
            "$previous_copy" "$previous_stamp_copy" "$previous_preferred" \
            "$previous_loaded" "$runtime_changed"
        return 1
    fi
    if ! depmod -a "$kernel"; then
        fail_with_restore \
            "dependency metadata refresh failed after native module removal" \
            "$previous_copy" "$previous_stamp_copy" "$previous_preferred" \
            "$previous_loaded" "$runtime_changed"
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
