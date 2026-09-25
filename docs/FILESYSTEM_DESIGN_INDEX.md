# Filesystem Design Documents

Filesystem design documents record format/protocol facts, current ownership
state and the intended canonical-engine versus platform boundary. A design
document does not by itself claim that the filesystem has been rewritten or
qualified.

## Structurally reviewed reference-state filesystems

- [9P](../native/filesystems/9p/DESIGN.md) — Linux V9FS retained only as
  reference evidence; the target Filesystem Support implementation is a
  canonical 9P core behind the shared userspace-service boundary.
- [CephFS](../native/filesystems/cephfs/DESIGN.md) — Linux kernel client
  retained as distributed-filesystem reference evidence; the target is a
  canonical CephFS client core behind the shared userspace-service boundary.
- [SMB/CIFS](../native/filesystems/cifs/DESIGN.md) — Linux SMB client/common
  sources retained as network-filesystem reference evidence; future project
  code uses one canonical SMB client core behind the userspace service.
- [ADFS](../native/filesystems/adfs/DESIGN.md) — Linux ADFS retained only as
  reference evidence; the target is one canonical ADFS engine with thin native
  Linux and Windows adapters.
- [Bcachefs](../native/filesystems/bcachefs/DESIGN.md) — large
  upstream Linux implementation retained as reference evidence only; the target
  remains one host-neutral Bcachefs engine with thin native platform adapters.
- [BeFS](../native/filesystems/befs/DESIGN.md) — upstream Linux BeFS
  retained as read-oriented reference evidence; the target is one canonical
  BeFS engine with thin Linux/Windows adapters.
- [SCO BFS](../native/filesystems/bfs/DESIGN.md) — upstream Linux BFS retained
  only as reference evidence; the target is one canonical Boot File System
  engine with thin Linux/Windows adapters.
- [Btrfs](../native/filesystems/btrfs/DESIGN.md) — large upstream Linux
  implementation retained as reference evidence only; future project code must
  use one canonical Btrfs engine with thin host adapters.
- [CramFS](../native/filesystems/cramfs/DESIGN.md) — upstream Linux
  read-only CramFS retained as reference evidence; the target is one canonical
  compressed-image filesystem engine with thin native adapters.

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
- [bindfs](../native/filesystems/bindfs/DESIGN.md) — userspace overlay
  that remaps ownership/permission presentation over an existing tree; no
  independent disk-format or kernel engine belongs here.
- [ConvmvFS](../native/filesystems/convmvfs/DESIGN.md) — userspace
  filename-charset translation overlay. Underlying filesystem semantics remain
  with the filesystem being mirrored.

## Structurally reviewed external-provider entries

- [APFS-DKMS](../native/filesystems/apfs-dkms/DESIGN.md) — Debian's
  experimental out-of-tree APFS provider. It does not own APFS semantics in
  this repository; a future first-party APFS implementation must be one
  canonical APFS engine shared by native platform adapters.
- [APFS-FUSE](../native/filesystems/apfs-fuse/DESIGN.md) — conservative
  userspace APFS access through Debian's libfsapfs provider. It is a fallback
  provider identity, not an independent APFS implementation.
- [CephFS via FUSE](../native/filesystems/ceph-fuse/DESIGN.md) —
  external userspace CephFS client provider. CephFS semantics belong to one
  canonical CephFS implementation, not a FUSE-specific fork.

## Structurally reviewed storage/container entries

- [BitLocker](../native/filesystems/bitlocker/DESIGN.md) — encrypted Windows
  volume/container. A future first-party implementation may own a portable
  container/decryption core, but the decrypted filesystem remains NTFS and is
  handled by the NTFS implementation.

## Structurally reviewed tools-only / future-native entries

- [CP/M](../native/filesystems/cpm/DESIGN.md) — currently exposed through
  cpmtools only, but it is a real disk-filesystem family and therefore reserves
  a future canonical core plus thin native platform adapters.

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
