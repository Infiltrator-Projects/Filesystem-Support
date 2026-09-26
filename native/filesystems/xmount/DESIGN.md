# xmount Design and Ownership

## Classification

xmount is a userspace/FUSE forensic disk-image presentation and conversion
provider. It exposes supported forensic and virtual disk-image containers as a
virtual namespace or translated image view so the filesystem contained inside
can be inspected separately.

It is not an on-disk filesystem format and must not own the semantics of any
filesystem found inside an image.

## Current implementation state

Filesystem Support contains no first-party xmount implementation.

The current product path is the external Debian `xmount` provider managed
through the catalogue and action backend. The previous `.gitkeep` represented
no implementation, provenance or useful ownership boundary.

## Intended architecture

If equivalent first-party functionality is ever admitted, it belongs at the
userspace image/provider boundary:

```text
native/filesystems/xmount/
  DESIGN.md
  userspace/            image-container translation/presentation, if implemented
```

There is deliberately no disk-filesystem `core/`, Linux VFS filesystem engine
or project `.ko` here.

Generic bounded file/device I/O, cancellation, temporary-file handling and
read-only image access may be shared when their contracts are provider-neutral.

## Ownership rules

A future project-owned xmount-equivalent may own only image-container behaviour,
including:

- recognising explicitly supported forensic/virtual image container formats;
- validating container headers, segment/chunk tables and logical image geometry;
- exposing a bounded logical disk-image byte view;
- deterministic conversion/presentation metadata;
- read-only namespace or FUSE integration used to expose that image view.

EXT, NTFS, XFS, HFS, APFS and every other filesystem contained in the logical
image remain owned by their respective canonical filesystem implementations.
Image translation must never become a second parser for those filesystems.

## Safety and failure behaviour

Container offsets, lengths, segment maps and sparse regions are untrusted.
Integer overflow, overlapping/out-of-range extents, truncated segments,
unsupported container variants and path escape must fail closed.

Forensic use is read-only by default. A translated view must not modify the
source evidence merely because the host filesystem permits writes.

## Completion rule

The current entry is complete only as an external userspace image-provider
catalogue contract. It is not a project-authored filesystem implementation.

A first-party replacement requires an explicit roadmap decision, independent
container fixtures, malformed-image tests and preservation checks proving that
the source image is not modified by read-only access.
