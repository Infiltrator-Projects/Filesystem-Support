# GVfs FUSE Bridge Design and Ownership

## Classification

GVfs-FUSE is a desktop/userspace bridge. It exposes existing GVfs/GIO-backed
mounts through a FUSE namespace so applications that do not use GIO can access
them.

It is not an on-disk filesystem and it is not an implementation of FTP, SFTP,
SMB, WebDAV or the other protocols GVfs may provide.

## Current state

Filesystem Support contains no first-party GVfs-FUSE implementation. The former
`.gitkeep` has been removed; Debian's `gvfs-fuse` remains the provider.

## Target architecture

```text
native/filesystems/gvfs-fuse/
  DESIGN.md
  userspace/            desktop bridge/provider glue, if ever implemented
```

There is deliberately no `kernel/` or filesystem-format `core/`.

Underlying protocol and filesystem semantics remain with their owning
providers. A bridge may translate namespace, metadata and lifecycle between
GVfs/GIO and the shared userspace mount surface, but it must not duplicate the
protocol engines beneath GVfs.

## Completion rule

The current entry is an external desktop userspace-provider catalogue contract
only.
