// SPDX-License-Identifier: GPL-3.0-or-later
#include "catalog.hpp"

#include <infiltratr/core.h>

#include <unordered_set>

namespace filesystem_support {

const std::vector<FilesystemDescriptor>& catalog()
{
    static const std::vector<FilesystemDescriptor> entries = {
        // Native and mainstream Linux filesystems.
        {"ext", "EXT2 / EXT3 / EXT4", "Linux",
         "Linux extended filesystem family.",
         {"ext4"}, {"e2fsprogs"}, AccessMode::ReadWrite,
         "The ext4 driver handles the modern family; e2fsprogs supplies creation, checking and administration tools."},
        {"xfs", "SGI XFS", "Unix / Linux",
         "High-performance journaling filesystem originally developed by Silicon Graphics.",
         {"xfs"}, {"xfsprogs"}, AccessMode::ReadWrite, ""},
        {"btrfs", "Btrfs", "Linux",
         "Copy-on-write Linux filesystem with checksums, snapshots and subvolumes.",
         {"btrfs"}, {"btrfs-progs"}, AccessMode::ReadWrite, ""},
        {"f2fs", "F2FS", "Linux",
         "Flash-Friendly File System designed for NAND-backed storage.",
         {"f2fs"}, {"f2fs-tools"}, AccessMode::ReadWrite, ""},
        {"jfs", "IBM JFS", "Unix / Linux",
         "IBM journaling filesystem supported by the Linux JFS driver.",
         {"jfs"}, {"jfsutils"}, AccessMode::ReadWrite, ""},
        {"nilfs2", "NILFS2", "Linux",
         "Log-structured filesystem with continuous snapshotting.",
         {"nilfs2"}, {"nilfs-tools"}, AccessMode::ReadWrite, ""},
        {"reiserfs", "ReiserFS", "Linux / Legacy",
         "Legacy journaling filesystem with Debian userspace administration tools.",
         {"reiserfs"}, {"reiserfsprogs"}, AccessMode::Mixed,
         "The kernel driver is deprecated and may be absent from newer kernels even though Debian still packages the userspace tools."},
        {"erofs", "EROFS", "Linux / Image",
         "Enhanced read-only filesystem used for compressed immutable images and embedded systems.",
         {"erofs"}, {"erofs-utils"}, AccessMode::ReadOnly, ""},
        {"minix", "Minix filesystem", "Unix / Legacy",
         "Filesystem family used by Minix and early Linux systems.",
         {"minix"}, {"util-linux"}, AccessMode::ReadWrite,
         "Debian's util-linux supplies mkfs.minix and fsck.minix."},
        {"romfs", "ROMFS", "Linux / Embedded",
         "Small read-only filesystem intended for embedded and rescue images.",
         {"romfs"}, {"genromfs"}, AccessMode::ReadOnly, ""},
        {"cramfs", "CramFS", "Linux / Embedded",
         "Compressed read-only Linux filesystem used by older embedded systems.",
         {"cramfs"}, {"util-linux"}, AccessMode::ReadOnly,
         "util-linux provides cramfs creation/checking utilities where supported."},
        {"squashfs", "SquashFS", "Linux / Image",
         "Compressed read-only filesystem widely used by live media and appliance images.",
         {"squashfs"}, {"squashfs-tools"}, AccessMode::ReadOnly, ""},
        {"bcachefs", "Bcachefs", "Linux",
         "Copy-on-write Linux filesystem integrated into newer kernels.",
         {"bcachefs"}, {}, AccessMode::Mixed,
         "Debian trixie stable does not ship bcachefs-tools; kernel mount support is shown when the running Debian kernel provides it."},
        {"overlayfs", "OverlayFS", "Linux / Overlay",
         "Native Linux union/overlay filesystem used heavily by containers.",
         {"overlay"}, {}, AccessMode::ReadWrite, ""},
        {"ecryptfs", "eCryptfs", "Linux / Encryption",
         "Native stacked cryptographic filesystem.",
         {"ecryptfs"}, {"ecryptfs-utils"}, AccessMode::ReadWrite, ""},

        // Cluster and pooled filesystems.
        {"gfs2", "GFS2", "Cluster",
         "Red Hat Global File System 2 for concurrent access to shared cluster storage.",
         {"gfs2"}, {"gfs2-utils"}, AccessMode::ReadWrite, ""},
        {"ocfs2", "OCFS2", "Cluster",
         "Oracle Cluster File System 2 general-purpose clustered filesystem.",
         {"ocfs2"}, {"ocfs2-tools"}, AccessMode::ReadWrite, ""},
        {"zfs", "OpenZFS", "Unix / Linux",
         "Pooled copy-on-write filesystem and volume manager.",
         {"zfs"}, {"zfsutils-linux"}, AccessMode::ReadWrite,
         "Availability depends on the Debian repository components enabled on the machine."},

        // Historical workstation, Unix and other operating-system formats.
        {"adfs", "Acorn ADFS", "Acorn / RISC OS",
         "Acorn Disc Filing System used by RISC OS and earlier Acorn systems.",
         {"adfs"}, {}, AccessMode::Mixed,
         "Support depends on whether the running Debian kernel was built with the ADFS driver."},
        {"affs", "Amiga OFS / FFS (AFFS)", "Amiga",
         "Classic Commodore Amiga OFS/FFS family handled by the Linux AFFS driver.",
         {"affs"}, {}, AccessMode::Mixed,
         "DOS0-DOS3 are read/write; DOS4-DOS5 directory-cache variants are read-only."},
        {"befs", "BeOS BeFS", "BeOS / Haiku",
         "BeOS filesystem support from the Linux BeFS driver.",
         {"befs"}, {}, AccessMode::ReadOnly,
         "Linux BeFS support is primarily for reading legacy BeOS volumes."},
        {"bfs", "SCO BFS", "Unix / Legacy",
         "SCO/UnixWare Boot File System support from the Linux BFS driver.",
         {"bfs"}, {}, AccessMode::Mixed, ""},
        {"efs", "SGI EFS", "SGI / Unix",
         "Silicon Graphics Extent File System used before XFS.",
         {"efs"}, {}, AccessMode::ReadOnly, ""},
        {"hpfs", "OS/2 HPFS", "IBM / OS/2",
         "High Performance File System used by IBM OS/2.",
         {"hpfs"}, {}, AccessMode::Mixed,
         "This is legacy kernel support; write capability depends on the kernel configuration."},
        {"qnx4", "QNX4 filesystem", "QNX",
         "QNX4 filesystem support provided by the Linux kernel.",
         {"qnx4"}, {}, AccessMode::ReadOnly, ""},
        {"qnx6", "QNX6 filesystem", "QNX",
         "QNX6 Power-Safe filesystem support provided by the Linux kernel.",
         {"qnx6"}, {}, AccessMode::ReadOnly, ""},
        {"sysv", "System V / Xenix / Coherent FS", "Unix / Legacy",
         "Linux sysv driver for several historical System V-derived filesystem variants.",
         {"sysv"}, {}, AccessMode::Mixed, ""},
        {"ufs", "UFS / BSD FFS", "Unix / BSD",
         "Unix File System family used by BSD, Sun and other Unix systems.",
         {"ufs"}, {}, AccessMode::Mixed,
         "Linux write support is deliberately limited and depends on the exact UFS variant."},
        {"omfs", "OMFS", "Embedded / Legacy",
         "Optimized MPEG File System used by some embedded media devices.",
         {"omfs"}, {}, AccessMode::Mixed, ""},

        // Microsoft and DOS family.
        {"fat", "FAT12 / FAT16 / FAT32", "Microsoft / DOS",
         "Classic DOS and Windows FAT filesystems.",
         {"vfat", "msdos"}, {"dosfstools"}, AccessMode::ReadWrite, ""},
        {"exfat", "exFAT", "Microsoft",
         "Microsoft removable-media filesystem with native Linux kernel support.",
         {"exfat"}, {"exfatprogs"}, AccessMode::ReadWrite, ""},
        {"ntfs", "NTFS", "Microsoft",
         "Windows NT filesystem using Debian's mature NTFS-3G userspace implementation.",
         {}, {"ntfs-3g"}, AccessMode::Userspace,
         "Newer Linux kernels may also expose the in-kernel ntfs3 driver independently."},
        {"bitlocker", "BitLocker volumes", "Microsoft / Encryption",
         "Access to BitLocker-encrypted Windows volumes through Dislocker.",
         {}, {"dislocker"}, AccessMode::Userspace,
         "Dislocker exposes a virtual NTFS volume which is then mounted using NTFS support."},

        // Apple family.
        {"hfs", "Apple HFS", "Apple / Classic Mac",
         "Classic Macintosh Hierarchical File System.",
         {"hfs"}, {"hfsprogs"}, AccessMode::Mixed,
         "Debian trixie no longer carries hfsutils in stable; hfsprogs and the kernel driver provide the current packaged path."},
        {"hfsplus", "Apple HFS+", "Apple",
         "Mac OS Extended / HFS Plus filesystem.",
         {"hfsplus"}, {"hfsprogs"}, AccessMode::Mixed,
         "Journaled or feature-rich volumes can restrict Linux write support."},
        {"apfs-fuse", "Apple APFS (FUSE access)", "Apple",
         "Userspace APFS access through Debian's libfsapfs implementation.",
         {}, {"libfsapfs-utils"}, AccessMode::Userspace,
         "This is the conservative userspace access path and includes a FUSE mount implementation."},
        {"apfs-dkms", "Apple APFS (experimental kernel driver)", "Apple / Experimental",
         "Out-of-tree APFS kernel module and tools packaged by Debian.",
         {"apfs"}, {"apfs-dkms", "apfsprogs"}, AccessMode::Experimental,
         "Write support is explicitly experimental; Debian's package description advises caution."},

        // Optical, immutable and image formats.
        {"iso9660", "ISO 9660 / Joliet / Rock Ridge", "Optical / Image",
         "CD-ROM filesystem and common extensions used by optical media and ISO images.",
         {"isofs", "iso9660"}, {}, AccessMode::ReadOnly, ""},
        {"udf", "UDF", "Optical / Image",
         "Universal Disk Format used by optical media, removable disks and images.",
         {"udf"}, {"udftools"}, AccessMode::ReadWrite, ""},
        {"udfclient", "UDF userspace client", "Optical / Userspace",
         "Independent userland implementation and inspection tools for UDF.",
         {}, {"udfclient"}, AccessMode::ToolsOnly,
         "Provides an FTP-like userland client and UDF creation/inspection utilities rather than a kernel mount."},
        {"squashfuse", "SquashFS via FUSE", "Image / FUSE",
         "Userspace SquashFS mount implementation.",
         {}, {"squashfuse"}, AccessMode::Userspace, ""},
        {"erofsfuse", "EROFS via FUSE", "Image / FUSE",
         "Userspace mount implementation for EROFS images.",
         {}, {"erofsfuse"}, AccessMode::Userspace, ""},
        {"fuse2fs", "EXT2 / EXT3 / EXT4 via FUSE", "Linux / FUSE",
         "Userspace read/write ext-family filesystem client for devices and images.",
         {}, {"fuse2fs"}, AccessMode::Userspace, ""},
        {"exfat-fuse", "exFAT via FUSE", "Microsoft / FUSE",
         "Userspace exFAT implementation retained as an alternative to the native kernel driver.",
         {}, {"exfat-fuse"}, AccessMode::Userspace, ""},
        {"fusefat", "FAT family via FUSE", "Microsoft / DOS / FUSE",
         "Unprivileged FUSE access to FAT12, FAT16, FAT32 and exFAT filesystems.",
         {}, {"fusefat"}, AccessMode::Userspace, ""},
        {"fuseiso", "ISO images via FUSE", "Image / FUSE",
         "Userspace mounting for ISO and several single-track image formats.",
         {}, {"fuseiso"}, AccessMode::Userspace, ""},
        {"fusezip", "ZIP archives via FUSE", "Archive / FUSE",
         "Read/write ZIP archive filesystem.",
         {}, {"fuse-zip"}, AccessMode::Userspace, ""},
        {"archivemount", "ArchiveMount", "Archive / FUSE",
         "Mount many archive and compressed-file formats as a filesystem.",
         {}, {"archivemount"}, AccessMode::Userspace,
         "Supports numerous libarchive formats including tar, cpio, ISO and ZIP/RAR families."},

        // Virtualisation and retro formats.
        {"vmfs", "VMware VMFS3 / VMFS5", "Virtualisation",
         "Userspace access to VMware VMFS filesystems.",
         {}, {"vmfs-tools"}, AccessMode::ReadOnly,
         "Debian's vmfs-tools provides read-only command-line and FUSE access."},
        {"vmfs6", "VMware VMFS6", "Virtualisation",
         "Userspace access to VMware VMFS6 filesystems.",
         {}, {"vmfs6-tools"}, AccessMode::ReadOnly,
         "Debian's VMFS6 implementation currently provides read-only access."},
        {"cpm", "CP/M filesystems", "Retro",
         "Tools for reading and writing CP/M filesystem media and images.",
         {}, {"cpmtools"}, AccessMode::ToolsOnly,
         "cpmtools supports the CP/M filesystem structures directly rather than using a Linux kernel driver."},

        // Network, distributed and remote filesystems.
        {"cifs", "SMB / CIFS", "Network",
         "Windows-compatible network filesystem client.",
         {"cifs"}, {"cifs-utils"}, AccessMode::ReadWrite, ""},
        {"nfs", "NFS", "Network",
         "Network File System client support.",
         {"nfs"}, {"nfs-common"}, AccessMode::ReadWrite, ""},
        {"cephfs", "CephFS (kernel client)", "Distributed",
         "Native Linux client for the Ceph distributed filesystem.",
         {"ceph"}, {"ceph-common"}, AccessMode::ReadWrite,
         "The native kernel client is generally preferred when available."},
        {"ceph-fuse", "CephFS via FUSE", "Distributed / FUSE",
         "Userspace Ceph filesystem client.",
         {}, {"ceph-fuse"}, AccessMode::Userspace, ""},
        {"glusterfs", "GlusterFS", "Distributed / FUSE",
         "Client for Gluster distributed storage volumes.",
         {}, {"glusterfs-client"}, AccessMode::Userspace, ""},
        {"openafs", "OpenAFS", "Distributed",
         "Andrew File System client with Debian-packaged DKMS kernel module.",
         {"openafs"}, {"openafs-client", "openafs-modules-dkms"}, AccessMode::ReadWrite, ""},
        {"9p", "Plan 9 9P", "Network / Plan 9",
         "Linux 9P filesystem client used by Plan 9 and virtualisation environments.",
         {"9p"}, {}, AccessMode::ReadWrite, ""},
        {"orangefs", "OrangeFS", "Distributed",
         "Linux kernel client support for the OrangeFS distributed filesystem.",
         {"orangefs"}, {}, AccessMode::Mixed,
         "Debian stable does not provide a matching OrangeFS client package; this row reports kernel support only."},
        {"sshfs", "SSHFS", "Network / FUSE",
         "Mount remote filesystems over SSH/SFTP.",
         {}, {"sshfs"}, AccessMode::Userspace, ""},
        {"davfs2", "WebDAV", "Network",
         "Mount WebDAV resources as a filesystem.",
         {}, {"davfs2"}, AccessMode::Userspace, ""},
        {"curlftpfs", "FTP via FUSE", "Network / FUSE",
         "Mount FTP servers through libcurl and FUSE.",
         {}, {"curlftpfs"}, AccessMode::Userspace, ""},
        {"s3fs", "S3 object storage via FUSE", "Cloud / FUSE",
         "Mount S3-compatible object storage through FUSE.",
         {}, {"s3fs"}, AccessMode::Userspace, ""},
        {"rclone", "Rclone remote mounts", "Cloud / Network",
         "FUSE-backed mounts for the many remote storage providers supported by rclone.",
         {}, {"rclone"}, AccessMode::Userspace, ""},
        {"afuse", "AFUSE automounter", "Network / FUSE",
         "FUSE automounter that can dynamically invoke filesystem clients on demand.",
         {}, {"afuse"}, AccessMode::Userspace, ""},

        // Generic overlays and encrypted userspace filesystems.
        {"mergerfs", "mergerfs", "Overlay / FUSE",
         "FUSE union filesystem for pooling multiple storage locations.",
         {}, {"mergerfs"}, AccessMode::Userspace, ""},
        {"fuse-overlayfs", "fuse-overlayfs", "Overlay / FUSE",
         "Userspace OverlayFS implementation commonly used by rootless containers.",
         {}, {"fuse-overlayfs"}, AccessMode::Userspace, ""},
        {"bindfs", "bindfs", "Overlay / FUSE",
         "Mirror a directory through FUSE while changing permission and ownership presentation.",
         {}, {"bindfs"}, AccessMode::Userspace, ""},
        {"encfs", "EncFS", "Encryption / FUSE",
         "Encrypted virtual filesystem storing encrypted files in an ordinary backing directory.",
         {}, {"encfs"}, AccessMode::Userspace, ""},
        {"gocryptfs", "gocryptfs", "Encryption / FUSE",
         "Modern encrypted overlay filesystem built on FUSE.",
         {}, {"gocryptfs"}, AccessMode::Userspace, ""},
        {"cryfs", "CryFS", "Encryption / FUSE",
         "Encrypted cloud-oriented filesystem implemented with FUSE.",
         {}, {"cryfs"}, AccessMode::Userspace, ""}
    };

    return entries;
}

const char* access_mode_label(const AccessMode mode)
{
    switch (mode) {
    case AccessMode::ReadWrite:
        return "Read / write";
    case AccessMode::ReadOnly:
        return "Read-only";
    case AccessMode::Mixed:
        return "Variant / kernel dependent";
    case AccessMode::Userspace:
        return "Userspace / FUSE";
    case AccessMode::ToolsOnly:
        return "Userspace tools";
    case AccessMode::Experimental:
        return "Experimental";
    }
    return "Unknown";
}

bool catalog_is_valid()
{
    std::unordered_set<std::string_view> ids;

    for (const auto& entry : catalog()) {
        if (entry.id.empty() || entry.name.empty() || entry.family.empty() ||
            entry.description.empty()) {
            return false;
        }

        if (!ids.insert(entry.id).second) {
            return false;
        }

        if (!infiltratr_ascii_is_alnum(
                static_cast<unsigned char>(entry.id.front()))) {
            return false;
        }

        for (const auto package : entry.packages) {
            if (package.empty()) {
                return false;
            }
        }

        for (const auto module : entry.modules) {
            if (module.empty()) {
                return false;
            }
        }
    }

    return true;
}

} // namespace filesystem_support
