// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"
#include <infiltratr/core.h>
#include <unordered_set>

namespace filesystem_support {

const std::vector<FilesystemDescriptor>& catalog()
{
    static const std::vector<FilesystemDescriptor> entries = {
        {"ext", "EXT2 / EXT3 / EXT4", "Linux", "The traditional Linux extended filesystem family.", {"ext4"}, {"e2fsprogs"}, AccessMode::ReadWrite, "Modern kernels normally use the ext4 driver for the family."},
        {"xfs", "SGI XFS", "Unix / Linux", "High-performance journaling filesystem originally developed by Silicon Graphics.", {"xfs"}, {"xfsprogs"}, AccessMode::ReadWrite, ""},
        {"btrfs", "Btrfs", "Linux", "Copy-on-write Linux filesystem with checksums, snapshots and subvolumes.", {"btrfs"}, {"btrfs-progs"}, AccessMode::ReadWrite, ""},
        {"f2fs", "F2FS", "Linux", "Flash-Friendly File System designed for NAND-backed storage.", {"f2fs"}, {"f2fs-tools"}, AccessMode::ReadWrite, ""},
        {"jfs", "IBM JFS", "Unix / Linux", "IBM journaling filesystem supported by the Linux JFS driver.", {"jfs"}, {"jfsutils"}, AccessMode::ReadWrite, ""},
        {"nilfs2", "NILFS2", "Linux", "Log-structured filesystem with continuous snapshotting.", {"nilfs2"}, {"nilfs-tools"}, AccessMode::ReadWrite, ""},
        {"fat", "FAT12 / FAT16 / FAT32", "Microsoft / DOS", "Classic DOS/Windows FAT filesystems.", {"vfat"}, {"dosfstools"}, AccessMode::ReadWrite, ""},
        {"exfat", "exFAT", "Microsoft", "Modern Microsoft removable-media filesystem.", {"exfat"}, {"exfatprogs"}, AccessMode::ReadWrite, ""},
        {"affs", "Amiga OFS / FFS (AFFS)", "Amiga", "Classic Commodore Amiga filesystem handled by the Linux AFFS driver.", {"affs"}, {}, AccessMode::Mixed, "DOS0-DOS3 are supported read/write; DOS4-DOS5 directory-cache variants are read-only."},
        {"hfs", "Apple HFS", "Apple", "Classic Macintosh Hierarchical File System.", {"hfs"}, {"hfsutils"}, AccessMode::ReadWrite, ""},
        {"hfsplus", "Apple HFS+", "Apple", "Mac OS Extended / HFS Plus filesystem.", {"hfsplus"}, {"hfsprogs"}, AccessMode::Mixed, "Linux write support depends on volume features; journaled volumes can be restricted."},
        {"iso9660", "ISO 9660", "Optical / Image", "CD-ROM filesystem used by optical media and many ISO images.", {"isofs"}, {}, AccessMode::ReadOnly, ""},
        {"udf", "UDF", "Optical / Image", "Universal Disk Format used by optical media and disk images.", {"udf"}, {"udftools"}, AccessMode::ReadWrite, ""},
        {"squashfs", "SquashFS", "Image", "Compressed read-only filesystem widely used by live media and application images.", {"squashfs"}, {"squashfs-tools"}, AccessMode::ReadOnly, ""},
        {"ufs", "UFS", "Unix", "Unix File System family used historically by BSD and other Unix systems.", {"ufs"}, {}, AccessMode::Mixed, "Linux UFS write support is limited and depends on the exact UFS variant."},
        {"zfs", "OpenZFS", "Unix / Linux", "Advanced pooled copy-on-write filesystem and volume manager.", {"zfs"}, {"zfsutils-linux"}, AccessMode::ReadWrite, ""},
        {"vmfs6", "VMware VMFS6", "Virtualisation", "VMware virtual-machine filesystem accessed through userspace tools.", {}, {"vmfs6-tools"}, AccessMode::Userspace, ""},
        {"cpm", "CP/M", "Retro", "Utilities for working with CP/M filesystem images and media.", {}, {"cpmtools"}, AccessMode::Userspace, ""},
        {"cifs", "SMB / CIFS", "Network", "Windows-compatible network filesystem client support.", {"cifs"}, {"cifs-utils"}, AccessMode::ReadWrite, ""},
        {"nfs", "NFS", "Network", "Network File System client support.", {"nfs"}, {"nfs-common"}, AccessMode::ReadWrite, ""}
    };
    return entries;
}

const char* access_mode_label(const AccessMode mode)
{
    switch (mode) {
    case AccessMode::ReadWrite: return "Read / write";
    case AccessMode::ReadOnly: return "Read-only";
    case AccessMode::Mixed: return "Variant-dependent";
    case AccessMode::Userspace: return "Userspace";
    }
    return "Unknown";
}

bool catalog_is_valid()
{
    std::unordered_set<std::string_view> ids;
    for (const auto& entry : catalog()) {
        if (entry.id.empty() || entry.name.empty() || entry.family.empty() || entry.description.empty()) return false;
        if (!ids.insert(entry.id).second) return false;
        if (!infiltratr_ascii_is_alnum(static_cast<unsigned char>(entry.id.front()))) return false;
        for (const auto package : entry.packages) if (package.empty()) return false;
        for (const auto module : entry.modules) if (module.empty()) return false;
    }
    return true;
}
} // namespace filesystem_support
