# LUKS / luksde Design and Ownership

## Classification

The `luksde` catalogue identity represents LUKS encrypted-volume access through
an external userspace provider. LUKS is an encrypted block-container format,
not a filesystem.

The decrypted block view must be handed to the actual contained filesystem
implementation. LUKS support must never duplicate EXT, XFS, Btrfs or other
filesystem semantics.

## Current implementation state

Filesystem Support contains no first-party LUKS/luksde implementation. The
current product path is an external provider managed through the catalogue and
action backend. The former `.gitkeep` represented no implementation and is
removed by this layout pass.

## Intended architecture

If a first-party LUKS-compatible implementation is admitted, the correct shape
is:

```text
native/filesystems/luksde/
  DESIGN.md
  core/                 portable supported LUKS metadata/crypto semantics
  userspace/            credential and decrypted-block exposure
  reference/            optional external evidence with provenance
```

There is deliberately no filesystem `linux/` or `kernel/` implementation
here merely to expose the encrypted container.

## Core ownership

A future core may own only LUKS-defined behaviour: supported header/version
parsing, keyslot and metadata validation, key derivation/unwrapping and
validated sector encryption/decryption transforms.

Contained filesystem allocation, namespace, metadata and recovery remain with
the owning canonical filesystem engine.

## Security and failure behaviour

Credentials, volume keys and decrypted data are sensitive. A first-party
implementation must fail closed on unsupported versions or malformed metadata,
avoid logging secrets, bound all offsets/sizes before I/O, zero transient key
material where practical, and expose read-only operation before writable
container access is considered.

## Completion rule

The current entry is complete only as an external encrypted-volume provider
contract. It is not a project-authored filesystem implementation.
