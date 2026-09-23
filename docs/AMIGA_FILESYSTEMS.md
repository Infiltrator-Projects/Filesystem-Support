# Amiga filesystem architecture

Filesystem Support treats these as **five independent canonical filesystem
implementations**:

- OFS
- FFS
- SFS
- SFS2
- PFS3

Historical relationships do not make their on-disk semantics interchangeable.

## Target layout

```text
native/filesystems/ofs/
  core/
  linux/
  windows/

native/filesystems/ffs/
  core/
  linux/
  windows/

native/filesystems/sfs/
  core/
  linux/
  windows/

native/filesystems/sfs2/
  core/
  linux/
  windows/

native/filesystems/pfs3/
  core/
  linux/
  windows/
```

Each `core/` ultimately owns that filesystem's format interpretation,
allocation and mapping, directory and metadata rules, validation, mutation and
recovery semantics. Linux and Windows remain host adapters.

## Source bases

OFS and FFS start from the pinned Linux v6.12.107 `fs/affs` implementation.
Linux upstream combines those formats; Filesystem Support does not. The proven
upstream code was copied into the independent OFS and FFS migration trees and
is modified there. No separate live `native/filesystems/affs/` implementation
is retained: provenance is preserved by the copied source headers, this
document and Git history rather than by maintaining a third duplicate tree.

SFS starts from the real ASFS Linux implementation by Marek Szyprowski and is
kept distinct from SFS2.

SFS2 starts from a real SFS2-capable implementation and owns its
`SFS\\2` format rules separately. It must not be implemented as an SFS mode
when the on-disk rule differs.

PFS3 starts from the real open PFS3 implementation. Its source licence remains
part of the migration contract and must not be obscured by restructuring.

## Binary rule

The intended Linux result is one independently deployable module per
filesystem: `ofs.ko`, `ffs.ko`, `sfs.ko`, `sfs2.ko`, and
`pfs3.ko` as each implementation reaches qualification.

A genuinely identical primitive may be shared through neutral infrastructure.
The small AmigaDOS primitive layer lives at `native/primitives/amiga_dos/`,
outside the filesystem tree. It is not a filesystem implementation and owns no
mount, inode, namespace, allocation-policy or recovery behaviour. OFS and FFS
own those behaviours independently in their own directories.

Sharing a primitive never merges the five filesystem identities.
