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

Until every gate is satisfied, ExtFS-for-Windows remains a read-only preservation source.

## Migration baseline

The preservation baseline for this architectural migration is:

- Filesystem-Support main: `0f9da8ca450917005b8bba9f18d8fa720cd09aad`
- ExtFS-for-Windows main: `dda219be47033372217149bce8c8ee20f19ded0a`
- ExtFS-for-Windows release baseline: `v0.9.9`

Any later change to ExtFS-for-Windows before migration closure must be re-audited and incorporated before deletion is considered safe.
