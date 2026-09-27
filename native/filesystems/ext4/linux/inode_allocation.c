/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Infiltrator Filesystem Support — EXT4 inode allocation.
 *
 * This implementation owns inode-bitmap access, allocation policy, inode
 * accounting and lazy inode-table initialisation for the canonical EXT4
 * driver.  It deliberately uses a simple deterministic group scan rather
 * than importing another filesystem driver's allocation policy.
 */

// SPDX-License-Identifier: GPL-2.0

#include <linux/bitops.h>
#include <linux/buffer_head.h>
#include <linux/cred.h>
#include <linux/fs.h>
#include <linux/quotaops.h>
#include <linux/random.h>
#include <linux/time.h>

#include "ext4.h"
#include "ext4_jbd2.h"
#include "xattr.h"

static bool ifs_ext4_inode_number_valid(struct super_block *sb,
					unsigned long ino)
{
	const unsigned long maximum =
		le32_to_cpu(EXT4_SB(sb)->s_es->s_inodes_count);

	return ino >= EXT4_FIRST_INO(sb) && ino <= maximum;
}

void ext4_mark_bitmap_end(int start_bit, int end_bit, char *bitmap)
{
	if (!bitmap || start_bit >= end_bit)
		return;
	bitmap_set((unsigned long *)bitmap, start_bit, end_bit - start_bit);
}

void ext4_end_bitmap_read(struct buffer_head *bh, int uptodate)
{
	if (uptodate) {
		set_buffer_uptodate(bh);
		set_bitmap_uptodate(bh);
	}
	unlock_buffer(bh);
	put_bh(bh);
}

static int ifs_ext4_verify_inode_bitmap(struct super_block *sb,
					ext4_group_t group,
					struct ext4_group_desc *desc,
					struct buffer_head *bh)
{
	struct ext4_group_info *info;

	if (EXT4_SB(sb)->s_mount_state & EXT4_FC_REPLAY)
		return 0;
	if (buffer_verified(bh))
		return 0;

	info = ext4_get_group_info(sb, group);
	if (!info || EXT4_MB_GRP_IBITMAP_CORRUPT(info))
		return -EFSCORRUPTED;

	ext4_lock_group(sb, group);
	if (!buffer_verified(bh)) {
		if (!ext4_inode_bitmap_csum_verify(sb, desc, bh) ||
		    ext4_simulate_fail(sb, EXT4_SIM_IBITMAP_CRC)) {
			ext4_unlock_group(sb, group);
			ext4_mark_group_bitmap_corrupted(
				sb, group, EXT4_GROUP_INFO_IBITMAP_CORRUPT);
			ext4_error(
				sb,
				"inode bitmap checksum failed in group %u",
				group);
			return -EFSBADCRC;
		}
		set_buffer_verified(bh);
	}
	ext4_unlock_group(sb, group);
	return 0;
}

static struct buffer_head *ifs_ext4_read_inode_bitmap(struct super_block *sb,
						      ext4_group_t group)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_group_desc *desc;
	struct buffer_head *bh;
	ext4_fsblk_t block;
	int error;

	desc = ext4_get_group_desc(sb, group, NULL);
	if (!desc)
		return ERR_PTR(-EFSCORRUPTED);

	block = ext4_inode_bitmap(sb, desc);
	if (block <= le32_to_cpu(sbi->s_es->s_first_data_block) ||
	    block >= ext4_blocks_count(sbi->s_es)) {
		ext4_mark_group_bitmap_corrupted(
			sb, group, EXT4_GROUP_INFO_IBITMAP_CORRUPT);
		return ERR_PTR(-EFSCORRUPTED);
	}

	bh = sb_getblk(sb, block);
	if (!bh)
		return ERR_PTR(-ENOMEM);

	if (bitmap_uptodate(bh))
		goto verify;

	if (ext4_has_group_desc_csum(sb) &&
	    (desc->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT))) {
		lock_buffer(bh);
		if (!bitmap_uptodate(bh)) {
			memset(
				bh->b_data, 0,
				(EXT4_INODES_PER_GROUP(sb) + 7U) / 8U);
			ext4_mark_bitmap_end(
				EXT4_INODES_PER_GROUP(sb),
				sb->s_blocksize * 8U,
				bh->b_data);
			set_buffer_uptodate(bh);
			set_bitmap_uptodate(bh);
			set_buffer_verified(bh);
		}
		unlock_buffer(bh);
		return bh;
	}

	error = bh_read(bh, 0);
	if (error < 0) {
		brelse(bh);
		ext4_mark_group_bitmap_corrupted(
			sb, group, EXT4_GROUP_INFO_IBITMAP_CORRUPT);
		return ERR_PTR(error);
	}
	set_bitmap_uptodate(bh);

verify:
	error = ifs_ext4_verify_inode_bitmap(sb, group, desc, bh);
	if (error) {
		brelse(bh);
		return ERR_PTR(error);
	}
	return bh;
}

static int ifs_ext4_prepare_group_descriptor(handle_t *handle,
					      struct super_block *sb,
					      ext4_group_t group,
					      struct ext4_group_desc **desc_out,
					      struct buffer_head **bh_out)
{
	struct ext4_group_desc *desc;
	struct buffer_head *bh;
	int error;

	desc = ext4_get_group_desc(sb, group, &bh);
	if (!desc || !bh)
		return -EIO;

	error = ext4_journal_get_write_access(
		handle, sb, bh, EXT4_JTR_NONE);
	if (error)
		return error;

	*desc_out = desc;
	*bh_out = bh;
	return 0;
}

static void ifs_ext4_update_flex_inode_counters(struct ext4_sb_info *sbi,
						 ext4_group_t group,
						 int inode_delta,
						 int dir_delta)
{
	struct flex_groups *flex;

	if (!sbi->s_log_groups_per_flex)
		return;

	flex = sbi_array_rcu_deref(
		sbi, s_flex_groups, ext4_flex_group(sbi, group));
	if (inode_delta > 0)
		atomic_add(inode_delta, &flex->free_inodes);
	else if (inode_delta < 0)
		atomic_sub(-inode_delta, &flex->free_inodes);

	if (dir_delta > 0)
		atomic_add(dir_delta, &flex->used_dirs);
	else if (dir_delta < 0)
		atomic_sub(-dir_delta, &flex->used_dirs);
}

static int ifs_ext4_account_inode(handle_t *handle,
				   struct super_block *sb,
				   ext4_group_t group,
				   struct ext4_group_desc *desc,
				   struct buffer_head *desc_bh,
				   struct buffer_head *bitmap_bh,
				   bool directory,
				   int delta)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned int free_count;
	unsigned int dir_count;
	int error;

	ext4_lock_group(sb, group);
	free_count = ext4_free_inodes_count(sb, desc);
	dir_count = ext4_used_dirs_count(sb, desc);

	if (delta < 0) {
		if (free_count == 0U) {
			ext4_unlock_group(sb, group);
			return -ENOSPC;
		}
		ext4_free_inodes_set(sb, desc, free_count - 1U);
		if (directory)
			ext4_used_dirs_set(sb, desc, dir_count + 1U);
	} else {
		ext4_free_inodes_set(sb, desc, free_count + 1U);
		if (directory) {
			if (dir_count == 0U) {
				ext4_unlock_group(sb, group);
				return -EFSCORRUPTED;
			}
			ext4_used_dirs_set(sb, desc, dir_count - 1U);
		}
	}

	if (ext4_has_group_desc_csum(sb)) {
		ext4_inode_bitmap_csum_set(sb, desc, bitmap_bh);
		ext4_group_desc_csum_set(sb, group, desc);
	}
	ext4_unlock_group(sb, group);

	error = ext4_handle_dirty_metadata(handle, NULL, desc_bh);
	if (error)
		return error;

	if (delta < 0) {
		if (percpu_counter_initialized(&sbi->s_freeinodes_counter))
			percpu_counter_dec(&sbi->s_freeinodes_counter);
		if (directory &&
		    percpu_counter_initialized(&sbi->s_dirs_counter))
			percpu_counter_inc(&sbi->s_dirs_counter);
		ifs_ext4_update_flex_inode_counters(
			sbi, group, -1, directory ? 1 : 0);
	} else {
		if (percpu_counter_initialized(&sbi->s_freeinodes_counter))
			percpu_counter_inc(&sbi->s_freeinodes_counter);
		if (directory &&
		    percpu_counter_initialized(&sbi->s_dirs_counter))
			percpu_counter_dec(&sbi->s_dirs_counter);
		ifs_ext4_update_flex_inode_counters(
			sbi, group, 1, directory ? -1 : 0);
	}

	return 0;
}

static int ifs_ext4_publish_bitmap(handle_t *handle,
				    struct super_block *sb,
				    struct buffer_head *bitmap_bh)
{
	return ext4_handle_dirty_metadata(handle, NULL, bitmap_bh);
}

static ext4_group_t ifs_ext4_preferred_group(struct inode *dir,
					      umode_t mode)
{
	struct super_block *sb = dir->i_sb;
	const ext4_group_t groups = ext4_get_groups_count(sb);
	ext4_group_t parent_group = EXT4_I(dir)->i_block_group;
	ext4_group_t best = groups;
	unsigned int best_free = 0U;
	ext4_group_t step;

	if (groups == 0)
		return 0;
	if (parent_group >= groups)
		parent_group = 0;

	if (!S_ISDIR(mode))
		return parent_group;

	for (step = 0; step < groups; ++step) {
		const ext4_group_t group =
			(parent_group + step) % groups;
		struct ext4_group_desc *desc =
			ext4_get_group_desc(sb, group, NULL);
		unsigned int free_inodes;

		if (!desc)
			continue;
		free_inodes = ext4_free_inodes_count(sb, desc);
		if (free_inodes > best_free) {
			best = group;
			best_free = free_inodes;
		}
	}

	return best == groups ? parent_group : best;
}

static int ifs_ext4_find_free_inode(struct super_block *sb,
				     ext4_group_t start_group,
				     unsigned long goal,
				     ext4_group_t *group_out,
				     unsigned long *bit_out,
				     struct buffer_head **bitmap_out)
{
	const ext4_group_t groups = ext4_get_groups_count(sb);
	ext4_group_t pass;

	for (pass = 0; pass < groups; ++pass) {
		const ext4_group_t group =
			(start_group + pass) % groups;
		struct ext4_group_desc *desc;
		struct ext4_group_info *info;
		struct buffer_head *bitmap;
		unsigned long bit = 0U;

		desc = ext4_get_group_desc(sb, group, NULL);
		if (!desc || ext4_free_inodes_count(sb, desc) == 0U)
			continue;

		if (!(EXT4_SB(sb)->s_mount_state & EXT4_FC_REPLAY)) {
			info = ext4_get_group_info(sb, group);
			if (!info || EXT4_MB_GRP_IBITMAP_CORRUPT(info))
				continue;
		}

		bitmap = ifs_ext4_read_inode_bitmap(sb, group);
		if (IS_ERR(bitmap))
			continue;

		if (pass == 0 && goal != 0U)
			bit = goal;
		if (group == 0 && bit + 1U < EXT4_FIRST_INO(sb))
			bit = EXT4_FIRST_INO(sb) - 1U;

		for (;;) {
			bit = ext4_find_next_zero_bit(
				(unsigned long *)bitmap->b_data,
				EXT4_INODES_PER_GROUP(sb),
				bit);
			if (bit >= EXT4_INODES_PER_GROUP(sb))
				break;

			ext4_lock_group(sb, group);
			if (!ext4_test_and_set_bit(bit, bitmap->b_data)) {
				ext4_unlock_group(sb, group);
				*group_out = group;
				*bit_out = bit;
				*bitmap_out = bitmap;
				return 0;
			}
			ext4_unlock_group(sb, group);
			++bit;
		}

		brelse(bitmap);
	}

	return -ENOSPC;
}

static void ifs_ext4_unclaim_inode(struct super_block *sb,
				    ext4_group_t group,
				    unsigned long bit,
				    struct buffer_head *bitmap)
{
	ext4_lock_group(sb, group);
	ext4_clear_bit(bit, bitmap->b_data);
	ext4_unlock_group(sb, group);
}

static int ifs_ext4_activate_group_if_needed(handle_t *handle,
					      struct super_block *sb,
					      ext4_group_t group,
					      struct ext4_group_desc *desc)
{
	struct buffer_head *block_bitmap;
	int error = 0;

	if (!ext4_has_group_desc_csum(sb) ||
	    !(desc->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT)))
		return 0;

	block_bitmap = ext4_read_block_bitmap(sb, group);
	if (IS_ERR(block_bitmap))
		return PTR_ERR(block_bitmap);

	error = ext4_journal_get_write_access(
		handle, sb, block_bitmap, EXT4_JTR_NONE);
	if (error)
		goto out;

	ext4_lock_group(sb, group);
	if (desc->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT)) {
		desc->bg_flags &= cpu_to_le16(~EXT4_BG_BLOCK_UNINIT);
		ext4_free_group_clusters_set(
			sb, desc, ext4_free_clusters_after_init(sb, group, desc));
		ext4_block_bitmap_csum_set(sb, desc, block_bitmap);
		ext4_group_desc_csum_set(sb, group, desc);
	}
	ext4_unlock_group(sb, group);

	error = ext4_handle_dirty_metadata(handle, NULL, block_bitmap);
out:
	brelse(block_bitmap);
	return error;
}

static void ifs_ext4_note_inode_table_use(struct super_block *sb,
					   ext4_group_t group,
					   struct ext4_group_desc *desc,
					   unsigned long bit)
{
	struct ext4_group_info *info = NULL;
	unsigned int initialized;

	if (!ext4_has_group_desc_csum(sb))
		return;

	if (!(EXT4_SB(sb)->s_mount_state & EXT4_FC_REPLAY)) {
		info = ext4_get_group_info(sb, group);
		if (!info)
			return;
		down_read(&info->alloc_sem);
	}

	ext4_lock_group(sb, group);
	initialized =
		EXT4_INODES_PER_GROUP(sb) - ext4_itable_unused_count(sb, desc);
	if (desc->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT)) {
		desc->bg_flags &= cpu_to_le16(~EXT4_BG_INODE_UNINIT);
		initialized = 0U;
	}
	if (bit + 1U > initialized)
		ext4_itable_unused_set(
			sb, desc, EXT4_INODES_PER_GROUP(sb) - bit - 1U);
	ext4_unlock_group(sb, group);

	if (info)
		up_read(&info->alloc_sem);
}

int ext4_mark_inode_used(struct super_block *sb, int ino)
{
	struct ext4_group_desc *desc;
	struct buffer_head *desc_bh;
	struct buffer_head *bitmap;
	ext4_group_t group;
	unsigned long bit;
	int error;

	if (!ifs_ext4_inode_number_valid(sb, ino))
		return -EFSCORRUPTED;

	group = (ino - 1U) / EXT4_INODES_PER_GROUP(sb);
	bit = (ino - 1U) % EXT4_INODES_PER_GROUP(sb);
	bitmap = ifs_ext4_read_inode_bitmap(sb, group);
	if (IS_ERR(bitmap))
		return PTR_ERR(bitmap);

	desc = ext4_get_group_desc(sb, group, &desc_bh);
	if (!desc || !desc_bh) {
		brelse(bitmap);
		return -EIO;
	}

	ext4_lock_group(sb, group);
	if (ext4_test_bit(bit, bitmap->b_data)) {
		ext4_unlock_group(sb, group);
		brelse(bitmap);
		return 0;
	}
	ext4_set_bit(bit, bitmap->b_data);
	ext4_unlock_group(sb, group);

	ifs_ext4_note_inode_table_use(sb, group, desc, bit);

	ext4_lock_group(sb, group);
	if (ext4_free_inodes_count(sb, desc) == 0U) {
		ext4_clear_bit(bit, bitmap->b_data);
		ext4_unlock_group(sb, group);
		brelse(bitmap);
		return -EFSCORRUPTED;
	}
	ext4_free_inodes_set(
		sb, desc, ext4_free_inodes_count(sb, desc) - 1U);
	if (ext4_has_group_desc_csum(sb)) {
		ext4_inode_bitmap_csum_set(sb, desc, bitmap);
		ext4_group_desc_csum_set(sb, group, desc);
	}
	ext4_unlock_group(sb, group);

	mark_buffer_dirty(bitmap);
	mark_buffer_dirty(desc_bh);
	error = sync_dirty_buffer(bitmap);
	if (!error)
		error = sync_dirty_buffer(desc_bh);
	brelse(bitmap);
	return error;
}

void ext4_free_inode(handle_t *handle, struct inode *inode)
{
	struct super_block *sb;
	struct ext4_group_desc *desc;
	struct buffer_head *desc_bh;
	struct buffer_head *bitmap;
	ext4_group_t group;
	unsigned long bit;
	bool directory;
	int error;

	if (!inode || !inode->i_sb)
		return;
	sb = inode->i_sb;

	if (inode->i_nlink != 0 ||
	    !ifs_ext4_inode_number_valid(sb, inode->i_ino)) {
		ext4_error(sb, "refusing invalid inode free %lu", inode->i_ino);
		return;
	}

	directory = S_ISDIR(inode->i_mode);
	dquot_initialize(inode);
	dquot_free_inode(inode);
	ext4_clear_inode(inode);

	group = (inode->i_ino - 1U) / EXT4_INODES_PER_GROUP(sb);
	bit = (inode->i_ino - 1U) % EXT4_INODES_PER_GROUP(sb);

	bitmap = ifs_ext4_read_inode_bitmap(sb, group);
	if (IS_ERR(bitmap)) {
		ext4_std_error(sb, PTR_ERR(bitmap));
		return;
	}

	error = ext4_journal_get_write_access(
		handle, sb, bitmap, EXT4_JTR_NONE);
	if (error)
		goto out;

	error = ifs_ext4_prepare_group_descriptor(
		handle, sb, group, &desc, &desc_bh);
	if (error)
		goto out;

	ext4_lock_group(sb, group);
	if (!ext4_test_and_clear_bit(bit, bitmap->b_data)) {
		ext4_unlock_group(sb, group);
		ext4_mark_group_bitmap_corrupted(
			sb, group, EXT4_GROUP_INFO_IBITMAP_CORRUPT);
		error = -EFSCORRUPTED;
		goto out;
	}
	ext4_unlock_group(sb, group);

	error = ifs_ext4_account_inode(
		handle, sb, group, desc, desc_bh,
		bitmap, directory, 1);
	if (error)
		goto out;

	error = ifs_ext4_publish_bitmap(handle, sb, bitmap);
out:
	brelse(bitmap);
	ext4_std_error(sb, error);
}

static int ifs_ext4_new_inode_xattr_credits(struct inode *dir,
					     umode_t mode,
					     bool encrypt)
{
	struct super_block *sb = dir->i_sb;
	int credits = 0;

#ifdef CONFIG_EXT4_FS_POSIX_ACL
	{
		struct posix_acl *acl = get_inode_acl(dir, ACL_TYPE_DEFAULT);
		if (IS_ERR(acl))
			return PTR_ERR(acl);
		if (acl) {
			const int bytes =
				acl->a_count * sizeof(ext4_acl_entry);
			credits +=
				(S_ISDIR(mode) ? 2 : 1) *
				__ext4_xattr_set_credits(
					sb, NULL, NULL, bytes, true);
			posix_acl_release(acl);
		}
	}
#endif

#ifdef CONFIG_SECURITY
	credits += __ext4_xattr_set_credits(
		sb, NULL, NULL, 1024, true);
#ifdef CONFIG_INTEGRITY
	credits += __ext4_xattr_set_credits(
		sb, NULL, NULL, 1024, true);
#endif
#endif

	if (encrypt)
		credits += __ext4_xattr_set_credits(
			sb, NULL, NULL, FSCRYPT_SET_CONTEXT_MAX_SIZE, true);
	return credits;
}

static void ifs_ext4_initialize_new_inode(struct inode *inode,
					   struct inode *dir,
					   umode_t mode,
					   __u32 i_flags,
					   ext4_group_t group)
{
	struct ext4_inode_info *ei = EXT4_I(inode);

	inode->i_blocks = 0;
	simple_inode_init_ts(inode);
	ei->i_crtime = inode_get_mtime(inode);
	memset(ei->i_data, 0, sizeof(ei->i_data));
	ei->i_dir_start_lookup = 0;
	ei->i_disksize = 0;
	ei->i_flags =
		ext4_mask_flags(mode, EXT4_I(dir)->i_flags & EXT4_FL_INHERITED);
	ei->i_flags |= i_flags;
	ei->i_file_acl = 0;
	ei->i_dtime = 0;
	ei->i_block_group = group;
	ei->i_last_alloc_group = ~0U;
	ei->i_extra_isize = EXT4_SB(inode->i_sb)->s_want_extra_isize;
	ei->i_inline_off = 0;

	ext4_set_inode_flags(inode, true);
	ext4_set_inode_state(inode, EXT4_STATE_NEW);
	if (ext4_has_feature_inline_data(inode->i_sb) &&
	    (!(ei->i_flags & EXT4_DAX_FL) || S_ISDIR(mode)))
		ext4_set_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);
}

static void ifs_ext4_initialize_inode_checksum_seed(struct inode *inode)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_inode_info *ei = EXT4_I(inode);
	__le32 inum;
	__le32 generation;
	__u32 seed;

	if (!ext4_has_metadata_csum(inode->i_sb))
		return;

	inum = cpu_to_le32(inode->i_ino);
	generation = cpu_to_le32(inode->i_generation);
	seed = ext4_chksum(
		sbi, sbi->s_csum_seed, (__u8 *)&inum, sizeof(inum));
	ei->i_csum_seed = ext4_chksum(
		sbi, seed, (__u8 *)&generation, sizeof(generation));
}

struct inode *__ext4_new_inode(struct mnt_idmap *idmap,
			       handle_t *handle,
			       struct inode *dir,
			       umode_t mode,
			       const struct qstr *qstr,
			       __u32 goal,
			       uid_t *owner,
			       __u32 i_flags,
			       int handle_type,
			       unsigned int line_no,
			       int nblocks)
{
	struct super_block *sb;
	struct ext4_sb_info *sbi;
	struct inode *inode;
	struct ext4_inode_info *ei;
	struct ext4_group_desc *desc = NULL;
	struct buffer_head *desc_bh = NULL;
	struct buffer_head *bitmap = NULL;
	ext4_group_t group;
	ext4_group_t start_group;
	unsigned long bit = 0U;
	unsigned long goal_bit = 0U;
	bool encrypt = false;
	bool directory;
	bool started_handle = false;
	bool bit_claimed = false;
	int error;

	if (!dir || !dir->i_nlink)
		return ERR_PTR(-EPERM);
	sb = dir->i_sb;
	sbi = EXT4_SB(sb);
	if (ext4_forced_shutdown(sb))
		return ERR_PTR(-EIO);

	inode = new_inode(sb);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	ei = EXT4_I(inode);
	directory = S_ISDIR(mode);

	if (owner) {
		inode->i_mode = mode;
		i_uid_write(inode, owner[0]);
		i_gid_write(inode, owner[1]);
	} else if (test_opt(sb, GRPID)) {
		inode->i_mode = mode;
		inode_fsuid_set(inode, idmap);
		inode->i_gid = dir->i_gid;
	} else {
		inode_init_owner(idmap, inode, dir, mode);
	}

	if (ext4_has_feature_project(sb) &&
	    ext4_test_inode_flag(dir, EXT4_INODE_PROJINHERIT))
		ei->i_projid = EXT4_I(dir)->i_projid;
	else
		ei->i_projid = make_kprojid(&init_user_ns, EXT4_DEF_PROJID);

	if (!(i_flags & EXT4_EA_INODE_FL)) {
		error = fscrypt_prepare_new_inode(dir, inode, &encrypt);
		if (error)
			goto fail_inode;
	}

	error = dquot_initialize(inode);
	if (error)
		goto fail_inode;

	if (!handle && sbi->s_journal && !(i_flags & EXT4_EA_INODE_FL)) {
		const int extra =
			ifs_ext4_new_inode_xattr_credits(dir, mode, encrypt);

		if (extra < 0) {
			error = extra;
			goto fail_inode;
		}
		nblocks += extra;
		if (nblocks <= 0)
			nblocks = EXT4_DATA_TRANS_BLOCKS(sb) + 8;
		handle = __ext4_journal_start_sb(
			NULL, sb, line_no, handle_type, nblocks, 0,
			ext4_trans_default_revoke_credits(sb));
		if (IS_ERR(handle)) {
			error = PTR_ERR(handle);
			handle = NULL;
			goto fail_inode;
		}
		started_handle = true;
	}

	if (goal && goal <= le32_to_cpu(sbi->s_es->s_inodes_count)) {
		start_group = (goal - 1U) / EXT4_INODES_PER_GROUP(sb);
		goal_bit = (goal - 1U) % EXT4_INODES_PER_GROUP(sb);
	} else {
		start_group = ifs_ext4_preferred_group(dir, mode);
	}

	error = ifs_ext4_find_free_inode(
		sb, start_group, goal_bit, &group, &bit, &bitmap);
	if (error)
		goto fail_handle;
	bit_claimed = true;

	error = ext4_journal_get_write_access(
		handle, sb, bitmap, EXT4_JTR_NONE);
	if (error)
		goto fail_claim;

	error = ifs_ext4_prepare_group_descriptor(
		handle, sb, group, &desc, &desc_bh);
	if (error)
		goto fail_claim;

	error = ifs_ext4_activate_group_if_needed(
		handle, sb, group, desc);
	if (error)
		goto fail_claim;

	ifs_ext4_note_inode_table_use(sb, group, desc, bit);

	error = ifs_ext4_account_inode(
		handle, sb, group, desc, desc_bh, bitmap,
		directory, -1);
	if (error)
		goto fail_claim;

	error = ifs_ext4_publish_bitmap(handle, sb, bitmap);
	if (error)
		goto fail_accounting;

	inode->i_ino =
		bit + 1U + (unsigned long)group * EXT4_INODES_PER_GROUP(sb);
	EXT4_I(dir)->i_last_alloc_group = group;
	ifs_ext4_initialize_new_inode(
		inode, dir, mode, i_flags, group);

	if (insert_inode_locked(inode) < 0) {
		error = -EIO;
		goto fail_accounting;
	}

	inode->i_generation = get_random_u32();
	ifs_ext4_initialize_inode_checksum_seed(inode);

	error = dquot_alloc_inode(inode);
	if (error)
		goto fail_unhash;

	if (encrypt) {
		error = fscrypt_set_context(inode, handle);
		if (error)
			goto fail_quota;
	}

	if (!(i_flags & EXT4_EA_INODE_FL)) {
		error = ext4_init_acl(handle, inode, dir);
		if (error)
			goto fail_quota;
		error = ext4_init_security(handle, inode, dir, qstr);
		if (error)
			goto fail_quota;
	}

	if (ext4_has_feature_extents(sb) &&
	    (S_ISDIR(mode) || S_ISREG(mode) || S_ISLNK(mode))) {
		ext4_set_inode_flag(inode, EXT4_INODE_EXTENTS);
		ext4_ext_tree_init(handle, inode);
	}

	ext4_update_inode_fsync_trans(handle, inode, 1);
	error = ext4_mark_inode_dirty(handle, inode);
	if (error)
		goto fail_quota;

	if (IS_DIRSYNC(inode))
		ext4_handle_sync(handle);

	brelse(bitmap);
	if (started_handle) {
		error = ext4_journal_stop(handle);
		if (error) {
			clear_nlink(inode);
			unlock_new_inode(inode);
			iput(inode);
			return ERR_PTR(error);
		}
	}
	return inode;

fail_quota:
	dquot_free_inode(inode);
fail_unhash:
	clear_nlink(inode);
	unlock_new_inode(inode);
fail_accounting:
	if (desc && desc_bh && bitmap)
		ifs_ext4_account_inode(
			handle, sb, group, desc, desc_bh, bitmap,
			directory, 1);
fail_claim:
	if (bit_claimed && bitmap)
		ifs_ext4_unclaim_inode(sb, group, bit, bitmap);
	brelse(bitmap);
fail_handle:
	if (started_handle && handle)
		ext4_journal_stop(handle);
fail_inode:
	dquot_drop(inode);
	inode->i_flags |= S_NOQUOTA;
	iput(inode);
	return ERR_PTR(error);
}

struct inode *ext4_orphan_get(struct super_block *sb, unsigned long ino)
{
	struct buffer_head *bitmap;
	struct inode *inode;
	ext4_group_t group;
	unsigned long bit;

	if (!ifs_ext4_inode_number_valid(sb, ino))
		return ERR_PTR(-EFSCORRUPTED);

	group = (ino - 1U) / EXT4_INODES_PER_GROUP(sb);
	bit = (ino - 1U) % EXT4_INODES_PER_GROUP(sb);
	bitmap = ifs_ext4_read_inode_bitmap(sb, group);
	if (IS_ERR(bitmap))
		return ERR_CAST(bitmap);

	if (!ext4_test_bit(bit, bitmap->b_data)) {
		brelse(bitmap);
		return ERR_PTR(-EFSCORRUPTED);
	}
	brelse(bitmap);

	inode = ext4_iget(sb, ino, EXT4_IGET_NORMAL);
	if (IS_ERR(inode))
		return inode;

	if (is_bad_inode(inode) ||
	    (inode->i_nlink && !ext4_can_truncate(inode)) ||
	    NEXT_ORPHAN(inode) >
		le32_to_cpu(EXT4_SB(sb)->s_es->s_inodes_count)) {
		if (inode->i_nlink == 0)
			inode->i_blocks = 0;
		iput(inode);
		return ERR_PTR(-EFSCORRUPTED);
	}

	return inode;
}

unsigned long ext4_count_free_inodes(struct super_block *sb)
{
	const ext4_group_t groups = ext4_get_groups_count(sb);
	unsigned long total = 0U;
	ext4_group_t group;

	for (group = 0; group < groups; ++group) {
		struct ext4_group_desc *desc =
			ext4_get_group_desc(sb, group, NULL);

		if (desc)
			total += ext4_free_inodes_count(sb, desc);
		cond_resched();
	}
	return total;
}

unsigned long ext4_count_dirs(struct super_block *sb)
{
	const ext4_group_t groups = ext4_get_groups_count(sb);
	unsigned long total = 0U;
	ext4_group_t group;

	for (group = 0; group < groups; ++group) {
		struct ext4_group_desc *desc =
			ext4_get_group_desc(sb, group, NULL);

		if (desc)
			total += ext4_used_dirs_count(sb, desc);
	}
	return total;
}

int ext4_init_inode_table(struct super_block *sb,
			  ext4_group_t group,
			  int barrier)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_group_info *info;
	struct ext4_group_desc *desc;
	struct buffer_head *desc_bh;
	handle_t *handle;
	ext4_fsblk_t first;
	unsigned int initialized_inodes = 0U;
	unsigned int used_blocks = 0U;
	unsigned int zero_blocks;
	int error = 0;

	info = ext4_get_group_info(sb, group);
	desc = ext4_get_group_desc(sb, group, &desc_bh);
	if (!info || !desc || !desc_bh)
		return -EIO;

	if (desc->bg_flags & cpu_to_le16(EXT4_BG_INODE_ZEROED))
		return 0;

	handle = ext4_journal_start_sb(sb, EXT4_HT_MISC, 1);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	down_write(&info->alloc_sem);

	if (!(desc->bg_flags & cpu_to_le16(EXT4_BG_INODE_UNINIT))) {
		initialized_inodes =
			EXT4_INODES_PER_GROUP(sb) -
			ext4_itable_unused_count(sb, desc);
		used_blocks = DIV_ROUND_UP(
			initialized_inodes, sbi->s_inodes_per_block);
		if (used_blocks > sbi->s_itb_per_group) {
			error = -EFSCORRUPTED;
			goto out_unlock;
		}
	}

	error = ext4_journal_get_write_access(
		handle, sb, desc_bh, EXT4_JTR_NONE);
	if (error)
		goto out_unlock;

	first = ext4_inode_table(sb, desc) + used_blocks;
	zero_blocks = sbi->s_itb_per_group - used_blocks;
	if (zero_blocks) {
		error = sb_issue_zeroout(
			sb, first, zero_blocks, GFP_NOFS);
		if (error)
			goto out_unlock;
		if (barrier)
			error = blkdev_issue_flush(sb->s_bdev);
		if (error)
			goto out_unlock;
	}

	ext4_lock_group(sb, group);
	desc->bg_flags |= cpu_to_le16(EXT4_BG_INODE_ZEROED);
	ext4_group_desc_csum_set(sb, group, desc);
	ext4_unlock_group(sb, group);

	error = ext4_handle_dirty_metadata(handle, NULL, desc_bh);

out_unlock:
	up_write(&info->alloc_sem);
	{
		const int stop_error = ext4_journal_stop(handle);
		if (!error)
			error = stop_error;
	}
	return error;
}
