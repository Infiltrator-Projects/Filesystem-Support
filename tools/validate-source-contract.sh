#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Repository/source boundary contract.
#
# Keep mutable filesystem-set knowledge in data manifests, not in CI YAML.
# This script enforces invariants that are genuinely about implementation
# boundaries and can therefore be run locally or by any CI system.
set -euo pipefail

root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"

source_manifest=data/source-filesystems.txt
retired_manifest=data/retired-source-identities.txt
native_manifest=data/native-filesystems.tsv

for manifest in "$source_manifest" "$retired_manifest" "$native_manifest"; do
    [[ -s "$manifest" ]] || {
        echo "Required source contract manifest is missing or empty: $manifest" >&2
        exit 1
    }
done

manifest_values() {
    grep -Ev '^[[:space:]]*(#|$)' "$1"
}

test ! -e tools/shape-ext2.sh
test ! -e tools/shape-ext3.sh
test ! -e tools/shape-ext4.sh
test ! -e tools/harden-ext-ranges.py

# Source trees are declared once in data/source-filesystems.txt. CI does not
# maintain its own copy of the catalogue subset.
manifest_values "$source_manifest" | sort > /tmp/filesystem-support-source.expected
if [[ $(wc -l < /tmp/filesystem-support-source.expected) -ne \
      $(sort -u /tmp/filesystem-support-source.expected | wc -l) ]]; then
    echo "Duplicate filesystem in $source_manifest" >&2
    exit 1
fi
find native/filesystems -mindepth 1 -maxdepth 1 -type d -printf '%f\n' |
    sort > /tmp/filesystem-support-source.actual
diff -u /tmp/filesystem-support-source.expected /tmp/filesystem-support-source.actual

while IFS= read -r fs; do
    [[ "$fs" =~ ^[A-Za-z0-9_.+-]+$ ]] || {
        echo "Invalid source filesystem token: $fs" >&2
        exit 1
    }
    test -s "native/filesystems/$fs/DESIGN.md" || {
        echo "Missing design/provenance contract for $fs" >&2
        exit 1
    }
done < /tmp/filesystem-support-source.expected

# Retired provider/transport/overlay/archive identities are kept in one data
# file so adding or removing an historical guard does not require editing CI.
while IFS= read -r fs; do
    [[ "$fs" =~ ^[A-Za-z0-9_.+-]+$ ]] || {
        echo "Invalid retired filesystem token: $fs" >&2
        exit 1
    }
    test ! -e "native/filesystems/$fs" || {
        echo "Retired non-format source tree returned: $fs" >&2
        exit 1
    }
done < <(manifest_values "$retired_manifest")

# Validate the native deployment manifest structurally and ensure every managed
# filesystem has exactly the source required by the installer. The manifest
# itself is the authority; there is deliberately no second expected list.
awk -F '\t' '
    BEGIN { ok = 1 }
    /^[[:space:]]*#/ || /^[[:space:]]*$/ { next }
    NF != 4 {
        print "Invalid native manifest row: " $0 > "/dev/stderr"
        ok = 0
        next
    }
    $1 !~ /^[A-Za-z0-9_.+-]+$/ || $2 !~ /^[A-Za-z0-9_.+-]+$/ {
        print "Invalid native filesystem/module token: " $0 > "/dev/stderr"
        ok = 0
    }
    $3 != "none" && $3 != "amiga" {
        print "Invalid native desktop kind: " $0 > "/dev/stderr"
        ok = 0
    }
    $3 == "none" && $4 != "0" {
        print "Non-desktop filesystem has non-zero variant: " $0 > "/dev/stderr"
        ok = 0
    }
    $3 == "amiga" && $4 !~ /^[1-9][0-9]*$/ {
        print "Amiga filesystem has invalid desktop variant: " $0 > "/dev/stderr"
        ok = 0
    }
    seen_fs[$1]++ {
        print "Duplicate native filesystem: " $1 > "/dev/stderr"
        ok = 0
    }
    seen_module[$2]++ {
        print "Duplicate native module: " $2 > "/dev/stderr"
        ok = 0
    }
    $4 != "0" && seen_variant[$4]++ {
        print "Duplicate desktop variant: " $4 > "/dev/stderr"
        ok = 0
    }
    END { exit ok ? 0 : 1 }
' "$native_manifest"

while IFS=$'\t' read -r fs module desktop_kind desktop_variant; do
    [[ -n "$fs" && "${fs:0:1}" != "#" ]] || continue
    grep -Fxq "$fs" /tmp/filesystem-support-source.expected || {
        echo "Native filesystem $fs is not present in $source_manifest" >&2
        exit 1
    }
    test -d "native/filesystems/$fs/core"
    test -f "native/filesystems/$fs/linux/Makefile"
done < "$native_manifest"

# OFS and FFS must not share AFFS-family runtime state. Only the tiny generic
# kernel compatibility layer may be shared.
test -s native/platform/linux_kernel/vfs_compat.h
test ! -e native/platform/linux_kernel/amiga_affs_compat.h
test -s native/filesystems/ofs/linux/affs_compat.h
test -s native/filesystems/ffs/linux/affs_compat.h
grep -Fq '#include "affs_compat.h"' native/filesystems/ofs/linux/linux_adapter.h
grep -Fq '#include "affs_compat.h"' native/filesystems/ffs/linux/linux_adapter.h
! grep -R -Fq '../../../platform/linux_kernel/amiga_affs_compat.h' \
    native/filesystems/ofs native/filesystems/ffs

test ! -e native/filesystems/amiga_common
test ! -e native/primitives/amiga_dos
rewritten=(
    native/filesystems/ofs/linux/core_bridge.c
    native/filesystems/ofs/linux/storage.c
    native/filesystems/ofs/linux/namespace.c
    native/filesystems/ofs/linux/lifecycle.c
    native/filesystems/ofs/linux/linux_adapter.h
    native/filesystems/ofs/core/ofs_disk_layout.h
    native/filesystems/ofs/core/ofs_primitives.c
    native/filesystems/ffs/linux/core_bridge.c
    native/filesystems/ffs/linux/storage.c
    native/filesystems/ffs/linux/namespace.c
    native/filesystems/ffs/linux/lifecycle.c
    native/filesystems/ffs/linux/linux_adapter.h
    native/filesystems/ffs/core/ffs_disk_layout.h
    native/filesystems/ffs/core/ffs_primitives.c
)
for file in "${rewritten[@]}"; do
    test -s "$file"
    ! grep -Eiq 'linux/fs/affs|Hans-Joachim Widmaier|Ray Burr|Eric Youngdale|Linus Torvalds' "$file"
done
! grep -R -Eq 'IFS_FFS_IS_OFS|SF_OFS|AFFS_MOUNT_SF_OFS|affs_aops_ofs|ifs_ffs_ofs_|AFFS_DATA_HEAD|AFFS_DATA\(' \
    native/filesystems/ffs/linux

# References are evidence only and are never compiled into active modules.
! grep -Fq '/reference' CMakeLists.txt
while IFS=$'\t' read -r fs module desktop_kind desktop_variant; do
    [[ -n "$fs" && "${fs:0:1}" != "#" ]] || continue
    [[ "$desktop_kind" == "amiga" ]] || continue
    ! grep -Fq 'reference' "native/filesystems/$fs/linux/Makefile"
done < "$native_manifest"

# Windows EXT2 must consume the canonical EXT2 engine rather than a second
# filesystem implementation.
for part in adapter_support name_translation file_dispatch directory_dispatch volume_lifecycle driver_entry; do
    test -s "native/filesystems/ext2/windows/$part.inc"
    grep -Fq "#include \"$part.inc\"" native/filesystems/ext2/windows/ext2_driver.c
done
test "$(wc -l < native/filesystems/ext2/windows/ext2_driver.c)" -lt 80
test ! -e native/filesystems/ext2/windows/ext2_compat.h
! grep -R -E 'archive/extfs-for-windows|core/extfs(_classic_resize|_resize_dispatch)?\.c' \
    native/filesystems/ext2/windows windows/build CMakeLists.txt
grep -Fq '..\core\ext2_core.c' \
    native/filesystems/ext2/windows/filesystem_support_ext2.vcxproj
grep -Fq '..\core\ext2_engine.c' \
    native/filesystems/ext2/windows/filesystem_support_ext2.vcxproj
grep -Fq '#include "../core/ext2_engine.h"' \
    native/filesystems/ext2/windows/ext2_driver.h
grep -Fq '#include "../core/ext2_engine.c"' \
    native/filesystems/ext2/linux/core_bridge.c
grep -Fq 'metadata.o' native/filesystems/ext2/linux/Makefile
grep -Fq -- '-DCONFIG_EXT2_FS_XATTR=1' native/filesystems/ext2/linux/Makefile
grep -Fq -- '-DCONFIG_EXT2_FS_POSIX_ACL=1' native/filesystems/ext2/linux/Makefile
grep -Fq -- '-DCONFIG_EXT2_FS_SECURITY=1' native/filesystems/ext2/linux/Makefile
