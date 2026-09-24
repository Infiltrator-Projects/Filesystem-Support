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
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/quotaops.h>
#include <linux/slab.h>

#include "ext3.h"

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
	const ext3_fsblk_t first =
		ext3_group_first_block_no(sb, group);
	const ext3_fsblk_t blocks =
		le32_to_cpu(EXT3_SB(sb)->s_es->s_blocks_count);
	ext3_fsblk_t last =
		first + EXT3_BLOCKS_PER_GROUP(sb) - 1U;

	if (last >= blocks)
		last = blocks - 1U;
	return last;
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
	spinlock_t *lock = sb_bgl_lock(EXT3_SB(sb), group);
	bool committed_busy = false;

	if (ext3_set_bit_atomic(lock, bit, bitmap->b_data))
		return 0;

	jbd_lock_bh_state(bitmap);
	if (jh->b_committed_data &&
	    ext3_test_bit(bit, jh->b_committed_data))
		committed_busy = true;
	jbd_unlock_bh_state(bitmap);

	if (committed_busy) {
		ext3_clear_bit_atomic(lock, bit, bitmap->b_data);
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

	start_group =
		(unsigned int)((goal -
		le32_to_cpu(es->s_first_data_block)) /
		EXT3_BLOCKS_PER_GROUP(sb));
	start_offset =
		(unsigned int)((goal -
		le32_to_cpu(es->s_first_data_block)) %
		EXT3_BLOCKS_PER_GROUP(sb));

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

static bool ifs_ext3_power_of(
	unsigned int value, unsigned int base)
{
	if (value < 1U || base < 2U)
		return false;
	while (value > 1U && value % base == 0U)
		value /= base;
	return value == 1U;
}

static bool ifs_ext3_sparse_group(unsigned int group)
{
	if (group <= 1U)
		return true;
	if ((group & 1U) == 0U)
		return false;
	return ifs_ext3_power_of(group, 3U) ||
	       ifs_ext3_power_of(group, 5U) ||
	       ifs_ext3_power_of(group, 7U);
}

int ext3_bg_has_super(struct super_block *sb, int group)
{
	if (group < 0)
		return 0;
	if (!EXT3_HAS_RO_COMPAT_FEATURE(
		    sb, EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER))
		return 1;
	return ifs_ext3_sparse_group((unsigned int)group);
}

static unsigned long ifs_ext3_meta_gdb_count(
	struct super_block *sb, unsigned int group)
{
	const unsigned long per_block =
		EXT3_DESC_PER_BLOCK(sb);
	const unsigned long meta_group =
		group / per_block;
	const unsigned long first =
		meta_group * per_block;
	const unsigned long last =
		first + per_block - 1U;

	return group == first ||
	       group == first + 1U ||
	       group == last ? 1U : 0U;
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
		sb, (unsigned int)group);
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
