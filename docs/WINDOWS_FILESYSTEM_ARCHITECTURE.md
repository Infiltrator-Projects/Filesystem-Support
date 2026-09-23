# Cross-platform native filesystem architecture

## Decision

Filesystem Support owns the canonical implementation of every filesystem supported by the project.

A filesystem algorithm is implemented once. Linux and Windows supply operating-system adapters around that same implementation; they do not maintain independent implementations of the on-disk format.

The first filesystem to complete under this model is EXT2. EXT3, EXT4 and the remainder of the Filesystem Support catalogue follow only after EXT2 proves the architecture end to end.

This decision supersedes the earlier Linux-only assumption that ROMFS would be the first native implementation.

## Product boundary

Filesystem Support is one product and one repository.

It contains:

- the catalogue of supported filesystem identities and capabilities;
- canonical per-filesystem format/semantic implementations;
- shared filesystem-neutral native mechanisms;
- a Linux adapter;
- a Windows adapter;
- platform build, installation, signing and qualification tooling;
- one management experience per supported operating system.

The existing ExtFS-for-Windows repository is a migration source, not a second long-term filesystem implementation.

## Non-negotiable source rule

For each filesystem there is exactly one canonical body of filesystem-specific code.

The canonical code owns filesystem facts such as:

- on-disk structures and feature flags;
- superblock and inode interpretation;
- directory formats;
- block mapping;
- allocation rules;
- extent trees;
- journal formats and recovery rules;
- metadata checksums;
- extended attributes and filesystem-specific metadata;
- corruption detection and validation;
- crash-consistency rules intrinsic to that filesystem.

The canonical filesystem code must not depend directly on Linux VFS objects or Windows IFS/WDK objects.

Linux and Windows may provide different implementations of the host services required by that code, but must not fork the filesystem semantics.

## Platform adapters

### Linux

The Linux adapter owns Linux-specific integration, including VFS registration, superblock/inode/file operations, folio/page-cache integration, block-device access, kernel locking, mount lifecycle and Linux error translation.

### Windows

The Windows adapter owns Windows-specific integration, including DriverEntry and filesystem registration, IRP dispatch, VCB/FCB/CCB lifetime, Cache Manager and Memory Manager integration, Windows locking, volume verification, mount/dismount, NTSTATUS translation, WDK packaging and Secure Boot signing.

Neither adapter is allowed to become an alternative implementation of the filesystem format.

## Source shape

The intended direction is:

```text
Filesystem-Support/
  native/
    core/                         filesystem-neutral mechanisms
    filesystems/
      ext2/
        core/                     canonical EXT2 implementation
        linux/                    EXT2-to-Linux glue only when format-specific
        windows/                  EXT2-to-Windows glue only when format-specific
      ext3/
      ext4/
      ...
    platform/
      linux/                      reusable Linux filesystem adapter/framework
      windows/                    reusable Windows filesystem adapter/framework

  src/                            management application/catalogue
  windows/                        Windows build/install/signing/qualification
  docs/
```

The exact directory names may evolve during the EXT2 extraction, but dependency direction must not.

## Host-services contract

The canonical filesystem layer interacts with the host through a small explicit contract rather than through Linux or Windows kernel APIs directly.

Expected services include bounded block/byte I/O, durable flush/barrier operations, checked memory/scratch access, time where the filesystem format requires it, synchronization hooks where unavoidable, and allocation of temporary working storage where an algorithm genuinely needs it.

The contract must be narrow enough to implement correctly on both Linux and Windows and must not smuggle OS-specific objects into filesystem semantics.

## Module model

Filesystem Support targets all catalogue filesystems, but an implementation is selectable on an operating system only after that filesystem has completed that platform's qualification gate.

The catalogue therefore distinguishes at least:

- filesystem identity;
- canonical-engine state;
- Linux availability;
- Windows availability;
- read capability;
- write capability;
- maturity/qualification state;
- driver/module identity and dependencies.

The Windows management application may display the full catalogue while disabling installation for filesystems whose Windows implementation is not yet qualified.

This allows the 104-filesystem destination to be represented from the beginning without pretending unfinished drivers exist.

## Windows build/install model

The current ExtFS NSIS setup is not the future 104-filesystem manager. Its valuable build, WDK, signing, INF/catalogue, architecture-validation, installer and qualification mechanisms are migration inputs.

Filesystem Support on Windows will provide a general build/package path capable of producing the Windows platform framework and the set of qualified filesystem modules.

The Windows management application selects which available filesystem support modules to install, remove, inspect or diagnose. It does not contain a second copy of filesystem algorithms.

## EXT2 proving milestone

EXT2 is the architectural proof.

Completion requires the same canonical EXT2 source to be exercised through both operating-system paths:

```text
canonical EXT2
   |-- Linux adapter --> Linux native filesystem support
   `-- Windows adapter --> Windows native filesystem support
```

EXT2 is not complete merely because both operating systems can mount EXT2. It is complete for this architecture only when filesystem-specific behaviour is not duplicated between the Linux and Windows implementations.

The existing Filesystem-Support EXT2 kernel source and the ExtFS-for-Windows portable EXT2 subset must therefore be decomposed and reconciled. Stronger logic is retained regardless of origin; duplicated implementations are removed only after tests prove the canonical replacement.

## Migration safety

ExtFS-for-Windows must not be deleted until all of the following are true:

1. its current source and documentation are preserved in Filesystem Support or explicitly recorded as intentionally obsolete;
2. unique Windows driver, build, install, signing and test logic has been migrated;
3. the old repository's release and issue/PR history has been recorded sufficiently for engineering provenance;
4. EXT2 uses the canonical Filesystem Support implementation rather than the independent ExtFS core;
5. Windows builds and qualification run from Filesystem Support;
6. a fresh clone of Filesystem Support contains everything required to build and test the Windows EXT2 path;
7. the migrated source is verified against the final ExtFS-for-Windows main commit used as the migration baseline.

All seven migration-safety gates were satisfied on 2026-09-23. The standalone ExtFS-for-Windows repository is therefore safe to delete. Its final source baseline remains preserved byte-for-byte in Filesystem Support, while live Windows EXT2 development proceeds only through the canonical engine and Filesystem Support Windows adapter/build paths.

## Migration baseline

The preservation baseline for this architectural migration is:

- Filesystem-Support main: `0f9da8ca450917005b8bba9f18d8fa720cd09aad`
- ExtFS-for-Windows main: `dda219be47033372217149bce8c8ee20f19ded0a`
- ExtFS-for-Windows release baseline: `v0.9.9`

Any later change to ExtFS-for-Windows before migration closure must be re-audited and incorporated before deletion is considered safe.


## Windows mounting decision: native IFS, not a loopback network share

The preserved ExtFS-for-Windows v0.9.9 implementation establishes the Windows
bootstrap mechanism for Filesystem Support. ExtFS was a native Windows
installable filesystem (IFS) driver. It registered with the Windows filesystem
stack, handled mount-volume and filesystem IRPs, owned VCB/FCB/CCB state, and
presented mounted volumes through the normal Windows storage/filesystem path.

This distinction is deliberate. Filesystem Support must not introduce an SMB,
WebDAV, localhost network share or other redirector merely to make a filesystem
visible in Explorer when the native IFS mechanism already provides the correct
host contract.

The reusable Windows path is therefore:

```text
physical disk / partition / virtual block device
                  |
                  v
       Windows storage / volume stack
                  |
                  v
      reusable Filesystem Support IFS framework
      - DriverEntry / filesystem registration
      - mount and dismount lifecycle
      - IRP translation
      - VCB / FCB / CCB ownership
      - Cache Manager / Memory Manager integration
      - locking and share-access rules
      - volume verification and removable-media handling
      - NTSTATUS translation
                  |
                  v
       thin filesystem-specific Windows bridge
                  |
                  v
       canonical filesystem core
```

For ordinary partitions, drive-letter assignment and Explorer visibility should
flow through Windows' normal volume/mount infrastructure. Image-file mounting,
if supported, requires a block-device/image provider beneath the filesystem
driver; it must not cause the filesystem itself to become a network protocol.

## Per-filesystem SYS module model

For the actively rewritten filesystems, the intended Windows deliverables are
independently installable filesystem-driver packages:

- `ext2.sys`
- `ext3.sys`
- `ext4.sys`
- `ofs.sys`
- `ffs.sys`
- `sfs.sys`
- `sfs2.sys`
- `pfs3.sys`

The filenames above describe the product/module identity; the final staged
binary names may carry the `filesystem_support_` prefix where required by the
build/package system.

Each driver is thin with respect to filesystem semantics. It links the
filesystem's canonical `core/` and the reusable Windows IFS framework. It
must not contain a separately maintained parser, allocator, directory engine,
journal implementation, checksum implementation or format-specific mutation
engine.

The ExtFS Windows driver is the engineering bootstrap for this framework. Its
valuable Windows-specific mechanisms should be extracted/generalised into
`native/platform/windows/` and shared by later filesystem drivers. Copying the
entire historical ExtFS driver into eight separate trees and then maintaining
eight divergent Windows implementations is explicitly prohibited.

## Installation and removal contract

The Windows Filesystem Support application is responsible for lifecycle
management of qualified filesystem modules.

For each qualified filesystem, **Install** means at minimum:

1. validate the staged `.sys`, `.inf` and `.cat` package;
2. verify architecture and production-signing requirements;
3. install the driver package through the supported Windows SetupAPI/primitive
   filesystem-driver path;
4. register/start the filesystem service when Windows permits immediate start;
5. report a required reboot rather than pretending installation is complete;
6. re-probe the driver and expose the resulting state in the manager.

**Remove** means at minimum:

1. refuse destructive removal while the filesystem driver still owns mounted
   volumes or cannot be stopped safely;
2. dismount/unload through supported Windows lifecycle rules;
3. remove the driver/service/package cleanly;
4. report any reboot requirement explicitly;
5. re-probe the system and update the manager state.

A filesystem is not "done on Windows" merely because its `.sys` compiles. It
is complete for the product only when the Windows manager can install, remove,
probe and diagnose the qualified driver and the driver can mount supported
media through the normal Windows filesystem stack.

## Reuse boundary from ExtFS

The preserved ExtFS implementation is authoritative historical evidence for
Windows IFS mechanics, including driver registration, mount-volume handling,
VCB/FCB/CCB lifetime, share-access enforcement, locking, volume I/O bridging,
WDK build/package validation, INF/catalogue generation, production signing and
installer safety.

Those Windows mechanisms may be reused and generalised aggressively.

Ext-specific format semantics from the archived ExtFS core remain migration
evidence only. They must not be resurrected as a second EXT implementation
beside the canonical EXT2/EXT3/EXT4 cores.


## Qualification-stage Windows exception

The native IFS architecture above is the **production destination**, not the
current installation mechanism for unqualified development drivers.

Windows will not be asked to install an unqualified Filesystem Support
filesystem driver merely so development media can be mounted. Until a
filesystem's Windows `.sys` package has completed the required Windows
qualification/signing path, Filesystem Support uses the existing temporary
file/volume-mounter bootstrap as a development compatibility bridge.

That bridge is deliberately a kludge and must be represented as such:

- it exists so the canonical filesystem engine can be exercised from Windows
  before the native driver is distributable;
- it is not evidence that the Windows IFS driver is installed;
- it must not change or duplicate filesystem semantics;
- it must not be treated as the final mount architecture;
- the manager must distinguish temporary-mounter availability from a qualified
  native Windows filesystem driver.

Once a filesystem's Windows driver is properly qualified and installable, the
temporary bridge for that filesystem can be retired in favour of the native IFS
path documented above.

The exact third-party/helper mounter identity is an implementation detail and
must be recorded when the preserved deployment path is reconstructed; the
architectural contract does not depend on a particular mounter product.

## Linux is not allowed to use the Windows workaround

The qualification-stage Windows exception does **not** apply to Linux.

For a Filesystem Support filesystem that has a project-owned Linux VFS module,
the Linux manager's Install/Remove control manages that real `.ko`. It must
not substitute FUSE, a loopback network share, an image mounter, or merely the
filesystem's administration tools and report that as native support.
