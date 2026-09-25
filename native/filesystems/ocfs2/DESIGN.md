# OCFS2 Design and Ownership

## Classification

OCFS2 is a shared-disk clustered filesystem. Multiple hosts may access the same
block storage while coordinating filesystem state through cluster membership,
heartbeat, distributed locking, journals and recovery.

OCFS2 is therefore neither a simple local-only filesystem nor a remote
namespace. Its permanent Filesystem Support architecture has one canonical
filesystem engine, with host adapters and a separate cluster-coordination
boundary around that engine.

## Current implementation state

OCFS2 is currently **reference/import state**.

The source under `reference/linux/` is the pinned upstream Linux OCFS2 tree,
including its O2CB cluster and DLM/DLMFS implementation. It is retained as
format, interoperability, cluster-state and failure/recovery evidence only.
Filesystem Support does not claim that source as project-authored code.

The former `kernel/` staging path was misleading because it looked like a
production Filesystem Support native module. The unchanged upstream blobs are
therefore kept under an explicit reference/provenance boundary.

## Target layout

```text
native/filesystems/ocfs2/
  DESIGN.md
  core/                 canonical OCFS2 format/filesystem semantics
  linux/                thin Linux VFS/block/module adapter
  windows/              thin Windows adapter if eventually justified
  cluster/              host-neutral cluster/locking service boundary
  reference/linux/      pinned upstream Linux OCFS2/O2CB/DLM evidence
```

The exact cluster transport may evolve. Filesystem semantics must not fork per
operating system or per cluster provider.

## Canonical-core ownership

The future `core/` owns OCFS2-defined behaviour that is independent of a host
kernel object, including as supported:

- superblock, slot-map and system-file interpretation;
- inode, directory and extent-tree semantics;
- allocation groups/suballocation and free-space accounting;
- refcount/reflink and extent-move semantics;
- local/global quota and metadata rules;
- journal format, transaction ordering and filesystem recovery invariants;
- xattr/ACL format semantics;
- resize and filesystem-defined metadata checks;
- filesystem-defined lock/resource identity and ordering requirements;
- corruption, bounds and cross-structure validation.

Linux VFS objects, buffer/page-cache mechanics, block-device APIs, workqueues,
module lifetime and Linux error translation belong in `linux/`.

## Cluster boundary

OCFS2's correctness depends on more than local media structures. Cluster
membership, heartbeat, quorum, distributed locking, node failure and recovery
must have an explicit adapter boundary.

A future `cluster/` layer may expose host-neutral membership, lease/lock,
heartbeat/fencing and recovery coordination mechanisms required by the
canonical filesystem engine. Linux O2CB/DLM objects and configfs/sysfs details
remain platform/reference mechanisms and must not leak into portable
filesystem semantics.

The canonical engine must define what ordering and exclusion it requires; a
cluster provider supplies those mechanisms without becoming a second OCFS2
implementation.

## File-boundary rule

Files below `reference/linux/` retain upstream filenames, SPDX identifiers,
copyrights and source boundaries, including `alloc.c`, `journal.c`,
`dlmglue.c`, the `cluster/` tree and the `dlm/` and `dlmfs/` trees.

Project-authored code must be organised around Filesystem Support
responsibilities and canonical/adapter ownership. Moving or renaming an
upstream OCFS2 file does not constitute an independent rewrite.

## Promotion sequence

OCFS2 leaves reference/import state only through deliberate milestones:

1. document supported on-disk features, cluster model and unsupported states;
2. implement bounded host-neutral superblock/system-file/inode/extent decoding;
3. qualify single-node read-only media against independent images and malformed
   fixtures;
4. implement journal and recovery semantics in the canonical engine;
5. add a thin Linux adapter over that engine;
6. introduce allocation/mutation only with durable recovery and independent
   post-operation verification;
7. implement the cluster adapter and qualify lock ordering, heartbeat,
   membership loss, node failure and recovery on real multi-node storage;
8. add another OS adapter only against the same canonical engine and cluster
   contract.

Clustered write support is not complete merely because a single-node mount can
modify an image.

## Failure policy

Invalid system-file references, impossible allocation metadata, corrupt extent
trees, journal inconsistencies, contradictory slot/lock ownership, lost quorum,
uncertain fencing state and ambiguous node-recovery state must fail closed.

Mutation must not continue when the project cannot prove exclusive or correctly
coordinated access to affected metadata.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/ocfs2` into
`reference/linux/`. Refreshing that tree may replace only copied reference
material and must never overwrite future project-owned `core/`, `linux/`,
`windows/` or `cluster/` source.
