# Fosfat / Smaky Filesystem Design and Ownership

## Classification

Fosfat provides access to Smaky-formatted disks. This catalogue identity
represents the actual retro disk filesystem, not merely a delivery mechanism.

The currently packaged Debian implementation is userspace/FUSE and read-only,
but a future first-party implementation should still follow the one-canonical-
filesystem rule.

## Current state

Filesystem Support contains no first-party Fosfat/Smaky engine. The former
`.gitkeep` has been removed. Debian's `fosfat` package remains the current
provider.

## Target layout

```text
native/filesystems/fosfat/
  DESIGN.md
  core/                 canonical Smaky/Fosfat format/read semantics
  userspace/            userspace/FUSE adapter over the core
  linux/                optional thin native Linux adapter if justified
  windows/              optional thin Windows adapter if justified
  reference/            optional external format/source evidence
```

## Ownership

The canonical core owns only filesystem-defined media geometry, allocation,
directory/file records, names, timestamps/attributes and validation rules.

FUSE callbacks, block-device APIs and OS-specific filesystem objects remain
adapter concerns.

## Read-only baseline

The existing provider is read-only. A project implementation should begin
read-only as well. Write support must not be inferred from a parser: it requires
exact allocation/update rules plus recovery/verification evidence.

## Completion rule

The present catalogue path is external and read-only. A native project
implementation is not claimed until the canonical core and an adapter have
independent compatibility/malformed-media tests.
