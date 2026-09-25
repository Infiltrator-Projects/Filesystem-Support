# ConvmvFS Design and Ownership

## Classification

ConvmvFS is a userspace/FUSE overlay that mirrors an existing filesystem tree
while translating filename character sets. It is not an on-disk filesystem.

The catalogue currently delegates this capability to Debian's
`fuse-convmvfs` package.

## Current state

There is no first-party ConvmvFS implementation in Filesystem Support. The
former `.gitkeep` has been removed because it represented no source.

## Target architecture

```text
native/filesystems/convmvfs/
  DESIGN.md
  userspace/            charset/path overlay policy, if implemented
```

There is deliberately no `kernel/` or disk-format `core/`.

## Ownership

A future implementation may own filename encoding conversion, reversible name
mapping, collision policy and overlay namespace presentation. It must not own
the underlying filesystem's allocation, metadata, directory or recovery
semantics.

Generic Unicode/encoding validation should reuse Common or shared Filesystem
Support primitives where the contract is identical and sufficiently strong.

## Safety

Conversion must be deterministic and round-trip policy must be explicit.
Invalid byte sequences, unrepresentable names, conversion collisions and path
escape attempts must fail predictably rather than silently aliasing different
objects.

## Completion rule

The current entry is an external userspace-provider catalogue contract only,
not a native filesystem implementation.
