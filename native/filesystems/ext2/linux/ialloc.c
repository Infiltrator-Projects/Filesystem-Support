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

#include "ext2.h"

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
	struct ext2_sb_info *sbi = EXT2_SB(sb);
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
	struct ext2_sb_info *sbi = EXT2_SB(sb);
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
