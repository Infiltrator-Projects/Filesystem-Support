<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# ExtFS roadmap

## 0.1–0.2 — reader and native Windows read-only IFS

- Portable ext2/ext3/ext4 parsing/traversal and native WDK filesystem recognition. **Implemented.**
- Native read/query/enumeration, VCB/shared-FCB/CCB model and removable-volume verification. **Implemented.**

## 0.3 — bounded data writes

- Same-size overwrite of allocated initialized regular-file data with whole-range preflight. **Implemented.**
- Windows `IRP_MJ_WRITE`, flush and serialized sector RMW. **Implemented.**
- Windows WDK + Code Analysis 0 warnings / 0 errors. **Verified.**

## 0.4 — ext2 allocation and resize

- Direct-block allocate/free, bitmap/counter/inode maintenance and zero-fill growth. **Implemented.**
- Windows append/extend and `FileEndOfFileInformation`. **Implemented.**
- Windows WDK + Code Analysis 0 warnings / 0 errors. **Verified.**

## 0.5 — JBD2 transaction foundation

- Clean internal JBD2 parser and synchronous descriptor/data/commit/checkpoint writer. **Implemented.**
- Checksum-v2/v3, 64-bit tags, escaped magic, timestamps and durability failure handling. **Implemented.**
- Windows WDK + Code Analysis 0 warnings / 0 errors. **Verified.**

## 0.6 — journaled ext3 direct-file resize

- Legacy direct-file inode/bitmap/group/superblock changes through JBD2. **Implemented.**
- Ordered zero-fill and fail-closed recovery handling. **Implemented.**
- Windows WDK + Code Analysis 0 warnings / 0 errors. **Verified 11 August 2026.**

## 0.7 — checksum-aware ext4 single-inline-extent resize

- Journaled growth/shrink of one initialized depth-0 extent in `inode.i_block`. **Implemented.**
- Rebuild block-bitmap, group-descriptor, inode and superblock `metadata_csum` values. **Implemented.**
- Portable verification completed; Windows qualification skipped before development continued.

## 0.8 — multi-extent inline-root allocation

- Zero through four initialized inline extents, first-extent creation, merge/append and shrink. **Implemented.**
- Fragmented EOF append within the inode-resident extent root. **Implemented.**
- Portable verification completed; Windows qualification skipped before development continued.

## 0.9 — bounded external extent tree and audit hardening

- 0.9.1 GPL-3.0-or-later/SPDX standardisation checkpoint. **Released.**
- Promote a full inline root to one external depth-0 leaf behind a depth-1 inode index. **Implemented.**
- Append/merge initialized extents within that single leaf and collapse back to inline on shrink. **Implemented.**
- Validate/rebuild external extent-block checksum tails using `eh_max`. **Implemented.**
- Account for allocation/free of the extent metadata block in bitmap/counters/`i_blocks`. **Implemented.**
- 0.9.2 ext2 crash-consistency barriers: dirty marker flush, mutation flush, clean-marker flush. **Implemented.**
- 0.9.2 destructive real-image ext2/ext3/ext4 grow/shrink qualification followed by reopen and `e2fsck`. **Implemented in CI; qualification result recorded in `VERIFICATION.md`.**
- 0.9.2 Windows WDK + Code Analysis CI path. **Implemented; qualification result recorded in `VERIFICATION.md`.**
- 0.9.3 Windows FCB lifetime reclamation and removable-media read-only-state resynchronisation. **Implemented.**
- 0.9.3 Infiltratr Common 1.9.0 kernel-safe annotation dependency and release-metadata cleanup. **Implemented.**

## 0.10 — general ext4 extent allocation

- Multiple depth-1 leaf children and index insertion/split/merge.
- Deeper extent-tree split/merge and index propagation.
- 64-bit group descriptors/free-block counters and multi-group allocation.
- Unwritten-extent allocation/conversion where safe.

## 0.11 — namespace mutation and recovery

- Inode allocation/free and create/delete/rename/link/mkdir/rmdir.
- Directory growth/index mutation and persistent timestamps/checksums.
- Dirty-journal replay/recovery and crash-injection qualification.

## 1.0 — qualified read/write data driver

- Qualified feature matrix, cache/paging/dismount work and recovery guidance.
- Signed x64 and ARM64 packages with broad Windows/hardware testing.

## Later — system-volume research

NT security descriptors, ADS/reparse mapping, boot-start/WinPE/Setup and an ext-aware bootstrap come only after the data filesystem is independently crash-safe.
