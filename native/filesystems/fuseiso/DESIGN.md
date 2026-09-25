# fuseiso Provider Ownership

## Classification

fuseiso is an external userspace/FUSE provider for ISO images and several
single-track image/container forms. It is an access mechanism, not a separate
ISO9660 filesystem.

## One ISO9660 rule

Filesystem Support may implement ISO9660 semantics only once under the
canonical `native/filesystems/iso9660/` identity.

A future project userspace ISO mount belongs under `iso9660/userspace/` and
consumes the same canonical ISO9660 core as native adapters. Additional image
container decoding should remain a separate reusable container concern rather
than being confused with ISO9660 itself.

## Current state

There is no project-authored source in this directory. The former `.gitkeep`
has been removed; Debian's `fuseiso` package remains the external provider.

## Intended shape

```text
native/filesystems/fuseiso/
  DESIGN.md
```

There is intentionally no `core/` or `kernel/`.

## Completion rule

This entry is complete only as an external provider catalogue contract.
