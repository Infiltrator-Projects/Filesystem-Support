// SPDX-License-Identifier: GPL-2.0

/*
 * EXT4 — Truncation helpers
 *
 * Purpose:
 *   Provides compact helper contracts used while removing mappings beyond the new end of an inode.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Truncation helpers participate in block-release ordering and therefore inherit the caller's transaction and locking requirements.
 *
 * Project rules:
 *   - Register and implement EXT4 only; do not route EXT2 or EXT3 mounts through this module.
 *   - Preserve every valid EXT4 feature path supported by the pinned implementation.
 *   - Treat journaling, extents, allocation, checksums, recovery and feature negotiation as correctness-critical state machines.
 *
 * Commentary policy:
 *   Comments explain invariants, ownership, persistence ordering and
 *   non-obvious design intent. They deliberately avoid restating C syntax.
 */

/**
 * ext4_truncate_failed_write - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_truncate_failed_write(struct inode *inode)
{
	struct address_space *mapping = inode->i_mapping;


	filemap_invalidate_lock(mapping);
	truncate_inode_pages(mapping, inode->i_size);
	ext4_truncate(inode);
	filemap_invalidate_unlock(mapping);
}


/**
 * ext4_blocks_for_truncate - Implements the blocks for truncate operation within the truncation helpers subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline unsigned long ext4_blocks_for_truncate(struct inode *inode)
{
	ext4_lblk_t needed;

	needed = inode->i_blocks >> (inode->i_sb->s_blocksize_bits - 9);


	if (needed < 2)
		needed = 2;


	if (needed > EXT4_MAX_TRANS_DATA)
		needed = EXT4_MAX_TRANS_DATA;

	return EXT4_DATA_TRANS_BLOCKS(inode->i_sb) + needed;
}
