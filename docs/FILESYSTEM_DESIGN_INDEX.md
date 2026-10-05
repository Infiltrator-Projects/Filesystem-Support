# Filesystem Design Documents

Filesystem design documents record on-media format facts, current ownership state and the intended canonical-engine/platform-adapter boundary. A design document does not by itself claim that the filesystem has been rewritten, made writable, made bootable or qualified as an InfiltratorOS root.

The product catalogue is now deliberately limited to persistent local-storage filesystem formats. Provider, network, overlay, archive, encryption-container and device-namespace research is not part of the selectable filesystem contract even where historical reference material still exists in the repository.

## Active project-native formats

These formats already have active project-owned implementation work and retain independent filesystem identities:

- [EXT2](../native/filesystems/ext2/DESIGN.md)
- [EXT3](../native/filesystems/ext3/DESIGN.md)
- [EXT4](../native/filesystems/ext4/DESIGN.md)
- [Amiga OFS](../native/filesystems/ofs/DESIGN.md)
- [Amiga FFS](../native/filesystems/ffs/DESIGN.md)
- [Amiga SFS](../native/filesystems/sfs/DESIGN.md)
- [Amiga SFS2](../native/filesystems/sfs2/DESIGN.md)
- [Amiga PFS3](../native/filesystems/pfs3/DESIGN.md)

InfiltratorFS itself is maintained in the separate InfiltratorFS repository and is now a first-class Filesystem Support catalogue identity because it is a real local disk format and intended InfiltratorOS root target.

## In-scope reference-state disk formats

The following reviewed design/reference areas correspond to genuine disk filesystem identities retained by the product catalogue. Imported upstream source is evidence only until independently rewritten into the canonical engine/adapter architecture.

- [ADFS](../native/filesystems/adfs/DESIGN.md)
- [Bcachefs](../native/filesystems/bcachefs/DESIGN.md)
- [BeFS](../native/filesystems/befs/DESIGN.md)
- [SCO BFS](../native/filesystems/bfs/DESIGN.md)
- [Btrfs](../native/filesystems/btrfs/DESIGN.md)
- [SGI EFS](../native/filesystems/efs/DESIGN.md)
- [exFAT](../native/filesystems/exfat/DESIGN.md)
- [F2FS](../native/filesystems/f2fs/DESIGN.md)
- [FAT12/16/32](../native/filesystems/fat/DESIGN.md)
- [GFS2](../native/filesystems/gfs2/DESIGN.md)
- [OCFS2](../native/filesystems/ocfs2/DESIGN.md)
- [Classic HFS](../native/filesystems/hfs/DESIGN.md)
- [OMFS](../native/filesystems/omfs/DESIGN.md)
- [CP/M](../native/filesystems/cpm/DESIGN.md)
- [NTFS](../native/filesystems/ntfs/DESIGN.md)
- [Fosfat / Smaky](../native/filesystems/fosfat/DESIGN.md)

A current provider can be userspace or read-only without changing the format's identity. For example, NTFS-3G and Linux NTFS3 are implementation paths for the single NTFS format and must not acquire separate product identities.

## Provider/reference material outside catalogue scope

Older development work imported or documented a much wider set of Linux mount mechanisms. That historical material does **not** define Filesystem Support product scope.

In particular, design/reference directories for provider duplicates, remote/network/cloud clients, overlays, encrypted containers, archive mounts, virtual/device namespaces or immutable deployment images must not be treated as selectable filesystems, counted in the support matrix, or promoted into independent native modules merely because a directory exists.

Examples include old APFS provider-specific directories (`apfs-fuse`, `apfs-dkms`), NTFS3 provider reference material, FUSE variants, 9P/SMB/Ceph/SSHFS client material, archive/overlay tools and encryption-container research. Where such material is still useful as engineering evidence it is reference-only and subordinate to the real format or to another product boundary. It is not a reason to re-expand the filesystem catalogue.

The long-term cleanup direction is to remove reference-only material that no longer has engineering value rather than letting historical imports dictate architecture.

## Documentation rule

Every catalogue format has exactly one identity in `src/catalog.cpp` and exactly one row in [`FILESYSTEM_SUPPORT_MATRIX.md`](FILESYSTEM_SUPPORT_MATRIX.md). A provider-specific `DESIGN.md` must never create a second filesystem identity.

A design document must separate:

1. on-media format facts;
2. capabilities of an external/current provider;
3. project-native implementation state;
4. Linux/Windows adapter state;
5. EFI/rEFInd reader state; and
6. InfiltratorOS root/setup qualification state.

Those states are not interchangeable. A format can be well documented but unimplemented, mountable but not safely writable, writable but not suitable as `/`, or root-capable in Linux while still lacking an EFI reader.

## Bootable-root rule

For the InfiltratorOS setup path, the format design is only one part of the contract. A filesystem is offered as an installation target only after its formatter, Linux root semantics, early-boot module availability and matching rEFInd/EFI reader are qualified together.

The setup shell consumes the same format identity and capability data as the desktop manager; it does not maintain a second list of filesystems.
