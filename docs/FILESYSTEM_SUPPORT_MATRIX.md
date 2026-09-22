# Filesystem Support Target Matrix

This document is the human-readable support contract for **Filesystem Support**.

It mirrors the 104 entries currently compiled into `src/catalog.cpp`. Every row is something the project intentionally aims to detect and represent. Where a Debian package is listed, the application may offer installation when that package is available from the user's configured repositories. Where only a kernel module is listed, support depends on the running Debian kernel.

This matrix deliberately distinguishes native kernel filesystems, FUSE/userspace filesystems, tools-only formats, read-only implementations and experimental support. “Target” therefore means **correctly detect and expose the available Debian/Linux support path**, not “promise unrestricted read/write mounting for every format.”

**Baseline:** Debian stable (Debian 13 “trixie” at the time this matrix was created)  
**Catalogue entries:** 104  
**Code source of truth:** `src/catalog.cpp`  
**Documentation invariant:** CI verifies that every catalogue ID occurs exactly once in this matrix.

| # | ID | Name | Family | Intended capability | Kernel module(s) | Debian package(s) | Purpose / limitation |
|---:|---|---|---|---|---|---|---|
| 1 | `ext` | EXT2 / EXT3 / EXT4 | Linux | Read / write | `ext4` | `e2fsprogs` | Linux extended filesystem family. The ext4 driver handles the modern family; e2fsprogs supplies creation, checking and administration tools. |
| 2 | `xfs` | SGI XFS | Unix / Linux | Read / write | `xfs` | `xfsprogs` | High-performance journaling filesystem originally developed by Silicon Graphics. |
| 3 | `btrfs` | Btrfs | Linux | Read / write | `btrfs` | `btrfs-progs` | Copy-on-write Linux filesystem with checksums, snapshots and subvolumes. |
| 4 | `f2fs` | F2FS | Linux | Read / write | `f2fs` | `f2fs-tools` | Flash-Friendly File System designed for NAND-backed storage. |
| 5 | `jfs` | IBM JFS | Unix / Linux | Read / write | `jfs` | `jfsutils` | IBM journaling filesystem supported by the Linux JFS driver. |
| 6 | `nilfs2` | NILFS2 | Linux | Read / write | `nilfs2` | `nilfs-tools` | Log-structured filesystem with continuous snapshotting. |
| 7 | `reiserfs` | ReiserFS | Linux / Legacy | Variant / kernel dependent | `reiserfs` | `reiserfsprogs` | Legacy journaling filesystem with Debian userspace administration tools. The kernel driver is deprecated and may be absent from newer kernels even though Debian still packages the userspace tools. |
| 8 | `erofs` | EROFS | Linux / Image | Read-only | `erofs` | `erofs-utils` | Enhanced read-only filesystem used for compressed immutable images and embedded systems. |
| 9 | `minix` | Minix filesystem | Unix / Legacy | Read / write | `minix` | `util-linux` | Filesystem family used by Minix and early Linux systems. Debian's util-linux supplies mkfs.minix and fsck.minix. |
| 10 | `romfs` | ROMFS | Linux / Embedded | Read-only | `romfs` | `genromfs` | Small read-only filesystem intended for embedded and rescue images. |
| 11 | `cramfs` | CramFS | Linux / Embedded | Read-only | `cramfs` | `util-linux` | Compressed read-only Linux filesystem used by older embedded systems. util-linux provides cramfs creation/checking utilities where supported. |
| 12 | `squashfs` | SquashFS | Linux / Image | Read-only | `squashfs` | `squashfs-tools` | Compressed read-only filesystem widely used by live media and appliance images. |
| 13 | `bcachefs` | Bcachefs | Linux | Variant / kernel dependent | `bcachefs` | — | Copy-on-write Linux filesystem integrated into newer kernels. Debian trixie stable does not ship bcachefs-tools; kernel mount support is shown when the running Debian kernel provides it. |
| 14 | `overlayfs` | OverlayFS | Linux / Overlay | Read / write | `overlay` | — | Native Linux union/overlay filesystem used heavily by containers. |
| 15 | `ecryptfs` | eCryptfs | Linux / Encryption | Read / write | `ecryptfs` | `ecryptfs-utils` | Native stacked cryptographic filesystem. |
| 16 | `jffs2` | JFFS2 | Linux / Flash | Read / write | `jffs2` | `mtd-utils` | Journalling Flash File System v2 for raw flash memory devices. mtd-utils supplies mkfs.jffs2, readers and raw-flash administration tools. |
| 17 | `ubifs` | UBIFS / UBI | Linux / Flash | Read / write | `ubifs` | `mtd-utils` | UBI File System for raw flash managed through the Unsorted Block Images layer. mtd-utils supplies mkfs.ubifs plus the UBI attach, format, volume and inspection utilities. |
| 18 | `zonefs` | zonefs | Linux / Zoned Storage | Read / write | `zonefs` | `zonefs-tools` | Filesystem exposing zones of a zoned block device as files. zonefs is intentionally simple and derives most layout information directly from the zoned device. |
| 19 | `gfs2` | GFS2 | Cluster | Read / write | `gfs2` | `gfs2-utils` | Red Hat Global File System 2 for concurrent access to shared cluster storage. |
| 20 | `ocfs2` | OCFS2 | Cluster | Read / write | `ocfs2` | `ocfs2-tools` | Oracle Cluster File System 2 general-purpose clustered filesystem. |
| 21 | `zfs` | OpenZFS | Unix / Linux | Read / write | `zfs` | `zfsutils-linux` | Pooled copy-on-write filesystem and volume manager. Availability depends on the Debian repository components enabled on the machine. |
| 22 | `zfs-fuse` | ZFS via FUSE | Unix / FUSE | Userspace / FUSE | — | `zfs-fuse` | Userspace implementation of ZFS provided through FUSE. This is a separate userspace implementation from OpenZFS and is retained as an alternative Debian-packaged path. |
| 23 | `adfs` | Acorn ADFS | Acorn / RISC OS | Variant / kernel dependent | `adfs` | — | Acorn Disc Filing System used by RISC OS and earlier Acorn systems. Support depends on whether the running Debian kernel was built with the ADFS driver. |
| 24 | `affs` | Amiga OFS / FFS (AFFS) | Amiga | Variant / kernel dependent | `affs` | — | Classic Commodore Amiga OFS/FFS family handled by the Linux AFFS driver. DOS0-DOS3 are read/write; DOS4-DOS5 directory-cache variants are read-only. |
| 25 | `befs` | BeOS BeFS | BeOS / Haiku | Read-only | `befs` | — | BeOS filesystem support from the Linux BeFS driver. Linux BeFS support is primarily for reading legacy BeOS volumes. |
| 26 | `bfs` | SCO BFS | Unix / Legacy | Variant / kernel dependent | `bfs` | — | SCO/UnixWare Boot File System support from the Linux BFS driver. |
| 27 | `efs` | SGI EFS | SGI / Unix | Read-only | `efs` | — | Silicon Graphics Extent File System used before XFS. |
| 28 | `hpfs` | OS/2 HPFS | IBM / OS/2 | Variant / kernel dependent | `hpfs` | — | High Performance File System used by IBM OS/2. This is legacy kernel support; write capability depends on the kernel configuration. |
| 29 | `qnx4` | QNX4 filesystem | QNX | Read-only | `qnx4` | — | QNX4 filesystem support provided by the Linux kernel. |
| 30 | `qnx6` | QNX6 filesystem | QNX | Read-only | `qnx6` | — | QNX6 Power-Safe filesystem support provided by the Linux kernel. |
| 31 | `sysv` | System V / Xenix / Coherent FS | Unix / Legacy | Variant / kernel dependent | `sysv` | — | Linux sysv driver for several historical System V-derived filesystem variants. |
| 32 | `ufs` | UFS / BSD FFS | Unix / BSD | Variant / kernel dependent | `ufs` | — | Unix File System family used by BSD, Sun and other Unix systems. Linux write support is deliberately limited and depends on the exact UFS variant. |
| 33 | `omfs` | OMFS | Embedded / Legacy | Variant / kernel dependent | `omfs` | — | Optimized MPEG File System used by some embedded media devices. |
| 34 | `fat` | FAT12 / FAT16 / FAT32 | Microsoft / DOS | Read / write | `vfat`, `msdos` | `dosfstools` | Classic DOS and Windows FAT filesystems. |
| 35 | `exfat` | exFAT | Microsoft | Read / write | `exfat` | `exfatprogs` | Microsoft removable-media filesystem with native Linux kernel support. |
| 36 | `ntfs` | NTFS | Microsoft | Userspace / FUSE | — | `ntfs-3g` | Windows NT filesystem using Debian's mature NTFS-3G userspace implementation. Newer Linux kernels may also expose the in-kernel ntfs3 driver independently. |
| 37 | `ntfs3` | NTFS (native ntfs3 driver) | Microsoft / Kernel | Read / write | `ntfs3` | — | Native Linux read/write NTFS implementation present in newer kernels. Shown as available only when the running Debian kernel actually provides ntfs3. |
| 38 | `bitlocker` | BitLocker volumes | Microsoft / Encryption | Userspace / FUSE | — | `dislocker` | Access to BitLocker-encrypted Windows volumes through Dislocker. Dislocker exposes a virtual NTFS volume which is then mounted using NTFS support. |
| 39 | `luksde` | LUKS forensic/access utilities | Linux / Encryption | Userspace tools | — | `libluksde-utils` | Independent userspace tools for inspecting and accessing LUKS disk-encryption volumes. This complements, rather than replaces, the normal Linux device-mapper/cryptsetup path. |
| 40 | `hfs` | Apple HFS | Apple / Classic Mac | Variant / kernel dependent | `hfs` | `hfsprogs` | Classic Macintosh Hierarchical File System. Debian trixie no longer carries hfsutils in stable; hfsprogs and the kernel driver provide the current packaged path. |
| 41 | `hfsplus` | Apple HFS+ | Apple | Variant / kernel dependent | `hfsplus` | `hfsprogs` | Mac OS Extended / HFS Plus filesystem. Journaled or feature-rich volumes can restrict Linux write support. |
| 42 | `apfs-fuse` | Apple APFS (FUSE access) | Apple | Userspace / FUSE | — | `libfsapfs-utils` | Userspace APFS access through Debian's libfsapfs implementation. This is the conservative userspace access path and includes a FUSE mount implementation. |
| 43 | `apfs-dkms` | Apple APFS (experimental kernel driver) | Apple / Experimental | Experimental | `apfs` | `apfs-dkms`, `apfsprogs` | Out-of-tree APFS kernel module and tools packaged by Debian. Write support is explicitly experimental; Debian's package description advises caution. |
| 44 | `filevault` | Apple FileVault / FVDE | Apple / Encryption / FUSE | Userspace / FUSE | — | `libfvde-utils` | Access FileVault-encrypted macOS storage through Debian's libfvde tools. The package includes fvdemount for exposing decrypted container contents through FUSE. |
| 45 | `tmfs` | Apple Time Machine filesystem | Apple / Backup / FUSE | Read-only | — | `tmfs` | Read Apple Time Machine backups through a reconstructed read-only FUSE namespace. tmfs reconstructs hard-linked backup directories from HFS+ Time Machine metadata. |
| 46 | `iso9660` | ISO 9660 / Joliet / Rock Ridge | Optical / Image | Read-only | `isofs`, `iso9660` | — | CD-ROM filesystem and common extensions used by optical media and ISO images. |
| 47 | `udf` | UDF | Optical / Image | Read / write | `udf` | `udftools` | Universal Disk Format used by optical media, removable disks and images. |
| 48 | `udfclient` | UDF userspace client | Optical / Userspace | Userspace tools | — | `udfclient` | Independent userland implementation and inspection tools for UDF. Provides an FTP-like userland client and UDF creation/inspection utilities rather than a kernel mount. |
| 49 | `squashfuse` | SquashFS via FUSE | Image / FUSE | Userspace / FUSE | — | `squashfuse` | Userspace SquashFS mount implementation. |
| 50 | `erofsfuse` | EROFS via FUSE | Image / FUSE | Userspace / FUSE | — | `erofsfuse` | Userspace mount implementation for EROFS images. |
| 51 | `fuse2fs` | EXT2 / EXT3 / EXT4 via FUSE | Linux / FUSE | Userspace / FUSE | — | `fuse2fs` | Userspace read/write ext-family filesystem client for devices and images. |
| 52 | `exfat-fuse` | exFAT via FUSE | Microsoft / FUSE | Userspace / FUSE | — | `exfat-fuse` | Userspace exFAT implementation retained as an alternative to the native kernel driver. |
| 53 | `fusefat` | FAT family via FUSE | Microsoft / DOS / FUSE | Userspace / FUSE | — | `fusefat` | Unprivileged FUSE access to FAT12, FAT16, FAT32 and exFAT filesystems. |
| 54 | `fuseiso` | ISO images via FUSE | Image / FUSE | Userspace / FUSE | — | `fuseiso` | Userspace mounting for ISO and several single-track image formats. |
| 55 | `fusezip` | ZIP archives via FUSE | Archive / FUSE | Userspace / FUSE | — | `fuse-zip` | Read/write ZIP archive filesystem. |
| 56 | `archivemount` | ArchiveMount | Archive / FUSE | Userspace / FUSE | — | `archivemount` | Mount many archive and compressed-file formats as a filesystem. Supports numerous libarchive formats including tar, cpio, ISO and ZIP/RAR families. |
| 57 | `avfs` | AVFS | Archive / Remote / FUSE | Userspace / FUSE | — | `avfs` | Virtual filesystem for archives, compressed files, disk images and remote locations. Debian's AVFS supports archive formats plus FTP, HTTP, WebDAV and SSH/SCP access. |
| 58 | `guestmount` | Guestmount / libguestfs | Virtualisation / Image / FUSE | Userspace / FUSE | — | `guestmount` | Mount filesystems contained inside virtual-machine disk images through libguestfs. Useful when the filesystem is inside a guest image rather than directly exposed as a host block device. |
| 59 | `wim` | Windows Imaging Format (WIM) | Microsoft / Image / FUSE | Userspace / FUSE | — | `wimtools` | Mount and manipulate Windows Imaging Format archives. wimtools includes both wimmount and wimmountrw as well as extraction and image-maintenance utilities. |
| 60 | `xmount` | Forensic disk-image cross-mount | Image / FUSE | Userspace / FUSE | — | `xmount` | Expose supported forensic and virtual disk-image formats through a FUSE virtual filesystem. Useful as an image-container layer before mounting or examining the filesystem stored inside. |
| 61 | `vmfs` | VMware VMFS3 / VMFS5 | Virtualisation | Read-only | — | `vmfs-tools` | Userspace access to VMware VMFS filesystems. Debian's vmfs-tools provides read-only command-line and FUSE access. |
| 62 | `vmfs6` | VMware VMFS6 | Virtualisation | Read-only | — | `vmfs6-tools` | Userspace access to VMware VMFS6 filesystems. Debian's VMFS6 implementation currently provides read-only access. |
| 63 | `virtiofs` | VirtioFS | Virtualisation | Read / write | `virtiofs` | `virtiofsd` | High-performance shared-directory filesystem for virtual machines. virtiofsd serves a host directory to guests; guest-side support is provided by the Linux virtiofs driver. |
| 64 | `vmhgfs` | VMware Shared Folders (HGFS) | Virtualisation / FUSE | Userspace / FUSE | — | `open-vm-tools` | Mount VMware host shared folders inside a Linux guest using vmhgfs-fuse. Debian's open-vm-tools package contains vmhgfs-fuse. |
| 65 | `cpm` | CP/M filesystems | Retro | Userspace tools | — | `cpmtools` | Tools for reading and writing CP/M filesystem media and images. cpmtools supports the CP/M filesystem structures directly rather than using a Linux kernel driver. |
| 66 | `fosfat` | Smaky filesystem (Fosfat) | Retro / FUSE | Read-only | — | `fosfat` | Read Smaky-formatted disks through Debian's Fosfat implementation. Fosfat provides read-only directory/file access and a FUSE extension. |
| 67 | `cifs` | SMB / CIFS | Network | Read / write | `cifs` | `cifs-utils` | Windows-compatible network filesystem client. |
| 68 | `nfs` | NFS | Network | Read / write | `nfs` | `nfs-common` | Network File System client support. |
| 69 | `cephfs` | CephFS (kernel client) | Distributed | Read / write | `ceph` | `ceph-common` | Native Linux client for the Ceph distributed filesystem. The native kernel client is generally preferred when available. |
| 70 | `ceph-fuse` | CephFS via FUSE | Distributed / FUSE | Userspace / FUSE | — | `ceph-fuse` | Userspace Ceph filesystem client. |
| 71 | `glusterfs` | GlusterFS | Distributed / FUSE | Userspace / FUSE | — | `glusterfs-client` | Client for Gluster distributed storage volumes. |
| 72 | `moosefs` | MooseFS | Distributed / FUSE | Userspace / FUSE | — | `moosefs-client` | Fault-tolerant scale-out distributed filesystem mounted through FUSE. Debian's client package supplies mfsmount and the associated client tools. |
| 73 | `openafs` | OpenAFS | Distributed | Read / write | `openafs` | `openafs-client`, `openafs-modules-dkms` | Andrew File System client with Debian-packaged DKMS kernel module. |
| 74 | `openafs-fuse` | OpenAFS via FUSE | Distributed / FUSE / Experimental | Experimental | — | `openafs-client`, `openafs-fuse` | Experimental userspace OpenAFS client for systems where the kernel module is unsuitable. Debian documents this FUSE client as read-only and not fully compatible with the normal OpenAFS client tools. |
| 75 | `9p` | Plan 9 9P | Network / Plan 9 | Read / write | `9p` | — | Linux 9P filesystem client used by Plan 9 and virtualisation environments. |
| 76 | `orangefs` | OrangeFS | Distributed | Variant / kernel dependent | `orangefs` | — | Linux kernel client support for the OrangeFS distributed filesystem. Debian stable does not provide a matching OrangeFS client package; this row reports kernel support only. |
| 77 | `sshfs` | SSHFS | Network / FUSE | Userspace / FUSE | — | `sshfs` | Mount remote filesystems over SSH/SFTP. |
| 78 | `davfs2` | WebDAV | Network | Userspace / FUSE | — | `davfs2` | Mount WebDAV resources as a filesystem. |
| 79 | `curlftpfs` | FTP via FUSE | Network / FUSE | Userspace / FUSE | — | `curlftpfs` | Mount FTP servers through libcurl and FUSE. |
| 80 | `httpdirfs` | HTTP directory listings via FUSE | Network / FUSE | Read-only | — | `httpdirfs` | Mount HTTP directory listings as a virtual filesystem. Designed for browsable HTTP directory indexes and supports HTTP basic authentication. |
| 81 | `s3fs` | S3 object storage via FUSE | Cloud / FUSE | Userspace / FUSE | — | `s3fs` | Mount S3-compatible object storage through FUSE. |
| 82 | `s3ql` | S3QL | Cloud / FUSE | Userspace / FUSE | — | `s3ql` | Full-featured encrypted, compressed and deduplicating filesystem backed by online object storage. S3QL presents a conventional Unix filesystem over providers such as S3, Google Storage and OpenStack. |
| 83 | `onedriver` | Microsoft OneDrive via FUSE | Cloud / FUSE | Userspace / FUSE | — | `onedriver` | Native Linux filesystem interface for Microsoft OneDrive. |
| 84 | `rclone` | Rclone remote mounts | Cloud / Network | Userspace / FUSE | — | `rclone` | FUSE-backed mounts for the many remote storage providers supported by rclone. |
| 85 | `afuse` | AFUSE automounter | Network / FUSE | Userspace / FUSE | — | `afuse` | FUSE automounter that can dynamically invoke filesystem clients on demand. |
| 86 | `smbnetfs` | SMBNetFS | Network / FUSE | Userspace / FUSE | — | `smbnetfs` | Userspace filesystem exposing an SMB/NMB network beneath one mount point. Workgroups, servers and shares can be browsed as a filesystem hierarchy. |
| 87 | `gvfs-fuse` | GVfs FUSE bridge | Desktop / Network / FUSE | Userspace / FUSE | — | `gvfs-fuse` | Expose GVfs mounts to applications that do not use GIO. Bridges GVfs-backed FTP, SFTP, SMB, WebDAV and other desktop mounts into a FUSE namespace. |
| 88 | `mergerfs` | mergerfs | Overlay / FUSE | Userspace / FUSE | — | `mergerfs` | FUSE union filesystem for pooling multiple storage locations. |
| 89 | `fuse-overlayfs` | fuse-overlayfs | Overlay / FUSE | Userspace / FUSE | — | `fuse-overlayfs` | Userspace OverlayFS implementation commonly used by rootless containers. |
| 90 | `bindfs` | bindfs | Overlay / FUSE | Userspace / FUSE | — | `bindfs` | Mirror a directory through FUSE while changing permission and ownership presentation. |
| 91 | `posixovl` | POSIX Overlay for FAT/NTFS | Overlay / FUSE | Userspace / FUSE | — | `fuse-posixovl` | Add POSIX permissions, ownership and symbolic-link semantics over non-POSIX filesystems. The underlying FAT, VFAT or NTFS filesystem remains unmodified; POSIX metadata is stored separately. |
| 92 | `convmvfs` | ConvmvFS charset overlay | Overlay / FUSE | Userspace / FUSE | — | `fuse-convmvfs` | Mirror a filesystem tree while translating filename character sets on the fly. |
| 93 | `disorderfs` | disorderfs | Overlay / FUSE / Testing | Userspace / FUSE | — | `disorderfs` | Overlay filesystem that deliberately introduces filesystem metadata non-determinism. Primarily useful for reproducible-build testing rather than storage compatibility. |
| 94 | `loggedfs` | LoggedFS | Overlay / FUSE / Diagnostics | Userspace / FUSE | — | `loggedfs` | Transparent logging filesystem that records operations beneath a mounted directory. Primarily a diagnostic filesystem, but it is a genuine Debian-packaged FUSE implementation. |
| 95 | `encfs` | EncFS | Encryption / FUSE | Userspace / FUSE | — | `encfs` | Encrypted virtual filesystem storing encrypted files in an ordinary backing directory. |
| 96 | `gocryptfs` | gocryptfs | Encryption / FUSE | Userspace / FUSE | — | `gocryptfs` | Modern encrypted overlay filesystem built on FUSE. |
| 97 | `cryfs` | CryFS | Encryption / FUSE | Userspace / FUSE | — | `cryfs` | Encrypted cloud-oriented filesystem implemented with FUSE. |
| 98 | `securefs` | securefs | Encryption / FUSE | Userspace / FUSE | — | `securefs` | Userspace filesystem providing transparent authenticated encryption over a backing directory. |
| 99 | `fscrypt` | Linux native filesystem encryption (fscrypt) | Linux / Encryption | Userspace tools | — | `fscrypt` | Management tooling for native per-directory encryption in supported Linux filesystems. Debian documents ext4, F2FS and UBIFS as supported filesystem backends. |
| 100 | `ifuse` | Apple iPhone / iPod via iFuse | Device / Apple / FUSE | Experimental | — | `ifuse` | Expose Apple AFC-accessible device storage through a FUSE filesystem. Debian describes iFuse as working but still experimental. |
| 101 | `gphotofs` | Digital cameras via GPhotoFS | Device / Camera / FUSE | Userspace / FUSE | — | `gphotofs` | Expose cameras supported by libgphoto2 as a filesystem, including PTP-only devices. |
| 102 | `jmtpfs` | Android / MTP via jmtpfs | Device / MTP / FUSE | Userspace / FUSE | — | `jmtpfs` | FUSE filesystem for Media Transfer Protocol devices such as many Android phones. |
| 103 | `go-mtpfs` | Android / MTP via go-mtpfs | Device / MTP / FUSE | Userspace / FUSE | — | `go-mtpfs` | Alternative FUSE filesystem for Media Transfer Protocol devices. |
| 104 | `lxcfs` | LXCFS | Container / FUSE | Userspace / FUSE | — | `lxcfs` | FUSE filesystem providing cgroup-aware /proc-style views to Linux containers. This is container virtual-filesystem support rather than an on-disk storage format. |

## Meaning of the capability column

- **Read / write** — the intended Debian/Linux path supports normal read/write operation when the required driver/tools and the filesystem itself permit it.
- **Read-only** — the target is intentionally represented as read-only.
- **Variant / kernel dependent** — support varies by on-disk variant, kernel configuration or filesystem feature set.
- **Userspace / FUSE** — access is provided primarily through a userspace filesystem rather than a native kernel filesystem driver.
- **Userspace tools** — Debian provides direct inspection/manipulation tools, but not a normal mount path represented by this entry.
- **Experimental** — support exists but is explicitly experimental and must be presented as such.

## Project rule

When support is added, removed or materially changed in `src/catalog.cpp`, this matrix must be updated in the same commit. CI treats a missing or duplicated catalogue ID in this document as a failure. This prevents the implemented catalogue and the declared project target from silently drifting apart.
