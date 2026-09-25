# BitLocker Design and Ownership

## Classification

BitLocker is a storage-encryption/container format, not a filesystem.

The current Filesystem Support catalogue delegates access to Debian's
`dislocker` package. Dislocker exposes a decrypted virtual volume; the
filesystem inside that volume (typically NTFS) is then mounted by the
appropriate filesystem implementation.

That layering must remain explicit.

## Current implementation state

Filesystem Support does not currently contain a first-party BitLocker engine.
The former `.gitkeep` represented no implementation and has been removed.

The catalogue/action backend owns discovery and installation of the external
provider.

## Target architecture

A future project-owned implementation may legitimately have a portable
container core because BitLocker has format-defined metadata, key and sector
transformation semantics:

```text
native/filesystems/bitlocker/
  DESIGN.md
  core/                 portable BitLocker/FVE container semantics, if admitted
  userspace/            decrypted block-volume exposure/service glue
  reference/            optional third-party/public evidence with provenance
```

There is deliberately no filesystem `kernel/` module here. A decrypted
BitLocker container is passed onward as a block-volume view; NTFS or another
contained filesystem remains owned by its own canonical filesystem engine.

## Core ownership

If implemented, `core/` may own only BitLocker/container-defined behaviour,
including supported FVE metadata interpretation, key-material handling and
validated sector encryption/decryption transforms.

It must not contain NTFS parsing, allocation, directory or repair semantics.

Platform code owns secure credential acquisition, device access, protected
memory integration and block-device/service presentation.

## Safety and security

Encrypted-volume code is a security boundary. Any first-party implementation
must:

- fail closed on unsupported metadata/encryption variants;
- authenticate/validate metadata as required by the supported format;
- avoid logging keys or decrypted key material;
- zero sensitive transient buffers where practical and defined;
- keep credential acquisition outside format parsing;
- expose a read-only path before any writable container path is considered;
- keep underlying filesystem repair/mutation in the owning filesystem layer.

## Completion rule

The current entry is an external userspace-provider catalogue contract only.
A future BitLocker core is complete only when its format support, key handling,
failure behaviour, interoperability fixtures and security review are explicit.
