# bindfs Design and Ownership

## Classification

bindfs is a userspace/FUSE overlay that mirrors an existing directory while
changing ownership and permission presentation. It is not an on-disk
filesystem format and does not own the underlying filesystem's data layout.

The current catalogue delegates this capability to Debian's `bindfs` package.

## Current state

Filesystem Support contains no first-party bindfs implementation. The former
`.gitkeep` represented no code and has been removed.

## Target architecture

Any future first-party equivalent belongs at the userspace-service boundary:

```text
native/filesystems/bindfs/
  DESIGN.md
  userspace/            bindfs-specific mapping/presentation policy, if admitted
```

There is deliberately no `kernel/` or disk-format `core/` here.

## Ownership

A future implementation may own path mirroring, UID/GID mapping, permission
mask/transformation policy and the exact interaction between presented and
underlying metadata. It must not duplicate the underlying filesystem's parser,
allocation or recovery logic.

Generic mount-service lifecycle, credential handling and path validation belong
in shared userspace infrastructure when reusable.

## Safety

Permission/ownership translation is a security boundary. Mapping must be
deterministic, overflow-safe and explicit about unmapped identities. Path
translation must not escape the configured underlying tree.

## Completion rule

The current entry is an external userspace-provider catalogue contract only.
It is not a first-party filesystem implementation.
