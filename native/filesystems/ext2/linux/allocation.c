/* Infiltrator Filesystem Support — EXT2 Linux allocation adapter.
 * Block and inode allocation share one Linux-facing responsibility. Portable
 * geometry and format rules remain in the canonical EXT2 core.
 */

/*
 * Infiltrator Filesystem Support — EXT2 block allocation.
 *
 * The canonical EXT2 core owns filesystem geometry and sparse-super policy.
 * This Linux adapter owns buffer-cache access, reservation windows, quota
 * integration and atomic publication of allocation bitmap/accounting state.
 */

#include "linux_adapter.h"

#include <linux/buffer_head.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/overflow.h>
#include <linux/quotaops.h>
#include <linux/slab.h>

struct ext2_group_desc *ext2_get_group_desc(
	struct super_block *sb, unsigned int group,
	struct buffer_head **bh_out)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	unsigned long descriptor_block;
	unsigned long descriptor_index;
	struct ext2_group_desc *base;

	if (group >= sbi->s_groups_count)
		return NULL;

	descriptor_block = group >> EXT2_DESC_PER_BLOCK_BITS(sb);
	descriptor_index = group & (EXT2_DESC_PER_BLOCK(sb) - 1U);
	if (!sbi->s_group_desc ||
	    !sbi->s_group_desc[descriptor_block])
		return NULL;

	if (bh_out)
		*bh_out = sbi->s_group_desc[descriptor_block];

	base = (struct ext2_group_desc *)
		sbi->s_group_desc[descriptor_block]->b_data;
	return base + descriptor_index;
}

static bool ext2_group_metadata_reserved(
	struct super_block *sb, unsigned int group,
	struct ext2_group_desc *desc, struct buffer_head *bitmap)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	IfsExt2Superblock core_super = { 0 };
	IfsExt2GroupDescriptor core_desc = { 0 };
	IfsExt2Status status;

	core_super.first_data_block =
		le32_to_cpu(sbi->s_es->s_first_data_block);
	core_super.blocks_count =
		le32_to_cpu(sbi->s_es->s_blocks_count);
	core_super.blocks_per_group = sbi->s_blocks_per_group;
	core_super.inode_table_blocks_per_group = sbi->s_itb_per_group;
	core_super.group_count = sbi->s_groups_count;

	core_desc.block_bitmap = le32_to_cpu(desc->bg_block_bitmap);
	core_desc.inode_bitmap = le32_to_cpu(desc->bg_inode_bitmap);
	core_desc.inode_table = le32_to_cpu(desc->bg_inode_table);
	core_desc.free_blocks_count = le16_to_cpu(desc->bg_free_blocks_count);
	core_desc.free_inodes_count = le16_to_cpu(desc->bg_free_inodes_count);
	core_desc.used_dirs_count = le16_to_cpu(desc->bg_used_dirs_count);

	status = ifs_ext2_validate_group_metadata_bitmap(
		&core_super, group, &core_desc,
		bitmap->b_data, sb->s_blocksize);
	if (status == IFS_EXT2_OK)
		return true;

	ext2_error(sb, __func__,
		   "group %u has corrupt metadata allocation map: %s",
		   group, ifs_ext2_status_string(status));
	return false;
}

static struct buffer_head *ext2_read_block_bitmap(
	struct super_block *sb, unsigned int group)
{
	struct ext2_group_desc *desc;
	struct buffer_head *bh;
	u32 block;
	int status;

	desc = ext2_get_group_desc(sb, group, NULL);
	if (!desc)
		return NULL;

	block = le32_to_cpu(desc->bg_block_bitmap);
	bh = sb_getblk(sb, block);
	if (!bh)
		return NULL;

	status = bh_read(bh, 0);
	if (status < 0) {
		brelse(bh);
		ext2_error(sb, __func__,
			   "cannot read block bitmap %u for group %u",
			   block, group);
		return NULL;
	}

	if (status == 0 &&
	    !ext2_group_metadata_reserved(sb, group, desc, bh)) {
		brelse(bh);
		return NULL;
	}

	return bh;
}

static void ext2_adjust_group_free_blocks(
	struct super_block *sb, unsigned int group,
	struct ext2_group_desc *desc, struct buffer_head *desc_bh,
	int delta)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	unsigned int free_count;

	if (delta == 0)
		return;

	spin_lock(sb_bgl_lock(sbi, group));
	free_count = le16_to_cpu(desc->bg_free_blocks_count);
	if (delta < 0 && free_count < (unsigned int)(-delta)) {
		spin_unlock(sb_bgl_lock(sbi, group));
		ext2_error(sb, __func__,
			   "free-block count underflow in group %u", group);
		return;
	}
	desc->bg_free_blocks_count =
		cpu_to_le16((unsigned int)((int)free_count + delta));
	spin_unlock(sb_bgl_lock(sbi, group));
	mark_buffer_dirty(desc_bh);
}

static bool ext2_reservation_empty(const struct ext2_reserve_window *window)
{
	return window->_rsv_end == EXT2_RESERVE_WINDOW_NOT_ALLOCATED;
}

static bool ext2_reservation_intersects_group(
	const struct ext2_reserve_window *window,
	struct super_block *sb, unsigned int group)
{
	const ext2_fsblk_t first = ext2_group_first_block_no(sb, group);
	const ext2_fsblk_t last = ext2_group_last_block_no(sb, group);

	return window->_rsv_start <= last && window->_rsv_end >= first;
}

static bool ext2_goal_inside_reservation(
	const struct ext2_reserve_window *window,
	struct super_block *sb, unsigned int group,
	ext2_grpblk_t group_goal)
{
	ext2_fsblk_t absolute;

	if (!ext2_reservation_intersects_group(window, sb, group))
		return false;
	if (group_goal < 0)
		return true;

	absolute = ext2_group_first_block_no(sb, group) + group_goal;
	return absolute >= window->_rsv_start &&
	       absolute <= window->_rsv_end;
}

static struct ext2_reserve_window_node *ext2_reservation_at_or_before(
	struct rb_root *root, ext2_fsblk_t block)
{
	struct rb_node *node = root->rb_node;
	struct ext2_reserve_window_node *found = NULL;

	while (node) {
		struct ext2_reserve_window_node *entry =
			rb_entry(node, struct ext2_reserve_window_node, rsv_node);

		if (block < entry->rsv_start) {
			node = node->rb_left;
		} else {
			found = entry;
			if (block <= entry->rsv_end)
				break;
			node = node->rb_right;
		}
	}

	return found;
}

void ext2_rsv_window_add(
	struct super_block *sb, struct ext2_reserve_window_node *window)
{
	struct rb_root *root = &EXT2_SB(sb)->s_rsv_window_root;
	struct rb_node **link = &root->rb_node;
	struct rb_node *parent = NULL;

	while (*link) {
		struct ext2_reserve_window_node *entry =
			rb_entry(*link, struct ext2_reserve_window_node, rsv_node);

		parent = *link;
		if (window->rsv_end < entry->rsv_start)
			link = &(*link)->rb_left;
		else if (window->rsv_start > entry->rsv_end)
			link = &(*link)->rb_right;
		else
			BUG();
	}

	rb_link_node(&window->rsv_node, parent, link);
	rb_insert_color(&window->rsv_node, root);
}

static void ext2_reservation_remove(
	struct super_block *sb, struct ext2_reserve_window_node *window)
{
	if (ext2_reservation_empty(&window->rsv_window))
		return;

	rb_erase(&window->rsv_node, &EXT2_SB(sb)->s_rsv_window_root);
	window->rsv_start = EXT2_RESERVE_WINDOW_NOT_ALLOCATED;
	window->rsv_end = EXT2_RESERVE_WINDOW_NOT_ALLOCATED;
	window->rsv_alloc_hit = 0U;
}

void ext2_init_block_alloc_info(struct inode *inode)
{
	struct ext2_block_alloc_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info) {
		EXT2_I(inode)->i_block_alloc_info = NULL;
		return;
	}

	info->rsv_window_node.rsv_start =
		EXT2_RESERVE_WINDOW_NOT_ALLOCATED;
	info->rsv_window_node.rsv_end =
		EXT2_RESERVE_WINDOW_NOT_ALLOCATED;
	info->rsv_window_node.rsv_goal_size =
		test_opt(inode->i_sb, RESERVATION)
		? EXT2_DEFAULT_RESERVE_BLOCKS : 0U;
	info->rsv_window_node.rsv_alloc_hit = 0U;
	EXT2_I(inode)->i_block_alloc_info = info;
}

void ext2_discard_reservation(struct inode *inode)
{
	struct ext2_block_alloc_info *info =
		EXT2_I(inode)->i_block_alloc_info;
	struct ext2_sb_info *sbi = EXT2_SB(inode->i_sb);

	if (!info)
		return;

	spin_lock(&sbi->s_rsv_window_lock);
	ext2_reservation_remove(
		inode->i_sb, &info->rsv_window_node);
	spin_unlock(&sbi->s_rsv_window_lock);
}

static int ext2_find_reservation_gap(
	struct super_block *sb,
	struct ext2_reserve_window_node *window,
	ext2_fsblk_t start, ext2_fsblk_t last)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_reserve_window_node *cursor;
	ext2_fsblk_t candidate = start;
	ext2_fsblk_t end;
	unsigned int size = window->rsv_goal_size;

	if (size == 0U)
		return -ENOSPC;

	cursor = ext2_reservation_at_or_before(
		&sbi->s_rsv_window_root, candidate);
	if (cursor && candidate <= cursor->rsv_end) {
		if (check_add_overflow(
			    cursor->rsv_end, (ext2_fsblk_t)1, &candidate))
			return -ENOSPC;
	}

	while (candidate <= last) {
		struct rb_node *next_node;
		struct ext2_reserve_window_node *next = NULL;

		if (check_add_overflow(
			    candidate, (ext2_fsblk_t)(size - 1U), &end) ||
		    end > last)
			return -ENOSPC;

		cursor = ext2_reservation_at_or_before(
			&sbi->s_rsv_window_root, candidate);
		if (cursor) {
			next_node = rb_next(&cursor->rsv_node);
			if (next_node)
				next = rb_entry(
					next_node,
					struct ext2_reserve_window_node,
					rsv_node);
		} else {
			next_node = rb_first(&sbi->s_rsv_window_root);
			if (next_node)
				next = rb_entry(
					next_node,
					struct ext2_reserve_window_node,
					rsv_node);
		}

		if (!next || end < next->rsv_start)
			break;

		if (check_add_overflow(
			    next->rsv_end, (ext2_fsblk_t)1, &candidate))
			return -ENOSPC;
	}

	ext2_reservation_remove(sb, window);
	window->rsv_start = candidate;
	window->rsv_end = end;
	window->rsv_alloc_hit = 0U;
	ext2_rsv_window_add(sb, window);
	return 0;
}

static int ext2_refresh_reservation(
	struct super_block *sb, unsigned int group,
	struct buffer_head *bitmap,
	ext2_grpblk_t group_goal,
	struct ext2_reserve_window_node *window)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	const ext2_fsblk_t group_first =
		ext2_group_first_block_no(sb, group);
	const ext2_fsblk_t group_last =
		ext2_group_last_block_no(sb, group);
	ext2_fsblk_t start = group_first;
	ext2_grpblk_t free_bit;
	int result;

	if (group_goal >= 0)
		start += group_goal;

	if (!ext2_reservation_empty(&window->rsv_window) &&
	    window->rsv_alloc_hit >
	    (window->rsv_end - window->rsv_start + 1U) / 2U) {
		window->rsv_goal_size =
			min_t(unsigned int,
			      window->rsv_goal_size * 2U,
			      EXT2_MAX_RESERVE_BLOCKS);
	}

	spin_lock(&sbi->s_rsv_window_lock);
	result = ext2_find_reservation_gap(
		sb, window, start, group_last);
	spin_unlock(&sbi->s_rsv_window_lock);
	if (result != 0)
		return result;

	free_bit = ext2_find_next_zero_bit(
		bitmap->b_data,
		group_last - group_first + 1U,
		window->rsv_start - group_first);
	if (free_bit > group_last - group_first ||
	    group_first + free_bit > window->rsv_end) {
		spin_lock(&sbi->s_rsv_window_lock);
		ext2_reservation_remove(sb, window);
		spin_unlock(&sbi->s_rsv_window_lock);
		return -ENOSPC;
	}

	return 0;
}

static ext2_grpblk_t ext2_find_candidate_bit(
	struct buffer_head *bitmap, ext2_grpblk_t start,
	ext2_grpblk_t end)
{
	ext2_grpblk_t bit;

	if (start < 0)
		start = 0;
	if (start >= end)
		return -1;

	bit = ext2_find_next_zero_bit(bitmap->b_data, end, start);
	return bit < end ? bit : -1;
}

static ext2_grpblk_t ext2_claim_run(
	struct super_block *sb, unsigned int group,
	struct buffer_head *bitmap,
	ext2_grpblk_t goal, unsigned long *count,
	const struct ext2_reserve_window *reservation)
{
	const ext2_fsblk_t group_first =
		ext2_group_first_block_no(sb, group);
	const ext2_fsblk_t group_last =
		ext2_group_last_block_no(sb, group);
	ext2_grpblk_t start = 0;
	ext2_grpblk_t end =
		(ext2_grpblk_t)(group_last - group_first + 1U);
	ext2_grpblk_t bit;
	unsigned long claimed = 0U;

	if (reservation) {
		if (reservation->_rsv_start > group_first)
			start = reservation->_rsv_start - group_first;
		if (reservation->_rsv_end < group_last)
			end = reservation->_rsv_end - group_first + 1U;
		if (goal < start || goal >= end)
			goal = -1;
	}

	bit = goal >= 0
		? ext2_find_candidate_bit(bitmap, goal, end)
		: ext2_find_candidate_bit(bitmap, start, end);
	if (bit < 0)
		return -1;

	if (!reservation && goal < 0) {
		unsigned int rewind = 0U;

		while (bit > start && rewind < 7U &&
		       !ext2_test_bit(bit - 1, bitmap->b_data)) {
			--bit;
			++rewind;
		}
	}

	goal = bit;
	while (claimed < *count && bit < end) {
		if (ext2_set_bit_atomic(
			    sb_bgl_lock(EXT2_SB(sb), group),
			    bit, bitmap->b_data)) {
			if (claimed != 0U)
				break;
			++bit;
			goal = bit;
			continue;
		}
		++claimed;
		++bit;
	}

	if (claimed == 0U)
		return -1;

	*count = claimed;
	return goal;
}

static ext2_grpblk_t ext2_claim_with_reservation(
	struct super_block *sb, unsigned int group,
	struct buffer_head *bitmap,
	ext2_grpblk_t goal,
	struct ext2_reserve_window_node *window,
	unsigned long *count)
{
	unsigned long wanted = *count;
	ext2_grpblk_t result;

	if (!window)
		return ext2_claim_run(
			sb, group, bitmap, goal, count, NULL);

	for (;;) {
		if (ext2_reservation_empty(&window->rsv_window) ||
		    !ext2_goal_inside_reservation(
			    &window->rsv_window, sb, group, goal)) {
			if (window->rsv_goal_size < wanted)
				window->rsv_goal_size = wanted;
			if (ext2_refresh_reservation(
				    sb, group, bitmap, goal, window) != 0)
				return -1;
			if (!ext2_goal_inside_reservation(
				    &window->rsv_window, sb, group, goal))
				goal = -1;
		}

		*count = wanted;
		result = ext2_claim_run(
			sb, group, bitmap, goal, count,
			&window->rsv_window);
		if (result >= 0) {
			window->rsv_alloc_hit += *count;
			return result;
		}

		spin_lock(&EXT2_SB(sb)->s_rsv_window_lock);
		ext2_reservation_remove(sb, window);
		spin_unlock(&EXT2_SB(sb)->s_rsv_window_lock);
	}
}

static bool ext2_range_hits_system_zone(
	struct super_block *sb, struct ext2_group_desc *desc,
	ext2_fsblk_t start, unsigned long count)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	ext2_fsblk_t last;

	if (count == 0U ||
	    check_add_overflow(
		    start, (ext2_fsblk_t)(count - 1U), &last))
		return true;

	if (in_range(
		    le32_to_cpu(desc->bg_block_bitmap), start, count) ||
	    in_range(
		    le32_to_cpu(desc->bg_inode_bitmap), start, count) ||
	    in_range(start, le32_to_cpu(desc->bg_inode_table),
		     sbi->s_itb_per_group) ||
	    in_range(last, le32_to_cpu(desc->bg_inode_table),
		     sbi->s_itb_per_group))
		return true;

	return false;
}

void ext2_free_blocks(
	struct inode *inode, ext2_fsblk_t block, unsigned long count)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = sbi->s_es;
	unsigned long total_freed = 0U;

	if (!ext2_data_block_valid(sbi, block, count)) {
		ext2_error(sb, __func__,
			   "invalid free range " E2FSBLK "+%lu",
			   block, count);
		return;
	}

	while (count != 0U) {
		ifs_ext2_u32 group;
		ifs_ext2_u32 bit;
		struct ext2_group_desc *desc;
		struct buffer_head *desc_bh;
		struct buffer_head *bitmap;
		unsigned long this_count;
		unsigned long freed = 0U;
		unsigned long i;

		if (ifs_ext2_block_group_position(
			    le32_to_cpu(es->s_first_data_block),
			    EXT2_BLOCKS_PER_GROUP(sb),
			    le32_to_cpu(es->s_blocks_count),
			    (ifs_ext2_u32)block,
			    &group, &bit) != IFS_EXT2_OK) {
			ext2_error(sb, __func__,
				   "free range escapes filesystem geometry");
			break;
		}

		this_count = min_t(
			unsigned long, count,
			EXT2_BLOCKS_PER_GROUP(sb) - bit);

		desc = ext2_get_group_desc(sb, group, &desc_bh);
		if (!desc)
			break;
		if (ext2_range_hits_system_zone(
			    sb, desc, block, this_count)) {
			ext2_error(sb, __func__,
				   "attempt to free EXT2 metadata blocks");
			break;
		}

		bitmap = ext2_read_block_bitmap(sb, group);
		if (!bitmap)
			break;

		for (i = 0; i < this_count; ++i) {
			if (ext2_clear_bit_atomic(
				    sb_bgl_lock(sbi, group),
				    bit + i, bitmap->b_data))
				++freed;
			else
				ext2_error(sb, __func__,
					   "block " E2FSBLK " was already free",
					   block + i);
		}

		if (freed != 0U) {
			mark_buffer_dirty(bitmap);
			if (sb->s_flags & SB_SYNCHRONOUS)
				sync_dirty_buffer(bitmap);
			ext2_adjust_group_free_blocks(
				sb, group, desc, desc_bh, freed);
			total_freed += freed;
		}
		brelse(bitmap);

		block += this_count;
		count -= this_count;
	}

	if (total_freed != 0U) {
		percpu_counter_add(
			&sbi->s_freeblocks_counter, total_freed);
		dquot_free_block_nodirty(inode, total_freed);
		mark_inode_dirty(inode);
	}
}

static bool ext2_caller_may_use_reserved_blocks(
	struct ext2_sb_info *sbi)
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

static bool ext2_has_allocatable_blocks(struct ext2_sb_info *sbi)
{
	const ext2_fsblk_t free_blocks =
		percpu_counter_read_positive(&sbi->s_freeblocks_counter);
	const ext2_fsblk_t reserved =
		le32_to_cpu(sbi->s_es->s_r_blocks_count);

	if (free_blocks > reserved)
		return true;
	return ext2_caller_may_use_reserved_blocks(sbi) &&
	       free_blocks != 0U;
}

int ext2_data_block_valid(
	struct ext2_sb_info *sbi, ext2_fsblk_t start,
	unsigned int count)
{
	return ifs_ext2_data_block_range_valid(
		le32_to_cpu(sbi->s_es->s_first_data_block),
		le32_to_cpu(sbi->s_es->s_blocks_count),
		sbi->s_sb_block,
		start,
		count);
}

static int ext2_try_group(
	struct inode *inode, unsigned int group,
	ext2_grpblk_t goal,
	struct ext2_reserve_window_node *reservation,
	unsigned long *count,
	ext2_fsblk_t *block_out,
	struct ext2_group_desc **desc_out,
	struct buffer_head **desc_bh_out,
	struct buffer_head **bitmap_out)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_group_desc *desc;
	struct buffer_head *desc_bh;
	struct buffer_head *bitmap;
	ext2_grpblk_t group_block;
	ext2_fsblk_t absolute;
	unsigned long wanted = *count;

	desc = ext2_get_group_desc(sb, group, &desc_bh);
	if (!desc)
		return -EIO;
	if (le16_to_cpu(desc->bg_free_blocks_count) == 0U)
		return -ENOSPC;

	bitmap = ext2_read_block_bitmap(sb, group);
	if (!bitmap)
		return -EIO;

	group_block = ext2_claim_with_reservation(
		sb, group, bitmap, goal, reservation, count);
	if (group_block < 0) {
		brelse(bitmap);
		*count = wanted;
		return -ENOSPC;
	}

	absolute =
		ext2_group_first_block_no(sb, group) + group_block;
	if (ext2_range_hits_system_zone(sb, desc, absolute, *count) ||
	    !ext2_data_block_valid(EXT2_SB(sb), absolute, *count)) {
		unsigned long i;

		for (i = 0; i < *count; ++i)
			ext2_clear_bit_atomic(
				sb_bgl_lock(EXT2_SB(sb), group),
				group_block + i, bitmap->b_data);
		mark_buffer_dirty(bitmap);
		brelse(bitmap);
		*count = wanted;
		return -EUCLEAN;
	}

	*block_out = absolute;
	*desc_out = desc;
	*desc_bh_out = desc_bh;
	*bitmap_out = bitmap;
	return 0;
}

ext2_fsblk_t ext2_new_blocks(
	struct inode *inode, ext2_fsblk_t goal,
	unsigned long *count, int *errp, unsigned int flags)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = sbi->s_es;
	struct ext2_block_alloc_info *allocation =
		EXT2_I(inode)->i_block_alloc_info;
	struct ext2_reserve_window_node *reservation = NULL;
	struct ext2_group_desc *desc = NULL;
	struct buffer_head *desc_bh = NULL;
	struct buffer_head *bitmap = NULL;
	unsigned long requested;
	unsigned long attempt_count;
	unsigned int start_group;
	unsigned int group;
	unsigned int i;
	ifs_ext2_u32 mapped_group;
	ifs_ext2_u32 mapped_offset;
	ext2_fsblk_t block = 0U;
	int result;

	if (!count || !errp || *count == 0U)
		return 0U;

	requested = *count;
	*errp = -ENOSPC;

	result = dquot_alloc_block(inode, requested);
	if (result != 0) {
		*errp = result;
		return 0U;
	}

	if (!ext2_has_allocatable_blocks(sbi))
		goto fail_quota;

	if (goal < le32_to_cpu(es->s_first_data_block) ||
	    goal >= le32_to_cpu(es->s_blocks_count))
		goal = le32_to_cpu(es->s_first_data_block);

	if (ifs_ext2_block_group_position(
		    le32_to_cpu(es->s_first_data_block),
		    EXT2_BLOCKS_PER_GROUP(sb),
		    le32_to_cpu(es->s_blocks_count),
		    (ifs_ext2_u32)goal,
		    &mapped_group, &mapped_offset) != IFS_EXT2_OK) {
		*errp = -EIO;
		goto fail_quota;
	}

	start_group = mapped_group;
	if (!(flags & EXT2_ALLOC_NORESERVE) &&
	    allocation &&
	    allocation->rsv_window_node.rsv_goal_size != 0U)
		reservation = &allocation->rsv_window_node;

	for (attempt_count = 0U;
	     attempt_count < (reservation ? 2U : 1U);
	     ++attempt_count) {
		for (i = 0U; i < sbi->s_groups_count; ++i) {
			ext2_grpblk_t group_goal;
			unsigned long wanted = requested;

			group = (start_group + i) % sbi->s_groups_count;
			group_goal = group == start_group
				? (ext2_grpblk_t)mapped_offset : -1;

			result = ext2_try_group(
				inode, group, group_goal,
				reservation, &wanted,
				&block, &desc, &desc_bh, &bitmap);
			if (result == -EIO) {
				*errp = -EIO;
				goto fail_quota;
			}
			if (result != 0)
				continue;

			*count = wanted;
			ext2_adjust_group_free_blocks(
				sb, group, desc, desc_bh,
				-(int)wanted);
			percpu_counter_sub(
				&sbi->s_freeblocks_counter, wanted);

			mark_buffer_dirty(bitmap);
			if (sb->s_flags & SB_SYNCHRONOUS)
				sync_dirty_buffer(bitmap);
			brelse(bitmap);

			if (wanted < requested) {
				dquot_free_block_nodirty(
					inode, requested - wanted);
				mark_inode_dirty(inode);
			}

			*errp = 0;
			return block;
		}

		reservation = NULL;
	}

fail_quota:
	dquot_free_block_nodirty(inode, requested);
	mark_inode_dirty(inode);
	return 0U;
}

#ifdef EXT2FS_DEBUG
unsigned long ext2_count_free(
	struct buffer_head *map, unsigned int bytes)
{
	return bytes * BITS_PER_BYTE -
	       memweight(map->b_data, bytes);
}
#endif

unsigned long ext2_count_free_blocks(struct super_block *sb)
{
	unsigned long total = 0U;
	unsigned int group;

	for (group = 0U;
	     group < EXT2_SB(sb)->s_groups_count;
	     ++group) {
		struct ext2_group_desc *desc =
			ext2_get_group_desc(sb, group, NULL);

		if (desc)
			total +=
				le16_to_cpu(desc->bg_free_blocks_count);
	}
	return total;
}

int ext2_bg_has_super(struct super_block *sb, int group)
{
	const int sparse =
		EXT2_HAS_RO_COMPAT_FEATURE(
			sb, EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) != 0;

	if (group < 0)
		return 0;

	return ifs_ext2_group_has_super(
		sparse, (ifs_ext2_u32)group);
}

unsigned long ext2_bg_num_gdb(
	struct super_block *sb, int group)
{
	return ext2_bg_has_super(sb, group)
		? EXT2_SB(sb)->s_gdb_count : 0U;
}


/* ===== inode allocation ===== */
/*
 * Infiltrator Filesystem Support — EXT2 inode allocation.
 *
 * Group selection is policy. Bitmap state, group counters and global counters
 * are correctness state and are updated together under the owning group lock.
 */

#include <linux/backing-dev.h>
#include <linux/buffer_head.h>
#include <linux/quotaops.h>
#include <linux/random.h>
#include <linux/sched.h>

#include "linux_adapter.h"

#define EXT2_DIR_INODE_COST 64
#define EXT2_DIR_BLOCK_COST 256

static struct buffer_head *ext2_read_inode_bitmap(
	struct super_block *sb, unsigned int group)
{
	struct ext2_group_desc *desc;
	u32 block;

	desc = ext2_get_group_desc(sb, group, NULL);
	if (!desc)
		return NULL;

	block = le32_to_cpu(desc->bg_inode_bitmap);
	if (block == 0U) {
		ext2_error(sb, __func__,
			   "group %u has no inode bitmap", group);
		return NULL;
	}

	{
		struct buffer_head *bh = sb_bread(sb, block);

		if (!bh)
			ext2_error(sb, __func__,
				   "cannot read inode bitmap for group %u at block %u",
				   group, block);
		return bh;
	}
}

static int ext2_inode_position(
	struct super_block *sb, ino_t ino,
	unsigned int *group, unsigned int *bit)
{
	const u32 inodes_per_group = EXT2_INODES_PER_GROUP(sb);
	const u32 inode_count =
		le32_to_cpu(EXT2_SB(sb)->s_es->s_inodes_count);
	u64 zero_based;

	if (!group || !bit || inodes_per_group == 0U ||
	    ino < EXT2_FIRST_INO(sb) || ino > inode_count)
		return -EINVAL;

	zero_based = (u64)ino - 1U;
	*group = (unsigned int)(zero_based / inodes_per_group);
	*bit = (unsigned int)(zero_based % inodes_per_group);

	if (*group >= EXT2_SB(sb)->s_groups_count)
		return -EUCLEAN;
	return 0;
}

static void ext2_account_inode_release(
	struct super_block *sb, unsigned int group, bool directory)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_group_desc *desc;
	struct buffer_head *desc_bh;

	desc = ext2_get_group_desc(sb, group, &desc_bh);
	if (!desc) {
		ext2_error(sb, __func__,
			   "missing group descriptor %u", group);
		return;
	}

	spin_lock(sb_bgl_lock(sbi, group));
	le16_add_cpu(&desc->bg_free_inodes_count, 1);
	if (directory)
		le16_add_cpu(&desc->bg_used_dirs_count, -1);
	spin_unlock(sb_bgl_lock(sbi, group));

	percpu_counter_inc(&sbi->s_freeinodes_counter);
	if (directory)
		percpu_counter_dec(&sbi->s_dirs_counter);
	mark_buffer_dirty(desc_bh);
}

void ext2_free_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bitmap_bh;
	unsigned int group;
	unsigned int bit;
	int result;

	dquot_free_inode(inode);
	dquot_drop(inode);

	result = ext2_inode_position(sb, inode->i_ino, &group, &bit);
	if (result != 0) {
		ext2_error(sb, __func__,
			   "invalid inode %lu", (unsigned long)inode->i_ino);
		return;
	}

	bitmap_bh = ext2_read_inode_bitmap(sb, group);
	if (!bitmap_bh)
		return;

	if (!ext2_clear_bit_atomic(
		    sb_bgl_lock(sbi, group), bit, bitmap_bh->b_data)) {
		ext2_error(sb, __func__,
			   "inode %lu bitmap bit was already clear",
			   (unsigned long)inode->i_ino);
	} else {
		ext2_account_inode_release(
			sb, group, S_ISDIR(inode->i_mode));
	}

	mark_buffer_dirty(bitmap_bh);
	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);
	brelse(bitmap_bh);
}

static void ext2_preread_inode(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ext2_group_desc *desc;
	unsigned int group;
	unsigned int bit;
	u64 byte_offset;
	u32 table_block;

	if (ext2_inode_position(sb, inode->i_ino, &group, &bit) != 0)
		return;

	desc = ext2_get_group_desc(sb, group, NULL);
	if (!desc)
		return;

	byte_offset = (u64)bit * EXT2_INODE_SIZE(sb);
	table_block = le32_to_cpu(desc->bg_inode_table) +
		(u32)(byte_offset >> EXT2_BLOCK_SIZE_BITS(sb));
	sb_breadahead(sb, table_block);
}

static int ext2_choose_old_directory_group(struct super_block *sb)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	const int groups = sbi->s_groups_count;
	const unsigned long free_inodes = ext2_count_free_inodes(sb);
	const unsigned long average =
		groups > 0 ? free_inodes / (unsigned long)groups : 0U;
	struct ext2_group_desc *best = NULL;
	int best_group = -1;
	int group;

	for (group = 0; group < groups; ++group) {
		struct ext2_group_desc *desc =
			ext2_get_group_desc(sb, group, NULL);

		if (!desc)
			continue;
		if (le16_to_cpu(desc->bg_free_inodes_count) < average)
			continue;
		if (!best ||
		    le16_to_cpu(desc->bg_free_blocks_count) >
		    le16_to_cpu(best->bg_free_blocks_count)) {
			best = desc;
			best_group = group;
		}
	}

	return best_group;
}

static int ext2_choose_directory_group(
	struct super_block *sb, struct inode *parent)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = sbi->s_es;
	const int groups = sbi->s_groups_count;
	const int inodes_per_group = EXT2_INODES_PER_GROUP(sb);
	int parent_group = EXT2_I(parent)->i_block_group;
	int free_inodes;
	int free_blocks;
	int directories;
	int average_inodes;
	int average_blocks;
	int max_dirs;
	int min_inodes;
	int min_blocks;
	int blocks_per_dir;
	int max_debt;
	int group;
	int i;

	if (groups <= 0)
		return -1;

	free_inodes =
		percpu_counter_read_positive(&sbi->s_freeinodes_counter);
	free_blocks =
		percpu_counter_read_positive(&sbi->s_freeblocks_counter);
	directories =
		percpu_counter_read_positive(&sbi->s_dirs_counter);
	average_inodes = free_inodes / groups;
	average_blocks = free_blocks / groups;

	if (parent == d_inode(sb->s_root) ||
	    (EXT2_I(parent)->i_flags & EXT2_TOPDIR_FL)) {
		int best_group = -1;
		int best_dirs = inodes_per_group;
		int start = get_random_u32_below(groups);

		for (i = 0; i < groups; ++i) {
			struct ext2_group_desc *desc;

			group = (start + i) % groups;
			desc = ext2_get_group_desc(sb, group, NULL);
			if (!desc)
				continue;
			if (le16_to_cpu(desc->bg_free_inodes_count) <
			    average_inodes)
				continue;
			if (le16_to_cpu(desc->bg_free_blocks_count) <
			    average_blocks)
				continue;
			if (le16_to_cpu(desc->bg_used_dirs_count) >= best_dirs)
				continue;

			best_group = group;
			best_dirs = le16_to_cpu(desc->bg_used_dirs_count);
		}
		if (best_group >= 0)
			return best_group;
	}

	if (directories <= 0)
		directories = 1;
	blocks_per_dir =
		(le32_to_cpu(es->s_blocks_count) - free_blocks) / directories;
	max_dirs = directories / groups + inodes_per_group / 16;
	min_inodes = average_inodes - inodes_per_group / 4;
	min_blocks =
		average_blocks - EXT2_BLOCKS_PER_GROUP(sb) / 4;
	max_debt = EXT2_BLOCKS_PER_GROUP(sb) /
		max(blocks_per_dir, EXT2_DIR_BLOCK_COST);
	if (max_debt * EXT2_DIR_INODE_COST > inodes_per_group)
		max_debt = inodes_per_group / EXT2_DIR_INODE_COST;
	max_debt = clamp(max_debt, 1, 255);

	for (i = 0; i < groups; ++i) {
		struct ext2_group_desc *desc;

		group = (parent_group + i) % groups;
		desc = ext2_get_group_desc(sb, group, NULL);
		if (!desc)
			continue;
		if (sbi->s_debts[group] >= max_debt)
			continue;
		if (le16_to_cpu(desc->bg_used_dirs_count) >= max_dirs)
			continue;
		if (le16_to_cpu(desc->bg_free_inodes_count) < min_inodes)
			continue;
		if (le16_to_cpu(desc->bg_free_blocks_count) < min_blocks)
			continue;
		return group;
	}

	for (i = 0; i < groups; ++i) {
		struct ext2_group_desc *desc;

		group = (parent_group + i) % groups;
		desc = ext2_get_group_desc(sb, group, NULL);
		if (desc &&
		    le16_to_cpu(desc->bg_free_inodes_count) >= average_inodes)
			return group;
	}

	if (average_inodes != 0) {
		for (i = 0; i < groups; ++i) {
			struct ext2_group_desc *desc =
				ext2_get_group_desc(sb, i, NULL);

			if (desc && le16_to_cpu(desc->bg_free_inodes_count) != 0)
				return i;
		}
	}

	return -1;
}

static int ext2_choose_nondirectory_group(
	struct super_block *sb, struct inode *parent)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	const int groups = sbi->s_groups_count;
	const int parent_group = EXT2_I(parent)->i_block_group;
	struct ext2_group_desc *desc;
	int group;
	int step;
	int i;

	if (groups <= 0)
		return -1;

	desc = ext2_get_group_desc(sb, parent_group, NULL);
	if (desc &&
	    le16_to_cpu(desc->bg_free_inodes_count) != 0 &&
	    le16_to_cpu(desc->bg_free_blocks_count) != 0)
		return parent_group;

	group = (parent_group + parent->i_ino) % groups;
	for (step = 1; step < groups; step <<= 1) {
		group += step;
		if (group >= groups)
			group -= groups;
		desc = ext2_get_group_desc(sb, group, NULL);
		if (desc &&
		    le16_to_cpu(desc->bg_free_inodes_count) != 0 &&
		    le16_to_cpu(desc->bg_free_blocks_count) != 0)
			return group;
	}

	group = parent_group;
	for (i = 0; i < groups; ++i) {
		if (++group >= groups)
			group = 0;
		desc = ext2_get_group_desc(sb, group, NULL);
		if (desc && le16_to_cpu(desc->bg_free_inodes_count) != 0)
			return group;
	}

	return -1;
}

static int ext2_claim_inode_bit(
	struct super_block *sb, int preferred_group,
	struct buffer_head **bitmap_out,
	struct ext2_group_desc **desc_out,
	struct buffer_head **desc_bh_out,
	unsigned int *group_out,
	unsigned int *bit_out)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	const unsigned int inodes_per_group = EXT2_INODES_PER_GROUP(sb);
	int attempt;

	if (!bitmap_out || !desc_out || !desc_bh_out ||
	    !group_out || !bit_out || preferred_group < 0)
		return -EINVAL;

	for (attempt = 0; attempt < sbi->s_groups_count; ++attempt) {
		unsigned int group =
			(preferred_group + attempt) % sbi->s_groups_count;
		struct ext2_group_desc *desc;
		struct buffer_head *desc_bh;
		struct buffer_head *bitmap_bh;
		unsigned int bit = 0;

		desc = ext2_get_group_desc(sb, group, &desc_bh);
		if (!desc ||
		    le16_to_cpu(desc->bg_free_inodes_count) == 0)
			continue;

		bitmap_bh = ext2_read_inode_bitmap(sb, group);
		if (!bitmap_bh)
			return -EIO;

		while (bit < inodes_per_group) {
			bit = ext2_find_next_zero_bit(
				(unsigned long *)bitmap_bh->b_data,
				inodes_per_group, bit);
			if (bit >= inodes_per_group)
				break;

			if (!ext2_set_bit_atomic(
				    sb_bgl_lock(sbi, group),
				    bit, bitmap_bh->b_data)) {
				*bitmap_out = bitmap_bh;
				*desc_out = desc;
				*desc_bh_out = desc_bh;
				*group_out = group;
				*bit_out = bit;
				return 0;
			}
			++bit;
		}

		brelse(bitmap_bh);
	}

	return -ENOSPC;
}

static void ext2_unclaim_inode_bit(
	struct super_block *sb, unsigned int group, unsigned int bit)
{
	struct buffer_head *bitmap_bh =
		ext2_read_inode_bitmap(sb, group);

	if (!bitmap_bh) {
		ext2_error(sb, __func__,
			   "cannot roll back inode bit %u in group %u",
			   bit, group);
		return;
	}

	if (!ext2_clear_bit_atomic(
		    sb_bgl_lock(sbi, group), bit, bitmap_bh->b_data))
		ext2_error(sb, __func__,
			   "inode bit %u in group %u was already clear",
			   bit, group);

	mark_buffer_dirty(bitmap_bh);
	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);
	brelse(bitmap_bh);
}

static void ext2_account_inode_allocation(
	struct super_block *sb, unsigned int group,
	struct ext2_group_desc *desc,
	struct buffer_head *desc_bh,
	bool directory)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	percpu_counter_dec(&sbi->s_freeinodes_counter);
	if (directory)
		percpu_counter_inc(&sbi->s_dirs_counter);

	spin_lock(sb_bgl_lock(sbi, group));
	le16_add_cpu(&desc->bg_free_inodes_count, -1);
	if (directory) {
		if (sbi->s_debts[group] < 255)
			sbi->s_debts[group]++;
		le16_add_cpu(&desc->bg_used_dirs_count, 1);
	} else if (sbi->s_debts[group] != 0) {
		sbi->s_debts[group]--;
	}
	spin_unlock(sb_bgl_lock(sbi, group));

	mark_buffer_dirty(desc_bh);
}

static void ext2_initialise_new_inode(
	struct inode *inode, struct inode *parent,
	umode_t mode, unsigned int group)
{
	struct ext2_inode_info *info = EXT2_I(inode);
	struct ext2_sb_info *sbi = EXT2_SB(inode->i_sb);

	if (test_opt(inode->i_sb, GRPID)) {
		inode->i_mode = mode;
		inode->i_uid = current_fsuid();
		inode->i_gid = parent->i_gid;
	} else {
		inode_init_owner(&nop_mnt_idmap, inode, parent, mode);
	}

	inode->i_blocks = 0;
	simple_inode_init_ts(inode);
	memset(info->i_data, 0, sizeof(info->i_data));
	info->i_flags = ext2_mask_flags(
		mode, EXT2_I(parent)->i_flags & EXT2_FL_INHERITED);
	info->i_faddr = 0;
	info->i_frag_no = 0;
	info->i_frag_size = 0;
	info->i_file_acl = 0;
	info->i_dir_acl = 0;
	info->i_dtime = 0;
	info->i_block_alloc_info = NULL;
	info->i_block_group = group;
	info->i_dir_start_lookup = 0;
	info->i_state = EXT2_STATE_NEW;
	ext2_set_inode_flags(inode);

	spin_lock(&sbi->s_next_gen_lock);
	inode->i_generation = sbi->s_next_generation++;
	spin_unlock(&sbi->s_next_gen_lock);
}

struct inode *ext2_new_inode(
	struct inode *dir, umode_t mode, const struct qstr *qstr)
{
	struct super_block *sb = dir->i_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = sbi->s_es;
	struct buffer_head *bitmap_bh = NULL;
	struct buffer_head *desc_bh = NULL;
	struct ext2_group_desc *desc = NULL;
	struct inode *inode;
	unsigned int group = 0;
	unsigned int bit = 0;
	ino_t ino;
	int preferred;
	int result;

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);

	if (S_ISDIR(mode)) {
		preferred = test_opt(sb, OLDALLOC)
			? ext2_choose_old_directory_group(sb)
			: ext2_choose_directory_group(sb, dir);
	} else {
		preferred = ext2_choose_nondirectory_group(sb, dir);
	}

	if (preferred < 0) {
		result = -ENOSPC;
		goto fail_bad_inode;
	}

	result = ext2_claim_inode_bit(
		sb, preferred, &bitmap_bh, &desc, &desc_bh,
		&group, &bit);
	if (result != 0)
		goto fail_bad_inode;

	mark_buffer_dirty(bitmap_bh);
	if (sb->s_flags & SB_SYNCHRONOUS)
		sync_dirty_buffer(bitmap_bh);
	brelse(bitmap_bh);
	bitmap_bh = NULL;

	ino = (ino_t)group * EXT2_INODES_PER_GROUP(sb) + bit + 1U;
	if (ino < EXT2_FIRST_INO(sb) ||
	    ino > le32_to_cpu(es->s_inodes_count)) {
		ext2_error(sb, __func__,
			   "allocator produced invalid inode %lu in group %u",
			   (unsigned long)ino, group);
		result = -EUCLEAN;
		goto fail_release_claim;
	}

	ext2_account_inode_allocation(
		sb, group, desc, desc_bh, S_ISDIR(mode));

	inode->i_ino = ino;
	ext2_initialise_new_inode(inode, dir, mode, group);

	if (insert_inode_locked(inode) < 0) {
		ext2_error(sb, __func__,
			   "inode %lu is already instantiated",
			   (unsigned long)ino);
		result = -EIO;
		goto fail_after_accounting;
	}

	result = dquot_initialize(inode);
	if (result != 0)
		goto fail_drop;
	result = dquot_alloc_inode(inode);
	if (result != 0)
		goto fail_drop;
	result = ext2_init_acl(inode, dir);
	if (result != 0)
		goto fail_quota;
	result = ext2_init_security(inode, dir, qstr);
	if (result != 0)
		goto fail_quota;

	mark_inode_dirty(inode);
	ext2_preread_inode(inode);
	return inode;

fail_quota:
	dquot_free_inode(inode);
fail_drop:
	dquot_drop(inode);
	inode->i_flags |= S_NOQUOTA;
	clear_nlink(inode);
	discard_new_inode(inode);
	return ERR_PTR(result);

fail_after_accounting:
	ext2_unclaim_inode_bit(sb, group, bit);
	ext2_account_inode_release(sb, group, S_ISDIR(mode));
	goto fail_bad_inode;

fail_release_claim:
	if (bitmap_bh)
		brelse(bitmap_bh);
	ext2_unclaim_inode_bit(sb, group, bit);
fail_bad_inode:
	make_bad_inode(inode);
	iput(inode);
	return ERR_PTR(result);
}

unsigned long ext2_count_free_inodes(struct super_block *sb)
{
	unsigned long total = 0;
	int group;

	for (group = 0; group < EXT2_SB(sb)->s_groups_count; ++group) {
		struct ext2_group_desc *desc =
			ext2_get_group_desc(sb, group, NULL);

		if (desc)
			total += le16_to_cpu(desc->bg_free_inodes_count);
	}
	return total;
}

unsigned long ext2_count_dirs(struct super_block *sb)
{
	unsigned long total = 0;
	int group;

	for (group = 0; group < EXT2_SB(sb)->s_groups_count; ++group) {
		struct ext2_group_desc *desc =
			ext2_get_group_desc(sb, group, NULL);

		if (desc)
			total += le16_to_cpu(desc->bg_used_dirs_count);
	}
	return total;
}
