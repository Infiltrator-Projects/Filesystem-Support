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

#include "ext3.h"

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
	struct super_block *sb, const struct inode *parent)
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
	struct super_block *sb, const struct inode *parent)
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
	struct ext3_sb_info *sbi = EXT3_SB(sb);
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
			sb_bgl_lock(sbi, group), bit, bitmap->b_data);
	else
		changed = ext3_clear_bit_atomic(
			sb_bgl_lock(sbi, group), bit, bitmap->b_data);

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
