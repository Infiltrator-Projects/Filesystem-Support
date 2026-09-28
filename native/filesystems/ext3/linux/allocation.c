/* Infiltrator Filesystem Support — EXT3 Linux allocation adapter.
 * Block allocation, inode allocation and online-growth publication are one
 * Linux-facing storage responsibility. Portable format policy remains core.
 */

/*
 * Filesystem Support EXT3 block allocation.
 *
 * EXT3 block allocation is journal-owned metadata.  A block is reusable only
 * when it is clear in both the live bitmap and the journal's committed bitmap
 * snapshot.  Freeing performs the inverse publication: the committed snapshot
 * is marked allocated before the live bit is cleared, preventing premature
 * reuse before the freeing transaction commits.
 *
 * Reservation-window ioctls remain supported as per-inode allocation hints,
 * but allocation correctness does not depend on a shared reservation RB-tree.
 */

#include <linux/buffer_head.h>
#include <linux/blkdev.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/quotaops.h>
#include <linux/slab.h>

#include "linux_adapter.h"

struct ext3_group_desc *ext3_get_group_desc(
	struct super_block *sb, unsigned int group,
	struct buffer_head **bh_out)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	unsigned long descriptor_block;
	unsigned long descriptor_index;
	struct ext3_group_desc *base;

	if (group >= sbi->s_groups_count)
		return NULL;

	descriptor_block = group >> EXT3_DESC_PER_BLOCK_BITS(sb);
	descriptor_index = group & (EXT3_DESC_PER_BLOCK(sb) - 1U);
	if (!sbi->s_group_desc ||
	    !sbi->s_group_desc[descriptor_block])
		return NULL;

	if (bh_out)
		*bh_out = sbi->s_group_desc[descriptor_block];

	base = (struct ext3_group_desc *)
		sbi->s_group_desc[descriptor_block]->b_data;
	return base + descriptor_index;
}

static ext3_fsblk_t ifs_ext3_group_last_block(
	struct super_block *sb, unsigned int group)
{
	ifs_ext3_u64 first = 0U;
	ifs_ext3_u64 last = 0U;

	if (ifs_ext3_group_bounds(
		    group,
		    le32_to_cpu(EXT3_SB(sb)->s_es->s_first_data_block),
		    EXT3_BLOCKS_PER_GROUP(sb),
		    le32_to_cpu(EXT3_SB(sb)->s_es->s_blocks_count),
		    &first, &last) != IFS_EXT3_BLOCK_GROUP_OK)
		return ext3_group_first_block_no(sb, group);

	return (ext3_fsblk_t)last;
}

static bool ifs_ext3_data_range_valid(
	struct super_block *sb, ext3_fsblk_t start,
	unsigned long count)
{
	const struct ext3_super_block *es = EXT3_SB(sb)->s_es;
	const ext3_fsblk_t first =
		le32_to_cpu(es->s_first_data_block);
	const ext3_fsblk_t blocks =
		le32_to_cpu(es->s_blocks_count);

	if (count == 0U || start < first || start >= blocks)
		return false;
	return count <= blocks - start;
}

static bool ifs_ext3_range_hits_system_zone(
	struct super_block *sb,
	const struct ext3_group_desc *desc,
	ext3_fsblk_t start, unsigned long count)
{
	const unsigned long table_blocks = EXT3_SB(sb)->s_itb_per_group;
	ext3_fsblk_t last;

	if (count == 0U ||
	    start > (ext3_fsblk_t)(~0UL) - (count - 1U))
		return true;

	last = start + count - 1U;
	if (in_range(le32_to_cpu(desc->bg_block_bitmap), start, count) ||
	    in_range(le32_to_cpu(desc->bg_inode_bitmap), start, count) ||
	    in_range(start, le32_to_cpu(desc->bg_inode_table), table_blocks) ||
	    in_range(last, le32_to_cpu(desc->bg_inode_table), table_blocks))
		return true;

	return false;
}

static bool ifs_ext3_bitmap_metadata_reserved(
	struct super_block *sb, unsigned int group,
	const struct ext3_group_desc *desc,
	struct buffer_head *bitmap)
{
	const ext3_fsblk_t first =
		ext3_group_first_block_no(sb, group);
	const ext3_fsblk_t last =
		ifs_ext3_group_last_block(sb, group);
	const unsigned long table_blocks = EXT3_SB(sb)->s_itb_per_group;
	ext3_fsblk_t block;
	unsigned long bit;
	unsigned long table_end;

	block = le32_to_cpu(desc->bg_block_bitmap);
	if (block < first || block > last)
		goto corrupt;
	bit = block - first;
	if (!ext3_test_bit(bit, bitmap->b_data))
		goto corrupt;

	block = le32_to_cpu(desc->bg_inode_bitmap);
	if (block < first || block > last)
		goto corrupt;
	bit = block - first;
	if (!ext3_test_bit(bit, bitmap->b_data))
		goto corrupt;

	block = le32_to_cpu(desc->bg_inode_table);
	if (block < first || block > last || table_blocks == 0U)
		goto corrupt;
	bit = block - first;
	if (bit > (unsigned long)(last - first) ||
	    table_blocks > (unsigned long)(last - first + 1U) - bit)
		goto corrupt;
	table_end = bit + table_blocks;
	if (ext3_find_next_zero_bit(
		    bitmap->b_data, table_end, bit) < table_end)
		goto corrupt;

	return true;

corrupt:
	ext3_error(sb, __func__,
		   "group %u has corrupt metadata allocation map", group);
	return false;
}

static struct buffer_head *ifs_ext3_read_block_bitmap(
	struct super_block *sb, unsigned int group)
{
	struct ext3_group_desc *desc;
	struct buffer_head *bh;
	u32 block;
	int status;

	desc = ext3_get_group_desc(sb, group, NULL);
	if (!desc)
		return NULL;

	block = le32_to_cpu(desc->bg_block_bitmap);
	bh = sb_getblk(sb, block);
	if (!bh)
		return NULL;

	status = bh_read(bh, 0);
	if (status < 0) {
		brelse(bh);
		ext3_error(sb, __func__,
			   "cannot read block bitmap %u for group %u",
			   block, group);
		return NULL;
	}

	if (!ifs_ext3_bitmap_metadata_reserved(
		    sb, group, desc, bh)) {
		brelse(bh);
		return NULL;
	}

	return bh;
}

static bool ifs_ext3_bit_allocatable(
	struct buffer_head *bitmap, unsigned long bit)
{
	struct journal_head *jh = bh2jh(bitmap);
	bool available;

	if (ext3_test_bit(bit, bitmap->b_data))
		return false;

	jbd_lock_bh_state(bitmap);
	available = !jh->b_committed_data ||
		!ext3_test_bit(bit, jh->b_committed_data);
	jbd_unlock_bh_state(bitmap);
	return available;
}

static int ifs_ext3_claim_bit(
	struct super_block *sb, unsigned int group,
	struct buffer_head *bitmap, unsigned long bit)
{
	struct journal_head *jh = bh2jh(bitmap);
	bool committed_busy = false;

	if (ext3_set_bit_atomic(
		    sb_bgl_lock(EXT3_SB(sb), group),
		    bit, bitmap->b_data))
		return 0;

	jbd_lock_bh_state(bitmap);
	if (jh->b_committed_data &&
	    ext3_test_bit(bit, jh->b_committed_data))
		committed_busy = true;
	jbd_unlock_bh_state(bitmap);

	if (committed_busy) {
		ext3_clear_bit_atomic(
			sb_bgl_lock(EXT3_SB(sb), group),
			bit, bitmap->b_data);
		return 0;
	}

	return 1;
}

static ext3_grpblk_t ifs_ext3_find_candidate(
	struct buffer_head *bitmap,
	ext3_grpblk_t start,
	ext3_grpblk_t end)
{
	ext3_grpblk_t bit;

	if (start < 0)
		start = 0;
	while (start < end) {
		bit = ext3_find_next_zero_bit(
			bitmap->b_data, end, start);
		if (bit >= end)
			return -1;
		if (ifs_ext3_bit_allocatable(bitmap, bit))
			return bit;
		start = bit + 1;
	}

	return -1;
}

static ext3_grpblk_t ifs_ext3_claim_run(
	struct super_block *sb, unsigned int group,
	struct buffer_head *bitmap,
	ext3_grpblk_t goal,
	unsigned long *count)
{
	const ext3_grpblk_t end =
		(ext3_grpblk_t)(ifs_ext3_group_last_block(sb, group) -
		ext3_group_first_block_no(sb, group) + 1U);
	ext3_grpblk_t first;
	ext3_grpblk_t bit;
	unsigned long claimed = 0U;

	if (!count || *count == 0U)
		return -1;

	first = ifs_ext3_find_candidate(
		bitmap,
		goal >= 0 && goal < end ? goal : 0,
		end);
	if (first < 0 && goal > 0)
		first = ifs_ext3_find_candidate(bitmap, 0, goal);
	if (first < 0)
		return -1;

	bit = first;
	while (bit < end && claimed < *count) {
		if (!ifs_ext3_bit_allocatable(bitmap, bit) ||
		    !ifs_ext3_claim_bit(sb, group, bitmap, bit))
			break;
		claimed++;
		bit++;
	}

	if (claimed == 0U)
		return -1;

	*count = claimed;
	return first;
}

static void ifs_ext3_unclaim_run(
	struct super_block *sb, unsigned int group,
	struct buffer_head *bitmap,
	ext3_grpblk_t first, unsigned long count)
{
	unsigned long index;

	for (index = 0U; index < count; ++index)
		ext3_clear_bit_atomic(
			sb_bgl_lock(EXT3_SB(sb), group),
			first + index, bitmap->b_data);
}

static int ifs_ext3_adjust_free_blocks(
	handle_t *handle,
	struct super_block *sb,
	unsigned int group,
	struct ext3_group_desc *desc,
	struct buffer_head *desc_bh,
	int delta)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	unsigned int free_blocks;
	int error;

	error = ext3_journal_get_write_access(handle, desc_bh);
	if (error)
		return error;

	spin_lock(sb_bgl_lock(sbi, group));
	free_blocks = le16_to_cpu(desc->bg_free_blocks_count);
	if (delta < 0 && free_blocks < (unsigned int)(-delta)) {
		spin_unlock(sb_bgl_lock(sbi, group));
		return -EUCLEAN;
	}
	if (delta > 0 &&
	    free_blocks > 0xffffU - (unsigned int)delta) {
		spin_unlock(sb_bgl_lock(sbi, group));
		return -EUCLEAN;
	}
	desc->bg_free_blocks_count =
		cpu_to_le16((unsigned int)((int)free_blocks + delta));
	spin_unlock(sb_bgl_lock(sbi, group));

	error = ext3_journal_dirty_metadata(handle, desc_bh);
	if (error)
		return error;

	if (delta < 0)
		percpu_counter_sub(
			&sbi->s_freeblocks_counter,
			(unsigned int)(-delta));
	else if (delta > 0)
		percpu_counter_add(
			&sbi->s_freeblocks_counter,
			(unsigned int)delta);

	return 0;
}

void ext3_rsv_window_add(
	struct super_block *sb,
	struct ext3_reserve_window_node *reservation)
{
	/*
	 * Retained for the mount-time ABI. Allocation no longer depends on a
	 * filesystem-wide reservation tree; per-inode rsv_goal_size remains a
	 * locality hint exposed through the existing ioctl.
	 */
	(void)sb;
	if (!reservation)
		return;
	RB_CLEAR_NODE(&reservation->rsv_node);
}

void ext3_init_block_alloc_info(struct inode *inode)
{
	struct ext3_block_alloc_info *info;

	if (EXT3_I(inode)->i_block_alloc_info)
		return;

	info = kzalloc(sizeof(*info), GFP_NOFS);
	if (!info)
		return;

	info->rsv_window_node.rsv_start =
		EXT3_RESERVE_WINDOW_NOT_ALLOCATED;
	info->rsv_window_node.rsv_end =
		EXT3_RESERVE_WINDOW_NOT_ALLOCATED;
	info->rsv_window_node.rsv_goal_size =
		test_opt(inode->i_sb, RESERVATION) ?
		EXT3_DEFAULT_RESERVE_BLOCKS : 0U;
	info->rsv_window_node.rsv_alloc_hit = 0U;
	info->last_alloc_logical_block = 0U;
	info->last_alloc_physical_block = 0U;
	EXT3_I(inode)->i_block_alloc_info = info;
}

void ext3_discard_reservation(struct inode *inode)
{
	struct ext3_block_alloc_info *info =
		EXT3_I(inode)->i_block_alloc_info;

	if (!info)
		return;

	info->rsv_window_node.rsv_start =
		EXT3_RESERVE_WINDOW_NOT_ALLOCATED;
	info->rsv_window_node.rsv_end =
		EXT3_RESERVE_WINDOW_NOT_ALLOCATED;
	info->rsv_window_node.rsv_alloc_hit = 0U;
}

static bool ifs_ext3_caller_may_use_reserved(
	struct ext3_sb_info *sbi)
{
	if (capable(CAP_SYS_RESOURCE))
		return true;
	if (uid_eq(sbi->s_resuid, current_fsuid()))
		return true;
	if (!gid_eq(sbi->s_resgid, GLOBAL_ROOT_GID) &&
	    in_group_p(sbi->s_resgid))
		return true;
	return false;
}

static bool ifs_ext3_has_allocatable_blocks(
	struct ext3_sb_info *sbi, bool noquota)
{
	const ext3_fsblk_t free_blocks =
		percpu_counter_read_positive(&sbi->s_freeblocks_counter);
	const ext3_fsblk_t reserved =
		le32_to_cpu(sbi->s_es->s_r_blocks_count);

	if (free_blocks > reserved)
		return true;
	if (free_blocks == 0U)
		return false;
	return noquota || ifs_ext3_caller_may_use_reserved(sbi);
}

int ext3_should_retry_alloc(struct super_block *sb, int *retries)
{
	if (!retries ||
	    !ifs_ext3_has_allocatable_blocks(EXT3_SB(sb), false) ||
	    (*retries)++ > 3)
		return 0;

	return journal_force_commit_nested(EXT3_SB(sb)->s_journal);
}

void ext3_free_blocks_sb(
	handle_t *handle,
	struct super_block *sb,
	ext3_fsblk_t block,
	unsigned long count,
	unsigned long *quota_freed)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	unsigned long total_freed = 0U;
	int error = 0;

	if (quota_freed)
		*quota_freed = 0U;
	if (!handle ||
	    !ifs_ext3_data_range_valid(sb, block, count)) {
		ext3_error(
			sb, __func__,
			"invalid free range " E3FSBLK "+%lu",
			block, count);
		return;
	}

	while (count != 0U) {
		const ext3_fsblk_t first_data =
			le32_to_cpu(sbi->s_es->s_first_data_block);
		const unsigned int group =
			(unsigned int)((block - first_data) /
			EXT3_BLOCKS_PER_GROUP(sb));
		const unsigned long bit =
			(unsigned long)((block - first_data) %
			EXT3_BLOCKS_PER_GROUP(sb));
		const unsigned long part =
			min_t(unsigned long, count,
			      EXT3_BLOCKS_PER_GROUP(sb) - bit);
		struct ext3_group_desc *desc;
		struct buffer_head *desc_bh;
		struct buffer_head *bitmap;
		struct journal_head *jh;
		unsigned long index;
		unsigned long freed = 0U;

		desc = ext3_get_group_desc(sb, group, &desc_bh);
		if (!desc) {
			error = -EIO;
			break;
		}
		if (ifs_ext3_range_hits_system_zone(
			    sb, desc, block, part)) {
			ext3_error(
				sb, __func__,
				"attempt to free EXT3 metadata blocks");
			error = -EUCLEAN;
			break;
		}

		bitmap = ifs_ext3_read_block_bitmap(sb, group);
		if (!bitmap) {
			error = -EIO;
			break;
		}

		error = ext3_journal_get_undo_access(
			handle, bitmap);
		if (error) {
			brelse(bitmap);
			break;
		}
		error = ext3_journal_get_write_access(
			handle, desc_bh);
		if (error) {
			brelse(bitmap);
			break;
		}

		jh = bh2jh(bitmap);
		jbd_lock_bh_state(bitmap);
		if (!jh->b_committed_data) {
			jbd_unlock_bh_state(bitmap);
			brelse(bitmap);
			error = -EIO;
			break;
		}

		for (index = 0U; index < part; ++index) {
			ext3_set_bit_atomic(
				sb_bgl_lock(sbi, group),
				bit + index, jh->b_committed_data);
			if (ext3_clear_bit_atomic(
				    sb_bgl_lock(sbi, group),
				    bit + index, bitmap->b_data))
				freed++;
			else
				ext3_error(
					sb, __func__,
					"block " E3FSBLK " already free",
					block + index);
		}
		jbd_unlock_bh_state(bitmap);

		if (freed != 0U) {
			error = ifs_ext3_adjust_free_blocks(
				handle, sb, group, desc,
				desc_bh, (int)freed);
			if (!error)
				error = ext3_journal_dirty_metadata(
					handle, bitmap);
			total_freed += freed;
		}

		brelse(bitmap);
		if (error)
			break;

		block += part;
		count -= part;
		cond_resched();
	}

	if (quota_freed)
		*quota_freed = total_freed;
	ext3_std_error(sb, error);
}

void ext3_free_blocks(
	handle_t *handle,
	struct inode *inode,
	ext3_fsblk_t block,
	unsigned long count)
{
	unsigned long freed = 0U;

	ext3_free_blocks_sb(
		handle, inode->i_sb, block, count, &freed);
	if (freed != 0U)
		dquot_free_block(inode, freed);
}

static int ifs_ext3_allocate_from_group(
	handle_t *handle,
	struct inode *inode,
	unsigned int group,
	ext3_grpblk_t goal,
	unsigned long *count,
	ext3_fsblk_t *block_out)
{
	struct super_block *sb = inode->i_sb;
	struct ext3_group_desc *desc;
	struct buffer_head *desc_bh;
	struct buffer_head *bitmap;
	ext3_grpblk_t first;
	ext3_fsblk_t absolute;
	unsigned long wanted = *count;
	int error;

	desc = ext3_get_group_desc(sb, group, &desc_bh);
	if (!desc)
		return -EIO;
	if (le16_to_cpu(desc->bg_free_blocks_count) == 0U)
		return -ENOSPC;

	bitmap = ifs_ext3_read_block_bitmap(sb, group);
	if (!bitmap)
		return -EIO;

	error = ext3_journal_get_undo_access(
		handle, bitmap);
	if (error)
		goto out;
	error = ext3_journal_get_write_access(
		handle, desc_bh);
	if (error)
		goto out;

	first = ifs_ext3_claim_run(
		sb, group, bitmap, goal, count);
	if (first < 0) {
		*count = wanted;
		error = -ENOSPC;
		goto out;
	}

	absolute =
		ext3_group_first_block_no(sb, group) + first;
	if (!ifs_ext3_data_range_valid(sb, absolute, *count) ||
	    ifs_ext3_range_hits_system_zone(
		    sb, desc, absolute, *count)) {
		ifs_ext3_unclaim_run(
			sb, group, bitmap, first, *count);
		*count = wanted;
		error = -EUCLEAN;
		goto out;
	}

	error = ifs_ext3_adjust_free_blocks(
		handle, sb, group, desc,
		desc_bh, -(int)*count);
	if (error) {
		ifs_ext3_unclaim_run(
			sb, group, bitmap, first, *count);
		*count = wanted;
		goto out;
	}

	error = ext3_journal_dirty_metadata(handle, bitmap);
	if (error) {
		/*
		 * The journal owns failure recovery after descriptor publication.
		 * Report the error rather than locally undoing half a transaction.
		 */
		ext3_std_error(sb, error);
		goto out;
	}

	*block_out = absolute;
	error = 0;

out:
	brelse(bitmap);
	return error;
}

ext3_fsblk_t ext3_new_blocks(
	handle_t *handle,
	struct inode *inode,
	ext3_fsblk_t goal,
	unsigned long *count,
	int *errp)
{
	struct super_block *sb = inode->i_sb;
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	struct ext3_super_block *es = sbi->s_es;
	const unsigned long requested =
		count ? *count : 0U;
	ext3_fsblk_t block = 0U;
	unsigned int start_group;
	unsigned int start_offset;
	unsigned int pass;
	int error;

	if (!handle || !count || !errp || requested == 0U) {
		if (errp)
			*errp = -EINVAL;
		return 0U;
	}

	*errp = -ENOSPC;
	error = dquot_alloc_block(inode, requested);
	if (error) {
		*errp = error;
		return 0U;
	}

	if (!ifs_ext3_has_allocatable_blocks(
		    sbi, IS_NOQUOTA(inode))) {
		error = -ENOSPC;
		goto fail_quota;
	}

	if (goal < le32_to_cpu(es->s_first_data_block) ||
	    goal >= le32_to_cpu(es->s_blocks_count))
		goal = le32_to_cpu(es->s_first_data_block);

	{
		ifs_ext3_u32 mapped_group = 0U;
		ifs_ext3_u32 mapped_offset = 0U;

		if (ifs_ext3_block_group_position(
			    goal,
			    le32_to_cpu(es->s_first_data_block),
			    EXT3_BLOCKS_PER_GROUP(sb),
			    sbi->s_groups_count,
			    &mapped_group,
			    &mapped_offset) != IFS_EXT3_BLOCK_GROUP_OK) {
			error = -EUCLEAN;
			goto fail_quota;
		}
		start_group = mapped_group;
		start_offset = mapped_offset;
	}

	for (pass = 0U; pass < sbi->s_groups_count; ++pass) {
		const unsigned int group =
			(start_group + pass) % sbi->s_groups_count;
		const ext3_grpblk_t group_goal =
			pass == 0U ?
			(ext3_grpblk_t)start_offset : -1;
		unsigned long wanted = requested;

		error = ifs_ext3_allocate_from_group(
			handle, inode, group,
			group_goal, &wanted, &block);
		if (error == -ENOSPC)
			continue;
		if (error)
			goto fail_quota;

		*count = wanted;
		if (wanted < requested)
			dquot_free_block(
				inode, requested - wanted);
		*errp = 0;
		return block;
	}

	error = -ENOSPC;

fail_quota:
	dquot_free_block(inode, requested);
	*errp = error;
	if (error != -ENOSPC)
		ext3_std_error(sb, error);
	return 0U;
}

ext3_fsblk_t ext3_new_block(
	handle_t *handle,
	struct inode *inode,
	ext3_fsblk_t goal,
	int *errp)
{
	unsigned long count = 1U;

	return ext3_new_blocks(
		handle, inode, goal, &count, errp);
}

ext3_fsblk_t ext3_count_free_blocks(struct super_block *sb)
{
	ext3_fsblk_t count = 0U;
	unsigned int group;

	for (group = 0U;
	     group < EXT3_SB(sb)->s_groups_count;
	     ++group) {
		struct ext3_group_desc *desc =
			ext3_get_group_desc(sb, group, NULL);

		if (desc)
			count +=
				le16_to_cpu(desc->bg_free_blocks_count);
		cond_resched();
	}

	return count;
}

int ext3_bg_has_super(struct super_block *sb, int group)
{
	if (group < 0)
		return 0;
	return ifs_ext3_group_has_super(
		EXT3_HAS_RO_COMPAT_FEATURE(
			sb, EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER),
		(unsigned int)group);
}

unsigned long ext3_bg_num_gdb(
	struct super_block *sb, int group)
{
	const unsigned long first_meta =
		le32_to_cpu(EXT3_SB(sb)->s_es->s_first_meta_bg);
	unsigned long meta_group;

	if (group < 0)
		return 0U;

	meta_group =
		(unsigned int)group /
		EXT3_DESC_PER_BLOCK(sb);
	if (!EXT3_HAS_INCOMPAT_FEATURE(
		    sb, EXT3_FEATURE_INCOMPAT_META_BG) ||
	    meta_group < first_meta)
		return ext3_bg_has_super(sb, group) ?
			EXT3_SB(sb)->s_gdb_count : 0U;

	return ifs_ext3_meta_gdb_count(
		(unsigned int)group,
		EXT3_DESC_PER_BLOCK(sb));
}

static int ifs_ext3_trim_group(
	struct super_block *sb,
	unsigned int group,
	ext3_grpblk_t first,
	ext3_grpblk_t last,
	ext3_grpblk_t minimum,
	u64 *trimmed)
{
	handle_t *handle;
	struct ext3_group_desc *desc;
	struct buffer_head *desc_bh;
	struct buffer_head *bitmap;
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	int error = 0;
	ext3_grpblk_t cursor;

	handle = ext3_journal_start_sb(sb, 2);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	desc = ext3_get_group_desc(sb, group, &desc_bh);
	if (!desc) {
		error = -EIO;
		goto out_stop;
	}

	bitmap = ifs_ext3_read_block_bitmap(sb, group);
	if (!bitmap) {
		error = -EIO;
		goto out_stop;
	}

	error = ext3_journal_get_undo_access(
		handle, bitmap);
	if (error)
		goto out_bitmap;
	error = ext3_journal_get_write_access(
		handle, desc_bh);
	if (error)
		goto out_bitmap;

	cursor = first;
	while (cursor <= last) {
		ext3_grpblk_t run_start;
		ext3_grpblk_t run_end;
		ext3_grpblk_t bit;
		unsigned long run_length = 0U;
		int discard_error;

		run_start =
			ifs_ext3_find_candidate(
				bitmap, cursor, last + 1);
		if (run_start < 0)
			break;

		run_end = run_start;
		while (run_end <= last &&
		       ifs_ext3_bit_allocatable(
			       bitmap, run_end) &&
		       ifs_ext3_claim_bit(
			       sb, group, bitmap, run_end)) {
			run_end++;
			run_length++;
		}

		if (run_length == 0U) {
			cursor = run_start + 1;
			continue;
		}

		spin_lock(sb_bgl_lock(sbi, group));
		le16_add_cpu(
			&desc->bg_free_blocks_count,
			-(int)run_length);
		spin_unlock(sb_bgl_lock(sbi, group));
		percpu_counter_sub(
			&sbi->s_freeblocks_counter,
			run_length);

		discard_error = 0;
		if (run_length >= (unsigned long)minimum) {
			discard_error = sb_issue_discard(
				sb,
				ext3_group_first_block_no(
					sb, group) + run_start,
				run_length, GFP_NOFS, 0);
			if (!discard_error)
				*trimmed += run_length;
		}

		for (bit = run_start; bit < run_end; ++bit)
			ext3_clear_bit_atomic(
				sb_bgl_lock(sbi, group),
				bit, bitmap->b_data);

		spin_lock(sb_bgl_lock(sbi, group));
		le16_add_cpu(
			&desc->bg_free_blocks_count,
			(int)run_length);
		spin_unlock(sb_bgl_lock(sbi, group));
		percpu_counter_add(
			&sbi->s_freeblocks_counter,
			run_length);

		if (discard_error &&
		    discard_error != -EOPNOTSUPP) {
			error = discard_error;
			break;
		}
		if (fatal_signal_pending(current)) {
			error = -ERESTARTSYS;
			break;
		}

		cursor = run_end;
		cond_resched();
	}

	if (!error)
		error = ext3_journal_dirty_metadata(
			handle, bitmap);
	if (!error)
		error = ext3_journal_dirty_metadata(
			handle, desc_bh);

out_bitmap:
	brelse(bitmap);
out_stop:
	{
		const int stop_error =
			ext3_journal_stop(handle);
		if (!error)
			error = stop_error;
	}
	return error;
}

int ext3_trim_fs(
	struct super_block *sb,
	struct fstrim_range *range)
{
	const ext3_fsblk_t first_data =
		le32_to_cpu(EXT3_SB(sb)->s_es->s_first_data_block);
	const ext3_fsblk_t blocks =
		le32_to_cpu(EXT3_SB(sb)->s_es->s_blocks_count);
	u64 start;
	u64 end;
	u64 minimum;
	u64 trimmed = 0U;
	unsigned int first_group;
	unsigned int last_group;
	unsigned int group;
	int error = 0;

	if (!range || range->len < sb->s_blocksize)
		return -EINVAL;

	start = range->start >> sb->s_blocksize_bits;
	minimum = range->minlen >> sb->s_blocksize_bits;
	if (minimum > EXT3_BLOCKS_PER_GROUP(sb) ||
	    start >= blocks)
		return -EINVAL;

	end = start + (range->len >> sb->s_blocksize_bits);
	if (end == 0U)
		return -EINVAL;
	end--;
	if (end >= blocks)
		end = blocks - 1U;
	if (end < first_data) {
		range->len = 0U;
		return 0;
	}
	if (start < first_data)
		start = first_data;

	first_group =
		(unsigned int)((start - first_data) /
		EXT3_BLOCKS_PER_GROUP(sb));
	last_group =
		(unsigned int)((end - first_data) /
		EXT3_BLOCKS_PER_GROUP(sb));

	for (group = first_group;
	     group <= last_group;
	     ++group) {
		const ext3_fsblk_t group_first =
			ext3_group_first_block_no(sb, group);
		const ext3_grpblk_t local_first =
			group == first_group ?
			(ext3_grpblk_t)(start - group_first) : 0;
		const ext3_grpblk_t local_last =
			group == last_group ?
			(ext3_grpblk_t)(end - group_first) :
			(ext3_grpblk_t)(
				ifs_ext3_group_last_block(sb, group) -
				group_first);
		struct ext3_group_desc *desc =
			ext3_get_group_desc(sb, group, NULL);

		if (!desc) {
			error = -EIO;
			break;
		}
		if (le16_to_cpu(
			    desc->bg_free_blocks_count) < minimum)
			continue;

		error = ifs_ext3_trim_group(
			sb, group, local_first, local_last,
			(ext3_grpblk_t)minimum, &trimmed);
		if (error)
			break;
	}

	range->len = trimmed * sb->s_blocksize;
	return error;
}

#ifdef EXT3FS_DEBUG
unsigned long ext3_count_free(
	struct buffer_head *map,
	unsigned int bytes)
{
	return bytes * BITS_PER_BYTE -
		memweight(map->b_data, bytes);
}
#endif


/* ===== inode allocation ===== */
/*
 * Filesystem Support EXT3 inode allocation.
 *
 * This Linux adapter owns inode-bitmap reservation and EXT3 journal ordering.
 * Selection policy is deliberately separate from publication: a candidate
 * group is chosen first, then bitmap and descriptor accounting are changed
 * together through the caller's journal handle.
 */

#include <linux/buffer_head.h>
#include <linux/quotaops.h>
#include <linux/random.h>

#include "linux_adapter.h"

static struct buffer_head *ifs_ext3_read_inode_bitmap(
	struct super_block *sb, unsigned int group)
{
	struct ext3_group_desc *desc;
	u32 block;

	desc = ext3_get_group_desc(sb, group, NULL);
	if (!desc)
		return NULL;

	block = le32_to_cpu(desc->bg_inode_bitmap);
	if (block == 0U)
		return NULL;

	return sb_bread(sb, block);
}

static int ifs_ext3_inode_position(
	struct super_block *sb, unsigned long ino,
	unsigned int *group, unsigned int *bit)
{
	const u32 per_group = EXT3_INODES_PER_GROUP(sb);
	const u32 total = le32_to_cpu(EXT3_SB(sb)->s_es->s_inodes_count);
	u64 zero_based;

	if (!group || !bit || per_group == 0U ||
	    ino < EXT3_FIRST_INO(sb) || ino > total)
		return -EINVAL;

	zero_based = (u64)ino - 1U;
	*group = (unsigned int)(zero_based / per_group);
	*bit = (unsigned int)(zero_based % per_group);
	if (*group >= EXT3_SB(sb)->s_groups_count)
		return -EUCLEAN;
	return 0;
}

static int ifs_ext3_group_is_usable(
	struct super_block *sb, unsigned int group, bool directory)
{
	struct ext3_group_desc *desc;

	desc = ext3_get_group_desc(sb, group, NULL);
	if (!desc)
		return 0;
	if (le16_to_cpu(desc->bg_free_inodes_count) == 0U)
		return 0;
	if (!directory)
		return 1;
	return le16_to_cpu(desc->bg_free_blocks_count) != 0U;
}

static int ifs_ext3_choose_directory_group(
	struct super_block *sb, struct inode *parent)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	const unsigned int groups = sbi->s_groups_count;
	unsigned int free_inodes;
	unsigned int free_blocks;
	unsigned int average_inodes;
	unsigned int average_blocks;
	unsigned int start;
	unsigned int best = groups;
	unsigned int best_dirs = UINT_MAX;
	unsigned int step;

	if (groups == 0U)
		return -1;

	free_inodes =
		(unsigned int)percpu_counter_read_positive(&sbi->s_freeinodes_counter);
	free_blocks =
		(unsigned int)percpu_counter_read_positive(&sbi->s_freeblocks_counter);
	average_inodes = free_inodes / groups;
	average_blocks = free_blocks / groups;

	start = (EXT3_I(parent)->i_block_group +
		 (get_random_u32() % groups)) % groups;

	for (step = 0U; step < groups; ++step) {
		const unsigned int group = (start + step) % groups;
		struct ext3_group_desc *desc = ext3_get_group_desc(sb, group, NULL);
		unsigned int dirs;

		if (!desc)
			continue;
		if (le16_to_cpu(desc->bg_free_inodes_count) < average_inodes ||
		    le16_to_cpu(desc->bg_free_blocks_count) < average_blocks)
			continue;

		dirs = le16_to_cpu(desc->bg_used_dirs_count);
		if (dirs < best_dirs) {
			best = group;
			best_dirs = dirs;
		}
	}

	if (best != groups)
		return (int)best;

	for (step = 0U; step < groups; ++step) {
		const unsigned int group =
			(EXT3_I(parent)->i_block_group + step) % groups;

		if (ifs_ext3_group_is_usable(sb, group, true))
			return (int)group;
	}

	return -1;
}

static int ifs_ext3_choose_file_group(
	struct super_block *sb, struct inode *parent)
{
	const unsigned int groups = EXT3_SB(sb)->s_groups_count;
	const unsigned int parent_group = EXT3_I(parent)->i_block_group;
	unsigned int stride;
	unsigned int group;

	if (groups == 0U)
		return -1;

	if (parent_group < groups &&
	    ifs_ext3_group_is_usable(sb, parent_group, false))
		return (int)parent_group;

	group = (parent_group + (unsigned int)parent->i_ino) % groups;
	for (stride = 1U; stride < groups; stride <<= 1U) {
		group = (group + stride) % groups;
		if (ifs_ext3_group_is_usable(sb, group, false))
			return (int)group;
	}

	for (stride = 1U; stride <= groups; ++stride) {
		group = (parent_group + stride) % groups;
		if (ifs_ext3_group_is_usable(sb, group, false))
			return (int)group;
	}

	return -1;
}

static int ifs_ext3_mark_inode_accounting(
	handle_t *handle, struct super_block *sb,
	unsigned int group, bool directory, int delta)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	struct ext3_group_desc *desc;
	struct buffer_head *desc_bh;
	int error;

	desc = ext3_get_group_desc(sb, group, &desc_bh);
	if (!desc || !desc_bh)
		return -EIO;

	error = ext3_journal_get_write_access(handle, desc_bh);
	if (error)
		return error;

	spin_lock(sb_bgl_lock(sbi, group));
	le16_add_cpu(&desc->bg_free_inodes_count, delta);
	if (directory)
		le16_add_cpu(&desc->bg_used_dirs_count, -delta);
	spin_unlock(sb_bgl_lock(sbi, group));

	error = ext3_journal_dirty_metadata(handle, desc_bh);
	if (error)
		return error;

	if (delta < 0) {
		percpu_counter_dec(&sbi->s_freeinodes_counter);
		if (directory)
			percpu_counter_inc(&sbi->s_dirs_counter);
	} else {
		percpu_counter_inc(&sbi->s_freeinodes_counter);
		if (directory)
			percpu_counter_dec(&sbi->s_dirs_counter);
	}

	return 0;
}

static int ifs_ext3_change_inode_bit(
	handle_t *handle, struct super_block *sb,
	unsigned int group, unsigned int bit, bool allocate)
{
	struct buffer_head *bitmap;
	int changed;
	int error;

	bitmap = ifs_ext3_read_inode_bitmap(sb, group);
	if (!bitmap)
		return -EIO;

	error = ext3_journal_get_write_access(handle, bitmap);
	if (error)
		goto out;

	if (allocate)
		changed = !ext3_set_bit_atomic(
			sb_bgl_lock(EXT3_SB(sb), group), bit, bitmap->b_data);
	else
		changed = ext3_clear_bit_atomic(
			sb_bgl_lock(EXT3_SB(sb), group), bit, bitmap->b_data);

	if (!changed) {
		error = allocate ? -EAGAIN : -EUCLEAN;
		goto out_release;
	}

	error = ext3_journal_dirty_metadata(handle, bitmap);
	goto out;

out_release:
	journal_release_buffer(handle, bitmap);
out:
	brelse(bitmap);
	return error;
}

static int ifs_ext3_reserve_inode(
	handle_t *handle, struct super_block *sb,
	unsigned int preferred_group, bool directory,
	unsigned int *group_out, unsigned int *bit_out)
{
	struct ext3_sb_info *sbi = EXT3_SB(sb);
	const unsigned int groups = sbi->s_groups_count;
	const unsigned int per_group = EXT3_INODES_PER_GROUP(sb);
	unsigned int pass;

	if (!group_out || !bit_out || groups == 0U || per_group == 0U)
		return -EINVAL;

	for (pass = 0U; pass < groups; ++pass) {
		const unsigned int group = (preferred_group + pass) % groups;
		struct buffer_head *bitmap;
		unsigned long bit;
		unsigned long start = 0U;
		int error;

		if (!ifs_ext3_group_is_usable(sb, group, directory))
			continue;

		if (group == 0U && EXT3_FIRST_INO(sb) > 1U)
			start = EXT3_FIRST_INO(sb) - 1U;

		bitmap = ifs_ext3_read_inode_bitmap(sb, group);
		if (!bitmap)
			return -EIO;

		bit = ext3_find_next_zero_bit(
			(unsigned long *)bitmap->b_data, per_group, start);
		brelse(bitmap);
		while (bit < per_group) {
			error = ifs_ext3_change_inode_bit(
				handle, sb, group, (unsigned int)bit, true);
			if (error == -EAGAIN) {
				bit++;
				bitmap = ifs_ext3_read_inode_bitmap(sb, group);
				if (!bitmap)
					return -EIO;
				bit = ext3_find_next_zero_bit(
					(unsigned long *)bitmap->b_data,
					per_group, bit);
				brelse(bitmap);
				continue;
			}
			if (error)
				return error;

			error = ifs_ext3_mark_inode_accounting(
				handle, sb, group, directory, -1);
			if (error) {
				ifs_ext3_change_inode_bit(
					handle, sb, group, (unsigned int)bit, false);
				return error;
			}

			*group_out = group;
			*bit_out = (unsigned int)bit;
			return 0;
		}
	}

	return -ENOSPC;
}

static void ifs_ext3_rollback_inode_reservation(
	handle_t *handle, struct super_block *sb,
	unsigned int group, unsigned int bit, bool directory)
{
	int bit_error;
	int count_error;

	bit_error = ifs_ext3_change_inode_bit(
		handle, sb, group, bit, false);
	count_error = ifs_ext3_mark_inode_accounting(
		handle, sb, group, directory, 1);

	if (bit_error && bit_error != -EUCLEAN)
		ext3_std_error(sb, bit_error);
	if (count_error)
		ext3_std_error(sb, count_error);
}

void ext3_free_inode(handle_t *handle, struct inode *inode)
{
	struct super_block *sb;
	unsigned int group;
	unsigned int bit;
	const bool directory = S_ISDIR(inode->i_mode);
	int error;

	if (!handle || !inode || inode->i_nlink != 0U ||
	    atomic_read(&inode->i_count) > 1)
		return;

	sb = inode->i_sb;
	if (!sb)
		return;

	error = ifs_ext3_inode_position(
		sb, inode->i_ino, &group, &bit);
	if (error) {
		ext3_error(sb, __func__,
			   "invalid inode number %lu", inode->i_ino);
		return;
	}

	error = ifs_ext3_change_inode_bit(
		handle, sb, group, bit, false);
	if (error) {
		ext3_std_error(sb, error);
		return;
	}

	error = ifs_ext3_mark_inode_accounting(
		handle, sb, group, directory, 1);
	ext3_std_error(sb, error);
}

struct inode *ext3_new_inode(
	handle_t *handle, struct inode *dir,
	const struct qstr *qstr, umode_t mode)
{
	struct super_block *sb;
	struct ext3_sb_info *sbi;
	struct ext3_inode_info *info;
	struct inode *inode;
	unsigned int group;
	unsigned int bit;
	unsigned long ino;
	const bool directory = S_ISDIR(mode);
	int selected;
	int error;

	if (!handle || !dir || dir->i_nlink == 0U)
		return ERR_PTR(-EPERM);

	sb = dir->i_sb;
	sbi = EXT3_SB(sb);

	selected = directory ?
		ifs_ext3_choose_directory_group(sb, dir) :
		ifs_ext3_choose_file_group(sb, dir);
	if (selected < 0)
		return ERR_PTR(-ENOSPC);

	error = ifs_ext3_reserve_inode(
		handle, sb, (unsigned int)selected,
		directory, &group, &bit);
	if (error)
		return ERR_PTR(error);

	ino = (unsigned long)group * EXT3_INODES_PER_GROUP(sb) +
	      bit + 1U;
	if (ino < EXT3_FIRST_INO(sb) ||
	    ino > le32_to_cpu(sbi->s_es->s_inodes_count)) {
		ifs_ext3_rollback_inode_reservation(
			handle, sb, group, bit, directory);
		return ERR_PTR(-EUCLEAN);
	}

	inode = new_inode(sb);
	if (!inode) {
		ifs_ext3_rollback_inode_reservation(
			handle, sb, group, bit, directory);
		return ERR_PTR(-ENOMEM);
	}

	info = EXT3_I(inode);
	inode->i_ino = ino;
	inode->i_blocks = 0;

	if (test_opt(sb, GRPID)) {
		inode->i_mode = mode;
		inode->i_uid = current_fsuid();
		inode->i_gid = dir->i_gid;
	} else {
		inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	}

	{
		const struct timespec64 now = current_time(inode);
		inode_set_atime_to_ts(inode, now);
		inode_set_mtime_to_ts(inode, now);
		inode_set_ctime_to_ts(inode, now);
	}

	memset(info->i_data, 0, sizeof(info->i_data));
	info->i_dir_start_lookup = 0;
	info->i_disksize = 0;
	info->i_flags =
		ext3_mask_flags(mode, EXT3_I(dir)->i_flags & EXT3_FL_INHERITED);
#ifdef EXT3_FRAGMENTS
	info->i_faddr = 0;
	info->i_frag_no = 0;
	info->i_frag_size = 0;
#endif
	info->i_file_acl = 0;
	info->i_dir_acl = 0;
	info->i_dtime = 0;
	info->i_block_alloc_info = NULL;
	info->i_block_group = group;
	info->i_state_flags = 0;
	info->i_extra_isize =
		ino >= EXT3_FIRST_INO(sb) + 1U &&
		EXT3_INODE_SIZE(sb) > EXT3_GOOD_OLD_INODE_SIZE ?
		sizeof(struct ext3_inode) - EXT3_GOOD_OLD_INODE_SIZE : 0U;

	ext3_set_inode_flags(inode);
	ext3_set_inode_state(inode, EXT3_STATE_NEW);
	if (IS_DIRSYNC(inode))
		handle->h_sync = 1;

	if (insert_inode_locked(inode) < 0) {
		error = -EIO;
		goto fail_unpublished;
	}

	spin_lock(&sbi->s_next_gen_lock);
	inode->i_generation = sbi->s_next_generation++;
	spin_unlock(&sbi->s_next_gen_lock);

	dquot_initialize(inode);
	error = dquot_alloc_inode(inode);
	if (error)
		goto fail_inserted;

	error = ext3_init_acl(handle, inode, dir);
	if (error)
		goto fail_quota;

	error = ext3_init_security(handle, inode, dir, qstr);
	if (error)
		goto fail_quota;

	error = ext3_mark_inode_dirty(handle, inode);
	if (error)
		goto fail_quota;

	return inode;

fail_quota:
	dquot_free_inode(inode);
fail_inserted:
	dquot_drop(inode);
	inode->i_flags |= S_NOQUOTA;
	clear_nlink(inode);
	unlock_new_inode(inode);
	iput(inode);
	/*
	 * Once the inode is published, ext3_evict_inode() owns teardown and
	 * releases the bitmap reservation through ext3_free_inode().  Rolling
	 * it back here as well would double-free the inode number.
	 */
	ext3_std_error(sb, error);
	return ERR_PTR(error);

fail_unpublished:
	clear_nlink(inode);
	iput(inode);
	ifs_ext3_rollback_inode_reservation(
		handle, sb, group, bit, directory);
	ext3_std_error(sb, error);
	return ERR_PTR(error);
}

struct inode *ext3_orphan_get(struct super_block *sb, unsigned long ino)
{
	const unsigned long max_ino =
		le32_to_cpu(EXT3_SB(sb)->s_es->s_inodes_count);
	unsigned int group;
	unsigned int bit;
	struct buffer_head *bitmap;
	struct inode *inode;
	int error;

	error = ifs_ext3_inode_position(sb, ino, &group, &bit);
	if (error || ino > max_ino)
		return ERR_PTR(-EIO);

	bitmap = ifs_ext3_read_inode_bitmap(sb, group);
	if (!bitmap)
		return ERR_PTR(-EIO);

	if (!ext3_test_bit(bit, bitmap->b_data)) {
		brelse(bitmap);
		return ERR_PTR(-EIO);
	}
	brelse(bitmap);

	inode = ext3_iget(sb, ino);
	if (IS_ERR(inode))
		return inode;

	if ((inode->i_nlink != 0U && !ext3_can_truncate(inode)) ||
	    NEXT_ORPHAN(inode) > max_ino) {
		if (inode->i_nlink == 0U)
			inode->i_blocks = 0;
		iput(inode);
		return ERR_PTR(-EIO);
	}

	return inode;
}

unsigned long ext3_count_free_inodes(struct super_block *sb)
{
	unsigned long count = 0;
	unsigned int group;

	for (group = 0U; group < EXT3_SB(sb)->s_groups_count; ++group) {
		struct ext3_group_desc *desc =
			ext3_get_group_desc(sb, group, NULL);

		if (desc)
			count += le16_to_cpu(desc->bg_free_inodes_count);
		cond_resched();
	}

	return count;
}

unsigned long ext3_count_dirs(struct super_block *sb)
{
	unsigned long count = 0;
	unsigned int group;

	for (group = 0U; group < EXT3_SB(sb)->s_groups_count; ++group) {
		struct ext3_group_desc *desc =
			ext3_get_group_desc(sb, group, NULL);

		if (desc)
			count += le16_to_cpu(desc->bg_used_dirs_count);
	}

	return count;
}


/* ===== online growth publication ===== */
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

#include "linux_adapter.h"

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

