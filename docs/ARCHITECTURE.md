# Architecture

Filesystem Support is the shared filesystem-capability backend for both the normal InfiltratorOS desktop manager and the InfiltratorOS installation shell.

## Scope boundary

The catalogue contains one entry per **real persistent on-media local-storage filesystem format**. It is intentionally not a catalogue of everything Linux can mount.

A catalogue identity answers: **what filesystem is this storage formatted as?** It does not answer which Linux implementation happens to provide access today.

Therefore:

- NTFS is one entry. NTFS-3G and NTFS3 are providers for NTFS.
- APFS is one entry. FUSE and DKMS implementations are providers for APFS.
- ZFS is one entry. A FUSE implementation is not another filesystem identity.
- FAT12/FAT16/FAT32 are represented as the FAT format family rather than separate provider rows.

The product catalogue excludes network/cloud mounts, remote filesystem protocols, overlays/union mounts, archive namespaces, pseudo/device filesystems, encrypted container layers that expose some other filesystem, provider duplicates, and utilities that do not correspond to a local on-media filesystem format.

Shared-disk filesystems such as GFS2 and OCFS2 remain in scope because they define a real persistent disk format even though normal deployment may coordinate multiple hosts. UDF remains in scope because it can be a writable block-device filesystem. Immutable image-only formats are not InfiltratorOS target-root choices.

## Core layers

Filesystem Support is split into four backend layers plus presentation shells.

1. **Catalogue and capability model** owns filesystem identity, current provider requirements, kernel-module aliases, limitations and qualification state. The identity is the format; provider details are subordinate metadata.
2. **Probe** performs read-only inspection of installed packages, configured APT repositories, registered filesystems, built-in kernel drivers, loaded modules and loadable modules.
3. **Installer/action backend** is the only layer allowed to request privilege escalation. Package and module names originate only from the compiled catalogue/capability data.
4. **Filesystem implementation boundary** owns canonical format semantics and the platform adapters needed to expose them to Linux, Windows and the boot environment.
5. **Presentation shells** consume the backend. The desktop GTK shell manages support on a running system. The setup shell presents installation-time filesystem choices and must not maintain a second independent catalogue.

The baseline distribution is Debian stable. Filesystem Support must not silently add repositories, enable repository components or cross from stable into testing/unstable/experimental.

## Desktop support state

For the current desktop manager:

**Installed** means the declared current Linux provider and its required packages are present.

**Available** means required packages are missing but resolvable from repositories already configured by the user.

**Unavailable** means the declared provider cannot currently be supplied.

Kernel detection checks `/proc/filesystems` for registered/built-in support, `/sys/module` for loaded modules and `modinfo` for loadable modules. This avoids equating one provider-discovery mechanism with filesystem identity.

## Provider model

A filesystem may currently be implemented by a built-in/loadable kernel driver, a kernel driver plus userspace tools, a DKMS driver, a userspace/FUSE provider, or tools-only access while a native adapter is unfinished.

Those provider classes describe the **current access path**, not separate filesystem choices. Provider migration must leave the filesystem identity stable.

Where multiple providers exist, the long-term capability model should select or rank them behind the single format entry. Creating duplicate catalogue rows for alternate providers is prohibited.

For conventional local disk filesystems, the intended destination is one canonical filesystem engine with native platform adapters. External providers remain references or temporary access paths until project-native support is qualified.

## InfiltratorOS setup path

The setup shell uses the same backend but exposes a different workflow from the desktop manager.

A filesystem may be offered as an enabled InfiltratorOS installation target only when the complete setup path for that format is qualified:

```text
selected target device
        |
        v
format using selected filesystem
        |
        v
populate InfiltratorOS tree
        |
        +--> install Linux VFS support
        |    +--> place early-boot support in initramfs when required
        |
        +--> install matching rEFInd/UEFI filesystem reader
        |
        +--> create/configure boot entry
        v
boot qualification
```

Catalogue membership and root-install qualification are deliberately separate. A real disk format may remain visible in the desktop manager while its setup option is disabled because write support, formatting, POSIX/root semantics or EFI reading is incomplete.

The installer must never silently fall back to a different filesystem. If the user chooses OFS, the target is OFS. If OFS cannot satisfy the complete installation contract, OFS is disabled as a setup choice until it can.

## Boot/runtime division

The boot-time and runtime adapters solve different platform problems but must interpret the same filesystem format.

```text
UEFI/rEFInd -> EFI filesystem reader -> kernel + initramfs
                                      |
                                      v
Linux kernel -> Linux VFS -> filesystem adapter/module -> root volume
```

A Linux `.ko` cannot be used as a UEFI filesystem driver. The EFI reader is a separate adapter. Where practical both adapters should consume the same canonical format/parsing logic rather than independently reimplementing on-disk semantics.

For InfiltratorFS the Linux runtime adapter is `infiltratorfs.ko`. For OFS it is `ofs.ko`; for FFS it is `ffs.ko`; the same pattern applies to every project-native local filesystem.

## Privilege boundary

The desktop GUI, probes and package-removal simulation run unprivileged. Actual APT/module/formatting/device-write actions are delegated through explicitly privileged helpers or PolicyKit boundaries appropriate to the operation.

No search text, label or free-form user input may become an APT package name, module name, formatter command or target device without validation. Filesystem provider arguments come from trusted compiled capability data; target devices come from the setup/storage selection contract.

Package removal is simulated first. The backend refuses removal if Debian would remove unrelated catalogue-independent or protected packages and never invokes automatic `apt autoremove`.

## Shared code

Infiltratr Common is pinned as a git submodule. Filesystem Support consumes appropriate project identity/search/design infrastructure from Common, but the userspace Common library is never linked into kernel modules.

The canonical filesystem engine boundary and native source rules are defined in [`NATIVE_CODE_ARCHITECTURE.md`](NATIVE_CODE_ARCHITECTURE.md). The long-term product and bootable-root vision is defined in [`NATIVE_FILESYSTEM_VISION.md`](NATIVE_FILESYSTEM_VISION.md).
