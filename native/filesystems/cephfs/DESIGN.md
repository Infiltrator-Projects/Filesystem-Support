# CephFS Design and Ownership

## Classification

CephFS is a distributed/network filesystem. It is not a local on-disk
filesystem whose permanent implementation should be forced into an out-of-tree
kernel module.

Filesystem Support therefore applies the remote/distributed rule: CephFS
protocol/filesystem semantics belong in one canonical host-neutral client core,
with normal product access provided through the shared userspace-service
boundary.

## Current state

CephFS is currently **reference/import state**.

The source under `reference/linux/` is the pinned upstream Linux CephFS kernel
client. It is retained as compatibility and semantic evidence only. Filesystem
Support does not claim it as project-authored code.

The separate `ceph-fuse` catalogue identity is also only an external provider;
it must not become a second CephFS implementation.

## Target layout

```text
native/filesystems/cephfs/
  DESIGN.md
  core/                 canonical CephFS client/filesystem semantics
  userspace/            CephFS provider glue for shared userspace service
  reference/linux/      pinned upstream Linux client evidence
```

A platform-specific adapter may be introduced only if a host requires genuinely
CephFS-specific integration that cannot be shared. It must consume the same
`core/` rather than duplicating protocol logic.

## Canonical ownership

The future core may own CephFS-defined/client-protocol behaviour such as:

- metadata-server map and session interpretation;
- inode/capability and lease/coherency semantics;
- namespace operations and snapshots;
- quota and extended-attribute semantics defined by CephFS;
- file layout/object placement information needed by the client;
- reconnect/recovery state machines and protocol message validation;
- cryptographic/name handling where it is part of the supported CephFS client
  contract.

Linux VFS objects, kernel page cache, workqueues, debugfs and kernel credential
objects are reference-adapter details, not canonical filesystem semantics.

## File-boundary rule

Upstream files such as `mds_client.c`, `caps.c`, `inode.c`, `file.c` and
`super.c` remain unchanged in `reference/linux/`.

Project-owned source must be organised around portable CephFS protocol and
filesystem responsibilities rather than preserving Linux VFS translation-unit
boundaries.

## Promotion sequence

1. define the supported CephFS protocol/features and authentication boundary;
2. implement bounded host-neutral metadata/session/message decoding;
3. qualify against controlled Ceph clusters and recorded protocol fixtures;
4. implement read-only namespace/file access through the shared userspace
   service;
5. add cache/coherency, reconnect and mutation semantics only with explicit
   failure/recovery testing;
6. keep the external kernel/FUSE providers available until project support is
   independently mature.

## Failure policy

Malformed or contradictory server messages, unsupported feature negotiation,
invalid capability/session transitions and stale/unsafe recovery states must
fail closed. Remote peers and cluster metadata are untrusted input.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/ceph` to
`reference/linux/`. Refreshing it must never overwrite future project-owned
`core/` or `userspace/` source.
