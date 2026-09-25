# guestmount / libguestfs Design and Ownership

## Classification

guestmount is a userspace virtualisation/image access provider built around
libguestfs. It exposes filesystems contained inside guest disk images; it is not
itself an on-disk filesystem format.

## Current state

Filesystem Support contains no first-party guestmount implementation. The
former `.gitkeep` has been removed; Debian's `guestmount`/libguestfs path
remains external.

## Target architecture

If equivalent functionality is implemented, it belongs at an image/container
and userspace-service boundary:

```text
native/filesystems/guestmount/
  DESIGN.md
  userspace/            guest-image discovery/exposure glue, if implemented
```

Contained EXT, NTFS, FAT, XFS or other filesystem semantics remain exclusively
with those canonical filesystem engines. guestmount code must not duplicate
their parsers or mutation logic.

## Safety

Guest images are untrusted. Partition/container discovery, offsets/sizes and
nested format traversal must be bounded. Writable exposure requires explicit
copy-on-write/direct-write policy and must not silently mutate evidence images.

## Completion rule

The current entry is an external userspace virtualisation-provider contract
only.
