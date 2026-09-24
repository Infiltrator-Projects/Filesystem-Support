/*
 * Infiltrator Filesystem Support — EXT3 online growth.
 *
 * Online resize is split into two phases:
 *   1. prepare the new group's private metadata without publishing it;
 *   2. publish the group descriptor and global counters atomically through JBD.
 *
 * The resize inode is used only for reserved group-descriptor growth.  Backup
 * metadata is refreshed after the primary metadata is committed; failure to
 * refresh a backup marks the filesystem unclean so the next fsck repairs it.
 */

#include "ext3.h"

#include <linux/overflow.h>

struct ifs_ext3_group_layout {
	ext3_fsblk_t first;
	ext3_fsblk_t end;
	ext3_fsblk_t inode_table_end;
	ext3_fsblk_t metadata_end;
	unsigned int descriptor_blocks;
	unsigned int reserved_descriptor_blocks;
	unsigned int free_blocks;
};

struct ifs_ext3_backup_iter {
	unsigned int next_three;
	unsigned int next_five;
	unsigned int next_seven;
};

static bool ifs_ext3_block_in_half_open(ext3_fsblk_t block,
					ext3_fsblk_t first,
					ext3_fsblk_t end)
{
	return block >= first && block < end;
}

static bool ifs_ext3_ranges_overlap(ext3_fsblk_t first_a,
				    ext3_fsblk_t end_a,
				    ext3_fsblk_t first_b,
				    ext3_fsblk_t end_b)
{
	return first_a < end_b && first_b < end_a;
}

static int ifs_ext3_validate_group_layout(struct super_block *sb,
					  struct ext3_new_group_data *input,
					  struct ifs_ext3_group_layout *layout)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	struct ext3_super_block *es = sbi->s_es;
	ext3_fsblk_t filesystem_end = le32_to_cpu(es->s_blocks_count);
	ext3_fsblk_t group_end;
	ext3_fsblk_t table_end;
	u64 metadata_blocks;
	u64 consumed;
	struct buffer_head *probe;

	if (input->group != sbi->s_groups_count)
		return -EINVAL;

	if ((filesystem_end - le32_to_cpu(es->s_first_data_block)) %
	    EXT3_BLOCKS_PER_GROUP(sb))
		return -EINVAL;

	if (!input->blocks_count ||
	    check_add_overflow(filesystem_end,
			       (ext3_fsblk_t)input->blocks_count,
			       &group_end))
		return -EINVAL;

	if (check_add_overflow((ext3_fsblk_t)input->inode_table,
			       (ext3_fsblk_t)sbi->s_itb_per_group,
			       &table_end))
		return -EINVAL;

	layout->first = filesystem_end;
	layout->end = group_end;
	layout->inode_table_end = table_end;
	layout->descriptor_blocks =
		ext3_bg_has_super(sb, input->group) ?
		ext3_bg_num_gdb(sb, input->group) : 0;
	layout->reserved_descriptor_blocks =
		ext3_bg_has_super(sb, input->group) ?
		le16_to_cpu(es->s_reserved_gdt_blocks) : 0;

	metadata_blocks =
		(ext3_bg_has_super(sb, input->group) ? 1ULL : 0ULL) +
		layout->descriptor_blocks +
		layout->reserved_descriptor_blocks;
	if (metadata_blocks > (u64)(group_end - filesystem_end))
		return -EINVAL;

	layout->metadata_end = filesystem_end + metadata_blocks;

	consumed = metadata_blocks + 2ULL + sbi->s_itb_per_group;
	if (consumed > input->blocks_count)
		return -EINVAL;
	layout->free_blocks = input->blocks_count - (unsigned int)consumed;

	if (input->reserved_blocks > input->blocks_count / 5)
		return -EINVAL;

	if (!ifs_ext3_block_in_half_open(input->block_bitmap,
					 filesystem_end, group_end) ||
	    !ifs_ext3_block_in_half_open(input->inode_bitmap,
					 filesystem_end, group_end) ||
	    input->inode_table < filesystem_end ||
	    table_end > group_end)
		return -EINVAL;

	if (input->block_bitmap == input->inode_bitmap)
		return -EINVAL;

	if (ifs_ext3_ranges_overlap(input->block_bitmap,
				    (ext3_fsblk_t)input->block_bitmap + 1,
				    input->inode_table, table_end) ||
	    ifs_ext3_ranges_overlap(input->inode_bitmap,
				    (ext3_fsblk_t)input->inode_bitmap + 1,
				    input->inode_table, table_end))
		return -EINVAL;

	if (ifs_ext3_ranges_overlap(input->block_bitmap,
				    (ext3_fsblk_t)input->block_bitmap + 1,
				    filesystem_end, layout->metadata_end) ||
	    ifs_ext3_ranges_overlap(input->inode_bitmap,
				    (ext3_fsblk_t)input->inode_bitmap + 1,
				    filesystem_end, layout->metadata_end) ||
	    ifs_ext3_ranges_overlap(input->inode_table, table_end,
				    filesystem_end, layout->metadata_end))
		return -EINVAL;

	probe = sb_bread(sb, group_end - 1);
	if (!probe)
		return -EIO;
	brelse(probe);

	input->free_blocks_count = layout->free_blocks;
	return 0;
}

static struct buffer_head *ifs_ext3_zero_block(handle_t *handle,
					       struct super_block *sb,
					       ext3_fsblk_t block)
{
	struct buffer_head *bh;
	int error;

	bh = sb_getblk(sb, block);
	if (!bh)
		return ERR_PTR(-ENOMEM);

	error = ext3_journal_get_write_access(handle, bh);
	if (error) {
		brelse(bh);
		return ERR_PTR(error);
	}

	lock_buffer(bh);
	memset(bh->b_data, 0, sb->s_blocksize);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	return bh;
}

static void ifs_ext3_mark_tail_allocated(void *bitmap,
					 unsigned int first_bit,
					 unsigned int bit_limit)
{
	unsigned int bit;

	if (first_bit >= bit_limit)
		return;

	bit = first_bit;
	while (bit < bit_limit && (bit & 7U)) {
		ext3_set_bit(bit, bitmap);
		bit++;
	}

	if (bit < bit_limit)
		memset((u8 *)bitmap + (bit >> 3), 0xff,
		       (bit_limit - bit) >> 3);
}

static int ifs_ext3_ensure_credits(handle_t *handle, int needed,
				   struct buffer_head *preserve)
{
	int error;

	if (handle->h_buffer_credits >= needed)
		return 0;

	error = ext3_journal_extend(handle, EXT3_MAX_TRANS_DATA);
	if (error < 0)
		return error;
	if (!error)
		return 0;

	error = ext3_journal_restart(handle, EXT3_MAX_TRANS_DATA);
	if (error)
		return error;

	if (!preserve)
		return 0;
	return ext3_journal_get_write_access(handle, preserve);
}

static int ifs_ext3_prepare_new_group(struct super_block *sb,
				      struct ext3_new_group_data *input,
				      const struct ifs_ext3_group_layout *layout)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	handle_t *handle;
	struct buffer_head *block_bitmap = NULL;
	unsigned int bit;
	ext3_fsblk_t block;
	int error = 0;
	int stop_error;

	handle = ext3_journal_start_sb(sb, EXT3_MAX_TRANS_DATA);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	mutex_lock(&sbi->s_resize_lock);
	if (input->group != sbi->s_groups_count) {
		error = -EBUSY;
		goto out_unlock;
	}

	block_bitmap = ifs_ext3_zero_block(
		handle, sb, input->block_bitmap);
	if (IS_ERR(block_bitmap)) {
		error = PTR_ERR(block_bitmap);
		block_bitmap = NULL;
		goto out_unlock;
	}

	if (ext3_bg_has_super(sb, input->group))
		ext3_set_bit(0, block_bitmap->b_data);

	block = layout->first + 1;
	bit = 1;
	while (bit <= layout->descriptor_blocks) {
		struct buffer_head *copy;

		error = ifs_ext3_ensure_credits(handle, 1, block_bitmap);
		if (error)
			goto out_bitmap;

		copy = sb_getblk(sb, block);
		if (!copy) {
			error = -ENOMEM;
			goto out_bitmap;
		}

		error = ext3_journal_get_write_access(handle, copy);
		if (error) {
			brelse(copy);
			goto out_bitmap;
		}

		lock_buffer(copy);
		memcpy(copy->b_data,
		       sbi->s_group_desc[bit - 1]->b_data,
		       min_t(size_t, copy->b_size,
			     sbi->s_group_desc[bit - 1]->b_size));
		if (copy->b_size > sbi->s_group_desc[bit - 1]->b_size)
			memset(copy->b_data +
			       sbi->s_group_desc[bit - 1]->b_size,
			       0,
			       copy->b_size -
			       sbi->s_group_desc[bit - 1]->b_size);
		set_buffer_uptodate(copy);
		unlock_buffer(copy);

		error = ext3_journal_dirty_metadata(handle, copy);
		brelse(copy);
		if (error)
			goto out_bitmap;

		ext3_set_bit(bit, block_bitmap->b_data);
		bit++;
		block++;
	}

	while (bit <= layout->descriptor_blocks +
		      layout->reserved_descriptor_blocks) {
		struct buffer_head *reserved;

		error = ifs_ext3_ensure_credits(handle, 1, block_bitmap);
		if (error)
			goto out_bitmap;

		reserved = ifs_ext3_zero_block(handle, sb, block);
		if (IS_ERR(reserved)) {
			error = PTR_ERR(reserved);
			goto out_bitmap;
		}

		error = ext3_journal_dirty_metadata(handle, reserved);
		brelse(reserved);
		if (error)
			goto out_bitmap;

		ext3_set_bit(bit, block_bitmap->b_data);
		bit++;
		block++;
	}

	ext3_set_bit(input->block_bitmap - layout->first,
		     block_bitmap->b_data);
	ext3_set_bit(input->inode_bitmap - layout->first,
		     block_bitmap->b_data);

	for (block = input->inode_table;
	     block < layout->inode_table_end;
	     block++) {
		struct buffer_head *inode_block;

		error = ifs_ext3_ensure_credits(handle, 1, block_bitmap);
		if (error)
			goto out_bitmap;

		inode_block = ifs_ext3_zero_block(handle, sb, block);
		if (IS_ERR(inode_block)) {
			error = PTR_ERR(inode_block);
			goto out_bitmap;
		}

		error = ext3_journal_dirty_metadata(handle, inode_block);
		brelse(inode_block);
		if (error)
			goto out_bitmap;

		ext3_set_bit(block - layout->first,
			     block_bitmap->b_data);
	}

	ifs_ext3_mark_tail_allocated(
		block_bitmap->b_data, input->blocks_count,
		EXT3_BLOCKS_PER_GROUP(sb));
	error = ext3_journal_dirty_metadata(handle, block_bitmap);
	brelse(block_bitmap);
	block_bitmap = NULL;
	if (error)
		goto out_unlock;

	block_bitmap = ifs_ext3_zero_block(
		handle, sb, input->inode_bitmap);
	if (IS_ERR(block_bitmap)) {
		error = PTR_ERR(block_bitmap);
		block_bitmap = NULL;
		goto out_unlock;
	}

	ifs_ext3_mark_tail_allocated(
		block_bitmap->b_data, EXT3_INODES_PER_GROUP(sb),
		sb->s_blocksize * 8U);
	error = ext3_journal_dirty_metadata(handle, block_bitmap);

out_bitmap:
	brelse(block_bitmap);
out_unlock:
	mutex_unlock(&sbi->s_resize_lock);
	stop_error = ext3_journal_stop(handle);
	if (!error)
		error = stop_error;
	return error;
}

static void ifs_ext3_backup_iter_init(struct ifs_ext3_backup_iter *iter)
{
	iter->next_three = 1;
	iter->next_five = 5;
	iter->next_seven = 7;
}

static unsigned int ifs_ext3_next_backup_group(struct super_block *sb,
					       struct ifs_ext3_backup_iter *iter)
{
	unsigned int *candidate = &iter->next_three;
	unsigned int factor = 3;
	unsigned int result;

	if (!EXT3_HAS_RO_COMPAT_FEATURE(
		    sb, EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER))
		return iter->next_three++;

	if (iter->next_five < *candidate) {
		candidate = &iter->next_five;
		factor = 5;
	}
	if (iter->next_seven < *candidate) {
		candidate = &iter->next_seven;
		factor = 7;
	}

	result = *candidate;
	if (*candidate <= UINT_MAX / factor)
		*candidate *= factor;
	else
		*candidate = UINT_MAX;

	return result;
}

static int ifs_ext3_validate_reserved_descriptor(struct super_block *sb,
						  struct buffer_head *primary)
{
	struct ifs_ext3_backup_iter iter;
	unsigned int group;
	unsigned int entries = 0;
	__le32 *slot = (__le32 *)primary->b_data;

	ifs_ext3_backup_iter_init(&iter);
	while ((group = ifs_ext3_next_backup_group(sb, &iter)) <
	       EXT3_SB(sb)->s_groups_count) {
		ext3_fsblk_t expected =
			(ext3_fsblk_t)group * EXT3_BLOCKS_PER_GROUP(sb) +
			primary->b_blocknr;

		if (entries >= EXT3_ADDR_PER_BLOCK(sb))
			return -EFBIG;
		if (le32_to_cpu(slot[entries]) != expected)
			return -EINVAL;
		entries++;
	}

	return (int)entries;
}

static int ifs_ext3_publish_new_descriptor_block(
	handle_t *handle, struct inode *resize_inode,
	struct ext3_new_group_data *input,
	struct buffer_head **primary_out)
{
	struct super_block *sb = resize_inode->i_sb;
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	struct ext3_super_block *es = sbi->s_es;
	unsigned long descriptor_index =
		input->group / EXT3_DESC_PER_BLOCK(sb);
	ext3_fsblk_t descriptor_block =
		sbi->s_sbh->b_blocknr + 1 + descriptor_index;
	struct buffer_head *primary = NULL;
	struct buffer_head *indirect = NULL;
	struct buffer_head **replacement = NULL;
	struct buffer_head **old_array;
	struct ext3_iloc iloc;
	__le32 *slot;
	int backups;
	int error;

	if (sbi->s_sbh->b_blocknr !=
	    le32_to_cpu(es->s_first_data_block))
		return -EPERM;

	primary = sb_bread(sb, descriptor_block);
	if (!primary)
		return -EIO;

	backups = ifs_ext3_validate_reserved_descriptor(sb, primary);
	if (backups < 0) {
		error = backups;
		goto fail;
	}

	slot = EXT3_I(resize_inode)->i_data + EXT3_DIND_BLOCK;
	indirect = sb_bread(sb, le32_to_cpu(*slot));
	if (!indirect) {
		error = -EIO;
		goto fail;
	}

	slot = (__le32 *)indirect->b_data;
	if (le32_to_cpu(slot[
		    descriptor_index % EXT3_ADDR_PER_BLOCK(sb)]) !=
	    descriptor_block) {
		error = -EINVAL;
		goto fail;
	}

	error = ext3_journal_get_write_access(handle, sbi->s_sbh);
	if (error)
		goto fail;
	error = ext3_journal_get_write_access(handle, primary);
	if (error)
		goto fail;
	error = ext3_journal_get_write_access(handle, indirect);
	if (error)
		goto fail;
	error = ext3_reserve_inode_write(handle, resize_inode, &iloc);
	if (error)
		goto fail;

	replacement = kmalloc_array(
		descriptor_index + 1, sizeof(*replacement), GFP_NOFS);
	if (!replacement) {
		brelse(iloc.bh);
		error = -ENOMEM;
		goto fail;
	}

	slot[descriptor_index % EXT3_ADDR_PER_BLOCK(sb)] = 0;
	error = ext3_journal_dirty_metadata(handle, indirect);
	if (error)
		goto fail_iloc;

	memset(primary->b_data, 0, primary->b_size);
	set_buffer_uptodate(primary);
	error = ext3_journal_dirty_metadata(handle, primary);
	if (error)
		goto fail_iloc;

	resize_inode->i_blocks -=
		((u64)backups + 1) * sb->s_blocksize >> 9;
	error = ext3_mark_iloc_dirty(handle, resize_inode, &iloc);
	brelse(iloc.bh);
	if (error)
		goto fail_replacement;

	old_array = sbi->s_group_desc;
	memcpy(replacement, old_array,
	       sbi->s_gdb_count * sizeof(*replacement));
	replacement[descriptor_index] = primary;
	sbi->s_group_desc = replacement;
	sbi->s_gdb_count++;
	kfree(old_array);

	le16_add_cpu(&es->s_reserved_gdt_blocks, -1);
	error = ext3_journal_dirty_metadata(handle, sbi->s_sbh);
	if (error)
		return error;

	brelse(indirect);
	*primary_out = primary;
	return 0;

fail_iloc:
	brelse(iloc.bh);
fail_replacement:
	kfree(replacement);
fail:
	brelse(indirect);
	brelse(primary);
	return error;
}

static int ifs_ext3_reserve_future_backups(
	handle_t *handle, struct inode *resize_inode,
	const struct ext3_new_group_data *input)
{
	struct super_block *sb = resize_inode->i_sb;
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	unsigned int reserved =
		le16_to_cpu(sbi->s_es->s_reserved_gdt_blocks);
	struct buffer_head **blocks = NULL;
	struct buffer_head *indirect = NULL;
	struct ext3_iloc iloc;
	__le32 *cursor;
	__le32 *end;
	ext3_fsblk_t block;
	int backup_count = 0;
	unsigned int loaded = 0;
	unsigned int i;
	int error = 0;

	if (!reserved)
		return 0;

	blocks = kcalloc(reserved, sizeof(*blocks), GFP_NOFS);
	if (!blocks)
		return -ENOMEM;

	cursor = EXT3_I(resize_inode)->i_data + EXT3_DIND_BLOCK;
	indirect = sb_bread(sb, le32_to_cpu(*cursor));
	if (!indirect) {
		error = -EIO;
		goto out;
	}

	block = sbi->s_sbh->b_blocknr + 1 + sbi->s_gdb_count;
	cursor = (__le32 *)indirect->b_data +
		 (sbi->s_gdb_count % EXT3_ADDR_PER_BLOCK(sb));
	end = (__le32 *)indirect->b_data + EXT3_ADDR_PER_BLOCK(sb);

	for (loaded = 0; loaded < reserved; loaded++, block++) {
		int count;

		if (le32_to_cpu(*cursor) != block) {
			error = -EINVAL;
			goto out;
		}

		blocks[loaded] = sb_bread(sb, block);
		if (!blocks[loaded]) {
			error = -EIO;
			goto out;
		}

		count = ifs_ext3_validate_reserved_descriptor(
			sb, blocks[loaded]);
		if (count < 0) {
			error = count;
			goto out;
		}
		if (!loaded)
			backup_count = count;
		else if (count != backup_count) {
			error = -EUCLEAN;
			goto out;
		}

		cursor++;
		if (cursor == end)
			cursor = (__le32 *)indirect->b_data;
	}

	for (i = 0; i < reserved; i++) {
		error = ext3_journal_get_write_access(handle, blocks[i]);
		if (error)
			goto out;
	}

	error = ext3_reserve_inode_write(handle, resize_inode, &iloc);
	if (error)
		goto out;

	block = (ext3_fsblk_t)input->group *
		EXT3_BLOCKS_PER_GROUP(sb);
	for (i = 0; i < reserved; i++) {
		__le32 *entries = (__le32 *)blocks[i]->b_data;
		int dirty_error;

		if ((unsigned int)backup_count >=
		    EXT3_ADDR_PER_BLOCK(sb)) {
			error = -EFBIG;
			break;
		}

		entries[backup_count] =
			cpu_to_le32(block + blocks[i]->b_blocknr);
		dirty_error =
			ext3_journal_dirty_metadata(handle, blocks[i]);
		if (!error)
			error = dirty_error;
	}

	if (!error) {
		resize_inode->i_blocks +=
			(u64)reserved * sb->s_blocksize >> 9;
		error = ext3_mark_iloc_dirty(
			handle, resize_inode, &iloc);
	}
	brelse(iloc.bh);

out:
	for (i = 0; i < reserved; i++)
		brelse(blocks ? blocks[i] : NULL);
	brelse(indirect);
	kfree(blocks);
	return error;
}

static void ifs_ext3_refresh_backups(struct super_block *sb,
				     ext3_fsblk_t block_offset,
				     const void *source,
				     size_t bytes)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	struct ifs_ext3_backup_iter iter;
	unsigned int group;
	handle_t *handle;
	int error = 0;
	int stop_error;

	if (bytes > sb->s_blocksize) {
		error = -EINVAL;
		group = 0;
		goto failed;
	}

	handle = ext3_journal_start_sb(sb, EXT3_MAX_TRANS_DATA);
	if (IS_ERR(handle)) {
		error = PTR_ERR(handle);
		group = 0;
		goto failed;
	}

	ifs_ext3_backup_iter_init(&iter);
	while ((group = ifs_ext3_next_backup_group(sb, &iter)) <
	       sbi->s_groups_count) {
		struct buffer_head *bh;

		error = ifs_ext3_ensure_credits(handle, 1, NULL);
		if (error)
			break;

		bh = sb_getblk(
			sb,
			(ext3_fsblk_t)group *
				EXT3_BLOCKS_PER_GROUP(sb) +
			block_offset);
		if (!bh) {
			error = -ENOMEM;
			break;
		}

		error = ext3_journal_get_write_access(handle, bh);
		if (error) {
			brelse(bh);
			break;
		}

		lock_buffer(bh);
		memcpy(bh->b_data, source, bytes);
		if (bytes < bh->b_size)
			memset(bh->b_data + bytes, 0,
			       bh->b_size - bytes);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);

		error = ext3_journal_dirty_metadata(handle, bh);
		brelse(bh);
		if (error)
			break;
	}

	stop_error = ext3_journal_stop(handle);
	if (!error)
		error = stop_error;

failed:
	if (!error)
		return;

	ext3_warning(sb, __func__,
		     "backup metadata refresh failed for group %u (%d); "
		     "filesystem marked unclean",
		     group, error);
	sbi->s_mount_state &= ~EXT3_VALID_FS;
	sbi->s_es->s_state &= cpu_to_le16(~EXT3_VALID_FS);
	mark_buffer_dirty(sbi->s_sbh);
}

int ext3_group_add(struct super_block *sb,
		   struct ext3_new_group_data *input)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	struct ext3_super_block *es = sbi->s_es;
	struct ifs_ext3_group_layout layout;
	struct buffer_head *descriptor_block = NULL;
	struct ext3_group_desc *descriptor;
	struct inode *resize_inode = NULL;
	handle_t *handle;
	unsigned int descriptor_index;
	unsigned int descriptor_slot;
	unsigned int reserved;
	int error;
	int stop_error;

	if (input->blocks_count >
	    U32_MAX - le32_to_cpu(es->s_blocks_count) ||
	    EXT3_INODES_PER_GROUP(sb) >
	    U32_MAX - le32_to_cpu(es->s_inodes_count))
		return -EOVERFLOW;

	descriptor_index =
		input->group / EXT3_DESC_PER_BLOCK(sb);
	descriptor_slot =
		input->group % EXT3_DESC_PER_BLOCK(sb);
	reserved =
		ext3_bg_has_super(sb, input->group) ?
		le16_to_cpu(es->s_reserved_gdt_blocks) : 0;

	if (!descriptor_slot &&
	    !EXT3_HAS_RO_COMPAT_FEATURE(
		    sb, EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER))
		return -EPERM;

	if (reserved || !descriptor_slot) {
		if (!EXT3_HAS_COMPAT_FEATURE(
			    sb, EXT3_FEATURE_COMPAT_RESIZE_INODE) ||
		    !le16_to_cpu(es->s_reserved_gdt_blocks))
			return -EPERM;

		resize_inode = ext3_iget(sb, EXT3_RESIZE_INO);
		if (IS_ERR(resize_inode))
			return PTR_ERR(resize_inode);
	}

	error = ifs_ext3_validate_group_layout(sb, input, &layout);
	if (error)
		goto out_inode;

	error = ifs_ext3_prepare_new_group(sb, input, &layout);
	if (error)
		goto out_inode;

	handle = ext3_journal_start_sb(
		sb,
		ext3_bg_has_super(sb, input->group) ?
		3 + reserved : 4);
	if (IS_ERR(handle)) {
		error = PTR_ERR(handle);
		goto out_inode;
	}

	mutex_lock(&sbi->s_resize_lock);
	if (input->group != sbi->s_groups_count) {
		error = -EBUSY;
		goto out_transaction;
	}

	error = ext3_journal_get_write_access(handle, sbi->s_sbh);
	if (error)
		goto out_transaction;

	if (descriptor_slot) {
		descriptor_block =
			sbi->s_group_desc[descriptor_index];
		error = ext3_journal_get_write_access(
			handle, descriptor_block);
		if (error)
			goto out_transaction;

		if (reserved &&
		    ext3_bg_num_gdb(sb, input->group)) {
			error = ifs_ext3_reserve_future_backups(
				handle, resize_inode, input);
			if (error)
				goto out_transaction;
		}
	} else {
		error = ifs_ext3_publish_new_descriptor_block(
			handle, resize_inode, input,
			&descriptor_block);
		if (error)
			goto out_transaction;
	}

	descriptor =
		(struct ext3_group_desc *)descriptor_block->b_data +
		descriptor_slot;
	memset(descriptor, 0, sizeof(*descriptor));
	descriptor->bg_block_bitmap =
		cpu_to_le32(input->block_bitmap);
	descriptor->bg_inode_bitmap =
		cpu_to_le32(input->inode_bitmap);
	descriptor->bg_inode_table =
		cpu_to_le32(input->inode_table);
	descriptor->bg_free_blocks_count =
		cpu_to_le16(input->free_blocks_count);
	descriptor->bg_free_inodes_count =
		cpu_to_le16(EXT3_INODES_PER_GROUP(sb));

	le32_add_cpu(&es->s_blocks_count, input->blocks_count);
	le32_add_cpu(&es->s_inodes_count,
		     EXT3_INODES_PER_GROUP(sb));
	le32_add_cpu(&es->s_r_blocks_count,
		     input->reserved_blocks);

	/*
	 * Readers may use the group count to index the descriptor array.
	 * Publish the fully initialised descriptor before increasing the count.
	 */
	smp_wmb();
	sbi->s_groups_count++;

	error = ext3_journal_dirty_metadata(
		handle, descriptor_block);
	if (!error)
		error = ext3_journal_dirty_metadata(
			handle, sbi->s_sbh);
	if (!error) {
		percpu_counter_add(&sbi->s_freeblocks_counter,
				   input->free_blocks_count);
		percpu_counter_add(&sbi->s_freeinodes_counter,
				   EXT3_INODES_PER_GROUP(sb));
	}

out_transaction:
	mutex_unlock(&sbi->s_resize_lock);
	stop_error = ext3_journal_stop(handle);
	if (!error)
		error = stop_error;

	if (!error) {
		ifs_ext3_refresh_backups(
			sb, sbi->s_sbh->b_blocknr,
			es, sizeof(*es));
		ifs_ext3_refresh_backups(
			sb, descriptor_block->b_blocknr,
			descriptor_block->b_data,
			descriptor_block->b_size);
	}

out_inode:
	if (resize_inode)
		iput(resize_inode);
	return error;
}

int ext3_group_extend(struct super_block *sb,
		      struct ext3_super_block *es,
		      ext3_fsblk_t requested_blocks)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	ext3_fsblk_t old_blocks = le32_to_cpu(es->s_blocks_count);
	ext3_fsblk_t group_capacity;
	ext3_fsblk_t new_blocks;
	ext3_grpblk_t used_in_group;
	ext3_grpblk_t add;
	struct buffer_head *probe;
	handle_t *handle;
	unsigned long freed = 0;
	int error;
	int stop_error;

	if (!requested_blocks || requested_blocks == old_blocks)
		return 0;
	if (requested_blocks < old_blocks)
		return -EBUSY;

	if (sb->s_blocksize_bits < 9 ||
	    requested_blocks >
	    (sector_t)(~0ULL) >> (sb->s_blocksize_bits - 9))
		return -EINVAL;

	used_in_group =
		(old_blocks - le32_to_cpu(es->s_first_data_block)) %
		EXT3_BLOCKS_PER_GROUP(sb);
	if (!used_in_group)
		return -EPERM;

	group_capacity =
		EXT3_BLOCKS_PER_GROUP(sb) - used_in_group;
	add = min_t(ext3_fsblk_t,
		    requested_blocks - old_blocks,
		    group_capacity);

	if (check_add_overflow(old_blocks,
			       (ext3_fsblk_t)add,
			       &new_blocks))
		return -EOVERFLOW;

	probe = sb_bread(sb, new_blocks - 1);
	if (!probe)
		return -ENOSPC;
	brelse(probe);

	handle = ext3_journal_start_sb(sb, 3);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	mutex_lock(&sbi->s_resize_lock);
	if (old_blocks != le32_to_cpu(es->s_blocks_count)) {
		error = -EBUSY;
		goto out_transaction;
	}

	error = ext3_journal_get_write_access(
		handle, sbi->s_sbh);
	if (error)
		goto out_transaction;

	es->s_blocks_count = cpu_to_le32(new_blocks);
	error = ext3_journal_dirty_metadata(
		handle, sbi->s_sbh);
	if (error)
		goto out_transaction;

	/*
	 * The old filesystem end lies inside the final group's bitmap.  Once the
	 * new global size is journal-visible, release exactly the newly admitted
	 * range through the normal allocator accounting path.
	 */
	ext3_free_blocks_sb(
		handle, sb, old_blocks, add, &freed);

out_transaction:
	mutex_unlock(&sbi->s_resize_lock);
	stop_error = ext3_journal_stop(handle);
	if (!error)
		error = stop_error;

	if (!error)
		ifs_ext3_refresh_backups(
			sb, sbi->s_sbh->b_blocknr,
			es, sizeof(*es));

	return error;
}
