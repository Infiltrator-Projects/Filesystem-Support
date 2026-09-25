# Filesystem Design Documents

Filesystem design documents record format/protocol facts, current ownership
state and the intended canonical-engine versus platform boundary. A design
document does not by itself claim that the filesystem has been rewritten or
qualified.

## Structurally reviewed reference-state filesystems

- [9P](../native/filesystems/9p/DESIGN.md) — Linux V9FS retained only as
  reference evidence; the target Filesystem Support implementation is a
  canonical 9P core behind the shared userspace-service boundary.
- [ADFS](../native/filesystems/adfs/DESIGN.md) — Linux ADFS retained only as
  reference evidence; the target is one canonical ADFS engine with thin native
  Linux and Windows adapters.

## Active canonical/rewrite filesystems

- [EXT2](../native/filesystems/ext2/DESIGN.md)
- [EXT3](../native/filesystems/ext3/DESIGN.md)
- [EXT4](../native/filesystems/ext4/DESIGN.md)
- [Amiga OFS](../native/filesystems/ofs/DESIGN.md)
- [Amiga FFS](../native/filesystems/ffs/DESIGN.md)
- [Amiga SFS](../native/filesystems/sfs/DESIGN.md)
- [Amiga SFS2](../native/filesystems/sfs2/DESIGN.md)
- [Amiga PFS3](../native/filesystems/pfs3/DESIGN.md)

Each document separates the filesystem/protocol design from current
implementation state. Project-authored code uses Filesystem Support
responsibility boundaries rather than inherited upstream translation-unit
names.
