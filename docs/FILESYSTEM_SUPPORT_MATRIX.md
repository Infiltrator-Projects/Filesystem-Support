# Filesystem Support Target Matrix

This document is the human-readable support contract for **Filesystem Support**.

The catalogue is intentionally **format-centric** and contains **41 persistent local-storage filesystem formats**. Every row is a real on-media filesystem identity that the project intentionally aims to recognise and support. Provider names, network/cloud mounts, overlays, archive namespaces, encryption containers, device namespaces and pseudo-filesystems are not separate catalogue entries.

**Baseline:** Debian stable (Debian 13 “trixie” when this matrix was updated)  
**Catalogue entries:** 41  
**Code source of truth:** `src/catalog.cpp`  
**Documentation invariant:** CI verifies that every compiled catalogue ID has exactly one unique `support-<id>` row anchor in this matrix.

The **Current Linux path** column describes today's access provider. It is not the filesystem identity. A provider may change without creating a new catalogue row.

The desktop manager may expose formats whose current provider is read-only or tools-only. The InfiltratorOS setup shell must enable a format only after formatter, root-filesystem semantics, early-boot Linux support and the matching rEFInd/EFI reader are all qualified.

| # | ID | Name | Family | Current capability | Current Linux path | Kernel module(s) | Debian/Infiltrator package(s) | Purpose / limitation |
|---:|---|---|---|---|---|---|---|---|
| 1 | <a id="support-infiltratorfs"></a>`infiltratorfs` | InfiltratorFS | InfiltratorOS | Read / write | DKMS driver | `infiltratorfs` | `infiltratorfs` | First-class InfiltratorOS root-filesystem target. Linux runtime driver is the native InfiltratorFS VFS/DKMS module; matching EFI reader is a separate boot component. |
| 2 | <a id="support-ext2"></a>`ext2` | EXT2 | Linux | Read / write | Project-native kernel | `ext2` | — | Independent project-native EXT2 implementation. |
| 3 | <a id="support-ext3"></a>`ext3` | EXT3 | Linux | Read / write | Project-native kernel | `ext3` | — | Independent project-native EXT3 implementation; not an EXT2 fallback. |
| 4 | <a id="support-ext4"></a>`ext4` | EXT4 | Linux | Read / write | Project-native kernel | `ext4` | — | Independent project-native EXT4 implementation. |
| 5 | <a id="support-xfs"></a>`xfs` | SGI XFS | Unix / Linux | Read / write | Kernel + userspace | `xfs` | `xfsprogs` | High-performance journaling local disk filesystem. |
| 6 | <a id="support-btrfs"></a>`btrfs` | Btrfs | Linux | Read / write | Kernel + userspace | `btrfs` | `btrfs-progs` | Copy-on-write local disk filesystem with checksums, snapshots and subvolumes. |
| 7 | <a id="support-f2fs"></a>`f2fs` | F2FS | Linux | Read / write | Kernel + userspace | `f2fs` | `f2fs-tools` | Block-addressed flash/SSD filesystem. |
| 8 | <a id="support-jfs"></a>`jfs` | IBM JFS | Unix / Linux | Read / write | Kernel + userspace | `jfs` | `jfsutils` | IBM journaling disk filesystem. |
| 9 | <a id="support-nilfs2"></a>`nilfs2` | NILFS2 | Linux | Read / write | Kernel + userspace | `nilfs2` | `nilfs-tools` | Log-structured local filesystem with continuous snapshots. |
| 10 | <a id="support-reiserfs"></a>`reiserfs` | ReiserFS | Linux / Legacy | Variant / kernel dependent | Kernel + userspace | `reiserfs` | `reiserfsprogs` | Real disk format retained for compatibility; current Linux driver is deprecated and may be absent. |
| 11 | <a id="support-minix"></a>`minix` | Minix filesystem | Unix / Legacy | Read / write | Kernel + userspace | `minix` | `util-linux` | Minix/early-Linux disk filesystem; util-linux supplies formatter/checker tooling. |
| 12 | <a id="support-bcachefs"></a>`bcachefs` | Bcachefs | Linux | Variant / kernel dependent | Kernel | `bcachefs` | — | Real local-storage format; Debian trixie lacks bcachefs-tools, so formatter/setup qualification remains pending. |
| 13 | <a id="support-gfs2"></a>`gfs2` | GFS2 | Shared-disk / Linux | Read / write | Kernel + userspace | `gfs2` | `gfs2-utils` | Shared-disk clustered filesystem with a real persistent disk format. |
| 14 | <a id="support-ocfs2"></a>`ocfs2` | OCFS2 | Shared-disk / Linux | Read / write | Kernel + userspace | `ocfs2` | `ocfs2-tools` | Shared-disk clustered filesystem with a real persistent disk format. |
| 15 | <a id="support-zfs"></a>`zfs` | OpenZFS | Unix / Linux | Read / write | DKMS driver | `zfs` | `zfsutils-linux`, `zfs-dkms` | ZFS is one format identity; `zfs-fuse` is only an alternate provider and is not separately catalogued. |
| 16 | <a id="support-adfs"></a>`adfs` | Acorn ADFS | Acorn / RISC OS | Variant / kernel dependent | Kernel | `adfs` | — | Acorn disk format; current Linux availability depends on kernel configuration. |
| 17 | <a id="support-ofs"></a>`ofs` | Amiga OFS | Amiga | Variant / kernel dependent | Project-native kernel | `ofs` | — | Independent Infiltrator OFS module; intended long-term InfiltratorOS root target once complete boot/root semantics are qualified. |
| 18 | <a id="support-ffs"></a>`ffs` | Amiga FFS | Amiga | Variant / kernel dependent | Project-native kernel | `ffs` | — | Independent Infiltrator FFS module. |
| 19 | <a id="support-sfs"></a>`sfs` | Amiga SFS | Amiga | Read / write | Project-native kernel | `sfs` | — | Independent Infiltrator SFS module implementing the SFS disk format. |
| 20 | <a id="support-sfs2"></a>`sfs2` | Amiga SFS2 | Amiga | Read / write | Project-native kernel | `sfs2` | — | Independent Infiltrator SFS2 module. |
| 21 | <a id="support-pfs3"></a>`pfs3` | Amiga PFS3 | Amiga | Read-only | Project-native kernel | `pfs3` | — | Real disk format retained while project-native mutation support remains unqualified. |
| 22 | <a id="support-befs"></a>`befs` | BeOS BeFS | BeOS / Haiku | Read-only | Kernel | `befs` | — | Current Linux provider is read-oriented; format itself is a real local disk filesystem. |
| 23 | <a id="support-bfs"></a>`bfs` | SCO BFS | Unix / Legacy | Variant / kernel dependent | Kernel | `bfs` | — | SCO/UnixWare Boot File System. |
| 24 | <a id="support-efs"></a>`efs` | SGI EFS | SGI / Unix | Read-only | Kernel | `efs` | — | SGI Extent File System disk format used before XFS. |
| 25 | <a id="support-hpfs"></a>`hpfs` | OS/2 HPFS | IBM / OS/2 | Variant / kernel dependent | Kernel | `hpfs` | — | OS/2 High Performance File System; current write capability is kernel-dependent. |
| 26 | <a id="support-qnx4"></a>`qnx4` | QNX4 filesystem | QNX | Read-only | Kernel | `qnx4` | — | QNX4 local disk format. |
| 27 | <a id="support-qnx6"></a>`qnx6` | QNX6 filesystem | QNX | Read-only | Kernel | `qnx6` | — | QNX6 Power-Safe local disk format. |
| 28 | <a id="support-sysv"></a>`sysv` | System V / Xenix / Coherent FS | Unix / Legacy | Variant / kernel dependent | Kernel | `sysv` | — | Historical System V-derived disk format family. |
| 29 | <a id="support-ufs"></a>`ufs` | UFS / BSD FFS | Unix / BSD | Variant / kernel dependent | Kernel | `ufs` | — | UFS family used by BSD, Sun and other Unix systems; current Linux write support varies by variant. |
| 30 | <a id="support-omfs"></a>`omfs` | OMFS | Embedded / Legacy | Variant / kernel dependent | Kernel | `omfs` | — | Optimized MPEG File System disk format used by some embedded media devices. |
| 31 | <a id="support-fat"></a>`fat` | FAT12 / FAT16 / FAT32 | Microsoft / DOS | Read / write | Kernel + userspace | `vfat`, `msdos` | `dosfstools` | One FAT format-family identity. `fusefat` is a provider, not another filesystem choice. |
| 32 | <a id="support-exfat"></a>`exfat` | exFAT | Microsoft | Read / write | Kernel + userspace | `exfat` | `exfatprogs` | One exFAT identity. `exfat-fuse` is an alternate provider, not another catalogue row. |
| 33 | <a id="support-ntfs"></a>`ntfs` | NTFS | Microsoft | Userspace / FUSE | Userspace / FUSE | — | `ntfs-3g` | One NTFS identity. NTFS-3G is the current conservative Debian provider; `ntfs3` is not a second filesystem. |
| 34 | <a id="support-hfs"></a>`hfs` | Apple HFS | Apple / Classic Mac | Variant / kernel dependent | Kernel + userspace | `hfs` | `hfsprogs` | Classic Macintosh HFS disk format. |
| 35 | <a id="support-hfsplus"></a>`hfsplus` | Apple HFS+ | Apple | Variant / kernel dependent | Kernel + userspace | `hfsplus` | `hfsprogs` | Mac OS Extended disk format; current Linux write support depends on volume features. |
| 36 | <a id="support-apfs"></a>`apfs` | Apple APFS | Apple | Experimental | DKMS driver | `apfs` | `apfs-dkms`, `apfsprogs` | One APFS format identity. FUSE/DKMS provider names are implementation details; current Debian kernel provider is experimental. |
| 37 | <a id="support-udf"></a>`udf` | UDF | Optical / Local storage | Read / write | Kernel + userspace | `udf` | `udftools` | Persistent writable on-media format usable on block devices; retained as a real filesystem rather than an image helper. |
| 38 | <a id="support-vmfs"></a>`vmfs` | VMware VMFS3 / VMFS5 | Virtualisation / Disk | Read-only | Userspace / FUSE | — | `vmfs-tools` | Real persistent disk filesystem; current Debian access is read-only. |
| 39 | <a id="support-vmfs6"></a>`vmfs6` | VMware VMFS6 | Virtualisation / Disk | Read-only | Userspace / FUSE | — | `vmfs6-tools` | Real persistent disk filesystem; current Debian access is read-only. |
| 40 | <a id="support-cpm"></a>`cpm` | CP/M filesystems | Retro | Userspace tools | Tools only | — | `cpmtools` | Real disk-format family; current support is tools-only while a native/root-capable adapter remains future work. |
| 41 | <a id="support-fosfat"></a>`fosfat` | Smaky filesystem (Fosfat) | Retro | Read-only | Userspace / FUSE | — | `fosfat` | Real Smaky disk format; current Debian access is read-only userspace. |

## Deliberately removed catalogue identities

The previous 109-entry catalogue had drifted into being a registry of mount mechanisms rather than a list of selectable disk formats. The following classes are now outside the catalogue contract:

- network/remote/cloud clients such as NFS, SMB/CIFS, CephFS, GlusterFS, SSHFS, WebDAV, S3 and rclone mounts;
- provider duplicates such as `zfs-fuse`, `ntfs3`, `apfs-fuse`, `apfs-dkms`, `exfat-fuse`, `fusefat`, `squashfuse`, `erofsfuse`, `fuseiso` and `udfclient`;
- overlays and presentation namespaces such as OverlayFS, mergerfs, bindfs, fuse-overlayfs and charset/logging/testing overlays;
- encryption/container layers such as eCryptfs, BitLocker/Dislocker, LUKS access tools, FileVault/FVDE, EncFS, gocryptfs, CryFS and fscrypt tooling;
- archives/images and helper namespaces such as ArchiveMount, AVFS, Guestmount, WIM, xmount and ZIP FUSE mounts;
- virtual/device namespaces such as VirtioFS, HGFS, MTP/iFuse/camera mounts and LXCFS;
- immutable installation-image formats such as ISO9660, SquashFS, EROFS, ROMFS and CramFS, which are useful image technologies but not the writable filesystem underneath a normal installed InfiltratorOS system;
- raw-flash-specific JFFS2/UBIFS and zonefs, because this catalogue is the selectable local disk/block-storage filesystem set rather than every possible Linux storage namespace.

Removing an entry from this matrix does not deny that the technology exists or can be useful. It means only that it is not a valid answer to the setup question: **“What persistent filesystem should this InfiltratorOS target storage be formatted as?”**

## Setup qualification

A row is enabled in the InfiltratorOS setup shell only when its complete target-install path is qualified: create/format, populate, Linux root semantics, early-boot runtime driver, matching rEFInd/EFI reader, reboot/root mount, update and recovery. Current desktop capability in the table must never be mistaken for that stronger guarantee.
