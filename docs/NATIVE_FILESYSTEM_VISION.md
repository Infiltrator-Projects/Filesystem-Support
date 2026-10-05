# Native Filesystem Vision and Long-Term Architecture

## Purpose

Filesystem Support exists so InfiltratorOS is not permanently tied to one root filesystem. A user installing InfiltratorOS should ultimately be able to choose among qualified real disk filesystems, have the target formatted accordingly, and receive all runtime and boot support required for that choice.

The project therefore owns two related responsibilities:

1. provide and manage filesystem support on a running InfiltratorOS system; and
2. provide the filesystem capability backend used by the InfiltratorOS setup shell when selecting, formatting and boot-enabling the installation filesystem.

The catalogue is deliberately limited to **persistent local on-media filesystem formats**. It is not a general registry of every mountable namespace.

## One format identity, one canonical implementation

For every real filesystem, Filesystem Support aims to own one canonical implementation of the filesystem's semantics and expose that implementation through platform adapters.

```text
                         canonical filesystem engine
                         /          |          \
                        /           |           \
               Linux VFS        Windows IFS     EFI/rEFInd reader
                 adapter            adapter          adapter
```

On-disk structures, allocation, extent mapping, directory rules, checksums, validation, recovery and format-specific semantics belong to the canonical engine. Linux VFS, Windows IFS and UEFI adapters own the contract with their host environment; they do not fork the meaning of the disk format.

Some kernel environments will require a narrowly shared or mechanically portable subset rather than literally linking the same binary objects. The architectural rule remains that format logic has one authoritative implementation and test corpus.

## Format identity is not provider identity

The user selects a filesystem format, not a provider.

NTFS is NTFS whether today's Linux access path is NTFS-3G, NTFS3 or a future project-native implementation. APFS remains APFS whether an interim provider is FUSE or DKMS. ZFS remains one filesystem when accessed through different operating-system adapters.

Provider-specific rows such as `ntfs3`, `zfs-fuse`, `apfs-fuse`, `apfs-dkms`, `exfat-fuse`, `fusefat` and `udfclient` must not appear as independent filesystem choices. Provider selection belongs inside the capability record for the single format identity.

## What is in scope

A catalogue entry must denote a persistent filesystem format that can live on local storage and has a meaningful path toward normal file/directory access. This includes modern Linux filesystems, DOS/Microsoft formats, Apple formats, Amiga and other historical workstation formats, shared-disk formats, and real legacy disk formats.

A format may remain in the catalogue while current Linux support is read-only or tools-only. That records a real format whose implementation still needs work; it does not make the format immediately eligible as an InfiltratorOS root.

## What is not in scope

The Filesystem Support catalogue does not own generic network/cloud mounts, remote client protocols, overlay/union namespaces, archive mounts, device namespaces, virtual pseudo-filesystems, encryption containers that merely reveal another filesystem, or implementation/provider duplicates.

Examples deliberately outside the catalogue include NFS/SMB/SSHFS/CephFS client mounts, S3/rclone cloud mounts, OverlayFS/mergerfs/bindfs, archive FUSE tools, BitLocker/LUKS/FileVault container layers, MTP/camera/device namespaces, VirtioFS/HGFS shared-directory transports, and LXCFS.

Those technologies may belong to other InfiltratorOS components in future, but they are not choices for "what filesystem should this installation disk be formatted as?" and therefore do not belong in Filesystem Support's filesystem catalogue.

Immutable image formats such as ISO9660, SquashFS, EROFS, ROMFS and CramFS are likewise outside the selectable installation-root catalogue. They can be useful deployment/image technologies, but they are not the writable target filesystem underneath a normal installed InfiltratorOS system.

## The setup contract

The setup shell is a separate presentation layer over the same Filesystem Support backend. It should look and behave like an operating-system installer, not like the desktop Filesystem Support application.

For each selected filesystem, installation is one coordinated transaction:

```text
select filesystem
      |
      v
partition/prepare target
      |
      v
format target in selected filesystem
      |
      v
install InfiltratorOS files
      |
      +--> install selected filesystem's Linux runtime support
      |      +--> initramfs inclusion when required
      |
      +--> install selected filesystem's rEFInd/EFI reader
      |
      +--> write boot configuration
      v
boot and root-mount qualification
```

The installer does not substitute EXT4, InfiltratorFS or another format when the requested filesystem is not ready. Instead the requested format remains visibly unavailable until its complete installation contract is qualified.

## The boot contract

A bootable installation has two distinct filesystem adapters.

Before Linux exists, rEFInd/UEFI needs an EFI filesystem reader capable of locating and reading the kernel and initramfs from the selected filesystem.

After Linux starts, Linux VFS needs the filesystem's runtime adapter/module so the same volume can become `/`.

```text
UEFI
  -> rEFInd
  -> filesystem EFI reader
  -> kernel + initramfs
  -> Linux
  -> filesystem VFS adapter
  -> mount selected filesystem as /
  -> InfiltratorOS
```

For InfiltratorFS the runtime adapter is `infiltratorfs.ko`. For Amiga OFS it is `ofs.ko`; FFS uses `ffs.ko`; SFS uses `sfs.ko`, and so on. The matching EFI reader is separate UEFI code and cannot be replaced by a Linux `.ko`.

The custom rEFInd work is therefore part of the complete installation architecture: Filesystem Support supplies the selected-format capability data and installation plan, while the boot manager supplies/loads the corresponding EFI reader.

## Root-filesystem qualification

Catalogue membership is not root qualification. A format becomes selectable in setup only when all required gates pass for the target platform:

1. destructive formatter/create-volume path is implemented and qualified;
2. normal read/write filesystem operation is sufficient for an operating-system root;
3. namespace, ownership, permissions, links, special files and other required Linux semantics are native or safely represented;
4. early-boot driver/module availability is deterministic, including initramfs packaging where required;
5. the EFI/rEFInd reader can locate and read the boot payload from the format;
6. installation, reboot, root mount, update and recovery tests pass on a real or equivalent qualified target;
7. failure is fail-closed: no silent format substitution or unqualified write path.

A format whose current provider is read-only can still be a legitimate long-term catalogue target. It simply remains disabled in the setup shell until the missing gates are complete.

## Native Linux integration

For conventional local filesystems the intended Linux endpoint is a normal native VFS adapter, preferably an independently loadable out-of-tree module where that is technically appropriate. Filesystem Support must not require users to patch or replace the stock Debian kernel simply to gain a filesystem.

Linux kernel interfaces evolve, so out-of-tree adapters must be rebuilt and qualified for supported kernels. Filesystem Support should manage installed/running kernel detection, headers, builds, qualification, module signing, installation and `depmod` as a coherent lifecycle.

Secure Boot/module signature enforcement is a normal deployment case, not an exceptional manual procedure.

## Native Windows integration

The same format identity and canonical semantics should feed the Windows adapter. Windows receives a normal filesystem-driver integration through the supported WDK/IFS path rather than a separate reimplementation of the format.

The Windows lifecycle remains: build, warnings/static analysis, driver/package validation, signing, installation, load-state verification, diagnostics and clean update/removal.

Linux and Windows may reach qualification at different times while still sharing one canonical filesystem definition.

## EFI/rEFInd integration

The EFI reader has a deliberately smaller job than a full operating-system filesystem driver. Its required surface is the read-only subset needed by the boot manager: identify the volume, traverse the boot path, open files, read file data and expose the required metadata safely.

It must nonetheless use the same validated on-media rules as the canonical engine. Boot-time parsing is security-sensitive and must reject corrupt or unsupported structures rather than guessing.

The EFI adapter is not installed merely because a format exists in the desktop catalogue. It is installed/configured as part of the setup transaction for the filesystem actually selected for that InfiltratorOS installation.

## Native implementation maturity

Implementation state is tracked separately from catalogue identity. A useful progression is:

1. format documented/reference provider only;
2. canonical parser and validation under development;
3. native read-only adapter experimental;
4. native read-only adapter validated;
5. write path experimental;
6. read/write durability and recovery validated;
7. root-filesystem semantics validated;
8. EFI reader validated;
9. setup/install/reboot qualification passed;
10. preferred production provider.

The setup shell must gate on the appropriate later states rather than inferring readiness from "the volume mounts."

## Testing requirements

Native filesystem qualification should cover, where meaningful, empty/minimal/maximal valid structures, nested directories and difficult names, boundary sizes, sparse and fragmented files, alternate block/sector sizes, metadata/checksum variants, dirty/unclean states, corruption rejection, crash recovery, full and near-full media, concurrency, mmap/page-cache behaviour, ownership and permission semantics, formatter/fsck interoperability, and destructive write/recovery tests.

Root qualification adds installation and boot tests: formatter -> OS population -> initramfs/driver availability -> EFI read -> reboot -> root mount -> ordinary desktop/server workload -> update/reboot -> recovery.

## Non-negotiable design principles

- One filesystem identity per on-media format.
- One canonical semantic implementation per filesystem.
- Thin host adapters instead of duplicated format logic.
- Independent modules rather than one giant kernel binary.
- No custom Linux kernel requirement where an out-of-tree adapter is practical.
- Normal Windows signing/Secure Boot trust paths.
- No provider-specific catalogue duplicates.
- No network/cloud/overlay/container scope creep into the installation-filesystem catalogue.
- No claim of setup/root support until the complete boot and runtime path is qualified.
- No silent fallback to a different filesystem than the one the user selected.

The concrete source/dependency rules are defined in [`NATIVE_CODE_ARCHITECTURE.md`](NATIVE_CODE_ARCHITECTURE.md), and the current format/provider contract is enumerated in [`FILESYSTEM_SUPPORT_MATRIX.md`](FILESYSTEM_SUPPORT_MATRIX.md).
