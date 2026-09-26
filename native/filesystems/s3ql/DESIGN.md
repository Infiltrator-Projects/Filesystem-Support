# S3QL Design and Ownership

## Classification

S3QL is a userspace filesystem with its own filesystem semantics backed by
remote object storage. Unlike a simple object-store mount, S3QL defines
filesystem metadata, deduplication, compression and encryption behaviour above
the backing provider.

The current Filesystem Support catalogue delegates this capability to Debian's
external `s3ql` package.

## Current implementation state

Filesystem Support contains no first-party S3QL implementation.

The current product path is the external userspace provider managed through the
catalogue/action backend. The previous `.gitkeep` represented no
implementation or ownership boundary and is removed by this layout pass.

## Target architecture

If S3QL-compatible functionality is deliberately implemented in the future, its
portable filesystem semantics justify a canonical core:

```text
native/filesystems/s3ql/
  DESIGN.md
  core/                 portable S3QL filesystem/metadata semantics
  userspace/            mount and remote-provider integration
  reference/            optional external evidence with explicit provenance
```

There is deliberately no project kernel filesystem module. Remote object-store
access and mounting remain userspace responsibilities.

## Canonical ownership

A future `core/` may own only S3QL-defined behaviour, including:

- filesystem metadata and object identity;
- block/object mapping and deduplication semantics;
- supported compression representation;
- encryption/authentication and key-independent format validation;
- directory, inode, link and metadata rules;
- transaction/publication ordering and recovery semantics;
- consistency checks for remote metadata/data object sets.

Provider-neutral network transports, credentials, retries, cache service and
mount lifetime belong in shared userspace infrastructure where reusable.

## Safety and failure behaviour

Remote data and metadata are untrusted input. Unsupported format versions,
authentication failures, contradictory metadata/object state and ambiguous
remote mutation outcomes must fail closed. Encryption keys and decrypted
sensitive data must not be logged.

## Completion rule

The current entry is complete only as an external provider catalogue contract.
It is not a project-authored S3QL filesystem.

A future first-party implementation requires explicit format documentation,
independent fixtures, corruption/recovery tests, cryptographic review and
interoperability evidence before replacing the external provider.
