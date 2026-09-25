# LoggedFS Design and Ownership

## Classification

LoggedFS is a userspace/FUSE observation overlay that records filesystem
operations while forwarding them to an underlying directory tree. It is not an
on-disk filesystem format and does not own the backing filesystem's storage
semantics.

Filesystem Support must therefore treat LoggedFS as a userspace tooling/provider
identity rather than a native block filesystem.

## Current implementation state

Filesystem Support contains no first-party LoggedFS implementation.

The current product path is an external provider managed through the catalogue
and action backend. The former `.gitkeep` represented no implementation and
is removed by this layout pass.

## Intended layout

```text
native/filesystems/loggedfs/
  DESIGN.md
  userspace/            logging/observation overlay policy, if ever implemented
```

There is deliberately no disk-format `core/`, Linux VFS filesystem engine or
project kernel module here.

## Ownership

A future first-party equivalent may own only the observation layer: operation
selection, event representation, filtering, ordering and forwarding semantics.

It must not duplicate the backing filesystem's parser, allocation, namespace,
durability or recovery implementation. Generic mount-service lifecycle, path
validation and structured logging facilities should be shared when their
contracts are provider-neutral.

## Safety and failure behaviour

Paths and metadata are untrusted. Logging must not change the meaning or
ordering of successfully forwarded filesystem operations, must bound log data,
must avoid recursive logging loops, and must make explicit whether logging
failure can ever fail the forwarded operation.

Sensitive path or metadata logging requires an explicit privacy policy.

## Completion rule

The current entry is complete only as an external userspace tooling/provider
catalogue contract. It is not a project-authored filesystem implementation.
