# Windows Imaging Format (WIM) Design and Ownership

## Classification

WIM is a file/archive image format that can expose one or more captured
filesystem trees. It is not itself a block filesystem and does not justify a
native kernel filesystem driver.

The Filesystem Support catalogue currently delegates WIM access and mounting to
the external `wimtools` provider.

## Current implementation state

Filesystem Support contains no first-party WIM implementation. The current
product path is the external userspace provider managed through the
catalogue/action backend.

The previous `.gitkeep` represented no implementation or useful ownership
boundary and is removed by this layout pass.

## Target architecture

If project-owned WIM support is admitted:

```text
native/filesystems/wim/
  DESIGN.md
  core/                 portable WIM container/resource/namespace semantics
  userspace/            image I/O, extraction and mount-provider integration
  reference/            optional evidence with explicit provenance
```

There is deliberately no project `kernel/` filesystem implementation.

## Canonical ownership

A future `core/` may own WIM header/resource-table parsing, image selection,
metadata/resource identity, directory/file namespace reconstruction,
compression/resource decoding, reparse/security metadata representation and
container mutation/publication rules when explicitly supported.

FUSE mounting, host file I/O and command/UI presentation belong in userspace
adapters.

## Safety and failure behaviour

Container offsets, resource lengths, compressed chunks and metadata records are
untrusted. Malformed resources, unsupported compression or contradictory image
metadata must fail closed. Writable support must not publish a modified WIM
until all referenced resources and metadata are internally consistent.

## Completion rule

The current entry is complete only as an external userspace-provider catalogue
contract. It is not a first-party WIM implementation.
