// SPDX-License-Identifier: GPL-3.0-or-later
#include "platform_support.hpp"

#include "filesystem_identity.hpp"

#include <vector>

namespace filesystem_support {

const std::vector<FilesystemIdentity>& filesystem_identities()
{
    // Stable on-media format identity only. Linux package names, kernel module
    // names, provider classes and deployment state deliberately do not exist in
    // this table, so non-Linux consumers cannot acquire those dependencies by
    // construction.
    static const std::vector<FilesystemIdentity> identities = {
        {"infiltratorfs", "InfiltratorFS", "InfiltratorOS",
         "InfiltratorOS native copy-on-write filesystem.",
         "Native Linux VFS/DKMS provider from the InfiltratorFS project; this is a first-class InfiltratorOS root-filesystem target."},

        {"ext2", "EXT2", "Linux", "Second Extended Filesystem.",
         "Project-native EXT2 implementation and Linux module; no external EXT userspace provider."},
        {"ext3", "EXT3", "Linux", "Third Extended Filesystem.",
         "Project-native EXT3 implementation and Linux module, kept separate from EXT2 and EXT4."},
        {"ext4", "EXT4", "Linux", "Fourth Extended Filesystem.",
         "Project-native EXT4 implementation and Linux module; no external EXT userspace provider."},
        {"xfs", "SGI XFS", "Unix / Linux",
         "High-performance journaling filesystem originally developed by Silicon Graphics.", ""},
        {"btrfs", "Btrfs", "Linux",
         "Copy-on-write Linux filesystem with checksums, snapshots and subvolumes.", ""},
        {"f2fs", "F2FS", "Linux",
         "Flash-Friendly File System for block-addressed flash storage.", ""},
        {"jfs", "IBM JFS", "Unix / Linux",
         "IBM journaling filesystem supported by the Linux JFS driver.", ""},
        {"nilfs2", "NILFS2", "Linux",
         "Log-structured filesystem with continuous snapshotting.", ""},
        {"reiserfs", "ReiserFS", "Linux / Legacy",
         "Legacy journaling disk filesystem.",
         "The Linux driver is deprecated and may be absent from newer kernels; the format remains a real local disk filesystem target for compatibility work."},
        {"minix", "Minix filesystem", "Unix / Legacy",
         "Filesystem family used by Minix and early Linux systems.",
         "Debian's util-linux supplies mkfs.minix and fsck.minix."},
        {"bcachefs", "Bcachefs", "Linux",
         "Copy-on-write local-storage filesystem integrated into newer Linux kernels.",
         "Debian trixie stable does not ship bcachefs-tools; current availability therefore depends on the running kernel and formatter qualification is pending."},

        {"gfs2", "GFS2", "Shared-disk / Linux",
         "Red Hat Global File System 2 on-disk format.", ""},
        {"ocfs2", "OCFS2", "Shared-disk / Linux",
         "Oracle Cluster File System 2 on-disk format.", ""},
        {"zfs", "OpenZFS", "Unix / Linux",
         "ZFS pooled copy-on-write local-storage filesystem.",
         "ZFS is represented once by format identity; zfs-fuse is not a second filesystem entry."},

        {"adfs", "Acorn ADFS", "Acorn / RISC OS",
         "Acorn Disc Filing System used by RISC OS and earlier Acorn systems.",
         "Support depends on whether the running Debian kernel was built with the ADFS driver."},
        {"ofs", "Amiga OFS", "Amiga",
         "Classic Commodore Amiga Original File System as an independent native module.",
         "Project-native Linux OFS module; directory-cache variants remain conservative/read-only where required."},
        {"ffs", "Amiga FFS", "Amiga",
         "Classic Commodore Amiga Fast File System as an independent native module.",
         "Project-native Linux FFS module; directory-cache variants remain conservative/read-only where required."},
        {"sfs", "Amiga SFS", "Amiga",
         "Amiga Smart File System as an independent native Linux module.",
         "Project-native Linux SFS module implementing the SFS on-disk format."},
        {"sfs2", "Amiga SFS2", "Amiga",
         "Amiga Smart File System 2 as an independent native Linux module.",
         "Project-native Linux SFS2 module implementing the SFS2 on-disk format independently."},
        {"pfs3", "Amiga PFS3", "Amiga",
         "Amiga Professional File System 3 disk format.",
         "Project-native PFS3 adapter is intentionally read-only until writable metadata updates are fully qualified."},
        {"befs", "BeOS BeFS", "BeOS / Haiku",
         "BeOS filesystem disk format.",
         "Current Linux BeFS support is primarily read-only; the format itself remains a local disk filesystem."},
        {"bfs", "SCO BFS", "Unix / Legacy",
         "SCO/UnixWare Boot File System disk format.", ""},
        {"efs", "SGI EFS", "SGI / Unix",
         "Silicon Graphics Extent File System used before XFS.", ""},
        {"hpfs", "OS/2 HPFS", "IBM / OS/2",
         "High Performance File System used by IBM OS/2.",
         "Current Linux write capability depends on kernel configuration."},
        {"qnx4", "QNX4 filesystem", "QNX",
         "QNX4 local disk filesystem.", ""},
        {"qnx6", "QNX6 filesystem", "QNX",
         "QNX6 Power-Safe local disk filesystem.", ""},
        {"sysv", "System V / Xenix / Coherent FS", "Unix / Legacy",
         "System V-derived local disk filesystem family.", ""},
        {"ufs", "UFS / BSD FFS", "Unix / BSD",
         "Unix File System family used by BSD, Sun and other Unix systems.",
         "Current Linux write support is limited and depends on the exact UFS variant."},
        {"omfs", "OMFS", "Embedded / Legacy",
         "Optimized MPEG File System local disk format used by some embedded media devices.", ""},

        {"fat", "FAT12 / FAT16 / FAT32", "Microsoft / DOS",
         "Classic DOS and Windows FAT disk filesystems.",
         "FAT is one format-family entry; fusefat is only an alternative provider and is not separately catalogued."},
        {"exfat", "exFAT", "Microsoft",
         "Microsoft removable/local-storage filesystem.",
         "exFAT is represented once; exfat-fuse is not a second filesystem entry."},
        {"ntfs", "NTFS", "Microsoft",
         "Windows NT on-disk filesystem.",
         "NTFS is represented once. NTFS-3G is the current conservative Debian access provider; ntfs3 is a provider, not a second filesystem identity."},

        {"hfs", "Apple HFS", "Apple / Classic Mac",
         "Classic Macintosh Hierarchical File System.",
         "Debian trixie uses hfsprogs with the kernel driver."},
        {"hfsplus", "Apple HFS+", "Apple",
         "Mac OS Extended / HFS Plus filesystem.",
         "Journaled or feature-rich volumes can restrict current Linux write support."},
        {"apfs", "Apple APFS", "Apple",
         "Apple File System on-disk format.",
         "APFS has one catalogue identity. The current Debian kernel provider is experimental; FUSE/DKMS provider names are implementation details rather than separate filesystems."},

        {"udf", "UDF", "Optical / Local storage",
         "Universal Disk Format, including writable block-device media.",
         "UDF remains in scope because it is a persistent on-media filesystem format and can be used on writable block devices; ISO9660 and immutable image filesystems are out of setup-root scope."},
        {"vmfs", "VMware VMFS3 / VMFS5", "Virtualisation / Disk",
         "VMware VMFS3/VMFS5 on-disk filesystem format.",
         "Current Debian access is read-only; VMFS remains in scope because it is a real persistent disk filesystem."},
        {"vmfs6", "VMware VMFS6", "Virtualisation / Disk",
         "VMware VMFS6 on-disk filesystem format.",
         "Current Debian access is read-only; VMFS6 remains in scope because it is a real persistent disk filesystem."},
        {"cpm", "CP/M filesystems", "Retro",
         "CP/M local disk filesystem family.",
         "Current support is tools-only; the format remains a legitimate future native/root-capability target rather than a tools product identity."},
        {"fosfat", "Smaky filesystem (Fosfat)", "Retro",
         "Smaky local disk filesystem format.",
         "Current Debian access is read-only userspace; the catalogue identity is the disk format, not FUSE itself."}
    };

    return identities;
}

NativeImplementationState windows_native_state(const std::string_view filesystem_id)
{
    // EXT2 is the proving implementation for the shared-engine architecture.
    // Its canonical engine already serves both platform adapters; Windows
    // remains non-installable until the IFS lifecycle, trust and runtime
    // qualification contract is complete.
    if (filesystem_id == "ext2") {
        return NativeImplementationState::InProgress;
    }

    return NativeImplementationState::NotImplemented;
}

const char* native_implementation_state_label(const NativeImplementationState state)
{
    switch (state) {
    case NativeImplementationState::NotImplemented:
        return "Not implemented";
    case NativeImplementationState::InProgress:
        return "In progress";
    case NativeImplementationState::Qualified:
        return "Available";
    }
    return "Unknown";
}

bool windows_native_installable(const std::string_view filesystem_id)
{
    return windows_native_state(filesystem_id) == NativeImplementationState::Qualified;
}

} // namespace filesystem_support
