# APFS-DKMS Provider Ownership

## Classification

`apfs-dkms` is a Filesystem Support **catalogue provider identity**, not a
separate filesystem format.

The catalogue currently represents Debian's experimental out-of-tree APFS
kernel driver and tools:

- kernel module identity: `apfs`;
- packages: `apfs-dkms` and `apfsprogs`;
- support provider: DKMS;
- access mode: experimental;
- write support: explicitly treated as experimental/cautionary.

That provider is useful while Filesystem Support lacks a qualified first-party
APFS implementation. It must not become the owner of APFS filesystem semantics.

## One APFS rule

APFS is one filesystem. Filesystem Support must not create separate
implementations named after delivery mechanisms such as `apfs-dkms` and
`apfs-fuse`.

If APFS enters first-party development, its permanent implementation belongs in
a single canonical APFS filesystem tree, with one host-neutral engine and thin
platform adapters. External DKMS and FUSE providers remain compatibility,
reference or fallback providers until that canonical implementation is ready.

This directory therefore must not contain:

- an APFS parser or allocation engine;
- copied APFS kernel-driver source presented as project code;
- a private `core/` that competes with a future canonical APFS engine;
- a Filesystem Support `.ko` merely named after the external package.

## Current implementation state

There is currently no first-party source in this directory. The previous
`.gitkeep` represented no implementation and has been removed.

The external-provider behaviour is owned by the product catalogue and package
action backend in `src/`. This `DESIGN.md` records the boundary so the
provider identity cannot be mistaken for a filesystem implementation.

## Intended directory shape

The provider identity intentionally remains documentation-only:

```text
native/filesystems/apfs-dkms/
  DESIGN.md
```

Any future reference snapshot of the external provider, if deliberately added
for study, must live below an explicit `reference/<origin>/` provenance
boundary and retain its original licensing. It must not be linked as
project-authored production code.

A future project-owned APFS implementation belongs in a single canonical APFS
filesystem directory rather than here.

## Safety

Filesystem Support must continue to describe this provider honestly as
experimental. Installation availability is not evidence that a particular APFS
volume is safe to modify.

The manager must not infer write safety from the presence of the `apfs`
module, `apfs-dkms` package or `apfsprogs` package alone. Provider-specific
limitations remain visible until superseded by independently qualified
first-party support.

## Completion rule

This entry is complete as an **external-provider catalogue contract** when
catalogue detection, package actions, provider state and documentation agree.

It is not and must not be counted as completion of a Filesystem Support APFS
engine.
