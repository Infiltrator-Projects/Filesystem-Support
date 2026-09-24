#ifndef INFILTRATR_EXT4_TRUNCATE_H
#define INFILTRATR_EXT4_TRUNCATE_H

/*
 * Linux-facing truncation helpers. Mapping removal itself remains owned by
 * the EXT4 implementation; these helpers only bind that work to VFS cache
 * invalidation and transaction-credit accounting.
 */
static inline void ext4_truncate_failed_write(struct inode *inode)
{
	struct address_space *mapping = inode->i_mapping;

	filemap_invalidate_lock(mapping);
	truncate_inode_pages(mapping, inode->i_size);
	ext4_truncate(inode);
	filemap_invalidate_unlock(mapping);
}

static inline unsigned long ext4_blocks_for_truncate(struct inode *inode)
{
	ext4_lblk_t needed =
		inode->i_blocks >> (inode->i_sb->s_blocksize_bits - 9);

	if (needed < 2)
		needed = 2;
	if (needed > EXT4_MAX_TRANS_DATA)
		needed = EXT4_MAX_TRANS_DATA;

	return EXT4_DATA_TRANS_BLOCKS(inode->i_sb) + needed;
}

#endif
