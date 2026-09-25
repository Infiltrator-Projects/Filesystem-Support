# APFS-FUSE Provider Ownership

## Classification

`apfs-fuse` is a Filesystem Support **external userspace provider identity**
for APFS. It is not a separate filesystem format and must not own an
independent APFS implementation.

The current catalogue uses Debian's `libfsapfs-utils` package as the
conservative userspace/FUSE access path.

## One APFS rule

APFS filesystem semantics may be implemented only once by Filesystem Support.

The provider identities `apfs-fuse` and `apfs-dkms` describe two external
ways to access the same filesystem while the first-party implementation is
unfinished. They must not grow separate parsers, allocation logic, object
models or repair/write behaviour.

A future Filesystem Support APFS implementation belongs in one canonical APFS
filesystem tree and is then consumed by the applicable platform adapters.

## Current implementation state

There is no first-party source in this directory. The previous `.gitkeep`
represented no implementation and has been removed.

The current external provider is discovered and installed through the product
catalogue and package/action backend. This directory exists only to document
that ownership boundary.

## Intended directory shape

```text
native/filesystems/apfs-fuse/
  DESIGN.md
```

There is intentionally no `core/` and no `kernel/` here.

If a source snapshot of libfsapfs is ever retained for compatibility study, it
must live below an explicit `reference/<origin>/` directory with its original
licence and provenance intact. Reference source must never be linked or
described as the project APFS engine.

## Provider behaviour

The external FUSE provider may be useful for conservative access when native
support is unavailable or unsuitable. Its existence does not establish
Filesystem Support read/write completeness, recovery capability or safety for
all APFS feature combinations.

The catalogue must keep provider-specific capability and limitation text
separate from future canonical APFS capability.

## Future first-party APFS architecture

If APFS is promoted into first-party development, the project should establish
one canonical APFS engine containing filesystem-defined behaviour such as
container/object-map/checkpoint interpretation, B-tree semantics, allocation
and snapshot/clone rules, integrity validation and any safe mutation/recovery
logic.

Linux, Windows and userspace access paths must consume that same engine rather
than reproducing APFS rules separately.

## Completion rule

This entry is complete as an external-provider catalogue contract when package
discovery, installation/removal policy, capability reporting and documentation
agree.

It must not be counted as a Filesystem Support APFS implementation.
