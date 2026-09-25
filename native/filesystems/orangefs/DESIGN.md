# OrangeFS Design and Ownership

## Classification

OrangeFS is a distributed network filesystem. Its client protocol, namespace,
metadata and consistency behaviour are not a local on-disk filesystem format
that Filesystem Support should represent by copying a Linux kernel client into
a project `kernel/` directory.

Filesystem Support therefore applies the distributed-filesystem rule: one
canonical host-neutral OrangeFS client/filesystem core, with normal product
access through the shared userspace-service boundary. Host-specific integration
must remain an adapter around that core.

## Current implementation state

OrangeFS is currently **reference/import state**.

The source under `reference/linux/` is the pinned upstream Linux OrangeFS
client copied by `.github/workflows/import-linux-filesystems.yml`. It is
retained as protocol, interoperability and failure-behaviour evidence only.
Filesystem Support does not claim those files as project-authored code.

The former `kernel/` staging path was misleading because unchanged upstream
Linux source looked like a production Filesystem Support native module. This
layout pass moves the exact blobs, without implementation edits, beneath the
explicit reference/provenance boundary.

## Current layout

```text
native/filesystems/orangefs/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux OrangeFS client evidence
```

A future first-party implementation is added beside that evidence:

```text
native/filesystems/orangefs/
  DESIGN.md
  core/                  canonical OrangeFS client/filesystem semantics
  userspace/             provider glue for shared userspace service
  linux/                 optional thin host integration if genuinely required
  windows/               optional thin host integration if genuinely required
  reference/linux/       preserved upstream Linux evidence
```

## Canonical ownership

A future `core/` may own OrangeFS-defined behaviour including:

- protocol message encoding, decoding and validation;
- filesystem/object identity and namespace semantics;
- metadata and file-data request state;
- server/configuration discovery and failover behaviour;
- cache/coherency rules defined by the OrangeFS client protocol;
- credential/security identity representation;
- mutation acknowledgement, retry/idempotency and reconnect/recovery state;
- filesystem-defined error and capability negotiation.

Linux VFS objects, page-cache/buffer mechanics, device-node upcall/downcall
plumbing, debugfs/sysfs, workqueues and module lifetime are adapter/reference
mechanisms rather than canonical OrangeFS semantics.

Generic sockets, credentials, worker/event loops and mount-service lifetime
belong in shared userspace infrastructure when their contracts are
provider-neutral.

## File-boundary rule

Files such as `devorangefs-req.c`, `orangefs-mod.c`, `inode.c`,
`file.c` and `super.c` retain their upstream filenames, licences and
implementation boundaries only under `reference/linux/`.

Project-owned source must be cut around portable protocol/session/namespace/
data/recovery responsibilities. Moving or renaming imported Linux source is
never evidence that it has crossed the project-authorship boundary.

## Promotion sequence

1. define the supported OrangeFS protocol/features and authentication boundary;
2. implement bounded host-neutral message decoding and client/session state;
3. qualify against controlled OrangeFS servers and deterministic recorded
   protocol fixtures;
4. implement read-only namespace/file access through the shared userspace
   service;
5. add caching, reconnect and mutation only with explicit failure,
   retry/idempotency and server-loss tests;
6. add host-specific adapters only where a real platform requirement justifies
   them, consuming the same canonical core.

## Safety and failure behaviour

Remote peers, configuration data and protocol messages are untrusted. Lengths,
handles and identifiers must be bounded and validated. Retries must respect
operation idempotency, authentication failures must fail closed, and uncertain
mutation outcome after transport/server failure must not be reported as a
successful durable write.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/orangefs` into
`reference/linux/`. Refreshes may replace only copied reference evidence and
must never overwrite future project-owned `core/`, `userspace/`, `linux/`
or `windows/` source.

## Completion rule

The current tree is reference evidence only. OrangeFS is not a Filesystem
Support first-party engine until the canonical client and applicable
provider/adapter have independent implementation and qualification evidence.
