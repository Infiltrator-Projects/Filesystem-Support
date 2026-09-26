# UDFclient Provider Ownership

## Classification

UDFclient is a userspace/tooling access path for the UDF filesystem format. It
provides independent userland creation/inspection/access tooling, but it is not
a second UDF format.

The Filesystem Support catalogue currently delegates this capability to the
external `udfclient` package and classifies it as tools-only.

## Current implementation state

Filesystem Support contains no first-party UDFclient implementation. The
previous `.gitkeep` represented no implementation or useful ownership boundary
and is removed by this layout pass.

## Intended directory shape

```text
native/filesystems/udfclient/
  DESIGN.md
```

There is intentionally no independent `core/`, `linux/`, `windows/` or
`kernel/` implementation here.

Any future project-owned UDF userspace inspection or tooling belongs under the
canonical UDF implementation, for example:

```text
native/filesystems/udf/userspace/
```

and must consume `udf/core/`.

## Ownership rule

UDF descriptor parsing, volume/partition semantics, directory/inode rules,
allocation and mutation/recovery belong exclusively to the canonical UDF core.

A userland tool may own command presentation, image/device I/O adaptation,
interactive inspection and format/verification workflows, but not a parallel
UDF parser.

## Completion rule

This entry is complete only as an external tools-provider catalogue contract.
It is not a project-authored UDF filesystem implementation.
