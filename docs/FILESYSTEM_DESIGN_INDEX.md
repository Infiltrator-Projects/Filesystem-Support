# Filesystem Design Documents

Filesystem design documents record format/protocol facts, current ownership
state and the intended canonical-engine versus platform boundary. A design
document does not by itself claim that the filesystem has been rewritten or
qualified.

## Structurally reviewed reference-state filesystems

- [GFS2](../native/filesystems/gfs2/DESIGN.md) — shared-disk clustered
  filesystem; upstream Linux/DLM client is reference evidence while the target
  is one canonical GFS2 engine with thin host and cluster-lock adapters.


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
- [SGI EFS](../native/filesystems/efs/DESIGN.md) — upstream Linux
  read-only EFS retained as reference evidence; the target is one canonical
  EFS reader with thin host adapters.
- [exFAT](../native/filesystems/exfat/DESIGN.md) — upstream Linux exFAT
  retained as reference evidence; the target is one canonical exFAT engine
  shared by Linux, Windows and any userspace adapter.
- [F2FS](../native/filesystems/f2fs/DESIGN.md) — upstream Linux F2FS
  retained as reference evidence; the target is one canonical flash-filesystem
  engine with thin host adapters.
- [FAT12/16/32](../native/filesystems/fat/DESIGN.md) — upstream Linux
  FAT/MS-DOS/VFAT implementation retained as reference evidence; the target is
  one canonical FAT-family engine with explicit namespace policy.
- [SCO BFS](../native/filesystems/bfs/DESIGN.md) — upstream Linux BFS retained
  only as reference evidence; the target is one canonical Boot File System
  engine with thin Linux/Windows adapters.
- [Btrfs](../native/filesystems/btrfs/DESIGN.md) — large upstream Linux
  implementation retained as reference evidence only; future project code must
  use one canonical Btrfs engine with thin host adapters.
- [CramFS](../native/filesystems/cramfs/DESIGN.md) — upstream Linux
  read-only CramFS retained as reference evidence; the target is one canonical
  compressed-image filesystem engine with thin native adapters.
- [EROFS](../native/filesystems/erofs/DESIGN.md) — upstream Linux
  EROFS retained as reference evidence; the target is one canonical read-only
  compressed filesystem engine shared by native/userspace access paths.
- [eCryptfs](../native/filesystems/ecryptfs/DESIGN.md) — upstream Linux
  stacked-crypto implementation retained as reference evidence; future project
  code separates portable eCryptfs format/crypto semantics from host adapters.

## Structurally reviewed userspace-only entries

- [guestmount](../native/filesystems/guestmount/DESIGN.md) — userspace
  virtual-machine image/container access layer. It must dispatch contained
  filesystems to their owning canonical implementations.


- [GPhotoFS](../native/filesystems/gphotofs/DESIGN.md) — userspace
  camera/PTP device namespace provider; no disk-format engine belongs here.


- [gocryptfs](../native/filesystems/gocryptfs/DESIGN.md) — encrypted
  userspace overlay. A future first-party implementation may have a portable
  encrypted-overlay core plus userspace adapter, never a duplicate backing fs.


- [go-mtpfs](../native/filesystems/go-mtpfs/DESIGN.md) — userspace MTP
  device namespace provider; MTP transport/device semantics belong behind the
  shared userspace-service boundary.


- [GlusterFS](../native/filesystems/glusterfs/DESIGN.md) — distributed
  userspace filesystem client; any first-party implementation belongs behind
  the shared userspace-service boundary.


- [fusezip](../native/filesystems/fusezip/DESIGN.md) — userspace ZIP archive
  namespace. ZIP container semantics and mount presentation remain userspace
  concerns rather than a kernel filesystem.


- [fuse-overlayfs](../native/filesystems/fuse-overlayfs/DESIGN.md) —
  userspace OverlayFS-compatible provider for rootless/container workloads; it
  owns overlay presentation semantics, not an independent disk format.


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
- [disorderfs](../native/filesystems/disorderfs/DESIGN.md) — userspace
  testing overlay that intentionally perturbs metadata/order to expose
  reproducibility assumptions; it owns no disk-format semantics.
- [CryFS](../native/filesystems/cryfs/DESIGN.md) — encrypted
  userspace overlay. Any future first-party implementation belongs in a
  portable encrypted-overlay core plus userspace provider, not a kernel
  filesystem module.
- [EncFS](../native/filesystems/encfs/DESIGN.md) — encrypted userspace
  overlay. Future project code may use a portable encrypted-overlay core plus
  userspace provider, without duplicating backing-filesystem semantics.
- [CurlFtpFS](../native/filesystems/curlftpfs/DESIGN.md) — FTP
  remote-filesystem provider over userspace/FUSE. FTP protocol logic belongs
  behind the shared userspace-service boundary.
- [davfs2](../native/filesystems/davfs2/DESIGN.md) — WebDAV remote
  filesystem provider. WebDAV/HTTP semantics belong behind the shared
  userspace-service boundary, not in a kernel filesystem.

## Structurally reviewed external-provider entries

- [fuseiso](../native/filesystems/fuseiso/DESIGN.md) — external
  userspace ISO/image provider. ISO9660 semantics belong to the canonical
  `iso9660/` implementation, not this provider identity.


- [fusefat](../native/filesystems/fusefat/DESIGN.md) — external FUSE
  provider for FAT12/16/32 and exFAT. Future project userspace paths must consume
  the canonical `fat/` or `exfat/` core rather than duplicate either engine.


- [fuse2fs](../native/filesystems/fuse2fs/DESIGN.md) — external userspace
  provider for EXT2/EXT3/EXT4. It must not become a fourth EXT implementation;
  future project userspace adapters consume each filesystem's own canonical core.


- [APFS-DKMS](../native/filesystems/apfs-dkms/DESIGN.md) — Debian's
  experimental out-of-tree APFS provider. It does not own APFS semantics in
  this repository; a future first-party APFS implementation must be one
  canonical APFS engine shared by native platform adapters.
- [APFS-FUSE](../native/filesystems/apfs-fuse/DESIGN.md) — conservative
  userspace APFS access through Debian's libfsapfs provider. It is a fallback
  provider identity, not an independent APFS implementation.
- [EROFS via FUSE](../native/filesystems/erofsfuse/DESIGN.md) —
  external userspace provider for EROFS. EROFS semantics belong only in the
  canonical EROFS engine.
- [exFAT via FUSE](../native/filesystems/exfat-fuse/DESIGN.md) —
  external userspace provider for exFAT. All exFAT semantics remain in the
  canonical exFAT engine.
- [CephFS via FUSE](../native/filesystems/ceph-fuse/DESIGN.md) —
  external userspace CephFS client provider. CephFS semantics belong to one
  canonical CephFS implementation, not a FUSE-specific fork.

## Structurally reviewed storage/container entries

- [BitLocker](../native/filesystems/bitlocker/DESIGN.md) — encrypted Windows
  volume/container. A future first-party implementation may own a portable
  container/decryption core, but the decrypted filesystem remains NTFS and is
  handled by the NTFS implementation.
- [FileVault / FVDE](../native/filesystems/filevault/DESIGN.md) —
  Apple encrypted-volume/container layer. A future portable decryptor may
  expose a block view, while APFS/HFS filesystem semantics remain elsewhere.

## Structurally reviewed tools-only / future-native entries

- [fscrypt](../native/filesystems/fscrypt/DESIGN.md) — management/policy tooling
  for encryption implemented by filesystems such as EXT4, F2FS and UBIFS; it
  does not own an independent filesystem engine.
- [CP/M](../native/filesystems/cpm/DESIGN.md) — currently exposed through
  cpmtools only, but it is a real disk-filesystem family and therefore reserves
  a future canonical core plus thin native platform adapters.
- [Fosfat / Smaky](../native/filesystems/fosfat/DESIGN.md) — currently
  external read-only userspace access, but the entry represents a real disk
  filesystem and reserves one canonical format engine.

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
