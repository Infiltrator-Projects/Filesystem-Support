# Filesystems

Filesystems is the InfiltratorOS filesystem capability and integration manager. It owns the catalogue of real persistent local-storage filesystem formats that InfiltratorOS can recognise, install support for and, as each format is qualified, use as the filesystem beneath an InfiltratorOS installation.

The application and repository are named **Filesystems**. **Boot** starts the
selected operating system, and **Setup** is the name reserved for the future
installer. **InfiltratorFS** remains a distinct filesystem format in the catalogue.

The authoritative source/release version is stored in [`VERSION`](VERSION). Published releases use the corresponding `v<version>` tag only after that exact source revision and its release artefacts have completed qualification.

The baseline Linux distribution is **Debian stable**. As of September 2026 that is Debian 13 "trixie". Derivatives can benefit where they retain Debian-compatible package names and APT behaviour, but the catalogue is authored and qualified against Debian first.

## Product scope

The catalogue contains **41 real on-media filesystem formats**. A catalogue row is a filesystem format identity, not a Linux provider or a generic mount mechanism.

In scope are persistent local-storage formats such as InfiltratorFS, EXT2/3/4, XFS, Btrfs, FAT, exFAT, NTFS, HFS/HFS+, APFS, UDF, Amiga OFS/FFS/SFS/SFS2/PFS3, historical workstation formats and other genuine disk filesystems.

Out of scope are network and cloud mounts, distributed client protocols that do not define the selected local disk format, overlay/union filesystems, archive mounts, device namespaces, virtual filesystems, encryption/container layers that merely expose another filesystem, tools that are not a filesystem format, and duplicate provider identities such as `ntfs3`, `zfs-fuse`, `apfs-fuse` or `apfs-dkms`.

Provider choice is subordinate to filesystem identity. NTFS is one filesystem whether the current Linux access path is NTFS-3G, NTFS3 or a future Infiltrator-owned adapter. APFS is one filesystem whether accessed through a FUSE reference provider or a kernel driver. The user chooses a format, not an implementation brand.

The complete enumerated contract is maintained in [`docs/FILESYSTEMS_MATRIX.md`](docs/FILESYSTEMS_MATRIX.md). CI verifies that every compiled catalogue ID appears there exactly once.

## Two product surfaces, one identity contract

Filesystems has different presentation contexts that consume the same stable filesystem identities while keeping platform-specific support policy separate.

The normal **desktop manager** combines the identity contract with Linux support policy: it discovers support already present on the running system, installs or removes the current provider for a filesystem, loads or unloads eligible kernel modules, reports limitations and integrates supported formats with the desktop storage stack.

The planned **Setup** installer has its own interface. During operating-system installation it will ask which qualified filesystem should be used for the target storage and drive the installation transaction through the Filesystems setup capability contract. Windows, EFI and setup consumers use the identity-only API rather than inheriting Debian package or Linux module policy.

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
        v
boot the installed system
```

The setup surface exposes only filesystems whose complete install path is qualified. It does not infer support merely because a catalogue entry exists or because a Linux package can be installed.

## Support discovery

Filesystems treats support as a capability that must be observed rather than assumed.

The Linux manager scans the running system for:

- kernel-registered filesystems;
- loaded and available kernel modules;
- installed Debian packages;
- repository package availability;
- project-owned native module state;
- desktop identification/integration state.

Probe failures are explicit. A failed package, repository, module or kernel inventory is not converted into a normal "missing" result and does not enable an action based on incomplete state.

## Installation and removal

Install and remove operations are scoped to the selected filesystem. Project-native filesystems deploy their own kernel driver and their own desktop-identification material. Package-backed filesystems use the selected provider package without turning provider package names into new filesystem identities.

Destructive package removal is plan-bound: the exact package set shown for confirmation is simulated again immediately before execution and must still match. If package state changes underneath the confirmation, the operation fails closed and requires a new confirmation.

Privileged executable selection is fixed to trusted system/application locations rather than inherited from the caller's `PATH`, and privileged operations use structured argument vectors rather than shell command strings.

## Project-native Linux filesystems

The project currently owns native Linux implementations for:

- EXT2
- EXT3
- EXT4
- Amiga OFS
- Amiga FFS
- SFS
- SFS2
- PFS3

Each native filesystem is an independent deployment unit. Installing or removing one must not silently install, remove, unload or mutate another filesystem's driver.

Kernel-module replacement is transactional. If replacement fails, rollback restores both the previous persistent module image and the previous runtime selection when that module had been loaded.

## Desktop integration

Desktop identification is a separate transaction from kernel-driver deployment. It publishes filesystem identity to udev/UDisks and desktop tools without making desktop metadata authoritative over kernel support.

A successfully committed native driver is not rolled back merely because a later desktop-integration refresh fails; that partial success is reported explicitly so the failing layer can be repaired independently.

## Windows

The Windows tree contains the canonical project EXT2 driver build path and packaging/validation material. Windows support is maintained as a separate platform surface rather than by importing Debian/Linux package policy.

## Build

A normal Linux build uses CMake/Ninja:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Release qualification additionally exercises project-native filesystem builds, sanitizers/adversarial parser tests, runtime qualification, packaging, desktop-identification validation and Windows EXT2 code analysis.

## Packaging

The release produces one Debian package and one self-extracting `.run` installer. Per-filesystem desktop templates are carried inside the main package and activated only for the selected filesystem; there is no supplemental desktop/UDisks Debian package.

The central Infiltrator repository publication path accepts only an exact qualified release revision and verifies the advertised application/version after publication.

## Architecture documentation

Key contracts are documented under `docs/`, including:

- `FILESYSTEMS_MATRIX.md` — enumerated filesystem contract and status;
- `BRITTLENESS_REDUCTION.md` — ownership, transaction, coupling and failure-reporting boundaries;
- platform/build/runtime qualification documents and implementation notes.

The implementation deliberately keeps filesystem semantics, support policy, privileged mutation, desktop presentation and setup capability as separate responsibilities so that adding or repairing one filesystem does not create hidden coupling to unrelated formats.

[Validation](docs/VALIDATION.md) defines local tests, mounted-runtime evidence and immutable artifact publication. [PHILOSOPHY.md](docs/PHILOSOPHY.md) owns the engineering standard.
