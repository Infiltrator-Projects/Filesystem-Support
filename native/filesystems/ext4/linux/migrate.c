#include <linux/slab.h>

#include "ext4_extents.h"
#include "ext4_jbd2.h"

struct ext4_migrate_run {
	ext4_lblk_t logical_start;
	ext4_lblk_t logical_last;
	ext4_lblk_t logical_cursor;
	ext4_fsblk_t physical_start;
	ext4_fsblk_t physical_last;
};

static int ext4_migrate_flush_run(handle_t *handle, struct inode *inode,
				  struct ext4_migrate_run *run)
{
	struct ext4_extent extent;
	struct ext4_ext_path *path;
	unsigned int length;
	int credits;
	int err = 0;

	if (!run->physical_start)
		return 0;

	length = run->logical_last - run->logical_start + 1;
	memset(&extent, 0, sizeof(extent));
	extent.ee_block = cpu_to_le32(run->logical_start);
	extent.ee_len = cpu_to_le16(length);
	ext4_ext_store_pblock(&extent, run->physical_start);

	down_write(&EXT4_I(inode)->i_data_sem);
	path = ext4_find_extent(inode, run->logical_start, NULL, 0);
	if (IS_ERR(path)) {
		err = PTR_ERR(path);
		path = NULL;
		goto out_unlock;
	}

	credits = ext4_ext_calc_credits_for_single_extent(
		inode, length, path);
	err = ext4_datasem_ensure_credits(
		handle, inode, credits, credits, 0);
	if (err)
		goto out_unlock;

	path = ext4_ext_insert_extent(
		handle, inode, path, &extent, 0);
	if (IS_ERR(path)) {
		err = PTR_ERR(path);
		path = NULL;
	}

out_unlock:
	up_write(&EXT4_I(inode)->i_data_sem);
	ext4_free_ext_path(path);
	run->physical_start = 0;
	return err;
}

static int ext4_migrate_add_block(handle_t *handle, struct inode *inode,
				  ext4_fsblk_t physical,
				  struct ext4_migrate_run *run)
{
	int err;

	if (run->physical_start &&
	    run->physical_last + 1 == physical &&
	    run->logical_last + 1 == run->logical_cursor) {
		run->physical_last = physical;
		run->logical_last = run->logical_cursor++;
		return 0;
	}

	err = ext4_migrate_flush_run(handle, inode, run);
	if (err)
		return err;

	run->physical_start = physical;
	run->physical_last = physical;
	run->logical_start = run->logical_cursor;
	run->logical_last = run->logical_cursor++;
	return 0;
}

static int ext4_migrate_scan_indirect(handle_t *handle, struct inode *inode,
				      ext4_fsblk_t block,
				      unsigned int depth,
				      struct ext4_migrate_run *run)
{
	struct buffer_head *bh;
	__le32 *entries;
	const unsigned int per_block = inode->i_sb->s_blocksize >> 2;
	u64 hole_advance = 1;
	unsigned int i;
	int err = 0;

	if (depth == 0)
		return ext4_migrate_add_block(handle, inode, block, run);

	for (i = 1; i < depth; ++i)
		hole_advance *= per_block;

	bh = ext4_sb_bread(inode->i_sb, block, 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	entries = (__le32 *)bh->b_data;
	for (i = 0; i < per_block; ++i) {
		if (entries[i]) {
			err = ext4_migrate_scan_indirect(
				handle, inode, le32_to_cpu(entries[i]),
				depth - 1, run);
			if (err)
				break;
		} else {
			if (hole_advance > EXT4_MAX_LOGICAL_BLOCK -
			    run->logical_cursor) {
				err = -EFSCORRUPTED;
				break;
			}
			run->logical_cursor += hole_advance;
		}
	}

	put_bh(bh);
	return err;
}

static int ext4_migrate_free_indirect_tree(handle_t *handle,
					    struct inode *inode,
					    ext4_fsblk_t block,
					    unsigned int depth)
{
	struct buffer_head *bh;
	__le32 *entries;
	const unsigned int per_block = inode->i_sb->s_blocksize >> 2;
	unsigned int i;
	int err;

	if (!block)
		return 0;

	if (depth > 1) {
		bh = ext4_sb_bread(inode->i_sb, block, 0);
		if (IS_ERR(bh))
			return PTR_ERR(bh);

		entries = (__le32 *)bh->b_data;
		for (i = 0; i < per_block; ++i) {
			if (!entries[i])
				continue;
			err = ext4_migrate_free_indirect_tree(
				handle, inode, le32_to_cpu(entries[i]),
				depth - 1);
			if (err) {
				put_bh(bh);
				return err;
			}
		}
		put_bh(bh);
	}

	err = ext4_journal_ensure_credits(
		handle, EXT4_RESERVE_TRANS_BLOCKS,
		ext4_free_metadata_revoke_credits(inode->i_sb, 1));
	if (err)
		return err;

	ext4_free_blocks(
		handle, inode, NULL, block, 1,
		EXT4_FREE_BLOCKS_METADATA | EXT4_FREE_BLOCKS_FORGET);
	return 0;
}

static int ext4_migrate_free_old_indirects(handle_t *handle,
					    struct inode *inode,
					    const __le32 saved[3])
{
	int err;

	err = ext4_migrate_free_indirect_tree(
		handle, inode, le32_to_cpu(saved[0]), 1);
	if (err)
		return err;
	err = ext4_migrate_free_indirect_tree(
		handle, inode, le32_to_cpu(saved[1]), 2);
	if (err)
		return err;
	return ext4_migrate_free_indirect_tree(
		handle, inode, le32_to_cpu(saved[2]), 3);
}

static int ext4_migrate_swap_to_extents(handle_t *handle,
					 struct inode *inode,
					 struct inode *temp)
{
	struct ext4_inode_info *dst = EXT4_I(inode);
	struct ext4_inode_info *src = EXT4_I(temp);
	__le32 saved[3];
	int err;
	int mark_err;

	err = ext4_journal_ensure_credits(handle, 1, 0);
	if (err)
		return err;

	saved[0] = dst->i_data[EXT4_IND_BLOCK];
	saved[1] = dst->i_data[EXT4_DIND_BLOCK];
	saved[2] = dst->i_data[EXT4_TIND_BLOCK];

	down_write(&dst->i_data_sem);
	if (!ext4_test_inode_state(inode, EXT4_STATE_EXT_MIGRATE)) {
		up_write(&dst->i_data_sem);
		return -EAGAIN;
	}
	ext4_clear_inode_state(inode, EXT4_STATE_EXT_MIGRATE);

	ext4_set_inode_flag(inode, EXT4_INODE_EXTENTS);
	memcpy(dst->i_data, src->i_data, sizeof(dst->i_data));
	spin_lock(&inode->i_lock);
	inode->i_blocks += temp->i_blocks;
	spin_unlock(&inode->i_lock);
	up_write(&dst->i_data_sem);

	err = ext4_migrate_free_old_indirects(handle, inode, saved);
	mark_err = ext4_mark_inode_dirty(handle, inode);
	if (!err)
		err = mark_err;
	return err;
}

static int ext4_migrate_free_extent_index(handle_t *handle,
					   struct inode *inode,
					   struct ext4_extent_idx *index)
{
	struct buffer_head *bh;
	struct ext4_extent_header *header;
	struct ext4_extent_idx *child;
	ext4_fsblk_t block = ext4_idx_pblock(index);
	unsigned int i;
	int err;

	bh = ext4_sb_bread(inode->i_sb, block, 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	header = (struct ext4_extent_header *)bh->b_data;
	if (le16_to_cpu(header->eh_depth) != 0) {
		child = EXT_FIRST_INDEX(header);
		for (i = 0; i < le16_to_cpu(header->eh_entries); ++i, ++child) {
			err = ext4_migrate_free_extent_index(
				handle, inode, child);
			if (err) {
				put_bh(bh);
				return err;
			}
		}
	}
	put_bh(bh);

	err = ext4_journal_ensure_credits(
		handle, EXT4_RESERVE_TRANS_BLOCKS,
		ext4_free_metadata_revoke_credits(inode->i_sb, 1));
	if (err)
		return err;

	ext4_free_blocks(
		handle, inode, NULL, block, 1,
		EXT4_FREE_BLOCKS_METADATA | EXT4_FREE_BLOCKS_FORGET);
	return 0;
}

static int ext4_migrate_free_extent_tree(handle_t *handle,
					  struct inode *inode)
{
	struct ext4_extent_header *header = ext_inode_hdr(inode);
	struct ext4_extent_idx *index;
	unsigned int i;
	int err;

	if (le16_to_cpu(header->eh_depth) == 0)
		return 0;

	index = EXT_FIRST_INDEX(header);
	for (i = 0; i < le16_to_cpu(header->eh_entries); ++i, ++index) {
		err = ext4_migrate_free_extent_index(handle, inode, index);
		if (err)
			return err;
	}
	return 0;
}

static int ext4_migrate_build_extent_tree(handle_t *handle,
					   struct inode *source,
					   struct inode *temp)
{
	struct ext4_inode_info *ei = EXT4_I(source);
	struct ext4_migrate_run run = { 0 };
	const unsigned int per_block = source->i_sb->s_blocksize >> 2;
	unsigned int i;
	int err;

	for (i = 0; i < EXT4_NDIR_BLOCKS; ++i) {
		if (ei->i_data[i])
			err = ext4_migrate_add_block(
				handle, temp, le32_to_cpu(ei->i_data[i]), &run);
		else {
			run.logical_cursor++;
			err = 0;
		}
		if (err)
			return err;
	}

	if (ei->i_data[EXT4_IND_BLOCK])
		err = ext4_migrate_scan_indirect(
			handle, temp,
			le32_to_cpu(ei->i_data[EXT4_IND_BLOCK]), 1, &run);
	else {
		run.logical_cursor += per_block;
		err = 0;
	}
	if (err)
		return err;

	if (ei->i_data[EXT4_DIND_BLOCK])
		err = ext4_migrate_scan_indirect(
			handle, temp,
			le32_to_cpu(ei->i_data[EXT4_DIND_BLOCK]), 2, &run);
	else {
		run.logical_cursor += (ext4_lblk_t)per_block * per_block;
		err = 0;
	}
	if (err)
		return err;

	if (ei->i_data[EXT4_TIND_BLOCK]) {
		err = ext4_migrate_scan_indirect(
			handle, temp,
			le32_to_cpu(ei->i_data[EXT4_TIND_BLOCK]), 3, &run);
		if (err)
			return err;
	}

	return ext4_migrate_flush_run(handle, temp, &run);
}

int ext4_ext_migrate(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct inode *temp = NULL;
	handle_t *handle;
	uid_t owner[2];
	__u32 goal;
	__u32 original_temp_seed;
	int writepages_ctx;
	int err;

	if (!ext4_has_feature_extents(inode->i_sb) ||
	    ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS) ||
	    ext4_has_inline_data(inode))
		return -EINVAL;
	if (S_ISLNK(inode->i_mode) && inode->i_blocks == 0)
		return 0;

	writepages_ctx = ext4_writepages_down_write(inode->i_sb);
	handle = ext4_journal_start(
		inode, EXT4_HT_MIGRATE,
		3 + EXT4_MAXQUOTAS_TRANS_BLOCKS(inode->i_sb));
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out_unlock;
	}

	goal = ((inode->i_ino - 1) /
		EXT4_INODES_PER_GROUP(inode->i_sb)) *
		EXT4_INODES_PER_GROUP(inode->i_sb) + 1;
	owner[0] = i_uid_read(inode);
	owner[1] = i_gid_read(inode);

	temp = ext4_new_inode(
		handle, d_inode(inode->i_sb->s_root),
		S_IFREG, NULL, goal, owner, 0);
	if (IS_ERR(temp)) {
		err = PTR_ERR(temp);
		temp = NULL;
		ext4_journal_stop(handle);
		goto out_unlock;
	}

	original_temp_seed = EXT4_I(temp)->i_csum_seed;
	EXT4_I(temp)->i_csum_seed = ei->i_csum_seed;
	i_size_write(temp, i_size_read(inode));
	clear_nlink(temp);
	ext4_ext_tree_init(handle, temp);
	ext4_journal_stop(handle);

	down_write(&ei->i_data_sem);
	ext4_set_inode_state(inode, EXT4_STATE_EXT_MIGRATE);
	up_write(&ei->i_data_sem);

	handle = ext4_journal_start(inode, EXT4_HT_MIGRATE, 1);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out_temp;
	}

	err = ext4_migrate_build_extent_tree(handle, inode, temp);
	if (!err)
		err = ext4_migrate_swap_to_extents(handle, inode, temp);
	if (err)
		ext4_migrate_free_extent_tree(handle, temp);

	if (!ext4_journal_ensure_credits(handle, 1, 0)) {
		i_size_write(temp, 0);
		temp->i_blocks = 0;
		EXT4_I(temp)->i_csum_seed = original_temp_seed;
		ext4_ext_tree_init(handle, temp);
	}
	ext4_journal_stop(handle);

out_temp:
	if (temp) {
		unlock_new_inode(temp);
		iput(temp);
	}
out_unlock:
	ext4_writepages_up_write(inode->i_sb, writepages_ctx);
	return err;
}

int ext4_ind_migrate(struct inode *inode)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_extent_header *header;
	struct ext4_extent *extent;
	handle_t *handle;
	ext4_lblk_t logical_start = 0;
	ext4_lblk_t logical_end = 0;
	ext4_fsblk_t physical = 0;
	unsigned int length = 0;
	unsigned int i;
	int writepages_ctx;
	int err;
	int mark_err;

	if (!ext4_has_feature_extents(inode->i_sb) ||
	    !ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		return -EINVAL;
	if (ext4_has_feature_bigalloc(inode->i_sb))
		return -EOPNOTSUPP;

	if (test_opt(inode->i_sb, DELALLOC))
		ext4_alloc_da_blocks(inode);

	writepages_ctx = ext4_writepages_down_write(inode->i_sb);
	handle = ext4_journal_start(inode, EXT4_HT_MIGRATE, 1);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out_unlock;
	}

	down_write(&ei->i_data_sem);
	err = ext4_ext_check_inode(inode);
	if (err)
		goto out_sem;

	header = ext_inode_hdr(inode);
	if (ext4_blocks_count(sbi->s_es) > EXT4_MAX_BLOCK_FILE_PHYS ||
	    le16_to_cpu(header->eh_depth) != 0 ||
	    le16_to_cpu(header->eh_entries) > 1) {
		err = -EOPNOTSUPP;
		goto out_sem;
	}

	if (le16_to_cpu(header->eh_entries) == 1) {
		extent = EXT_FIRST_EXTENT(header);
		length = le16_to_cpu(extent->ee_len);
		physical = ext4_ext_pblock(extent);
		logical_start = le32_to_cpu(extent->ee_block);
		logical_end = logical_start + length - 1;
		if (logical_end >= EXT4_NDIR_BLOCKS) {
			err = -EOPNOTSUPP;
			goto out_sem;
		}
	}

	ext4_clear_inode_flag(inode, EXT4_INODE_EXTENTS);
	memset(ei->i_data, 0, sizeof(ei->i_data));
	if (length) {
		for (i = logical_start; i <= logical_end; ++i)
			ei->i_data[i] = cpu_to_le32(physical++);
	}

	mark_err = ext4_mark_inode_dirty(handle, inode);
	if (!err)
		err = mark_err;

out_sem:
	up_write(&ei->i_data_sem);
	ext4_journal_stop(handle);
out_unlock:
	ext4_writepages_up_write(inode->i_sb, writepages_ctx);
	return err;
}
