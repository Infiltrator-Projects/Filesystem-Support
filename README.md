# Filesystem Support

Filesystem Support is the InfiltratorOS filesystem capability and integration manager. It owns the catalogue of real persistent local-storage filesystem formats that InfiltratorOS can recognise, install support for and, as each format is qualified, use as the filesystem beneath an InfiltratorOS installation.

The baseline Linux distribution is **Debian stable**. As of September 2026 that is Debian 13 "trixie". Derivatives can benefit where they retain Debian-compatible package names and APT behaviour, but the catalogue is authored and qualified against Debian first.

## Product scope

The catalogue contains **41 real on-media filesystem formats**. A catalogue row is a filesystem format identity, not a Linux provider or a generic mount mechanism.

In scope are persistent local-storage formats such as InfiltratorFS, EXT2/3/4, XFS, Btrfs, FAT, exFAT, NTFS, HFS/HFS+, APFS, UDF, Amiga OFS/FFS/SFS/SFS2/PFS3, historical workstation formats and other genuine disk filesystems.

Out of scope are network and cloud mounts, distributed client protocols that do not define the selected local disk format, overlay/union filesystems, archive mounts, device namespaces, virtual filesystems, encryption/container layers that merely expose another filesystem, tools that are not a filesystem format, and duplicate provider identities such as `ntfs3`, `zfs-fuse`, `apfs-fuse` or `apfs-dkms`.

Provider choice is subordinate to filesystem identity. NTFS is one filesystem whether the current Linux access path is NTFS-3G, NTFS3 or a future Infiltrator-owned adapter. APFS is one filesystem whether accessed through a FUSE reference provider or a kernel driver. The user chooses a format, not an implementation brand.

The complete enumerated contract is maintained in [`docs/FILESYSTEM_SUPPORT_MATRIX.md`](docs/FILESYSTEM_SUPPORT_MATRIX.md). CI verifies that every compiled catalogue ID appears there exactly once.

## Two product surfaces, one backend

Filesystem Support has two different presentation contexts that consume the same filesystem definitions and capability state.

The normal **desktop manager** discovers support already present on the running system, installs or removes the current provider for a filesystem, loads or unloads eligible kernel modules, reports limitations and integrates supported formats with the desktop storage stack.

The **InfiltratorOS setup shell** is deliberately a different interface. During operating-system installation it asks which qualified filesystem should be used for the target storage and then drives the installation transaction through Filesystem Support's backend.

The intended setup flow is:

```text
choose target filesystem
        |
        v
format the target storage in that format
        |
        v
install/copy InfiltratorOS
        |
        +--> install the matching Linux VFS support
        |    and include it in the initramfs when required
        |
        +--> install the matching rEFInd/UEFI filesystem reader
        |
        +--> configure the boot entry
        v
qualified bootable InfiltratorOS installation
```

The corresponding boot path is:

```text
UEFI
  -> rEFInd
  -> selected filesystem's EFI reader
  -> kernel + initramfs stored on that filesystem
  -> Linux starts
  -> selected filesystem's Linux VFS adapter/module
  -> same filesystem mounts as /
  -> InfiltratorOS
```

For example, an OFS installation ultimately means an OFS-formatted target, an OFS EFI reader for rEFInd and `ofs.ko` for Linux. An InfiltratorFS installation means the corresponding InfiltratorFS EFI reader and `infiltratorfs.ko`.

Catalogue membership does **not** by itself claim that a format is already safe to offer in the setup shell. A format becomes an enabled installation choice only after the formatter, Linux runtime support, initramfs/root semantics and EFI reader are qualified together. Read-only or incomplete current providers may therefore remain visible in the desktop catalogue while their setup choice is disabled.

## Native implementation strategy

The long-term native implementation strategy is defined in [`docs/NATIVE_FILESYSTEM_VISION.md`](docs/NATIVE_FILESYSTEM_VISION.md). The goal is one canonical implementation of each filesystem's on-media semantics with thin operating-system adapters rather than independent Linux and Windows rewrites.

The source/dependency boundaries are defined in [`docs/NATIVE_CODE_ARCHITECTURE.md`](docs/NATIVE_CODE_ARCHITECTURE.md). Conventional local filesystems target independently deployable native adapters such as Linux VFS modules, without requiring a custom Linux kernel.

## Runtime support model

For the desktop manager a filesystem reports **Installed** when its declared current Linux provider and required packages are present. **Available** means missing packages can be resolved from repositories already configured by the user. **Unavailable** means the declared provider cannot currently be supplied.

Kernel detection checks registered/built-in filesystem support, loaded modules and loadable modules rather than assuming that `modinfo` alone describes support.

The current provider is an implementation detail. As project-native engines replace external providers, the catalogue identity remains stable.

## Safety and package policy

- The GUI itself remains unprivileged.
- Read-only probing uses the local kernel, `dpkg-query` and `apt-cache`.
- Install requests are delegated to PolicyKit and APT.
- Package names come only from the built-in catalogue.
- The application does not add repositories, enable Debian components, or silently pull packages from testing/unstable/experimental.
- A package missing from configured repositories is reported as unavailable rather than pretending it can be installed.
- Package removal is simulated first and is blocked if Debian would remove an unrelated or protected system package.
- The application never performs automatic `apt autoremove`.
- Loadable kernel modules may be loaded/unloaded; built-in drivers are never presented as removable.
- Experimental implementations are labelled explicitly.

## Shared library pin

Filesystem Support is pinned to Infiltratr Common **1.19.38**, commit `7070c5812b50821fd7580101cb2289a3184f6b2c`.

## Design

The desktop application separates filesystem identity/catalogue data, read-only support probing, the privileged installation backend and the GTK shell. The setup shell must reuse those backend contracts rather than cloning filesystem rules into a second installer-specific database.

The Linux desktop shell follows the InfiltratorOS control-centre visual language used by System Settings: a branded application header with integrated search, persistent platform/status navigation, a dashboard home page with live support metrics and quick actions, and card-based catalogue views.

## Amiga desktop integration

The main Debian package carries inactive desktop templates for each native Amiga filesystem. **Install native** installs that filesystem's own `.ko`, its scoped udev identification rule and its single-filesystem Disks naming file together. **Remove native** removes that same set and refreshes connected devices. OFS, FFS, SFS, SFS2 and PFS3 remain independent; installing one cannot activate another.

Desktop mounts of OFS, FFS, SFS and SFS2 pass the requesting user's UID and primary GID through their own `setuid`/`setgid` mount options. PFS3 remains read-only until its mutation path is qualified.

GNOME Disks loads only the independently installed naming files for drivers present on the running kernel. The stock UDisks library and original Disks executable retain their distribution ownership and updates.

The retired supplemental desktop package is removed automatically by the migration path introduced in 0.5.7. No additional Infiltrator desktop Debian package is built or distributed.

## Releases

The native `.run` contains the same qualified main Debian package and installs it through APT. A successful release publishes:

- `Filesystem-Support-<version>-amd64.deb`
- `Filesystem-Support-<version>-amd64.run`
- `RELEASE_SHA256SUMS.txt`
- GitHub source ZIP and TAR.GZ archives for the release tag.

## Licence

GPL-3.0-or-later.
