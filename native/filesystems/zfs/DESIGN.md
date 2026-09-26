# OpenZFS / ZFS Design and Ownership

## Classification

ZFS is a pooled copy-on-write filesystem and volume-management stack. It is a
real persistent storage/filesystem format family, not merely a Linux package
identity and not a FUSE presentation layer.

The current Filesystem Support catalogue delegates normal ZFS support to the
external OpenZFS Debian path:

- kernel/module identity: `zfs`;
- packages: `zfsutils-linux` and `zfs-dkms`;
- support provider: external DKMS driver plus userspace administration tools;
- access mode: read/write where the installed OpenZFS provider supports the
  pool and feature set.

That external provider remains the production path until a project-owned ZFS
implementation is deliberately admitted and independently qualified.

## One ZFS implementation rule

Filesystem Support may own only one canonical ZFS implementation.

The separate `zfs-fuse` catalogue identity is an external userspace provider
for the same filesystem family. It must not become a second project ZFS parser,
pool engine or mutation implementation.

If ZFS enters first-party development, persistent ZFS semantics belong in this
directory's canonical `core/`. Linux, Windows and userspace access paths must
consume that same engine rather than implementing ZFS independently.

## Current implementation state

ZFS is currently **external-provider / future-native state**.

There is no project-authored ZFS canonical engine, Linux ZFS module, Windows
ZFS driver or userspace ZFS provider in this directory. Unlike filesystems for
which a pinned Linux in-tree implementation is copied into
`reference/linux/`, OpenZFS is not imported here merely to make the directory
look populated.

The old empty `.gitkeep` represented no implementation and is not part of the
filesystem architecture.

## Current directory contract

While ZFS remains external-provider / future-native state, the directory
contains exactly:

```text
native/filesystems/zfs/
  DESIGN.md
```

The directories `core/`, `linux/`, `windows/`, `userspace/`,
`kernel/` and `reference/` are deliberately absent in the current state.

Creating project implementation directories is a promotion event. They must
arrive with independently authored code, explicit provenance, tests and the
qualification appropriate to the claimed capability rather than as empty
placeholders or renamed third-party source.

A future third-party source snapshot may be retained only under an explicit
`reference/<origin>/` provenance boundary with its original licensing and
copyright intact. Reference source must never be linked or represented as the
project ZFS implementation.

## Target architecture

When an independent implementation is admitted, the intended ownership shape is:

```text
native/filesystems/zfs/
  DESIGN.md
  core/                 canonical ZFS pool/filesystem semantics
  linux/                thin Linux host/module adapter where required
  windows/              thin Windows host/driver adapter where required
  userspace/            administration/inspection/provider adapter over the core
  reference/            optional third-party evidence with explicit provenance
```

ZFS tightly integrates pool, volume and filesystem semantics, so the canonical
engine must preserve those relationships rather than pretending ZPL can be
implemented correctly without the storage-pool layers beneath it.

## Canonical-core ownership

A future `core/` may own ZFS-defined, host-neutral behaviour including the
supported subset of:

- vdev labels, pool configuration and uberblock selection;
- transaction-group and checkpoint/publication semantics;
- metaslab, space-map and allocation behaviour;
- block-pointer interpretation, checksums, compression and supported encryption
  metadata/transform contracts;
- DMU object sets, dnodes and object/block mapping;
- ZAP objects and directory/property maps;
- ZPL inode, directory, link, xattr/ACL and namespace semantics;
- datasets, snapshots, clones and bookmarks where implemented;
- intent-log/replay and recovery rules;
- feature-flag negotiation and compatibility policy;
- corruption, range, checksum and topology validation.

Linux VFS objects, SPL/kernel primitives, bios, page cache, workqueues, module
registration and Linux-specific credentials belong in the Linux adapter or
platform layer.

Windows IFS objects, IRPs, Cache Manager integration, driver lifetime and
NTSTATUS translation belong in the Windows adapter.

Command-line presentation, device discovery and administration UI do not define
filesystem semantics and belong outside the canonical engine.

## Provider relationship

OpenZFS remains valuable interoperability and behaviour evidence while the
project implementation is absent or incomplete. The external provider can be
installed, removed and reported by Filesystem Support without implying that its
source belongs to this repository.

The `zfs-fuse` provider remains an alternative external userspace path. If a
project-owned userspace ZFS provider is later implemented, it belongs under
`zfs/userspace/` and consumes `zfs/core/`; it does not create a second
canonical engine under `zfs-fuse/`.

## Promotion sequence

ZFS leaves external-provider / future-native state only through a deliberate
milestone:

1. define the exact supported on-disk versions, feature flags and pool
   topologies;
2. implement bounded host-neutral label/configuration and uberblock validation;
3. implement read-only vdev/block-pointer/DMU/ZAP/ZPL traversal using
   independent pools and malformed fixtures;
4. qualify checksum, compression and feature negotiation before advertising
   them;
5. add a host adapter over the same core rather than importing an alternate
   semantic implementation;
6. add mutation only after allocation, transaction-group publication, ZIL/replay
   and crash-recovery contracts are explicit and destructively qualified;
7. add multi-device, snapshot/clone and repair functionality only to the extent
   independently implemented and verified;
8. retain external/reference provenance until every claimed project subsystem
   has actually crossed the authorship and qualification boundary.

Moving, renaming or recommenting OpenZFS source is never evidence of a rewrite.

## Safety and failure behaviour

Unknown incompatible feature flags, contradictory pool/vdev topology, invalid
labels or uberblocks, checksum failures, malformed block pointers, impossible
DMU/ZAP/ZPL structures, unsafe transaction/replay state and ambiguous
multi-device authority must fail closed.

A pool being mountable by external OpenZFS is interoperability evidence, not
proof that a future Filesystem Support writer is safe to mutate it.

## Completion rule

The present ZFS entry is complete only as an external-provider ownership and
architecture contract. It must not be counted as a project-authored ZFS engine.

A first-party ZFS capability is complete only when implementation, provenance,
tests, interoperability, failure handling, platform integration and maintained
documentation agree for the exact claimed feature/topology set.
