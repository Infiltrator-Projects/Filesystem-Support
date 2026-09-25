# NFS Design and Ownership

## Classification

NFS is a remote network filesystem protocol family. NFSv2, NFSv3, NFSv4.x and
pNFS are protocol generations/features of one client family, not local on-disk
filesystems.

Filesystem Support therefore applies the remote-filesystem architecture: one
host-neutral NFS client/filesystem core with normal access provided through the
shared userspace-service boundary. Host-specific integration must not fork the
protocol or namespace semantics.

## Current implementation state

NFS is currently **reference/import state**.

The Linux NFS client under `reference/linux/` is pinned upstream source kept as
protocol, interoperability and failure-behaviour evidence. It is not
project-authored Filesystem Support code.

Its upstream files, nested pNFS layout directories, licences and source
boundaries are preserved unchanged. Moving them from the retired `kernel/`
staging directory to `reference/linux/` is provenance/layout hygiene only.

## Current layout

```text
native/filesystems/nfs/
  DESIGN.md
  reference/
    linux/               pinned upstream Linux NFS client evidence
```

A future first-party implementation is added beside that evidence:

```text
native/filesystems/nfs/
  DESIGN.md
  core/                  canonical NFS protocol/client semantics
  userspace/             provider glue for shared userspace service
  linux/                 optional thin host integration if genuinely required
  windows/               optional thin host integration if genuinely required
  reference/linux/       preserved upstream evidence
```

## Canonical ownership

The future core may own protocol-defined behaviour including:

- NFSv2/v3/v4 request/response encoding and validation;
- mount/root discovery and filehandle semantics;
- namespace operations and attribute models;
- NFSv4 stateids, opens, locks, delegations, leases and sessions;
- id mapping and protocol security identity representation;
- caching/coherency/revalidation rules defined by NFS;
- recovery/reconnect and replay/idempotency state machines;
- pNFS layout negotiation and layout/device semantics for supported layouts;
- protocol error/state transitions and failover policy.

Linux VFS objects, page cache/netfs integration, kernel RPC plumbing, workqueues,
kernel credentials, sysfs and module lifetime are reference/adapter mechanisms,
not the canonical NFS model.

## File-boundary rule

Upstream filenames such as `nfs4proc.c`, `nfs4xdr.c`, `inode.c`,
`write.c` and the pNFS layout subdirectories remain unchanged only beneath
`reference/linux/`.

Project-authored source must be cut around portable protocol/session/namespace/
data/recovery responsibilities. Moving or renaming imported Linux source is
never evidence of a rewrite.

## Promotion sequence

1. define supported NFS protocol versions, security flavours and transports;
2. implement bounded host-neutral XDR/message validation and basic session state;
3. qualify against controlled NFS servers and deterministic protocol fixtures;
4. implement read-only namespace/file access through the userspace service;
5. add caching, reconnect, locking/delegation and mutation with explicit
   server-failure/idempotency tests;
6. add pNFS layouts only when their device/layout contracts are independently
   implemented and qualified;
7. add host-specific adapters only where a real platform requirement justifies
   them, consuming the same core.

## Safety and failure behaviour

Servers and network messages are untrusted. XDR lengths, filehandles, stateids,
sequence/session state, redirects/referrals and layout/device information must
be bounded and validated. Retries must respect protocol idempotency and must not
silently duplicate mutations.

## Reference provenance

The Linux reference refresh workflow pins Linux v6.12.107 and copies `fs/nfs`
into `reference/linux/`. Refreshes may update reference evidence but must never
overwrite future project-owned implementation directories.

## Completion rule

The current tree is reference evidence only. NFS is not a Filesystem Support
first-party engine until the canonical client and applicable provider/adapter
have independent implementation and qualification evidence.
