// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright IBM Corporation, 2007
 * Author Aneesh Kumar K.V <aneesh.kumar@linux.vnet.ibm.com>
 *
 */

/*
 * EXT4 — Mapping-format migration
 *
 * Purpose:
 *   Converts eligible inodes from legacy indirect mapping to extent mapping.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Migration changes persistent mapping representation and therefore requires rollback-safe ordering so either old or new mapping remains valid after failure.
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

#include <linux/slab.h>
#include "ext4_jbd2.h"
#include "ext4_extents.h"


/**
 * struct migrate_struct - Private EXT4 state/data structure used by mapping-format migration.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct migrate_struct {
	ext4_lblk_t first_block, last_block, curr_block;
	ext4_fsblk_t first_pblock, last_pblock;
};

/**
 * finish_range - Implements the finish range operation within the mapping-format migration subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int finish_range(handle_t *handle, struct inode *inode,
				struct migrate_struct *lb)

{
	int retval = 0, needed;
	struct ext4_extent newext;
	struct ext4_ext_path *path;
	if (lb->first_pblock == 0)
		return 0;


	newext.ee_block = cpu_to_le32(lb->first_block);
	newext.ee_len   = cpu_to_le16(lb->last_block - lb->first_block + 1);
	ext4_ext_store_pblock(&newext, lb->first_pblock);

	down_write(&EXT4_I(inode)->i_data_sem);
	path = ext4_find_extent(inode, lb->first_block, NULL, 0);
	if (IS_ERR(path)) {
		retval = PTR_ERR(path);
		goto err_out;
	}


	needed = ext4_ext_calc_credits_for_single_extent(inode,
		    lb->last_block - lb->first_block + 1, path);

	retval = ext4_datasem_ensure_credits(handle, inode, needed, needed, 0);
	if (retval < 0)
		goto err_out;
	path = ext4_ext_insert_extent(handle, inode, path, &newext, 0);
	if (IS_ERR(path))
		retval = PTR_ERR(path);
err_out:
	up_write((&EXT4_I(inode)->i_data_sem));
	ext4_free_ext_path(path);
	lb->first_pblock = 0;
	return retval;
}

/**
 * update_extent_range - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int update_extent_range(handle_t *handle, struct inode *inode,
			       ext4_fsblk_t pblock, struct migrate_struct *lb)
{
	int retval;


	if (lb->first_pblock &&
		(lb->last_pblock+1 == pblock) &&
		(lb->last_block+1 == lb->curr_block)) {
		lb->last_pblock = pblock;
		lb->last_block = lb->curr_block;
		lb->curr_block++;
		return 0;
	}


	retval = finish_range(handle, inode, lb);
	lb->first_pblock = lb->last_pblock = pblock;
	lb->first_block = lb->last_block = lb->curr_block;
	lb->curr_block++;
	return retval;
}

/**
 * update_ind_extent_range - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int update_ind_extent_range(handle_t *handle, struct inode *inode,
				   ext4_fsblk_t pblock,
				   struct migrate_struct *lb)
{
	struct buffer_head *bh;
	__le32 *i_data;
	int i, retval = 0;
	unsigned long max_entries = inode->i_sb->s_blocksize >> 2;

	bh = ext4_sb_bread(inode->i_sb, pblock, 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	i_data = (__le32 *)bh->b_data;
	for (i = 0; i < max_entries; i++) {
		if (i_data[i]) {
			retval = update_extent_range(handle, inode,
						le32_to_cpu(i_data[i]), lb);
			if (retval)
				break;
		} else {
			lb->curr_block++;
		}
	}
	put_bh(bh);
	return retval;

}

/**
 * update_dind_extent_range - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int update_dind_extent_range(handle_t *handle, struct inode *inode,
				    ext4_fsblk_t pblock,
				    struct migrate_struct *lb)
{
	struct buffer_head *bh;
	__le32 *i_data;
	int i, retval = 0;
	unsigned long max_entries = inode->i_sb->s_blocksize >> 2;

	bh = ext4_sb_bread(inode->i_sb, pblock, 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	i_data = (__le32 *)bh->b_data;
	for (i = 0; i < max_entries; i++) {
		if (i_data[i]) {
			retval = update_ind_extent_range(handle, inode,
						le32_to_cpu(i_data[i]), lb);
			if (retval)
				break;
		} else {

			lb->curr_block += max_entries;
		}
	}
	put_bh(bh);
	return retval;

}

/**
 * update_tind_extent_range - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int update_tind_extent_range(handle_t *handle, struct inode *inode,
				    ext4_fsblk_t pblock,
				    struct migrate_struct *lb)
{
	struct buffer_head *bh;
	__le32 *i_data;
	int i, retval = 0;
	unsigned long max_entries = inode->i_sb->s_blocksize >> 2;

	bh = ext4_sb_bread(inode->i_sb, pblock, 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	i_data = (__le32 *)bh->b_data;
	for (i = 0; i < max_entries; i++) {
		if (i_data[i]) {
			retval = update_dind_extent_range(handle, inode,
						le32_to_cpu(i_data[i]), lb);
			if (retval)
				break;
		} else {

			lb->curr_block += max_entries * max_entries;
		}
	}
	put_bh(bh);
	return retval;

}

/**
 * free_dind_blocks - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int free_dind_blocks(handle_t *handle,
				struct inode *inode, __le32 i_data)
{
	int i;
	__le32 *tmp_idata;
	struct buffer_head *bh;
	struct super_block *sb = inode->i_sb;
	unsigned long max_entries = inode->i_sb->s_blocksize >> 2;
	int err;

	bh = ext4_sb_bread(sb, le32_to_cpu(i_data), 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	tmp_idata = (__le32 *)bh->b_data;
	for (i = 0; i < max_entries; i++) {
		if (tmp_idata[i]) {
			err = ext4_journal_ensure_credits(handle,
				EXT4_RESERVE_TRANS_BLOCKS,
				ext4_free_metadata_revoke_credits(sb, 1));
			if (err < 0) {
				put_bh(bh);
				return err;
			}
			ext4_free_blocks(handle, inode, NULL,
					 le32_to_cpu(tmp_idata[i]), 1,
					 EXT4_FREE_BLOCKS_METADATA |
					 EXT4_FREE_BLOCKS_FORGET);
		}
	}
	put_bh(bh);
	err = ext4_journal_ensure_credits(handle, EXT4_RESERVE_TRANS_BLOCKS,
				ext4_free_metadata_revoke_credits(sb, 1));
	if (err < 0)
		return err;
	ext4_free_blocks(handle, inode, NULL, le32_to_cpu(i_data), 1,
			 EXT4_FREE_BLOCKS_METADATA |
			 EXT4_FREE_BLOCKS_FORGET);
	return 0;
}

/**
 * free_tind_blocks - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int free_tind_blocks(handle_t *handle,
				struct inode *inode, __le32 i_data)
{
	int i, retval = 0;
	__le32 *tmp_idata;
	struct buffer_head *bh;
	unsigned long max_entries = inode->i_sb->s_blocksize >> 2;

	bh = ext4_sb_bread(inode->i_sb, le32_to_cpu(i_data), 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	tmp_idata = (__le32 *)bh->b_data;
	for (i = 0; i < max_entries; i++) {
		if (tmp_idata[i]) {
			retval = free_dind_blocks(handle,
					inode, tmp_idata[i]);
			if (retval) {
				put_bh(bh);
				return retval;
			}
		}
	}
	put_bh(bh);
	retval = ext4_journal_ensure_credits(handle, EXT4_RESERVE_TRANS_BLOCKS,
			ext4_free_metadata_revoke_credits(inode->i_sb, 1));
	if (retval < 0)
		return retval;
	ext4_free_blocks(handle, inode, NULL, le32_to_cpu(i_data), 1,
			 EXT4_FREE_BLOCKS_METADATA |
			 EXT4_FREE_BLOCKS_FORGET);
	return 0;
}

/**
 * free_ind_block - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int free_ind_block(handle_t *handle, struct inode *inode, __le32 *i_data)
{
	int retval;


	if (i_data[0]) {
		retval = ext4_journal_ensure_credits(handle,
			EXT4_RESERVE_TRANS_BLOCKS,
			ext4_free_metadata_revoke_credits(inode->i_sb, 1));
		if (retval < 0)
			return retval;
		ext4_free_blocks(handle, inode, NULL,
				le32_to_cpu(i_data[0]), 1,
				 EXT4_FREE_BLOCKS_METADATA |
				 EXT4_FREE_BLOCKS_FORGET);
	}


	if (i_data[1]) {
		retval = free_dind_blocks(handle, inode, i_data[1]);
		if (retval)
			return retval;
	}


	if (i_data[2]) {
		retval = free_tind_blocks(handle, inode, i_data[2]);
		if (retval)
			return retval;
	}
	return 0;
}

/**
 * ext4_ext_swap_inode_data - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_ext_swap_inode_data(handle_t *handle, struct inode *inode,
						struct inode *tmp_inode)
{
	int retval, retval2 = 0;
	__le32	i_data[3];
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_inode_info *tmp_ei = EXT4_I(tmp_inode);


	retval = ext4_journal_ensure_credits(handle, 1, 0);
	if (retval < 0)
		goto err_out;

	i_data[0] = ei->i_data[EXT4_IND_BLOCK];
	i_data[1] = ei->i_data[EXT4_DIND_BLOCK];
	i_data[2] = ei->i_data[EXT4_TIND_BLOCK];

	down_write(&EXT4_I(inode)->i_data_sem);


	if (!ext4_test_inode_state(inode, EXT4_STATE_EXT_MIGRATE)) {
		retval = -EAGAIN;
		up_write(&EXT4_I(inode)->i_data_sem);
		goto err_out;
	} else
		ext4_clear_inode_state(inode, EXT4_STATE_EXT_MIGRATE);


	ext4_set_inode_flag(inode, EXT4_INODE_EXTENTS);
	memcpy(ei->i_data, tmp_ei->i_data, sizeof(ei->i_data));


	spin_lock(&inode->i_lock);
	inode->i_blocks += tmp_inode->i_blocks;
	spin_unlock(&inode->i_lock);
	up_write(&EXT4_I(inode)->i_data_sem);


	retval = free_ind_block(handle, inode, i_data);
	retval2 = ext4_mark_inode_dirty(handle, inode);
	if (unlikely(retval2 && !retval))
		retval = retval2;

err_out:
	return retval;
}

/**
 * free_ext_idx - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int free_ext_idx(handle_t *handle, struct inode *inode,
					struct ext4_extent_idx *ix)
{
	int i, retval = 0;
	ext4_fsblk_t block;
	struct buffer_head *bh;
	struct ext4_extent_header *eh;

	block = ext4_idx_pblock(ix);
	bh = ext4_sb_bread(inode->i_sb, block, 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	eh = (struct ext4_extent_header *)bh->b_data;
	if (eh->eh_depth != 0) {
		ix = EXT_FIRST_INDEX(eh);
		for (i = 0; i < le16_to_cpu(eh->eh_entries); i++, ix++) {
			retval = free_ext_idx(handle, inode, ix);
			if (retval) {
				put_bh(bh);
				return retval;
			}
		}
	}
	put_bh(bh);
	retval = ext4_journal_ensure_credits(handle, EXT4_RESERVE_TRANS_BLOCKS,
			ext4_free_metadata_revoke_credits(inode->i_sb, 1));
	if (retval < 0)
		return retval;
	ext4_free_blocks(handle, inode, NULL, block, 1,
			 EXT4_FREE_BLOCKS_METADATA | EXT4_FREE_BLOCKS_FORGET);
	return 0;
}


/**
 * free_ext_block - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int free_ext_block(handle_t *handle, struct inode *inode)
{
	int i, retval = 0;
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_extent_header *eh = (struct ext4_extent_header *)ei->i_data;
	struct ext4_extent_idx *ix;
	if (eh->eh_depth == 0)


		return 0;
	ix = EXT_FIRST_INDEX(eh);
	for (i = 0; i < le16_to_cpu(eh->eh_entries); i++, ix++) {
		retval = free_ext_idx(handle, inode, ix);
		if (retval)
			return retval;
	}
	return retval;
}

/**
 * ext4_ext_migrate - Implements the ext migrate operation within the mapping-format migration subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int ext4_ext_migrate(struct inode *inode)
{
	handle_t *handle;
	int retval = 0, i;
	__le32 *i_data;
	struct ext4_inode_info *ei;
	struct inode *tmp_inode = NULL;
	struct migrate_struct lb;
	unsigned long max_entries;
	__u32 goal, tmp_csum_seed;
	uid_t owner[2];
	int alloc_ctx;


	if (!ext4_has_feature_extents(inode->i_sb) ||
	    ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS) ||
	    ext4_has_inline_data(inode))
		return -EINVAL;

	if (S_ISLNK(inode->i_mode) && inode->i_blocks == 0)


		return retval;

	alloc_ctx = ext4_writepages_down_write(inode->i_sb);


	handle = ext4_journal_start(inode, EXT4_HT_MIGRATE,
		3 + EXT4_MAXQUOTAS_TRANS_BLOCKS(inode->i_sb));

	if (IS_ERR(handle)) {
		retval = PTR_ERR(handle);
		goto out_unlock;
	}
	goal = (((inode->i_ino - 1) / EXT4_INODES_PER_GROUP(inode->i_sb)) *
		EXT4_INODES_PER_GROUP(inode->i_sb)) + 1;
	owner[0] = i_uid_read(inode);
	owner[1] = i_gid_read(inode);
	tmp_inode = ext4_new_inode(handle, d_inode(inode->i_sb->s_root),
				   S_IFREG, NULL, goal, owner, 0);
	if (IS_ERR(tmp_inode)) {
		retval = PTR_ERR(tmp_inode);
		ext4_journal_stop(handle);
		goto out_unlock;
	}


	ei = EXT4_I(inode);
	tmp_csum_seed = EXT4_I(tmp_inode)->i_csum_seed;
	EXT4_I(tmp_inode)->i_csum_seed = ei->i_csum_seed;
	i_size_write(tmp_inode, i_size_read(inode));


	clear_nlink(tmp_inode);

	ext4_ext_tree_init(handle, tmp_inode);
	ext4_journal_stop(handle);


	down_read(&EXT4_I(inode)->i_data_sem);
	ext4_set_inode_state(inode, EXT4_STATE_EXT_MIGRATE);
	up_read((&EXT4_I(inode)->i_data_sem));

	handle = ext4_journal_start(inode, EXT4_HT_MIGRATE, 1);
	if (IS_ERR(handle)) {
		retval = PTR_ERR(handle);
		goto out_tmp_inode;
	}

	i_data = ei->i_data;
	memset(&lb, 0, sizeof(lb));


	max_entries = inode->i_sb->s_blocksize >> 2;
	for (i = 0; i < EXT4_NDIR_BLOCKS; i++) {
		if (i_data[i]) {
			retval = update_extent_range(handle, tmp_inode,
						le32_to_cpu(i_data[i]), &lb);
			if (retval)
				goto err_out;
		} else
			lb.curr_block++;
	}
	if (i_data[EXT4_IND_BLOCK]) {
		retval = update_ind_extent_range(handle, tmp_inode,
				le32_to_cpu(i_data[EXT4_IND_BLOCK]), &lb);
		if (retval)
			goto err_out;
	} else
		lb.curr_block += max_entries;
	if (i_data[EXT4_DIND_BLOCK]) {
		retval = update_dind_extent_range(handle, tmp_inode,
				le32_to_cpu(i_data[EXT4_DIND_BLOCK]), &lb);
		if (retval)
			goto err_out;
	} else
		lb.curr_block += max_entries * max_entries;
	if (i_data[EXT4_TIND_BLOCK]) {
		retval = update_tind_extent_range(handle, tmp_inode,
				le32_to_cpu(i_data[EXT4_TIND_BLOCK]), &lb);
		if (retval)
			goto err_out;
	}


	retval = finish_range(handle, tmp_inode, &lb);
err_out:
	if (retval)


		free_ext_block(handle, tmp_inode);
	else {
		retval = ext4_ext_swap_inode_data(handle, inode, tmp_inode);
		if (retval)


			free_ext_block(handle, tmp_inode);
	}


	retval = ext4_journal_ensure_credits(handle, 1, 0);
	if (retval < 0)
		goto out_stop;


	i_size_write(tmp_inode, 0);


	tmp_inode->i_blocks = 0;
	EXT4_I(tmp_inode)->i_csum_seed = tmp_csum_seed;


	ext4_ext_tree_init(handle, tmp_inode);
out_stop:
	ext4_journal_stop(handle);
out_tmp_inode:
	unlock_new_inode(tmp_inode);
	iput(tmp_inode);
out_unlock:
	ext4_writepages_up_write(inode->i_sb, alloc_ctx);
	return retval;
}


/**
 * ext4_ind_migrate - Implements the ind migrate operation within the mapping-format migration subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int ext4_ind_migrate(struct inode *inode)
{
	struct ext4_extent_header	*eh;
	struct ext4_sb_info		*sbi = EXT4_SB(inode->i_sb);
	struct ext4_super_block		*es = sbi->s_es;
	struct ext4_inode_info		*ei = EXT4_I(inode);
	struct ext4_extent		*ex;
	unsigned int			i, len;
	ext4_lblk_t			start, end;
	ext4_fsblk_t			blk;
	handle_t			*handle;
	int				ret, ret2 = 0;
	int				alloc_ctx;

	if (!ext4_has_feature_extents(inode->i_sb) ||
	    (!ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)))
		return -EINVAL;

	if (ext4_has_feature_bigalloc(inode->i_sb))
		return -EOPNOTSUPP;


	if (test_opt(inode->i_sb, DELALLOC))
		ext4_alloc_da_blocks(inode);

	alloc_ctx = ext4_writepages_down_write(inode->i_sb);

	handle = ext4_journal_start(inode, EXT4_HT_MIGRATE, 1);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		goto out_unlock;
	}

	down_write(&EXT4_I(inode)->i_data_sem);
	ret = ext4_ext_check_inode(inode);
	if (ret)
		goto errout;

	eh = ext_inode_hdr(inode);
	ex  = EXT_FIRST_EXTENT(eh);
	if (ext4_blocks_count(es) > EXT4_MAX_BLOCK_FILE_PHYS ||
	    eh->eh_depth != 0 || le16_to_cpu(eh->eh_entries) > 1) {
		ret = -EOPNOTSUPP;
		goto errout;
	}
	if (eh->eh_entries == 0)
		blk = len = start = end = 0;
	else {
		len = le16_to_cpu(ex->ee_len);
		blk = ext4_ext_pblock(ex);
		start = le32_to_cpu(ex->ee_block);
		end = start + len - 1;
		if (end >= EXT4_NDIR_BLOCKS) {
			ret = -EOPNOTSUPP;
			goto errout;
		}
	}

	ext4_clear_inode_flag(inode, EXT4_INODE_EXTENTS);
	memset(ei->i_data, 0, sizeof(ei->i_data));
	for (i = start; i <= end; i++)
		ei->i_data[i] = cpu_to_le32(blk++);
	ret2 = ext4_mark_inode_dirty(handle, inode);
	if (unlikely(ret2 && !ret))
		ret = ret2;
errout:
	up_write(&EXT4_I(inode)->i_data_sem);
	ext4_journal_stop(handle);
out_unlock:
	ext4_writepages_up_write(inode->i_sb, alloc_ctx);
	return ret;
}
