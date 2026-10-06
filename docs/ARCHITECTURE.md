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

## Coupling boundaries

Filesystem identity, platform support policy and deployment state are different responsibilities.

`filesystem_identities()` is the cross-platform identity contract. It exposes only stable format information: ID, name, family, description and note. Windows, EFI, setup and other non-Linux consumers must use this identity-only contract and must not depend on Linux package names, kernel module names, provider classes or `project_native_linux` state.

The current Linux manager retains the richer `catalog()` support-policy view because it needs Debian packages, Linux modules, provider/access state and project-native deployment information. That richer type is Linux policy, not the portable filesystem identity API. Until the underlying static data table is physically separated, the identity view is materialised from that table once; this storage detail must not leak back across the platform boundary.

A change to Debian packaging or Linux module selection must therefore not require a Windows/EFI/setup consumer change unless the actual filesystem identity changed.

The repository also keeps mutable filesystem-set knowledge out of CI YAML. `data/native-filesystems.tsv` is the authority for project-native deployment, `data/source-filesystems.txt` is the authority for in-repository format trees, and `data/retired-source-identities.txt` contains historical source identities that must remain absent. CI consumes those manifests rather than carrying independent handwritten copies.

## Core layers

Filesystem Support is split into backend responsibilities plus presentation shells.

1. **Filesystem identity contract** owns stable on-media format identity exposed through `filesystem_identities()`.
2. **Linux catalogue and capability policy** owns current Debian provider requirements, kernel-module aliases, limitations and qualification state for the Linux manager.
3. **Probe** performs read-only inspection of installed packages, configured APT repositories, registered filesystems, built-in kernel drivers, loaded modules and loadable modules.
4. **Installer/action backend** is the only layer allowed to request privilege escalation. Package and module names originate only from trusted compiled Linux capability data.
5. **Filesystem implementation boundary** owns canonical format semantics and the platform adapters needed to expose them to Linux, Windows and the boot environment.
6. **Presentation shells** consume the appropriate boundary. The desktop GTK shell manages Linux support on a running system. Windows consumes filesystem identity plus Windows implementation state. The setup shell must consume identity and explicit setup qualification rather than Linux desktop provider state.

The baseline Linux distribution is Debian stable. Filesystem Support must not silently add repositories, enable repository components or cross from stable into testing/unstable/experimental.

## Desktop support state

For the current Linux desktop manager:

**Installed** means the declared current Linux provider and its required packages are present.

**Available** means required packages are missing but resolvable from repositories already configured by the user.

**Unavailable** means the declared provider cannot currently be supplied.

Kernel detection checks `/proc/filesystems` for registered/built-in support, `/sys/module` for loaded modules and `modinfo` for loadable modules. This avoids equating one provider-discovery mechanism with filesystem identity.

## Provider model

A filesystem may currently be implemented by a built-in/loadable kernel driver, a kernel driver plus userspace tools, a DKMS driver, a userspace/FUSE provider, or tools-only access while a native adapter is unfinished.

Those provider classes describe the **current Linux access path**, not separate filesystem choices. Provider migration must leave the filesystem identity stable.

Where multiple providers exist, the Linux capability model should select or rank them behind the single format identity. Creating duplicate catalogue identities for alternate providers is prohibited.

For conventional local disk filesystems, the intended destination is one canonical filesystem engine with native platform adapters. External providers remain references or temporary access paths until project-native support is qualified.

## Amiga desktop integration boundary

The main package owns the shared GNOME Disks and optional Nemo launcher diversions. They are prepared during package-level `sync` and remain inert when no per-filesystem desktop plugin is active.

An OFS/FFS/SFS/SFS2/PFS3 install or remove operation owns only that filesystem's active desktop plugin and udev rule. Individual filesystem transactions must never create, remove or roll back the shared GNOME Disks/Nemo diversions. Shared launchers are removed only by package lifecycle cleanup after all active per-filesystem state has been removed.

Each per-filesystem mutation runs in its own snapshot/rollback scope. A failed OFS transaction may restore OFS state only; it must not alter FFS, SFS, SFS2 or PFS3 state.

## InfiltratorOS setup path

The setup shell uses the same filesystem identity contract but exposes a different workflow from the desktop manager.

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
