# Cross-platform native filesystem architecture

## Decision

Filesystem Support owns one canonical implementation of each project-native filesystem. Filesystem-specific semantics are implemented once; Linux and Windows provide operating-system adapters around that implementation and must not maintain competing interpretations of the on-disk format.

EXT2 is the current Windows proving implementation. The architecture is extended to another filesystem only when the common host contract is strong enough for that filesystem without weakening its semantics or moving format-specific policy into a generic layer.

## Product boundary

Filesystem Support owns:

- stable filesystem identities and capability presentation;
- canonical per-filesystem engines where the project has native ownership;
- genuinely filesystem-neutral native mechanisms;
- Linux and Windows adapter boundaries;
- platform build, installation, signing and qualification tooling;
- the management surface appropriate to each supported operating system.

The standalone ExtFS-for-Windows repository was a migration source. It is not a fallback implementation or a second product boundary. Historical migration facts are recorded in [`EXTFS_FOR_WINDOWS_MIGRATION.md`](EXTFS_FOR_WINDOWS_MIGRATION.md).

## Canonical-source rule

For each project-native filesystem there is exactly one canonical body of filesystem-specific code. It owns, as applicable:

- on-disk structures and feature flags;
- superblock, inode/object and directory interpretation;
- block/extent mapping and allocation semantics;
- journal and recovery rules;
- metadata checksums and extended metadata;
- corruption validation and checked geometry;
- crash-consistency rules intrinsic to that filesystem.

The canonical engine must not depend on Linux VFS objects or Windows IFS/WDK objects. Conversely, a platform adapter must not quietly become a second parser, allocator, namespace engine or recovery implementation.

## Platform ownership

### Linux

The Linux adapter owns Linux-specific VFS registration, superblock/inode/file integration, folio/page-cache and block-device integration, Linux locking, module/mount lifecycle and Linux error translation.

### Windows

The Windows adapter owns Windows-specific filesystem registration and DriverEntry, IRP translation, VCB/FCB/CCB lifetime, Cache Manager and Memory Manager integration, Windows locking/share-access rules, volume verification, mount/dismount, NTSTATUS translation, WDK package construction and driver-signing lifecycle.

Neither adapter owns format semantics merely because an operating system exposes a convenient primitive for them.

## Source shape

The dependency direction is:

```text
native/filesystems/<fs>/core/
        ^
        |
        +-- native/filesystems/<fs>/linux/     Linux adapter
        |
        `-- native/filesystems/<fs>/windows/   Windows adapter

native/platform/                               filesystem-neutral host mechanisms
windows/build/                                 WDK/package validation
windows/test/                                  Windows qualification
windows/manager/                               Windows product surface
```

Directory layout may evolve when a responsibility changes, but dependency direction may not: platform policy points inward toward canonical filesystem semantics; canonical semantics do not point outward into platform frameworks.

## Host-services contract

A canonical filesystem engine may consume only a narrow, explicit host contract for mechanisms that are genuinely host-dependent. Typical services include bounded block/byte I/O, durable flush/barrier operations, checked scratch allocation, time conversion where the format requires it and synchronization primitives whose ownership cannot remain entirely inside the engine.

The contract must not carry Linux or Windows filesystem objects through opaque handles merely to disguise platform coupling. If a service exists only because one filesystem needs a format-specific rule, that rule remains in that filesystem's canonical engine.

## Catalogue and implementation state

The product catalogue and Windows implementation set are different authorities. The catalogue may describe a filesystem that has no Windows driver; that does not create a placeholder native implementation. The Windows manager can present catalogue identities while keeping install/remove actions disabled until a real Windows package has completed its qualification gate.

The current catalogue size is determined by the compiled catalogue and support matrix, not by this architecture document. Historical migration targets such as a “104-filesystem” destination are not current product authority.

## Native Windows IFS model

The production Windows mechanism is a native installable filesystem (IFS) driver, not an SMB/WebDAV/localhost redirector and not an undocumented user-mode mount compatibility layer.

```text
physical disk / partition / block provider
                  |
                  v
       Windows storage / volume stack
                  |
                  v
       Windows IFS adapter/framework
       - DriverEntry / filesystem registration
       - mount and dismount lifecycle
       - IRP translation
       - VCB / FCB / CCB ownership
       - Cache Manager / Memory Manager integration
       - locking and share-access rules
       - volume verification
       - NTSTATUS translation
                  |
                  v
       thin filesystem-specific Windows bridge
                  |
                  v
       canonical filesystem core
```

Image-file support, if introduced, requires an explicit block-device/image-provider boundary beneath the filesystem driver. It must not change the filesystem itself into a network or archive-mount product.

There is currently no documented, repository-owned temporary Windows mounter that may be treated as a supported fallback. Development or qualification may use external laboratory tooling, but such tooling is evidence infrastructure only and must be named, versioned and deliberately introduced before it can become a product dependency.

## EXT2 proving milestone

EXT2 proves the cross-platform ownership model:

```text
canonical EXT2
   |-- Linux adapter   --> native Linux filesystem module
   `-- Windows adapter --> native Windows filesystem driver
```

The Windows EXT2 driver builds from the canonical EXT2 engine and its Windows adapter. Completion is not defined by compilation alone; it requires that Windows lifecycle, cache/locking behaviour, malformed-media handling and the filesystem's supported mutation semantics all pass their applicable qualification contracts without introducing a second EXT2 engine.

EXT3 and EXT4 remain distinct filesystem implementations. They are not Windows aliases of EXT2 and must not be represented as qualified merely because EXT2's IFS framework is reusable.

## Driver module model

Each qualified Windows filesystem is an independently installable filesystem-driver package. Binary naming may carry the `filesystem_support_` prefix required by the build/package system; module identity is subordinate to the filesystem identity rather than a reason to create another format identity.

A filesystem-specific Windows driver may link its canonical `core/` plus reusable Windows mechanisms. It must not copy a historical Windows filesystem implementation and then diverge it per filesystem.

## Installation and removal contract

For a qualified filesystem, installation must at minimum:

1. validate the staged `.sys`, `.inf` and `.cat` package;
2. verify target architecture and production trust/signing requirements;
3. install through supported Windows driver/package mechanisms;
4. register/start the filesystem service when the platform permits immediate activation;
5. report restart requirements explicitly;
6. re-probe the installed driver and expose observed state rather than assumed success.

Removal must at minimum:

1. refuse destructive removal while the selected filesystem still owns mounted volumes or cannot be stopped safely;
2. dismount/unload using supported lifecycle rules;
3. remove only the selected driver/service/package;
4. report resident-until-restart state explicitly;
5. re-probe and publish the observed result.

A `.sys` that compiles is not an installable product capability. Install/remove remains disabled until the complete platform qualification and trust contract is satisfied.

## Shared Windows mechanisms

The former ExtFS work remains historical evidence for Windows IFS concerns such as filesystem registration, mount-volume handling, object lifetimes, share-access enforcement, locking, WDK validation, INF/catalogue production, signing and diagnostic practice. Those are candidates for filesystem-neutral reuse only when the resulting contract is at least as strong as the filesystem-specific implementations it replaces.

Historical Ext-specific format semantics are not a shared Windows mechanism and must not return as a second EXT implementation.

## Qualification boundary

Windows platform qualification is separate from on-media format qualification. At minimum, a production-capable Windows filesystem path requires fresh-checkout build reproducibility, static/code analysis, INF/CAT/architecture validation, supported signing, service/install lifecycle testing, Driver Verifier on a disposable system, mount/dismount and removable-media testing, repeated open/cleanup/close, VCB/FCB/CCB lifetime checks, cache/MM interaction testing, I/O failure handling and diagnostic capture.

The live lifecycle requirements are maintained in [`windows/WINDOWS_DRIVER_LIFECYCLE.md`](windows/WINDOWS_DRIVER_LIFECYCLE.md). Filesystem-specific design documents remain authoritative for format semantics and capability limits.

## Linux boundary

No Windows development exception changes Linux ownership. Where Filesystem Support ships a project-owned Linux VFS module, Linux Install/Remove manages that real module. It must not substitute FUSE, a loopback network share, an image mounter or administration tools and report them as native support.
