# ArchiveMount Design and Ownership

## Classification

ArchiveMount is a userspace/FUSE archive namespace provider, not an on-disk
filesystem format.

Filesystem Support currently exposes Debian's `archivemount` package as a
userspace provider capable of presenting many archive/compressed formats as a
filesystem hierarchy. The archive container/codec is the data format; the
mount namespace is a userspace presentation layer.

## Current implementation state

There is no first-party ArchiveMount implementation in Filesystem Support.
The previous `.gitkeep` represented no code and has been removed.

Provider discovery and package actions belong to the product catalogue/action
backend. This directory exists to record the ownership boundary and prevent an
empty placeholder from being mistaken for unfinished native filesystem code.

## Target architecture

If the project later implements equivalent functionality, the correct shape is
userspace-only:

```text
native/filesystems/archivemount/
  DESIGN.md
  userspace/            archive namespace/provider glue, if admitted
```

Generic archive decoding should not be embedded in a kernel module. Reusable
archive-reader, bounded-I/O, mount-service, process-lifetime and namespace
mechanisms belong in shared userspace infrastructure when their contracts are
not ArchiveMount-specific.

There is deliberately no planned `kernel/` directory and no disk-format
`core/` in this provider identity.

## Ownership rules

A future first-party implementation may own:

- mapping archive entries to a filesystem-style namespace;
- directory synthesis and path-normalisation policy;
- read/write capability policy for supported archive classes;
- persistence/update semantics for mutable archives;
- explicit treatment of links, metadata and unsupported entry types.

Archive codec semantics should remain in format-appropriate reusable components
rather than being duplicated per mount provider.

## Safety

Archive paths and metadata are untrusted input. Any future implementation must
reject traversal outside the mounted namespace, invalid path components,
integer/size overflows, malformed archive records and unsupported mutation
states. It must never construct shell commands from archive names or paths.

## Completion rule

The present entry is complete only as an external-provider catalogue contract.
It is not a Filesystem Support native filesystem implementation and must not be
counted as one.
