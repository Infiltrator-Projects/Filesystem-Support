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
         {"ext4"}, {"e2fsprogs"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,"The ext4 driver handles the modern family; e2fsprogs supplies creation, checking and administration tools."},
        {"xfs", "SGI XFS", "Unix / Linux",
         "High-performance journaling filesystem originally developed by Silicon Graphics.",
         {"xfs"}, {"xfsprogs"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"btrfs", "Btrfs", "Linux",
         "Copy-on-write Linux filesystem with checksums, snapshots and subvolumes.",
         {"btrfs"}, {"btrfs-progs"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"f2fs", "F2FS", "Linux",
         "Flash-Friendly File System designed for NAND-backed storage.",
         {"f2fs"}, {"f2fs-tools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"jfs", "IBM JFS", "Unix / Linux",
         "IBM journaling filesystem supported by the Linux JFS driver.",
         {"jfs"}, {"jfsutils"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"nilfs2", "NILFS2", "Linux",
         "Log-structured filesystem with continuous snapshotting.",
         {"nilfs2"}, {"nilfs-tools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"reiserfs", "ReiserFS", "Linux / Legacy",
         "Legacy journaling filesystem with Debian userspace administration tools.",
         {"reiserfs"}, {"reiserfsprogs"}, AccessMode::Mixed, SupportProvider::KernelWithUserspace,"The kernel driver is deprecated and may be absent from newer kernels even though Debian still packages the userspace tools."},
        {"erofs", "EROFS", "Linux / Image",
         "Enhanced read-only filesystem used for compressed immutable images and embedded systems.",
         {"erofs"}, {"erofs-utils"}, AccessMode::ReadOnly, SupportProvider::KernelWithUserspace,""},
        {"minix", "Minix filesystem", "Unix / Legacy",
         "Filesystem family used by Minix and early Linux systems.",
         {"minix"}, {"util-linux"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,"Debian's util-linux supplies mkfs.minix and fsck.minix."},
        {"romfs", "ROMFS", "Linux / Embedded",
         "Small read-only filesystem intended for embedded and rescue images.",
         {"romfs"}, {"genromfs"}, AccessMode::ReadOnly, SupportProvider::KernelWithUserspace,""},
        {"cramfs", "CramFS", "Linux / Embedded",
         "Compressed read-only Linux filesystem used by older embedded systems.",
         {"cramfs"}, {"util-linux"}, AccessMode::ReadOnly, SupportProvider::KernelWithUserspace,"util-linux provides cramfs creation/checking utilities where supported."},
        {"squashfs", "SquashFS", "Linux / Image",
         "Compressed read-only filesystem widely used by live media and appliance images.",
         {"squashfs"}, {"squashfs-tools"}, AccessMode::ReadOnly, SupportProvider::KernelWithUserspace,""},
        {"bcachefs", "Bcachefs", "Linux",
         "Copy-on-write Linux filesystem integrated into newer kernels.",
         {"bcachefs"}, {}, AccessMode::Mixed, SupportProvider::Kernel,"Debian trixie stable does not ship bcachefs-tools; kernel mount support is shown when the running Debian kernel provides it."},
        {"overlayfs", "OverlayFS", "Linux / Overlay",
         "Native Linux union/overlay filesystem used heavily by containers.",
         {"overlay"}, {}, AccessMode::ReadWrite, SupportProvider::Kernel,""},
        {"ecryptfs", "eCryptfs", "Linux / Encryption",
         "Native stacked cryptographic filesystem.",
         {"ecryptfs"}, {"ecryptfs-utils"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"jffs2", "JFFS2", "Linux / Flash",
         "Journalling Flash File System v2 for raw flash memory devices.",
         {"jffs2"}, {"mtd-utils"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,"mtd-utils supplies mkfs.jffs2, readers and raw-flash administration tools."},
        {"ubifs", "UBIFS / UBI", "Linux / Flash",
         "UBI File System for raw flash managed through the Unsorted Block Images layer.",
         {"ubifs"}, {"mtd-utils"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,"mtd-utils supplies mkfs.ubifs plus the UBI attach, format, volume and inspection utilities."},
        {"zonefs", "zonefs", "Linux / Zoned Storage",
         "Filesystem exposing zones of a zoned block device as files.",
         {"zonefs"}, {"zonefs-tools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,"zonefs is intentionally simple and derives most layout information directly from the zoned device."},

        // Cluster and pooled filesystems.
        {"gfs2", "GFS2", "Cluster",
         "Red Hat Global File System 2 for concurrent access to shared cluster storage.",
         {"gfs2"}, {"gfs2-utils"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"ocfs2", "OCFS2", "Cluster",
         "Oracle Cluster File System 2 general-purpose clustered filesystem.",
         {"ocfs2"}, {"ocfs2-tools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"zfs", "OpenZFS", "Unix / Linux",
         "Pooled copy-on-write filesystem and volume manager.",
         {"zfs"}, {"zfsutils-linux", "zfs-dkms"}, AccessMode::ReadWrite, SupportProvider::Dkms,"Availability depends on the Debian repository components enabled on the machine."},
        {"zfs-fuse", "ZFS via FUSE", "Unix / FUSE",
         "Userspace implementation of ZFS provided through FUSE.",
         {}, {"zfs-fuse"}, AccessMode::Userspace, SupportProvider::Userspace,"This is a separate userspace implementation from OpenZFS and is retained as an alternative Debian-packaged path."},

        // Historical workstation, Unix and other operating-system formats.
        {"adfs", "Acorn ADFS", "Acorn / RISC OS",
         "Acorn Disc Filing System used by RISC OS and earlier Acorn systems.",
         {"adfs"}, {}, AccessMode::Mixed, SupportProvider::Kernel,"Support depends on whether the running Debian kernel was built with the ADFS driver."},
        {"affs", "Amiga OFS / FFS (AFFS)", "Amiga",
         "Classic Commodore Amiga OFS/FFS family handled by the Linux AFFS driver.",
         {"affs"}, {}, AccessMode::Mixed, SupportProvider::Kernel,"DOS0-DOS3 are read/write; DOS4-DOS5 directory-cache variants are read-only."},
        {"befs", "BeOS BeFS", "BeOS / Haiku",
         "BeOS filesystem support from the Linux BeFS driver.",
         {"befs"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel,"Linux BeFS support is primarily for reading legacy BeOS volumes."},
        {"bfs", "SCO BFS", "Unix / Legacy",
         "SCO/UnixWare Boot File System support from the Linux BFS driver.",
         {"bfs"}, {}, AccessMode::Mixed, SupportProvider::Kernel,""},
        {"efs", "SGI EFS", "SGI / Unix",
         "Silicon Graphics Extent File System used before XFS.",
         {"efs"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel,""},
        {"hpfs", "OS/2 HPFS", "IBM / OS/2",
         "High Performance File System used by IBM OS/2.",
         {"hpfs"}, {}, AccessMode::Mixed, SupportProvider::Kernel,"This is legacy kernel support; write capability depends on the kernel configuration."},
        {"qnx4", "QNX4 filesystem", "QNX",
         "QNX4 filesystem support provided by the Linux kernel.",
         {"qnx4"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel,""},
        {"qnx6", "QNX6 filesystem", "QNX",
         "QNX6 Power-Safe filesystem support provided by the Linux kernel.",
         {"qnx6"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel,""},
        {"sysv", "System V / Xenix / Coherent FS", "Unix / Legacy",
         "Linux sysv driver for several historical System V-derived filesystem variants.",
         {"sysv"}, {}, AccessMode::Mixed, SupportProvider::Kernel,""},
        {"ufs", "UFS / BSD FFS", "Unix / BSD",
         "Unix File System family used by BSD, Sun and other Unix systems.",
         {"ufs"}, {}, AccessMode::Mixed, SupportProvider::Kernel,"Linux write support is deliberately limited and depends on the exact UFS variant."},
        {"omfs", "OMFS", "Embedded / Legacy",
         "Optimized MPEG File System used by some embedded media devices.",
         {"omfs"}, {}, AccessMode::Mixed, SupportProvider::Kernel,""},

        // Microsoft and DOS family.
        {"fat", "FAT12 / FAT16 / FAT32", "Microsoft / DOS",
         "Classic DOS and Windows FAT filesystems.",
         {"vfat", "msdos"}, {"dosfstools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"exfat", "exFAT", "Microsoft",
         "Microsoft removable-media filesystem with native Linux kernel support.",
         {"exfat"}, {"exfatprogs"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"ntfs", "NTFS", "Microsoft",
         "Windows NT filesystem using Debian's mature NTFS-3G userspace implementation.",
         {}, {"ntfs-3g"}, AccessMode::Userspace, SupportProvider::Userspace,"Newer Linux kernels may also expose the in-kernel ntfs3 driver independently."},
        {"ntfs3", "NTFS (native ntfs3 driver)", "Microsoft / Kernel",
         "Native Linux read/write NTFS implementation present in newer kernels.",
         {"ntfs3"}, {}, AccessMode::ReadWrite, SupportProvider::Kernel,"Shown as available only when the running Debian kernel actually provides ntfs3."},
        {"bitlocker", "BitLocker volumes", "Microsoft / Encryption",
         "Access to BitLocker-encrypted Windows volumes through Dislocker.",
         {}, {"dislocker"}, AccessMode::Userspace, SupportProvider::Userspace,"Dislocker exposes a virtual NTFS volume which is then mounted using NTFS support."},
        {"luksde", "LUKS forensic/access utilities", "Linux / Encryption",
         "Independent userspace tools for inspecting and accessing LUKS disk-encryption volumes.",
         {}, {"libluksde-utils"}, AccessMode::ToolsOnly, SupportProvider::ToolsOnly,"This complements, rather than replaces, the normal Linux device-mapper/cryptsetup path."},

        // Apple family.
        {"hfs", "Apple HFS", "Apple / Classic Mac",
         "Classic Macintosh Hierarchical File System.",
         {"hfs"}, {"hfsprogs"}, AccessMode::Mixed, SupportProvider::KernelWithUserspace,"Debian trixie no longer carries hfsutils in stable; hfsprogs and the kernel driver provide the current packaged path."},
        {"hfsplus", "Apple HFS+", "Apple",
         "Mac OS Extended / HFS Plus filesystem.",
         {"hfsplus"}, {"hfsprogs"}, AccessMode::Mixed, SupportProvider::KernelWithUserspace,"Journaled or feature-rich volumes can restrict Linux write support."},
        {"apfs-fuse", "Apple APFS (FUSE access)", "Apple",
         "Userspace APFS access through Debian's libfsapfs implementation.",
         {}, {"libfsapfs-utils"}, AccessMode::Userspace, SupportProvider::Userspace,"This is the conservative userspace access path and includes a FUSE mount implementation."},
        {"apfs-dkms", "Apple APFS (experimental kernel driver)", "Apple / Experimental",
         "Out-of-tree APFS kernel module and tools packaged by Debian.",
         {"apfs"}, {"apfs-dkms", "apfsprogs"}, AccessMode::Experimental, SupportProvider::Dkms,"Write support is explicitly experimental; Debian's package description advises caution."},
        {"filevault", "Apple FileVault / FVDE", "Apple / Encryption / FUSE",
         "Access FileVault-encrypted macOS storage through Debian's libfvde tools.",
         {}, {"libfvde-utils"}, AccessMode::Userspace, SupportProvider::Userspace,"The package includes fvdemount for exposing decrypted container contents through FUSE."},
        {"tmfs", "Apple Time Machine filesystem", "Apple / Backup / FUSE",
         "Read Apple Time Machine backups through a reconstructed read-only FUSE namespace.",
         {}, {"tmfs"}, AccessMode::ReadOnly, SupportProvider::Userspace,"tmfs reconstructs hard-linked backup directories from HFS+ Time Machine metadata."},

        // Optical, immutable and image formats.
        {"iso9660", "ISO 9660 / Joliet / Rock Ridge", "Optical / Image",
         "CD-ROM filesystem and common extensions used by optical media and ISO images.",
         {"isofs", "iso9660"}, {}, AccessMode::ReadOnly, SupportProvider::Kernel,""},
        {"udf", "UDF", "Optical / Image",
         "Universal Disk Format used by optical media, removable disks and images.",
         {"udf"}, {"udftools"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"udfclient", "UDF userspace client", "Optical / Userspace",
         "Independent userland implementation and inspection tools for UDF.",
         {}, {"udfclient"}, AccessMode::ToolsOnly, SupportProvider::ToolsOnly,"Provides an FTP-like userland client and UDF creation/inspection utilities rather than a kernel mount."},
        {"squashfuse", "SquashFS via FUSE", "Image / FUSE",
         "Userspace SquashFS mount implementation.",
         {}, {"squashfuse"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"erofsfuse", "EROFS via FUSE", "Image / FUSE",
         "Userspace mount implementation for EROFS images.",
         {}, {"erofsfuse"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"fuse2fs", "EXT2 / EXT3 / EXT4 via FUSE", "Linux / FUSE",
         "Userspace read/write ext-family filesystem client for devices and images.",
         {}, {"fuse2fs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"exfat-fuse", "exFAT via FUSE", "Microsoft / FUSE",
         "Userspace exFAT implementation retained as an alternative to the native kernel driver.",
         {}, {"exfat-fuse"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"fusefat", "FAT family via FUSE", "Microsoft / DOS / FUSE",
         "Unprivileged FUSE access to FAT12, FAT16, FAT32 and exFAT filesystems.",
         {}, {"fusefat"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"fuseiso", "ISO images via FUSE", "Image / FUSE",
         "Userspace mounting for ISO and several single-track image formats.",
         {}, {"fuseiso"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"fusezip", "ZIP archives via FUSE", "Archive / FUSE",
         "Read/write ZIP archive filesystem.",
         {}, {"fuse-zip"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"archivemount", "ArchiveMount", "Archive / FUSE",
         "Mount many archive and compressed-file formats as a filesystem.",
         {}, {"archivemount"}, AccessMode::Userspace, SupportProvider::Userspace,"Supports numerous libarchive formats including tar, cpio, ISO and ZIP/RAR families."},
        {"avfs", "AVFS", "Archive / Remote / FUSE",
         "Virtual filesystem for archives, compressed files, disk images and remote locations.",
         {}, {"avfs"}, AccessMode::Userspace, SupportProvider::Userspace,"Debian's AVFS supports archive formats plus FTP, HTTP, WebDAV and SSH/SCP access."},
        {"guestmount", "Guestmount / libguestfs", "Virtualisation / Image / FUSE",
         "Mount filesystems contained inside virtual-machine disk images through libguestfs.",
         {}, {"guestmount"}, AccessMode::Userspace, SupportProvider::Userspace,"Useful when the filesystem is inside a guest image rather than directly exposed as a host block device."},
        {"wim", "Windows Imaging Format (WIM)", "Microsoft / Image / FUSE",
         "Mount and manipulate Windows Imaging Format archives.",
         {}, {"wimtools"}, AccessMode::Userspace, SupportProvider::Userspace,"wimtools includes both wimmount and wimmountrw as well as extraction and image-maintenance utilities."},
        {"xmount", "Forensic disk-image cross-mount", "Image / FUSE",
         "Expose supported forensic and virtual disk-image formats through a FUSE virtual filesystem.",
         {}, {"xmount"}, AccessMode::Userspace, SupportProvider::Userspace,"Useful as an image-container layer before mounting or examining the filesystem stored inside."},

        // Virtualisation and retro formats.
        {"vmfs", "VMware VMFS3 / VMFS5", "Virtualisation",
         "Userspace access to VMware VMFS filesystems.",
         {}, {"vmfs-tools"}, AccessMode::ReadOnly, SupportProvider::Userspace,"Debian's vmfs-tools provides read-only command-line and FUSE access."},
        {"vmfs6", "VMware VMFS6", "Virtualisation",
         "Userspace access to VMware VMFS6 filesystems.",
         {}, {"vmfs6-tools"}, AccessMode::ReadOnly, SupportProvider::Userspace,"Debian's VMFS6 implementation currently provides read-only access."},
        {"virtiofs", "VirtioFS", "Virtualisation",
         "High-performance shared-directory filesystem for virtual machines.",
         {"virtiofs"}, {"virtiofsd"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,"virtiofsd serves a host directory to guests; guest-side support is provided by the Linux virtiofs driver."},
        {"vmhgfs", "VMware Shared Folders (HGFS)", "Virtualisation / FUSE",
         "Mount VMware host shared folders inside a Linux guest using vmhgfs-fuse.",
         {}, {"open-vm-tools"}, AccessMode::Userspace, SupportProvider::Userspace,"Debian's open-vm-tools package contains vmhgfs-fuse."},
        {"cpm", "CP/M filesystems", "Retro",
         "Tools for reading and writing CP/M filesystem media and images.",
         {}, {"cpmtools"}, AccessMode::ToolsOnly, SupportProvider::ToolsOnly,"cpmtools supports the CP/M filesystem structures directly rather than using a Linux kernel driver."},
        {"fosfat", "Smaky filesystem (Fosfat)", "Retro / FUSE",
         "Read Smaky-formatted disks through Debian's Fosfat implementation.",
         {}, {"fosfat"}, AccessMode::ReadOnly, SupportProvider::Userspace,"Fosfat provides read-only directory/file access and a FUSE extension."},

        // Network, distributed and remote filesystems.
        {"cifs", "SMB / CIFS", "Network",
         "Windows-compatible network filesystem client.",
         {"cifs"}, {"cifs-utils"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"nfs", "NFS", "Network",
         "Network File System client support.",
         {"nfs"}, {"nfs-common"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,""},
        {"cephfs", "CephFS (kernel client)", "Distributed",
         "Native Linux client for the Ceph distributed filesystem.",
         {"ceph"}, {"ceph-common"}, AccessMode::ReadWrite, SupportProvider::KernelWithUserspace,"The native kernel client is generally preferred when available."},
        {"ceph-fuse", "CephFS via FUSE", "Distributed / FUSE",
         "Userspace Ceph filesystem client.",
         {}, {"ceph-fuse"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"glusterfs", "GlusterFS", "Distributed / FUSE",
         "Client for Gluster distributed storage volumes.",
         {}, {"glusterfs-client"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"moosefs", "MooseFS", "Distributed / FUSE",
         "Fault-tolerant scale-out distributed filesystem mounted through FUSE.",
         {}, {"moosefs-client"}, AccessMode::Userspace, SupportProvider::Userspace,"Debian's client package supplies mfsmount and the associated client tools."},
        {"openafs", "OpenAFS", "Distributed",
         "Andrew File System client with Debian-packaged DKMS kernel module.",
         {"openafs"}, {"openafs-client", "openafs-modules-dkms"}, AccessMode::ReadWrite, SupportProvider::Dkms,""},
        {"openafs-fuse", "OpenAFS via FUSE", "Distributed / FUSE / Experimental",
         "Experimental userspace OpenAFS client for systems where the kernel module is unsuitable.",
         {}, {"openafs-client", "openafs-fuse"}, AccessMode::Experimental, SupportProvider::Userspace,"Debian documents this FUSE client as read-only and not fully compatible with the normal OpenAFS client tools."},
        {"9p", "Plan 9 9P", "Network / Plan 9",
         "Linux 9P filesystem client used by Plan 9 and virtualisation environments.",
         {"9p"}, {}, AccessMode::ReadWrite, SupportProvider::Kernel,""},
        {"orangefs", "OrangeFS", "Distributed",
         "Linux kernel client support for the OrangeFS distributed filesystem.",
         {"orangefs"}, {}, AccessMode::Mixed, SupportProvider::Kernel,"Debian stable does not provide a matching OrangeFS client package; this row reports kernel support only."},
        {"sshfs", "SSHFS", "Network / FUSE",
         "Mount remote filesystems over SSH/SFTP.",
         {}, {"sshfs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"davfs2", "WebDAV", "Network",
         "Mount WebDAV resources as a filesystem.",
         {}, {"davfs2"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"curlftpfs", "FTP via FUSE", "Network / FUSE",
         "Mount FTP servers through libcurl and FUSE.",
         {}, {"curlftpfs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"httpdirfs", "HTTP directory listings via FUSE", "Network / FUSE",
         "Mount HTTP directory listings as a virtual filesystem.",
         {}, {"httpdirfs"}, AccessMode::ReadOnly, SupportProvider::Userspace,"Designed for browsable HTTP directory indexes and supports HTTP basic authentication."},
        {"s3fs", "S3 object storage via FUSE", "Cloud / FUSE",
         "Mount S3-compatible object storage through FUSE.",
         {}, {"s3fs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"s3ql", "S3QL", "Cloud / FUSE",
         "Full-featured encrypted, compressed and deduplicating filesystem backed by online object storage.",
         {}, {"s3ql"}, AccessMode::Userspace, SupportProvider::Userspace,"S3QL presents a conventional Unix filesystem over providers such as S3, Google Storage and OpenStack."},
        {"onedriver", "Microsoft OneDrive via FUSE", "Cloud / FUSE",
         "Native Linux filesystem interface for Microsoft OneDrive.",
         {}, {"onedriver"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"rclone", "Rclone remote mounts", "Cloud / Network",
         "FUSE-backed mounts for the many remote storage providers supported by rclone.",
         {}, {"rclone"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"afuse", "AFUSE automounter", "Network / FUSE",
         "FUSE automounter that can dynamically invoke filesystem clients on demand.",
         {}, {"afuse"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"smbnetfs", "SMBNetFS", "Network / FUSE",
         "Userspace filesystem exposing an SMB/NMB network beneath one mount point.",
         {}, {"smbnetfs"}, AccessMode::Userspace, SupportProvider::Userspace,"Workgroups, servers and shares can be browsed as a filesystem hierarchy."},
        {"gvfs-fuse", "GVfs FUSE bridge", "Desktop / Network / FUSE",
         "Expose GVfs mounts to applications that do not use GIO.",
         {}, {"gvfs-fuse"}, AccessMode::Userspace, SupportProvider::Userspace,"Bridges GVfs-backed FTP, SFTP, SMB, WebDAV and other desktop mounts into a FUSE namespace."},

        // Generic overlays and encrypted userspace filesystems.
        {"mergerfs", "mergerfs", "Overlay / FUSE",
         "FUSE union filesystem for pooling multiple storage locations.",
         {}, {"mergerfs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"fuse-overlayfs", "fuse-overlayfs", "Overlay / FUSE",
         "Userspace OverlayFS implementation commonly used by rootless containers.",
         {}, {"fuse-overlayfs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"bindfs", "bindfs", "Overlay / FUSE",
         "Mirror a directory through FUSE while changing permission and ownership presentation.",
         {}, {"bindfs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"posixovl", "POSIX Overlay for FAT/NTFS", "Overlay / FUSE",
         "Add POSIX permissions, ownership and symbolic-link semantics over non-POSIX filesystems.",
         {}, {"fuse-posixovl"}, AccessMode::Userspace, SupportProvider::Userspace,"The underlying FAT, VFAT or NTFS filesystem remains unmodified; POSIX metadata is stored separately."},
        {"convmvfs", "ConvmvFS charset overlay", "Overlay / FUSE",
         "Mirror a filesystem tree while translating filename character sets on the fly.",
         {}, {"fuse-convmvfs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"disorderfs", "disorderfs", "Overlay / FUSE / Testing",
         "Overlay filesystem that deliberately introduces filesystem metadata non-determinism.",
         {}, {"disorderfs"}, AccessMode::Userspace, SupportProvider::Userspace,"Primarily useful for reproducible-build testing rather than storage compatibility."},
        {"loggedfs", "LoggedFS", "Overlay / FUSE / Diagnostics",
         "Transparent logging filesystem that records operations beneath a mounted directory.",
         {}, {"loggedfs"}, AccessMode::Userspace, SupportProvider::Userspace,"Primarily a diagnostic filesystem, but it is a genuine Debian-packaged FUSE implementation."},
        {"encfs", "EncFS", "Encryption / FUSE",
         "Encrypted virtual filesystem storing encrypted files in an ordinary backing directory.",
         {}, {"encfs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"gocryptfs", "gocryptfs", "Encryption / FUSE",
         "Modern encrypted overlay filesystem built on FUSE.",
         {}, {"gocryptfs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"cryfs", "CryFS", "Encryption / FUSE",
         "Encrypted cloud-oriented filesystem implemented with FUSE.",
         {}, {"cryfs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"securefs", "securefs", "Encryption / FUSE",
         "Userspace filesystem providing transparent authenticated encryption over a backing directory.",
         {}, {"securefs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"fscrypt", "Linux native filesystem encryption (fscrypt)", "Linux / Encryption",
         "Management tooling for native per-directory encryption in supported Linux filesystems.",
         {}, {"fscrypt"}, AccessMode::ToolsOnly, SupportProvider::ToolsOnly,"Debian documents ext4, F2FS and UBIFS as supported filesystem backends."},

        // Device-backed userspace filesystems.
        {"ifuse", "Apple iPhone / iPod via iFuse", "Device / Apple / FUSE",
         "Expose Apple AFC-accessible device storage through a FUSE filesystem.",
         {}, {"ifuse"}, AccessMode::Experimental, SupportProvider::Userspace,"Debian describes iFuse as working but still experimental."},
        {"gphotofs", "Digital cameras via GPhotoFS", "Device / Camera / FUSE",
         "Expose cameras supported by libgphoto2 as a filesystem, including PTP-only devices.",
         {}, {"gphotofs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"jmtpfs", "Android / MTP via jmtpfs", "Device / MTP / FUSE",
         "FUSE filesystem for Media Transfer Protocol devices such as many Android phones.",
         {}, {"jmtpfs"}, AccessMode::Userspace, SupportProvider::Userspace,""},
        {"go-mtpfs", "Android / MTP via go-mtpfs", "Device / MTP / FUSE",
         "Alternative FUSE filesystem for Media Transfer Protocol devices.",
         {}, {"go-mtpfs"}, AccessMode::Userspace, SupportProvider::Userspace,""},

        // Container-specific virtual filesystems.
        {"lxcfs", "LXCFS", "Container / FUSE",
         "FUSE filesystem providing cgroup-aware /proc-style views to Linux containers.",
         {}, {"lxcfs"}, AccessMode::Userspace, SupportProvider::Userspace,"This is container virtual-filesystem support rather than an on-disk storage format."}
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

const char* support_provider_label(const SupportProvider provider)
{
    switch (provider) {
    case SupportProvider::Kernel:
        return "Kernel";
    case SupportProvider::KernelWithUserspace:
        return "Kernel + userspace";
    case SupportProvider::Dkms:
        return "DKMS driver";
    case SupportProvider::Userspace:
        return "Userspace / FUSE";
    case SupportProvider::ToolsOnly:
        return "Tools only";
    }
    return "Unknown";
}

bool package_is_catalogued(const std::string_view package)
{
    if (package.empty()) {
        return false;
    }

    for (const auto& entry : catalog()) {
        for (const auto candidate : entry.packages) {
            if (candidate == package) {
                return true;
            }
        }
    }

    return false;
}

bool module_is_catalogued(const std::string_view module)
{
    if (module.empty()) {
        return false;
    }

    for (const auto& entry : catalog()) {
        for (const auto candidate : entry.modules) {
            if (candidate == module) {
                return true;
            }
        }
    }

    return false;
}

std::vector<std::string_view> catalogue_entries_using_package(
    const std::string_view package)
{
    std::vector<std::string_view> entries;

    for (const auto& entry : catalog()) {
        for (const auto candidate : entry.packages) {
            if (candidate == package) {
                entries.push_back(entry.id);
                break;
            }
        }
    }

    return entries;
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
