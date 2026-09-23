#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
build="$root/build-cross"
cc=x86_64-w64-mingw32-gcc
windres=x86_64-w64-mingw32-windres

common_root="$root/third_party/infiltratr-common"
if [ ! -f "$common_root/VERSION" ]; then
    echo "Infiltratr Common 1.11.0 is missing. Clone with --recurse-submodules." >&2
    exit 1
fi
if [ "$(cat "$common_root/VERSION")" != "1.11.0" ]; then
    echo "ExtFS requires Infiltratr Common 1.11.0." >&2
    exit 1
fi

mkdir -p "$build"

# Build as freestanding kernel code: there is no C runtime in the final .sys.
common="-std=c11 -Wall -Wextra -Werror -D_AMD64_ -DAMD64 -DNTDDI_VERSION=0x0A000000 -I$root/include -I$common_root/include -ffreestanding -fno-stack-protector -fno-builtin"

$cc $common '-D_Dispatch_type_(x)=' \
    -I/usr/x86_64-w64-mingw32/include/ddk \
    -c "$root/windows/driver/extfs_driver.c" \
    -o "$build/extfs_driver.o"
$cc $common -c "$root/core/extfs.c" -o "$build/extfs_core.o"
$windres -I/usr/x86_64-w64-mingw32/include \
    "$root/windows/driver/extfs.rc" "$build/extfs_res.o"

# Produce a PE native-subsystem image with relocations/ASLR metadata retained.
$cc -nostdlib -shared \
    -Wl,--subsystem,native:6.02 \
    -Wl,--entry,DriverEntry \
    -Wl,--image-base,0x10000 \
    -Wl,--dynamicbase \
    -Wl,--enable-reloc-section \
    -Wl,--nxcompat \
    -Wl,--high-entropy-va \
    -Wl,--major-os-version,6 \
    -Wl,--minor-os-version,2 \
    -Wl,--major-image-version,0 \
    -Wl,--minor-image-version,9 \
    -Wl,--exclude-all-symbols \
    -o "$build/extfs.sys" \
    "$build/extfs_driver.o" "$build/extfs_core.o" "$build/extfs_res.o" \
    -lntoskrnl -lhal

echo "$build/extfs.sys"
