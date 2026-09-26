# SMB / CIFS Design and Ownership

## Classification

The `cifs` catalogue identity represents SMB/CIFS network filesystem access.
This is a remote protocol/filesystem, not a local on-disk format.

Filesystem Support therefore targets one canonical host-neutral SMB client and
filesystem-semantics core behind the shared userspace-service boundary.

## Current state

CIFS is currently **reference/import state**.

The Linux SMB client and shared SMB protocol source under
`reference/linux/client/` and `reference/linux/common/` are pinned upstream
reference evidence only. They are not project-authored Filesystem Support code.

## Current directory contract

While SMB/CIFS remains in reference/import state, the filesystem root contains
only `DESIGN.md` and `reference/`; the pinned Linux SMB client/common trees
remain below `reference/linux/` with their upstream filenames and subsystem
boundaries intact. The future `core/` and `userspace/` directories are created
only when an independent client implementation actually begins. `linux/`,
`windows/` and `kernel/` must not become alternate SMB semantic engines.

CI enforces this present-state layout and rejects accidental production linkage
of either imported reference subtree.

## Target layout

```text
native/filesystems/cifs/
  DESIGN.md
  core/                     canonical SMB/CIFS client semantics
  userspace/                provider glue for shared userspace service
  reference/
    linux/
      client/               upstream Linux SMB client evidence
      common/               upstream Linux SMB common protocol evidence
```

A host-specific adapter may be introduced only for integration that cannot be
shared. It must consume the same core rather than creating a second SMB stack.

## Canonical ownership

The future core may own negotiated SMB dialect/capability state, message
framing/validation, session/tree/file handle state, namespace operations,
leases/oplocks, durable/reconnect semantics, DFS behaviour, security-descriptor
and xattr interpretation, signing/encryption/compression protocol semantics and
other behaviour defined by the supported SMB protocol.

Linux VFS objects, kernel keyrings, workqueues, page cache/netfs integration and
kernel socket/lifetime mechanisms are adapter/reference concerns.

## File-boundary rule

Upstream filenames such as `cifssmb.c`, `smb2pdu.c`, `connect.c`,
`inode.c` and `file.c` stay unchanged in the reference tree. Project-owned
source is organised around portable protocol/session/namespace/data/security
responsibilities rather than Linux VFS translation units.

## Promotion sequence

Begin with bounded protocol framing, negotiation and read-only session/tree/file
operations using deterministic captured fixtures and controlled SMB servers.
Add authentication/security negotiation, reconnect, caching and write semantics
only with explicit interoperability and failure/recovery tests.

## Safety

Network messages, server metadata and path components are untrusted input.
Unsupported dialect/features, invalid lengths/offsets, contradictory credits or
handle state, signature/authentication failures and unsafe reconnect state must
fail closed.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/smb/client` plus
`fs/smb/common` into `reference/linux/`. Refreshing those trees must never
overwrite future project-owned `core/` or `userspace/` code.
