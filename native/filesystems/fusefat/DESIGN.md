# fusefat Provider Ownership

## Classification

fusefat is an external userspace/FUSE provider that offers unprivileged access
to FAT12, FAT16, FAT32 and exFAT media. It spans two Filesystem Support
filesystem identities: the classic FAT family and exFAT.

## One-engine rule

This provider directory must not own filesystem semantics.

Classic FAT semantics belong only in `native/filesystems/fat/core/`; exFAT
semantics belong only in `native/filesystems/exfat/core/`.

If first-party userspace mounting is later implemented, it belongs under the
owning filesystem's `userspace/` adapter and consumes that filesystem's core.

## Current state

There is no project-authored fusefat source. The former `.gitkeep` has been
removed; Debian's `fusefat` package remains the current external provider.

## Intended shape

```text
native/filesystems/fusefat/
  DESIGN.md
```

There is intentionally no `core/` or `kernel/`.

## Completion rule

This entry is complete only as an external provider catalogue contract and is
never counted as a separate FAT or exFAT implementation.
