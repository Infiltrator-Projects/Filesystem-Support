# Filesystem Design Documentation Forensic Audit

## Scope

This audit checks the eight Filesystem Support design documents against authoritative or primary documentation for the corresponding formats.

The audit deliberately separates three things:

1. on-disk format facts;
2. product/driver limits published by the operating-system or filesystem distribution;
3. the capabilities currently implemented by Filesystem Support.

Those are not interchangeable. A field may be representable on disk even when the official driver supports a smaller range, and Filesystem Support may currently implement only a subset of a documented format.

## Verification baseline

### EXT2

Compared against the Linux kernel EXT2 filesystem documentation and the EXT-family on-disk documentation. The review covered block groups, superblock placement, backup/sparse-super rules, inode layout, directory records, byte order and feature evolution.

### EXT3

Compared against the Linux kernel EXT3 documentation plus the historical JBD contract/source used by the standalone EXT3 implementation. The review covered EXT2 inheritance, journal byte order, journal headers/superblock, descriptor tag flags, revoke/commit semantics, internal/external journal state and orphan recovery.

### EXT4

Compared against the current Linux kernel EXT4 "Data Structures and Algorithms" documentation, including block groups, superblock features, special inodes, inode format, directories/HTree, checksums, JBD2, fast commit, orphan file and atomic block writes.

### OFS and FFS

Compared against AmigaOS low-level filesystem documentation describing the classic root/user-directory structures, bitmap state, directory hash chains, directory-cache extensions, RDB/DosEnvec filesystem identifiers and current AmigaOS filesystem limit tables.

### SFS and SFS2

Compared against the current Smart File System distribution documentation and AmigaOS filesystem tables, plus low-level SFS structure documentation for roots, block headers, objects, bitmaps, admin space and extent/object trees.

### PFS3

Compared against the PFS3 distribution's own hard-disk structure guide and the current open PFS3aio structure definitions for later large-file/identifier extensions.

## Material findings corrected in the DESIGN.md files

- EXT2 previously blurred the project's 64 KiB geometry acceptance with the historical Linux EXT2 block-size compatibility range. The document now distinguishes them.
- EXT2 was missing byte order, sparse-super selection, lifecycle/state fields, reserved inode conventions and several inode/directory details.
- EXT3 was missing the filesystem-vs-journal endian distinction, external journal layout, orphan-list recovery, JBD superblock fields and descriptor tag flags.
- EXT4 was missing a substantial part of the compatible feature namespace, special inodes, group-descriptor/lazy-init details, extended inode fields, directory/HTree checksum tails, complete checksum coverage, external JBD2, fast-commit record classes, orphan-file layout and modern hardware atomic-write semantics.
- OFS/FFS were missing boot/RDB context, exact classic root/directory layout invariants, bitmap-valid semantics and directory-cache authority rules.
- SFS incorrectly reflected the current core's 105-byte filename constant as if it were the filesystem limit. Current SFS documentation publishes 107-character names. This is now recorded as an implementation discrepancy.
- SFS was missing published block-size, file/partition limits, safe-write guarantee, case-sensitivity option, transparent optimiser/read-ahead and recycled-files feature surface.
- SFS2 previously treated the 48-bit encoded file-size field as the effective format limit. The document now distinguishes encoding width from the officially published 1 TiB supported file-size limit.
- SFS2 still lacks a complete independently verified low-level writer specification for every metadata block class; the design document now states that limitation rather than claiming completeness.
- PFS3 was the least complete document. It now covers boot/root placement, reserved/data areas, two bitmap domains, index blocks, anodes, directory blocks/entries/extensions, links, delete directory, root extension, super-index mode, rollover files and identifier namespace distinctions.

## Documentation-gap closure

The two documentation gaps identified by the first forensic pass have now been closed against the available primary/public implementation surface.

### SFS2

The SFS2 document now records the complete public low-level block family inherited from SFS, the exact SFS2 version-4 deltas, 27-byte object prefix, 16-byte extent leaf, 48-bit encoded file-size layout, empty-volume topology, transaction block family and the distinct `0xFFFFFFFE` whole-block checksum invariant.

The checksum distinction exposed a real project bug: the SFS2 canonical checksum generator was still producing the SFS0 `0xFFFFFFFF` whole-block sum. That implementation has now been corrected and covered by an explicit checksum-sum test.

### PFS3 later extensions

The PFS3 document now includes the later PFS3aio root-extension fields and semantics that were outside the original disk-structure guide: postponed-operation recovery arguments, extended roving state, filename-size state, super-index references, expanded delete-directory metadata, stored DosEnvec geometry, `MODE_STORED_GEOM` and the large-file/`PFS\\2` identity relationship.

### SFS filename limit

The SFS audit also exposed a concrete implementation mismatch. The project used a 105-byte filename ceiling while SmartFileSystem 1.279's public format/handler surface is 107 characters. Both the canonical SFS core and Linux adapter now use 107 and qualification tests cover the exact boundary.

## Completeness statement

For EXT2, EXT3, EXT4, OFS, FFS, SFS, SFS2 and PFS3, each `DESIGN.md` now covers the filesystem structures and feature surface established by the authoritative/public primary documentation used by this project.

This does **not** mean every documented feature is already implemented for reading and writing by Filesystem Support. Documentation completeness, implementation completeness and runtime qualification remain separately tracked engineering states.

## Rule for future documentation changes

A DESIGN.md must not promote an implementation constant into a filesystem-format fact merely because that constant exists in the current code. When an authoritative source gives a product limit that is narrower than the numerical encoding, both must be documented separately.

Likewise, a documented format feature is not automatically a Filesystem Support capability. Read support, write support, recovery support and qualification status must remain explicit.
