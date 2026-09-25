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
- [Bcachefs](../native/filesystems/bcachefs/DESIGN.md) — large
  upstream Linux implementation retained as reference evidence only; the target
  remains one host-neutral Bcachefs engine with thin native platform adapters.

## Structurally reviewed userspace-only entries

- [AFUSE](../native/filesystems/afuse/DESIGN.md) — catalogue-managed
  userspace/FUSE automounter. There is no native disk-format core or kernel
  module to invent; any future first-party implementation belongs behind the
  shared userspace-service boundary.
- [ArchiveMount](../native/filesystems/archivemount/DESIGN.md) —
  catalogue-managed userspace archive namespace. Any future first-party
  implementation belongs at the userspace-service/archive-adapter boundary,
  not in a kernel filesystem.
- [AVFS](../native/filesystems/avfs/DESIGN.md) — userspace virtual
  namespace spanning archives, images and remote locations. It belongs behind
  the shared userspace-service boundary, not in a kernel filesystem.

## Structurally reviewed external-provider entries

- [APFS-DKMS](../native/filesystems/apfs-dkms/DESIGN.md) — Debian's
  experimental out-of-tree APFS provider. It does not own APFS semantics in
  this repository; a future first-party APFS implementation must be one
  canonical APFS engine shared by native platform adapters.
- [APFS-FUSE](../native/filesystems/apfs-fuse/DESIGN.md) — conservative
  userspace APFS access through Debian's libfsapfs provider. It is a fallback
  provider identity, not an independent APFS implementation.

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
