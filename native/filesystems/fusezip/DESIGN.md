# fusezip Design and Ownership

## Classification

fusezip presents ZIP archives as a userspace/FUSE filesystem and can update
supported archives. ZIP is an archive/container format, not a block-device
filesystem.

The current catalogue delegates this capability to Debian's `fuse-zip`
package.

## Current state

Filesystem Support contains no first-party fusezip implementation. The former
`.gitkeep` represented no source and has been removed.

## Target architecture

If a first-party ZIP filesystem view is admitted, it belongs at the userspace
boundary:

```text
native/filesystems/fusezip/
  DESIGN.md
  userspace/            ZIP namespace/update provider, if implemented
```

Reusable ZIP parsing/creation should live in a suitable archive/container
component if shared elsewhere; kernel filesystem code must not embed it.

## Safety

Archive paths are untrusted. Path traversal, duplicate/colliding names,
oversized expansion, malformed central-directory records and failed archive
rewrite/publication must be handled explicitly. Mutable archive updates require
atomic/durable publication rather than in-place corruption-prone assumptions.

## Completion rule

The current entry is an external userspace-provider catalogue contract only.
