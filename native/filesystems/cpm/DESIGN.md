# CP/M Filesystem Design and Ownership

## Classification

The `cpm` catalogue identity represents CP/M filesystem media/images. Unlike
an overlay or archive mount, this is a real family of disk formats.

The current product path is tools-only through Debian's `cpmtools`; there is
no first-party filesystem engine yet.

## Current state

The previous `.gitkeep` represented no implementation and has been removed.
This directory now records the intended architecture before code is admitted.

## Target layout

```text
native/filesystems/cpm/
  DESIGN.md
  core/                 canonical CP/M disk-format/filesystem semantics
  linux/                thin Linux VFS/module adapter when implemented
  windows/              thin Windows IFS/WDK adapter when implemented
  reference/            optional external format/source evidence
```

## Canonical ownership

CP/M has multiple disk definitions/geometry conventions, so the canonical core
must make format/profile selection explicit rather than embedding one machine's
layout into an OS adapter.

The core should own supported disk parameter/profile interpretation, directory
entries/extents, block allocation, filename/user-area semantics, file-size
reconstruction, free-space validation and exact mutation/recovery rules.

OS adapters own block/device I/O integration and native filesystem objects only.

## Development sequence

1. define the exact CP/M variants/profiles to support;
2. build host-neutral read-only image/media parsing;
3. qualify against independently produced images and malformed fixtures;
4. add directory/file read access;
5. add a Linux adapter only after the core is stable;
6. add write support only with exact allocation/update/recovery qualification;
7. add Windows against the same canonical core.

The external cpmtools path remains useful as an interoperability oracle while
the native implementation is incomplete.

## Failure policy

Unknown geometry/profile, impossible extent chains, duplicate/conflicting
allocation, out-of-range blocks and ambiguous directory state must fail closed.

## Completion rule

The current catalogue entry remains tools-only. A project-native CP/M
filesystem is not claimed until the canonical core and relevant adapter have
their own tests and qualification evidence.
