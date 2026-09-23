<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Feature support on main after 0.9.3

The 0.9.3 maintenance release remains the published baseline. Current `main` adds the first bounded classic single-indirect resize tranche for ext2/ext3 while retaining the conservative fail-closed mutation policy.

| Capability | Status | Boundary |
|---|---|---|
| ext2 read/traverse | Supported | Validated supported layouts |
| ext3 read/traverse | Supported | Clean supported layouts |
| ext4 read/traverse | Supported subset | Unsupported modern layouts fail closed |
| Same-size regular-file overwrite | Supported subset | Existing allocated initialized blocks only; persistent inode timestamps are deferred |
| ext2 classic-file growth/truncate | Experimental | Twelve direct blocks plus exactly one classic single-indirect block; double/triple-indirect mutation refused; operations touching the indirect allocator/free path are bounded to one block group per resize |
| ext3 classic-file growth/truncate | Experimental | Clean internal JBD2; twelve direct blocks plus exactly one classic single-indirect block; double/triple-indirect mutation refused; one allocation/free group per resize |
| ext2/ext3 single-indirect accounting | Implemented bounded path | The indirect metadata block is allocated/freed in the block bitmap and included in `i_blocks`; newly exposed data is zeroed before size publication |
| ext3 single-indirect JBD2 transaction | Implemented bounded path | Bitmap, group descriptor, inode, indirect block when retained/created, and primary superblock are committed in one conservative JBD2 transaction |
| ext4 inline-root growth/truncate | Experimental | Zero to four initialized depth-0 extents in `inode.i_block` |
| ext4 depth-1 extent-tree growth/truncate | Experimental | Exactly one external depth-0 leaf and one root index; dense initialized extents only |
| ext4 inline-to-external promotion | Experimental | Full four-entry inline root can move to one newly allocated external leaf |
| ext4 external-to-inline collapse | Experimental | Shrink collapses to inode root when four or fewer extents remain |
| ext4 resize RO_COMPAT profile | Bounded | `sparse_super`, `large_file`, `btree_dir`, `extra_isize`, `metadata_csum`; `huge_file`, `dir_nlink` and other unlisted RO_COMPAT bits are refused for metadata resize |
| ext4 metadata checksums | Mutation support | Bitmap, group descriptor, inode, primary superblock and external extent leaf rebuilt/verified |
| ext4 extent-block checksum tail | Supported | Tail located from `eh_max`; not assumed to be final four bytes of block |
| Synthetic direct↔single-indirect qualification | CI coverage | ext2 and ext3 12→13→12 block transitions verify bitmap, indirect pointer, `i_blocks`, free-block counters and clean post-transaction state |
| Real-image destructive resize qualification | CI coverage | Disposable `mke2fs` ext2/ext3 images grow beyond the 12-block boundary and shrink back; ext4 remains on its bounded extent-tree fixture; all are reopened/content-compared and checked with `e2fsck` |
| ext2/ext3 double/triple-indirect mutation | Refused | Allocation/freeing of multi-level classic pointer trees is not implemented yet |
| Multiple external ext4 leaves / depth > 1 | Refused | Split/merge/index propagation not implemented yet |
| 64-bit/flex_bg ext4 allocation | Refused | 64-byte descriptors and broader placement/accounting not implemented |
| Multi-group allocation per bounded indirect/ext3/ext4 resize | Refused | One bitmap/descriptor pair per resize transaction; the legacy ext2 direct-only path retains its existing behavior |
| Sparse/unwritten allocation | Refused | No hole allocation or unwritten conversion yet |
| External inode xattr block during resize | Refused | Bounded `i_blocks` accounting does not yet include it |
| Create/delete/rename/mkdir/rmdir | Refused | Namespace mutation not implemented |
| Persistent file mtime/ctime updates | Deferred | Planned with inode metadata mutation semantics |
| Dirty-journal replay | Refused | Recovery engine not implemented |
| External JBD2 journal | Refused | Internal journal only |
| JBD2 checksum v2/v3 | Transaction writer supports | CRC32C only |
| JBD2 async/fast commit/checksum v1 | Refused | Not implemented |
| Paging writes | Refused | Cached/paging writer not qualified |
| Volume lock/unlock | Implemented | Direct volume handle only; lock succeeds only when no ordinary opens or mapped-section FCBs remain; ownership follows the issuing FILE_OBJECT and releases on unlock/cleanup |
| Locked-volume dismount | Implemented bounded path | `FSCTL_DISMOUNT_VOLUME` requires the issuing handle to own an ExtFS volume lock; unlocked forced-dismount teardown remains deferred |
| Unrepresentable raw ext names | Explicit failure | ext permits arbitrary non-NUL name bytes; Windows enumeration surfaces a name-conversion error rather than silently hiding a live entry |
| `meta_bg`, `bigalloc`, inline data, encrypted/casefold | Refused for mutation | Layout not implemented |
| MMP | Refused for writes | Multi-mount protection not implemented |
