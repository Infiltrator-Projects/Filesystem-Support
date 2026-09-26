# securefs Design and Ownership

## Classification

securefs is a userspace encrypted overlay filesystem. It stores encrypted,
authenticated representations in an ordinary backing directory and presents a
decrypted filesystem view through FUSE.

The Filesystem Support catalogue currently delegates this capability to the
external `securefs` package.

## Current implementation state

Filesystem Support contains no first-party securefs implementation. The current
product path is the external userspace provider managed through the
catalogue/action backend.

The previous `.gitkeep` represented no implementation or useful ownership
boundary and is removed by this layout pass.

## Target architecture

A future compatible implementation may legitimately contain a portable
encrypted-filesystem core:

```text
native/filesystems/securefs/
  DESIGN.md
  core/                 portable securefs format/crypto/name semantics
  userspace/            mount/provider integration
  reference/            optional external evidence with provenance
```

There is deliberately no project kernel filesystem module. The backing
filesystem's allocation, journalling and repair semantics remain with the
backing filesystem.

## Canonical ownership

A future `core/` may own only securefs-defined behaviour, including encrypted
filename/data representation, authenticated metadata, key-independent format
validation, object mapping and supported configuration/version semantics.

Credential acquisition, protected-memory integration, mount lifetime and
generic backing-directory I/O belong to platform/userspace infrastructure.

## Safety and failure behaviour

Encrypted metadata and data are untrusted until authenticated. Unsupported
format/crypto variants, authentication failures, malformed names/objects and
contradictory configuration must fail closed. Keys and decrypted sensitive data
must not be logged.

## Completion rule

The current entry is complete only as an external userspace-provider catalogue
contract. It is not a first-party securefs filesystem implementation.
