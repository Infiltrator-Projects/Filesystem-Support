// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2003-2006, Cluster File Systems, Inc, info@clusterfs.com
 * Written by Alex Tomas <alex@clusterfs.com>
 *
 * Architecture independence:
 *   Copyright (c) 2005, Bull S.A.
 *   Written by Pierre Peiffer <pierre.peiffer@bull.net>
 */

/*
 * EXT4 — Extent-tree engine
 *
 * Purpose:
 *   Implements lookup, insertion, splitting, removal, unwritten-extent conversion and tree maintenance for extent-mapped files.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Tree edits must preserve ordering, non-overlap, depth and checksum/transaction invariants across every split and rollback path.
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

#include <linux/fs.h>
#include <linux/time.h>
#include <linux/jbd2.h>
#include <linux/highuid.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fiemap.h>
#include <linux/iomap.h>
#include <linux/sched/mm.h>
#include "ext4_jbd2.h"
#include "ext4_extents.h"
#include "xattr.h"

#include <trace/events/ext4.h>


#define EXT4_EXT_MAY_ZEROOUT	0x1

#define EXT4_EXT_MARK_UNWRIT1	0x2
#define EXT4_EXT_MARK_UNWRIT2	0x4


#define EXT4_EXT_DATA_ENTIRE_VALID1	0x8
#define EXT4_EXT_DATA_PARTIAL_VALID1	0x10
#define EXT4_EXT_DATA_VALID1		(EXT4_EXT_DATA_ENTIRE_VALID1 | \
					 EXT4_EXT_DATA_PARTIAL_VALID1)

#define EXT4_EXT_DATA_VALID2	0x20

/**
 * ext4_extent_block_csum - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static __le32 ext4_extent_block_csum(struct inode *inode,
				     struct ext4_extent_header *eh)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	__u32 csum;

	csum = ext4_chksum(sbi, ei->i_csum_seed, (__u8 *)eh,
			   EXT4_EXTENT_TAIL_OFFSET(eh));
	return cpu_to_le32(csum);
}

/**
 * ext4_extent_block_csum_verify - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_extent_block_csum_verify(struct inode *inode,
					 struct ext4_extent_header *eh)
{
	struct ext4_extent_tail *et;

	if (!ext4_has_metadata_csum(inode->i_sb))
		return 1;

	et = find_ext4_extent_tail(eh);
	if (et->et_checksum != ext4_extent_block_csum(inode, eh))
		return 0;
	return 1;
}

/**
 * ext4_extent_block_csum_set - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_extent_block_csum_set(struct inode *inode,
				       struct ext4_extent_header *eh)
{
	struct ext4_extent_tail *et;

	if (!ext4_has_metadata_csum(inode->i_sb))
		return;

	et = find_ext4_extent_tail(eh);
	et->et_checksum = ext4_extent_block_csum(inode, eh);
}

static struct ext4_ext_path *ext4_split_extent_at(handle_t *handle,
						  struct inode *inode,
						  struct ext4_ext_path *path,
						  ext4_lblk_t split,
						  int split_flag, int flags);

/**
 * ext4_ext_trunc_restart_fn - Implements the ext trunc restart fn operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_ext_trunc_restart_fn(struct inode *inode, int *dropped)
{


	BUG_ON(EXT4_JOURNAL(inode) == NULL);
	ext4_discard_preallocations(inode);
	up_write(&EXT4_I(inode)->i_data_sem);
	*dropped = 1;
	return 0;
}

/**
 * ext4_ext_path_brelse - Implements the ext path brelse operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_ext_path_brelse(struct ext4_ext_path *path)
{
	brelse(path->p_bh);
	path->p_bh = NULL;
}

/**
 * ext4_ext_drop_refs - Implements the ext drop refs operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_ext_drop_refs(struct ext4_ext_path *path)
{
	int depth, i;

	if (IS_ERR_OR_NULL(path))
		return;
	depth = path->p_depth;
	for (i = 0; i <= depth; i++, path++)
		ext4_ext_path_brelse(path);
}

/**
 * ext4_free_ext_path - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_free_ext_path(struct ext4_ext_path *path)
{
	if (IS_ERR_OR_NULL(path))
		return;
	ext4_ext_drop_refs(path);
	kfree(path);
}


/**
 * ext4_datasem_ensure_credits - Implements the datasem ensure credits operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_datasem_ensure_credits(handle_t *handle, struct inode *inode,
				int check_cred, int restart_cred,
				int revoke_cred)
{
	int ret;
	int dropped = 0;

	ret = ext4_journal_ensure_credits_fn(handle, check_cred, restart_cred,
		revoke_cred, ext4_ext_trunc_restart_fn(inode, &dropped));
	if (dropped)
		down_write(&EXT4_I(inode)->i_data_sem);
	return ret;
}


/**
 * ext4_ext_get_access - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_ext_get_access(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path)
{
	int err = 0;

	if (path->p_bh) {

		BUFFER_TRACE(path->p_bh, "get_write_access");
		err = ext4_journal_get_write_access(handle, inode->i_sb,
						    path->p_bh, EXT4_JTR_NONE);


		if (!err)
			clear_buffer_verified(path->p_bh);
	}


	return err;
}


/**
 * __ext4_ext_dirty - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __ext4_ext_dirty(const char *where, unsigned int line,
			    handle_t *handle, struct inode *inode,
			    struct ext4_ext_path *path)
{
	int err;

	WARN_ON(!rwsem_is_locked(&EXT4_I(inode)->i_data_sem));
	if (path->p_bh) {
		ext4_extent_block_csum_set(inode, ext_block_hdr(path->p_bh));

		err = __ext4_handle_dirty_metadata(where, line, handle,
						   inode, path->p_bh);

		if (!err)
			set_buffer_verified(path->p_bh);
	} else {

		err = ext4_mark_inode_dirty(handle, inode);
	}
	return err;
}

#define ext4_ext_dirty(handle, inode, path) \
		__ext4_ext_dirty(__func__, __LINE__, (handle), (inode), (path))

/**
 * ext4_ext_find_goal - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ext4_fsblk_t ext4_ext_find_goal(struct inode *inode,
			      struct ext4_ext_path *path,
			      ext4_lblk_t block)
{
	if (path) {
		int depth = path->p_depth;
		struct ext4_extent *ex;


		ex = path[depth].p_ext;
		if (ex) {
			ext4_fsblk_t ext_pblk = ext4_ext_pblock(ex);
			ext4_lblk_t ext_block = le32_to_cpu(ex->ee_block);

			if (block > ext_block)
				return ext_pblk + (block - ext_block);
			else
				return ext_pblk - (ext_block - block);
		}


		if (path[depth].p_bh)
			return path[depth].p_bh->b_blocknr;
	}


	return ext4_inode_to_goal_block(inode);
}


/**
 * ext4_ext_new_meta_block - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ext4_fsblk_t
ext4_ext_new_meta_block(handle_t *handle, struct inode *inode,
			struct ext4_ext_path *path,
			struct ext4_extent *ex, int *err, unsigned int flags)
{
	ext4_fsblk_t goal, newblock;

	goal = ext4_ext_find_goal(inode, path, le32_to_cpu(ex->ee_block));
	newblock = ext4_new_meta_blocks(handle, inode, goal, flags,
					NULL, err);
	return newblock;
}

/**
 * ext4_ext_space_block - Implements the ext space block operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_ext_space_block(struct inode *inode, int check)
{
	int size;

	size = (inode->i_sb->s_blocksize - sizeof(struct ext4_extent_header))
			/ sizeof(struct ext4_extent);
#ifdef AGGRESSIVE_TEST
	if (!check && size > 6)
		size = 6;
#endif
	return size;
}

/**
 * ext4_ext_space_block_idx - Implements the ext space block idx operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_ext_space_block_idx(struct inode *inode, int check)
{
	int size;

	size = (inode->i_sb->s_blocksize - sizeof(struct ext4_extent_header))
			/ sizeof(struct ext4_extent_idx);
#ifdef AGGRESSIVE_TEST
	if (!check && size > 5)
		size = 5;
#endif
	return size;
}

/**
 * ext4_ext_space_root - Implements the ext space root operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_ext_space_root(struct inode *inode, int check)
{
	int size;

	size = sizeof(EXT4_I(inode)->i_data);
	size -= sizeof(struct ext4_extent_header);
	size /= sizeof(struct ext4_extent);
#ifdef AGGRESSIVE_TEST
	if (!check && size > 3)
		size = 3;
#endif
	return size;
}

/**
 * ext4_ext_space_root_idx - Implements the ext space root idx operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_ext_space_root_idx(struct inode *inode, int check)
{
	int size;

	size = sizeof(EXT4_I(inode)->i_data);
	size -= sizeof(struct ext4_extent_header);
	size /= sizeof(struct ext4_extent_idx);
#ifdef AGGRESSIVE_TEST
	if (!check && size > 4)
		size = 4;
#endif
	return size;
}

/**
 * ext4_force_split_extent_at - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline struct ext4_ext_path *
ext4_force_split_extent_at(handle_t *handle, struct inode *inode,
			   struct ext4_ext_path *path, ext4_lblk_t lblk,
			   int nofail)
{
	int unwritten = ext4_ext_is_unwritten(path[path->p_depth].p_ext);
	int flags = EXT4_EX_NOCACHE | EXT4_GET_BLOCKS_PRE_IO;

	if (nofail)
		flags |= EXT4_GET_BLOCKS_METADATA_NOFAIL | EXT4_EX_NOFAIL;

	return ext4_split_extent_at(handle, inode, path, lblk, unwritten ?
			EXT4_EXT_MARK_UNWRIT1|EXT4_EXT_MARK_UNWRIT2 : 0,
			flags);
}

/**
 * ext4_ext_max_entries - Implements the ext max entries operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_ext_max_entries(struct inode *inode, int depth)
{
	int max;

	if (depth == ext_depth(inode)) {
		if (depth == 0)
			max = ext4_ext_space_root(inode, 1);
		else
			max = ext4_ext_space_root_idx(inode, 1);
	} else {
		if (depth == 0)
			max = ext4_ext_space_block(inode, 1);
		else
			max = ext4_ext_space_block_idx(inode, 1);
	}

	return max;
}

/**
 * ext4_valid_extent - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_valid_extent(struct inode *inode, struct ext4_extent *ext)
{
	ext4_fsblk_t block = ext4_ext_pblock(ext);
	int len = ext4_ext_get_actual_len(ext);
	ext4_lblk_t lblock = le32_to_cpu(ext->ee_block);


	if (lblock + len <= lblock)
		return 0;
	return ext4_inode_block_valid(inode, block, len);
}

/**
 * ext4_valid_extent_idx - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_valid_extent_idx(struct inode *inode,
				struct ext4_extent_idx *ext_idx)
{
	ext4_fsblk_t block = ext4_idx_pblock(ext_idx);

	return ext4_inode_block_valid(inode, block, 1);
}

/**
 * ext4_valid_extent_entries - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_valid_extent_entries(struct inode *inode,
				     struct ext4_extent_header *eh,
				     ext4_lblk_t lblk, ext4_fsblk_t *pblk,
				     int depth)
{
	unsigned short entries;
	ext4_lblk_t lblock = 0;
	ext4_lblk_t cur = 0;

	if (eh->eh_entries == 0)
		return 1;

	entries = le16_to_cpu(eh->eh_entries);

	if (depth == 0) {

		struct ext4_extent *ext = EXT_FIRST_EXTENT(eh);


		if (depth != ext_depth(inode) &&
		    lblk != le32_to_cpu(ext->ee_block))
			return 0;
		while (entries) {
			if (!ext4_valid_extent(inode, ext))
				return 0;


			lblock = le32_to_cpu(ext->ee_block);
			if (lblock < cur) {
				*pblk = ext4_ext_pblock(ext);
				return 0;
			}
			cur = lblock + ext4_ext_get_actual_len(ext);
			ext++;
			entries--;
		}
	} else {
		struct ext4_extent_idx *ext_idx = EXT_FIRST_INDEX(eh);


		if (depth != ext_depth(inode) &&
		    lblk != le32_to_cpu(ext_idx->ei_block))
			return 0;
		while (entries) {
			if (!ext4_valid_extent_idx(inode, ext_idx))
				return 0;


			lblock = le32_to_cpu(ext_idx->ei_block);
			if (lblock < cur) {
				*pblk = ext4_idx_pblock(ext_idx);
				return 0;
			}
			ext_idx++;
			entries--;
			cur = lblock + 1;
		}
	}
	return 1;
}

/**
 * __ext4_ext_check - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __ext4_ext_check(const char *function, unsigned int line,
			    struct inode *inode, struct ext4_extent_header *eh,
			    int depth, ext4_fsblk_t pblk, ext4_lblk_t lblk)
{
	const char *error_msg;
	int max = 0, err = -EFSCORRUPTED;

	if (unlikely(eh->eh_magic != EXT4_EXT_MAGIC)) {
		error_msg = "invalid magic";
		goto corrupted;
	}
	if (unlikely(le16_to_cpu(eh->eh_depth) != depth)) {
		error_msg = "unexpected eh_depth";
		goto corrupted;
	}
	if (unlikely(eh->eh_max == 0)) {
		error_msg = "invalid eh_max";
		goto corrupted;
	}
	max = ext4_ext_max_entries(inode, depth);
	if (unlikely(le16_to_cpu(eh->eh_max) > max)) {
		error_msg = "too large eh_max";
		goto corrupted;
	}
	if (unlikely(le16_to_cpu(eh->eh_entries) > le16_to_cpu(eh->eh_max))) {
		error_msg = "invalid eh_entries";
		goto corrupted;
	}
	if (unlikely((eh->eh_entries == 0) && (depth > 0))) {
		error_msg = "eh_entries is 0 but eh_depth is > 0";
		goto corrupted;
	}
	if (!ext4_valid_extent_entries(inode, eh, lblk, &pblk, depth)) {
		error_msg = "invalid extent entries";
		goto corrupted;
	}
	if (unlikely(depth > 32)) {
		error_msg = "too large eh_depth";
		goto corrupted;
	}

	if (ext_depth(inode) != depth &&
	    !ext4_extent_block_csum_verify(inode, eh)) {
		error_msg = "extent tree corrupted";
		err = -EFSBADCRC;
		goto corrupted;
	}
	return 0;

corrupted:
	ext4_error_inode_err(inode, function, line, 0, -err,
			     "pblk %llu bad header/extent: %s - magic %x, "
			     "entries %u, max %u(%u), depth %u(%u)",
			     (unsigned long long) pblk, error_msg,
			     le16_to_cpu(eh->eh_magic),
			     le16_to_cpu(eh->eh_entries),
			     le16_to_cpu(eh->eh_max),
			     max, le16_to_cpu(eh->eh_depth), depth);
	return err;
}

#define ext4_ext_check(inode, eh, depth, pblk)			\
	__ext4_ext_check(__func__, __LINE__, (inode), (eh), (depth), (pblk), 0)

/**
 * ext4_ext_check_inode - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_ext_check_inode(struct inode *inode)
{
	return ext4_ext_check(inode, ext_inode_hdr(inode), ext_depth(inode), 0);
}

/**
 * ext4_cache_extents - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_cache_extents(struct inode *inode,
			       struct ext4_extent_header *eh)
{
	struct ext4_extent *ex = EXT_FIRST_EXTENT(eh);
	ext4_lblk_t prev = 0;
	int i;

	for (i = le16_to_cpu(eh->eh_entries); i > 0; i--, ex++) {
		unsigned int status = EXTENT_STATUS_WRITTEN;
		ext4_lblk_t lblk = le32_to_cpu(ex->ee_block);
		int len = ext4_ext_get_actual_len(ex);

		if (prev && (prev != lblk))
			ext4_es_cache_extent(inode, prev, lblk - prev, ~0,
					     EXTENT_STATUS_HOLE);

		if (ext4_ext_is_unwritten(ex))
			status = EXTENT_STATUS_UNWRITTEN;
		ext4_es_cache_extent(inode, lblk, len,
				     ext4_ext_pblock(ex), status);
		prev = lblk + len;
	}
}

/**
 * __read_extent_tree_block - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct buffer_head *
__read_extent_tree_block(const char *function, unsigned int line,
			 struct inode *inode, struct ext4_extent_idx *idx,
			 int depth, int flags)
{
	struct buffer_head		*bh;
	int				err;
	gfp_t				gfp_flags = __GFP_MOVABLE | GFP_NOFS;
	ext4_fsblk_t			pblk;

	if (flags & EXT4_EX_NOFAIL)
		gfp_flags |= __GFP_NOFAIL;

	pblk = ext4_idx_pblock(idx);
	bh = sb_getblk_gfp(inode->i_sb, pblk, gfp_flags);
	if (unlikely(!bh))
		return ERR_PTR(-ENOMEM);

	if (!bh_uptodate_or_lock(bh)) {
		trace_ext4_ext_load_extent(inode, pblk, _RET_IP_);
		err = ext4_read_bh(bh, 0, NULL, false);
		if (err < 0)
			goto errout;
	}
	if (buffer_verified(bh) && !(flags & EXT4_EX_FORCE_CACHE))
		return bh;
	err = __ext4_ext_check(function, line, inode, ext_block_hdr(bh),
			       depth, pblk, le32_to_cpu(idx->ei_block));
	if (err)
		goto errout;
	set_buffer_verified(bh);


	if (!(flags & EXT4_EX_NOCACHE) && depth == 0) {
		struct ext4_extent_header *eh = ext_block_hdr(bh);
		ext4_cache_extents(inode, eh);
	}
	return bh;
errout:
	put_bh(bh);
	return ERR_PTR(err);

}

#define read_extent_tree_block(inode, idx, depth, flags)		\
	__read_extent_tree_block(__func__, __LINE__, (inode), (idx),	\
				 (depth), (flags))


/**
 * ext4_ext_precache - Implements the ext precache operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_ext_precache(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_ext_path *path = NULL;
	struct buffer_head *bh;
	int i = 0, depth, ret = 0;

	if (!ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		return 0;

	down_read(&ei->i_data_sem);
	depth = ext_depth(inode);


	if (!depth) {
		up_read(&ei->i_data_sem);
		return ret;
	}

	path = kcalloc(depth + 1, sizeof(struct ext4_ext_path),
		       GFP_NOFS);
	if (path == NULL) {
		up_read(&ei->i_data_sem);
		return -ENOMEM;
	}

	path[0].p_hdr = ext_inode_hdr(inode);
	ret = ext4_ext_check(inode, path[0].p_hdr, depth, 0);
	if (ret)
		goto out;
	path[0].p_idx = EXT_FIRST_INDEX(path[0].p_hdr);
	while (i >= 0) {


		if ((i == depth) ||
		    path[i].p_idx > EXT_LAST_INDEX(path[i].p_hdr)) {
			ext4_ext_path_brelse(path + i);
			i--;
			continue;
		}
		bh = read_extent_tree_block(inode, path[i].p_idx++,
					    depth - i - 1,
					    EXT4_EX_FORCE_CACHE);
		if (IS_ERR(bh)) {
			ret = PTR_ERR(bh);
			break;
		}
		i++;
		path[i].p_bh = bh;
		path[i].p_hdr = ext_block_hdr(bh);
		path[i].p_idx = EXT_FIRST_INDEX(path[i].p_hdr);
	}
	ext4_set_inode_state(inode, EXT4_STATE_EXT_PRECACHED);
out:
	up_read(&ei->i_data_sem);
	ext4_free_ext_path(path);
	return ret;
}

#ifdef EXT_DEBUG
/**
 * ext4_ext_show_path - Implements the ext show path operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_ext_show_path(struct inode *inode, struct ext4_ext_path *path)
{
	int k, l = path->p_depth;

	ext_debug(inode, "path:");
	for (k = 0; k <= l; k++, path++) {
		if (path->p_idx) {
			ext_debug(inode, "  %d->%llu",
				  le32_to_cpu(path->p_idx->ei_block),
				  ext4_idx_pblock(path->p_idx));
		} else if (path->p_ext) {
			ext_debug(inode, "  %d:[%d]%d:%llu ",
				  le32_to_cpu(path->p_ext->ee_block),
				  ext4_ext_is_unwritten(path->p_ext),
				  ext4_ext_get_actual_len(path->p_ext),
				  ext4_ext_pblock(path->p_ext));
		} else
			ext_debug(inode, "  []");
	}
	ext_debug(inode, "\n");
}

/**
 * ext4_ext_show_leaf - Implements the ext show leaf operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_ext_show_leaf(struct inode *inode, struct ext4_ext_path *path)
{
	int depth = ext_depth(inode);
	struct ext4_extent_header *eh;
	struct ext4_extent *ex;
	int i;

	if (IS_ERR_OR_NULL(path))
		return;

	eh = path[depth].p_hdr;
	ex = EXT_FIRST_EXTENT(eh);

	ext_debug(inode, "Displaying leaf extents\n");

	for (i = 0; i < le16_to_cpu(eh->eh_entries); i++, ex++) {
		ext_debug(inode, "%d:[%d]%d:%llu ", le32_to_cpu(ex->ee_block),
			  ext4_ext_is_unwritten(ex),
			  ext4_ext_get_actual_len(ex), ext4_ext_pblock(ex));
	}
	ext_debug(inode, "\n");
}

/**
 * ext4_ext_show_move - Implements the ext show move operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_ext_show_move(struct inode *inode, struct ext4_ext_path *path,
			ext4_fsblk_t newblock, int level)
{
	int depth = ext_depth(inode);
	struct ext4_extent *ex;

	if (depth != level) {
		struct ext4_extent_idx *idx;
		idx = path[level].p_idx;
		while (idx <= EXT_MAX_INDEX(path[level].p_hdr)) {
			ext_debug(inode, "%d: move %d:%llu in new index %llu\n",
				  level, le32_to_cpu(idx->ei_block),
				  ext4_idx_pblock(idx), newblock);
			idx++;
		}

		return;
	}

	ex = path[depth].p_ext;
	while (ex <= EXT_MAX_EXTENT(path[depth].p_hdr)) {
		ext_debug(inode, "move %d:%llu:[%d]%d in new leaf %llu\n",
				le32_to_cpu(ex->ee_block),
				ext4_ext_pblock(ex),
				ext4_ext_is_unwritten(ex),
				ext4_ext_get_actual_len(ex),
				newblock);
		ex++;
	}
}

#else
#define ext4_ext_show_path(inode, path)
#define ext4_ext_show_leaf(inode, path)
#define ext4_ext_show_move(inode, path, newblock, level)
#endif


/**
 * ext4_ext_binsearch_idx - Implements the ext binsearch idx operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void
ext4_ext_binsearch_idx(struct inode *inode,
			struct ext4_ext_path *path, ext4_lblk_t block)
{
	struct ext4_extent_header *eh = path->p_hdr;
	struct ext4_extent_idx *r, *l, *m;


	ext_debug(inode, "binsearch for %u(idx):  ", block);

	l = EXT_FIRST_INDEX(eh) + 1;
	r = EXT_LAST_INDEX(eh);
	while (l <= r) {
		m = l + (r - l) / 2;
		ext_debug(inode, "%p(%u):%p(%u):%p(%u) ", l,
			  le32_to_cpu(l->ei_block), m, le32_to_cpu(m->ei_block),
			  r, le32_to_cpu(r->ei_block));

		if (block < le32_to_cpu(m->ei_block))
			r = m - 1;
		else
			l = m + 1;
	}

	path->p_idx = l - 1;
	ext_debug(inode, "  -> %u->%lld ", le32_to_cpu(path->p_idx->ei_block),
		  ext4_idx_pblock(path->p_idx));

#ifdef CHECK_BINSEARCH
	{
		struct ext4_extent_idx *chix, *ix;
		int k;

		chix = ix = EXT_FIRST_INDEX(eh);
		for (k = 0; k < le16_to_cpu(eh->eh_entries); k++, ix++) {
			if (k != 0 && le32_to_cpu(ix->ei_block) <=
			    le32_to_cpu(ix[-1].ei_block)) {
				printk(KERN_DEBUG "k=%d, ix=0x%p, "
				       "first=0x%p\n", k,
				       ix, EXT_FIRST_INDEX(eh));
				printk(KERN_DEBUG "%u <= %u\n",
				       le32_to_cpu(ix->ei_block),
				       le32_to_cpu(ix[-1].ei_block));
			}
			BUG_ON(k && le32_to_cpu(ix->ei_block)
					   <= le32_to_cpu(ix[-1].ei_block));
			if (block < le32_to_cpu(ix->ei_block))
				break;
			chix = ix;
		}
		BUG_ON(chix != path->p_idx);
	}
#endif

}


/**
 * ext4_ext_binsearch - Implements the ext binsearch operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void
ext4_ext_binsearch(struct inode *inode,
		struct ext4_ext_path *path, ext4_lblk_t block)
{
	struct ext4_extent_header *eh = path->p_hdr;
	struct ext4_extent *r, *l, *m;

	if (eh->eh_entries == 0) {


		return;
	}

	ext_debug(inode, "binsearch for %u:  ", block);

	l = EXT_FIRST_EXTENT(eh) + 1;
	r = EXT_LAST_EXTENT(eh);

	while (l <= r) {
		m = l + (r - l) / 2;
		ext_debug(inode, "%p(%u):%p(%u):%p(%u) ", l,
			  le32_to_cpu(l->ee_block), m, le32_to_cpu(m->ee_block),
			  r, le32_to_cpu(r->ee_block));

		if (block < le32_to_cpu(m->ee_block))
			r = m - 1;
		else
			l = m + 1;
	}

	path->p_ext = l - 1;
	ext_debug(inode, "  -> %d:%llu:[%d]%d ",
			le32_to_cpu(path->p_ext->ee_block),
			ext4_ext_pblock(path->p_ext),
			ext4_ext_is_unwritten(path->p_ext),
			ext4_ext_get_actual_len(path->p_ext));

#ifdef CHECK_BINSEARCH
	{
		struct ext4_extent *chex, *ex;
		int k;

		chex = ex = EXT_FIRST_EXTENT(eh);
		for (k = 0; k < le16_to_cpu(eh->eh_entries); k++, ex++) {
			BUG_ON(k && le32_to_cpu(ex->ee_block)
					  <= le32_to_cpu(ex[-1].ee_block));
			if (block < le32_to_cpu(ex->ee_block))
				break;
			chex = ex;
		}
		BUG_ON(chex != path->p_ext);
	}
#endif

}

/**
 * ext4_ext_tree_init - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_ext_tree_init(handle_t *handle, struct inode *inode)
{
	struct ext4_extent_header *eh;

	eh = ext_inode_hdr(inode);
	eh->eh_depth = 0;
	eh->eh_entries = 0;
	eh->eh_magic = EXT4_EXT_MAGIC;
	eh->eh_max = cpu_to_le16(ext4_ext_space_root(inode, 0));
	eh->eh_generation = 0;
	ext4_mark_inode_dirty(handle, inode);
}

/**
 * ext4_find_extent - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct ext4_ext_path *
ext4_find_extent(struct inode *inode, ext4_lblk_t block,
		 struct ext4_ext_path *path, int flags)
{
	struct ext4_extent_header *eh;
	struct buffer_head *bh;
	short int depth, i, ppos = 0;
	int ret;
	gfp_t gfp_flags = GFP_NOFS;

	if (flags & EXT4_EX_NOFAIL)
		gfp_flags |= __GFP_NOFAIL;

	eh = ext_inode_hdr(inode);
	depth = ext_depth(inode);
	if (depth < 0 || depth > EXT4_MAX_EXTENT_DEPTH) {
		EXT4_ERROR_INODE(inode, "inode has invalid extent depth: %d",
				 depth);
		ret = -EFSCORRUPTED;
		goto err;
	}

	if (path) {
		ext4_ext_drop_refs(path);
		if (depth > path[0].p_maxdepth) {
			kfree(path);
			path = NULL;
		}
	}
	if (!path) {

		path = kcalloc(depth + 2, sizeof(struct ext4_ext_path),
				gfp_flags);
		if (unlikely(!path))
			return ERR_PTR(-ENOMEM);
		path[0].p_maxdepth = depth + 1;
	}
	path[0].p_hdr = eh;
	path[0].p_bh = NULL;

	i = depth;
	if (!(flags & EXT4_EX_NOCACHE) && depth == 0)
		ext4_cache_extents(inode, eh);

	while (i) {
		ext_debug(inode, "depth %d: num %d, max %d\n",
			  ppos, le16_to_cpu(eh->eh_entries), le16_to_cpu(eh->eh_max));

		ext4_ext_binsearch_idx(inode, path + ppos, block);
		path[ppos].p_block = ext4_idx_pblock(path[ppos].p_idx);
		path[ppos].p_depth = i;
		path[ppos].p_ext = NULL;

		bh = read_extent_tree_block(inode, path[ppos].p_idx, --i, flags);
		if (IS_ERR(bh)) {
			ret = PTR_ERR(bh);
			goto err;
		}

		eh = ext_block_hdr(bh);
		ppos++;
		path[ppos].p_bh = bh;
		path[ppos].p_hdr = eh;
	}

	path[ppos].p_depth = i;
	path[ppos].p_ext = NULL;
	path[ppos].p_idx = NULL;


	ext4_ext_binsearch(inode, path + ppos, block);

	if (path[ppos].p_ext)
		path[ppos].p_block = ext4_ext_pblock(path[ppos].p_ext);

	ext4_ext_show_path(inode, path);

	return path;

err:
	ext4_free_ext_path(path);
	return ERR_PTR(ret);
}


/**
 * ext4_ext_insert_index - Implements the ext insert index operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_ext_insert_index(handle_t *handle, struct inode *inode,
				 struct ext4_ext_path *curp,
				 int logical, ext4_fsblk_t ptr)
{
	struct ext4_extent_idx *ix;
	int len, err;

	err = ext4_ext_get_access(handle, inode, curp);
	if (err)
		return err;

	if (unlikely(logical == le32_to_cpu(curp->p_idx->ei_block))) {
		EXT4_ERROR_INODE(inode,
				 "logical %d == ei_block %d!",
				 logical, le32_to_cpu(curp->p_idx->ei_block));
		return -EFSCORRUPTED;
	}

	if (unlikely(le16_to_cpu(curp->p_hdr->eh_entries)
			     >= le16_to_cpu(curp->p_hdr->eh_max))) {
		EXT4_ERROR_INODE(inode,
				 "eh_entries %d >= eh_max %d!",
				 le16_to_cpu(curp->p_hdr->eh_entries),
				 le16_to_cpu(curp->p_hdr->eh_max));
		return -EFSCORRUPTED;
	}

	if (logical > le32_to_cpu(curp->p_idx->ei_block)) {

		ext_debug(inode, "insert new index %d after: %llu\n",
			  logical, ptr);
		ix = curp->p_idx + 1;
	} else {

		ext_debug(inode, "insert new index %d before: %llu\n",
			  logical, ptr);
		ix = curp->p_idx;
	}

	if (unlikely(ix > EXT_MAX_INDEX(curp->p_hdr))) {
		EXT4_ERROR_INODE(inode, "ix > EXT_MAX_INDEX!");
		return -EFSCORRUPTED;
	}

	len = EXT_LAST_INDEX(curp->p_hdr) - ix + 1;
	BUG_ON(len < 0);
	if (len > 0) {
		ext_debug(inode, "insert new index %d: "
				"move %d indices from 0x%p to 0x%p\n",
				logical, len, ix, ix + 1);
		memmove(ix + 1, ix, len * sizeof(struct ext4_extent_idx));
	}

	ix->ei_block = cpu_to_le32(logical);
	ext4_idx_store_pblock(ix, ptr);
	le16_add_cpu(&curp->p_hdr->eh_entries, 1);

	if (unlikely(ix > EXT_LAST_INDEX(curp->p_hdr))) {
		EXT4_ERROR_INODE(inode, "ix > EXT_LAST_INDEX!");
		return -EFSCORRUPTED;
	}

	err = ext4_ext_dirty(handle, inode, curp);
	ext4_std_error(inode->i_sb, err);

	return err;
}


/**
 * ext4_ext_split - Implements the ext split operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_ext_split(handle_t *handle, struct inode *inode,
			  unsigned int flags,
			  struct ext4_ext_path *path,
			  struct ext4_extent *newext, int at)
{
	struct buffer_head *bh = NULL;
	int depth = ext_depth(inode);
	struct ext4_extent_header *neh;
	struct ext4_extent_idx *fidx;
	int i = at, k, m, a;
	ext4_fsblk_t newblock, oldblock;
	__le32 border;
	ext4_fsblk_t *ablocks = NULL;
	gfp_t gfp_flags = GFP_NOFS;
	int err = 0;
	size_t ext_size = 0;

	if (flags & EXT4_EX_NOFAIL)
		gfp_flags |= __GFP_NOFAIL;


	if (unlikely(path[depth].p_ext > EXT_MAX_EXTENT(path[depth].p_hdr))) {
		EXT4_ERROR_INODE(inode, "p_ext > EXT_MAX_EXTENT!");
		return -EFSCORRUPTED;
	}
	if (path[depth].p_ext != EXT_MAX_EXTENT(path[depth].p_hdr)) {
		border = path[depth].p_ext[1].ee_block;
		ext_debug(inode, "leaf will be split."
				" next leaf starts at %d\n",
				  le32_to_cpu(border));
	} else {
		border = newext->ee_block;
		ext_debug(inode, "leaf will be added."
				" next leaf starts at %d\n",
				le32_to_cpu(border));
	}


	ablocks = kcalloc(depth, sizeof(ext4_fsblk_t), gfp_flags);
	if (!ablocks)
		return -ENOMEM;


	ext_debug(inode, "allocate %d blocks for indexes/leaf\n", depth - at);
	for (a = 0; a < depth - at; a++) {
		newblock = ext4_ext_new_meta_block(handle, inode, path,
						   newext, &err, flags);
		if (newblock == 0)
			goto cleanup;
		ablocks[a] = newblock;
	}


	newblock = ablocks[--a];
	if (unlikely(newblock == 0)) {
		EXT4_ERROR_INODE(inode, "newblock == 0!");
		err = -EFSCORRUPTED;
		goto cleanup;
	}
	bh = sb_getblk_gfp(inode->i_sb, newblock, __GFP_MOVABLE | GFP_NOFS);
	if (unlikely(!bh)) {
		err = -ENOMEM;
		goto cleanup;
	}
	lock_buffer(bh);

	err = ext4_journal_get_create_access(handle, inode->i_sb, bh,
					     EXT4_JTR_NONE);
	if (err)
		goto cleanup;

	neh = ext_block_hdr(bh);
	neh->eh_entries = 0;
	neh->eh_max = cpu_to_le16(ext4_ext_space_block(inode, 0));
	neh->eh_magic = EXT4_EXT_MAGIC;
	neh->eh_depth = 0;
	neh->eh_generation = 0;


	if (unlikely(path[depth].p_hdr->eh_entries !=
		     path[depth].p_hdr->eh_max)) {
		EXT4_ERROR_INODE(inode, "eh_entries %d != eh_max %d!",
				 path[depth].p_hdr->eh_entries,
				 path[depth].p_hdr->eh_max);
		err = -EFSCORRUPTED;
		goto cleanup;
	}

	m = EXT_MAX_EXTENT(path[depth].p_hdr) - path[depth].p_ext++;
	ext4_ext_show_move(inode, path, newblock, depth);
	if (m) {
		struct ext4_extent *ex;
		ex = EXT_FIRST_EXTENT(neh);
		memmove(ex, path[depth].p_ext, sizeof(struct ext4_extent) * m);
		le16_add_cpu(&neh->eh_entries, m);
	}


	ext_size = sizeof(struct ext4_extent_header) +
		sizeof(struct ext4_extent) * le16_to_cpu(neh->eh_entries);
	memset(bh->b_data + ext_size, 0, inode->i_sb->s_blocksize - ext_size);
	ext4_extent_block_csum_set(inode, neh);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);

	err = ext4_handle_dirty_metadata(handle, inode, bh);
	if (err)
		goto cleanup;
	brelse(bh);
	bh = NULL;


	if (m) {
		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto cleanup;
		le16_add_cpu(&path[depth].p_hdr->eh_entries, -m);
		err = ext4_ext_dirty(handle, inode, path + depth);
		if (err)
			goto cleanup;

	}


	k = depth - at - 1;
	if (unlikely(k < 0)) {
		EXT4_ERROR_INODE(inode, "k %d < 0!", k);
		err = -EFSCORRUPTED;
		goto cleanup;
	}
	if (k)
		ext_debug(inode, "create %d intermediate indices\n", k);


	i = depth - 1;
	while (k--) {
		oldblock = newblock;
		newblock = ablocks[--a];
		bh = sb_getblk(inode->i_sb, newblock);
		if (unlikely(!bh)) {
			err = -ENOMEM;
			goto cleanup;
		}
		lock_buffer(bh);

		err = ext4_journal_get_create_access(handle, inode->i_sb, bh,
						     EXT4_JTR_NONE);
		if (err)
			goto cleanup;

		neh = ext_block_hdr(bh);
		neh->eh_entries = cpu_to_le16(1);
		neh->eh_magic = EXT4_EXT_MAGIC;
		neh->eh_max = cpu_to_le16(ext4_ext_space_block_idx(inode, 0));
		neh->eh_depth = cpu_to_le16(depth - i);
		neh->eh_generation = 0;
		fidx = EXT_FIRST_INDEX(neh);
		fidx->ei_block = border;
		ext4_idx_store_pblock(fidx, oldblock);

		ext_debug(inode, "int.index at %d (block %llu): %u -> %llu\n",
				i, newblock, le32_to_cpu(border), oldblock);


		if (unlikely(EXT_MAX_INDEX(path[i].p_hdr) !=
					EXT_LAST_INDEX(path[i].p_hdr))) {
			EXT4_ERROR_INODE(inode,
					 "EXT_MAX_INDEX != EXT_LAST_INDEX ee_block %d!",
					 le32_to_cpu(path[i].p_ext->ee_block));
			err = -EFSCORRUPTED;
			goto cleanup;
		}

		m = EXT_MAX_INDEX(path[i].p_hdr) - path[i].p_idx++;
		ext_debug(inode, "cur 0x%p, last 0x%p\n", path[i].p_idx,
				EXT_MAX_INDEX(path[i].p_hdr));
		ext4_ext_show_move(inode, path, newblock, i);
		if (m) {
			memmove(++fidx, path[i].p_idx,
				sizeof(struct ext4_extent_idx) * m);
			le16_add_cpu(&neh->eh_entries, m);
		}

		ext_size = sizeof(struct ext4_extent_header) +
		   (sizeof(struct ext4_extent) * le16_to_cpu(neh->eh_entries));
		memset(bh->b_data + ext_size, 0,
			inode->i_sb->s_blocksize - ext_size);
		ext4_extent_block_csum_set(inode, neh);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);

		err = ext4_handle_dirty_metadata(handle, inode, bh);
		if (err)
			goto cleanup;
		brelse(bh);
		bh = NULL;


		if (m) {
			err = ext4_ext_get_access(handle, inode, path + i);
			if (err)
				goto cleanup;
			le16_add_cpu(&path[i].p_hdr->eh_entries, -m);
			err = ext4_ext_dirty(handle, inode, path + i);
			if (err)
				goto cleanup;
		}

		i--;
	}


	err = ext4_ext_insert_index(handle, inode, path + at,
				    le32_to_cpu(border), newblock);

cleanup:
	if (bh) {
		if (buffer_locked(bh))
			unlock_buffer(bh);
		brelse(bh);
	}

	if (err) {

		for (i = 0; i < depth; i++) {
			if (!ablocks[i])
				continue;
			ext4_free_blocks(handle, inode, NULL, ablocks[i], 1,
					 EXT4_FREE_BLOCKS_METADATA);
		}
	}
	kfree(ablocks);

	return err;
}


/**
 * ext4_ext_grow_indepth - Changes filesystem geometry while preserving address-space, allocation and recovery invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_ext_grow_indepth(handle_t *handle, struct inode *inode,
				 unsigned int flags)
{
	struct ext4_extent_header *neh;
	struct buffer_head *bh;
	ext4_fsblk_t newblock, goal = 0;
	struct ext4_super_block *es = EXT4_SB(inode->i_sb)->s_es;
	int err = 0;
	size_t ext_size = 0;


	if (ext_depth(inode))
		goal = ext4_idx_pblock(EXT_FIRST_INDEX(ext_inode_hdr(inode)));
	if (goal > le32_to_cpu(es->s_first_data_block)) {
		flags |= EXT4_MB_HINT_TRY_GOAL;
		goal--;
	} else
		goal = ext4_inode_to_goal_block(inode);
	newblock = ext4_new_meta_blocks(handle, inode, goal, flags,
					NULL, &err);
	if (newblock == 0)
		return err;

	bh = sb_getblk_gfp(inode->i_sb, newblock, __GFP_MOVABLE | GFP_NOFS);
	if (unlikely(!bh))
		return -ENOMEM;
	lock_buffer(bh);

	err = ext4_journal_get_create_access(handle, inode->i_sb, bh,
					     EXT4_JTR_NONE);
	if (err) {
		unlock_buffer(bh);
		goto out;
	}

	ext_size = sizeof(EXT4_I(inode)->i_data);

	memmove(bh->b_data, EXT4_I(inode)->i_data, ext_size);

	memset(bh->b_data + ext_size, 0, inode->i_sb->s_blocksize - ext_size);


	neh = ext_block_hdr(bh);


	if (ext_depth(inode))
		neh->eh_max = cpu_to_le16(ext4_ext_space_block_idx(inode, 0));
	else
		neh->eh_max = cpu_to_le16(ext4_ext_space_block(inode, 0));
	neh->eh_magic = EXT4_EXT_MAGIC;
	ext4_extent_block_csum_set(inode, neh);
	set_buffer_uptodate(bh);
	set_buffer_verified(bh);
	unlock_buffer(bh);

	err = ext4_handle_dirty_metadata(handle, inode, bh);
	if (err)
		goto out;


	neh = ext_inode_hdr(inode);
	neh->eh_entries = cpu_to_le16(1);
	ext4_idx_store_pblock(EXT_FIRST_INDEX(neh), newblock);
	if (neh->eh_depth == 0) {

		neh->eh_max = cpu_to_le16(ext4_ext_space_root_idx(inode, 0));
		EXT_FIRST_INDEX(neh)->ei_block =
			EXT_FIRST_EXTENT(neh)->ee_block;
	}
	ext_debug(inode, "new root: num %d(%d), lblock %d, ptr %llu\n",
		  le16_to_cpu(neh->eh_entries), le16_to_cpu(neh->eh_max),
		  le32_to_cpu(EXT_FIRST_INDEX(neh)->ei_block),
		  ext4_idx_pblock(EXT_FIRST_INDEX(neh)));

	le16_add_cpu(&neh->eh_depth, 1);
	err = ext4_mark_inode_dirty(handle, inode);
out:
	brelse(bh);

	return err;
}


/**
 * ext4_ext_create_new_leaf - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct ext4_ext_path *
ext4_ext_create_new_leaf(handle_t *handle, struct inode *inode,
			 unsigned int mb_flags, unsigned int gb_flags,
			 struct ext4_ext_path *path,
			 struct ext4_extent *newext)
{
	struct ext4_ext_path *curp;
	int depth, i, err = 0;
	ext4_lblk_t ee_block = le32_to_cpu(newext->ee_block);

repeat:
	i = depth = ext_depth(inode);


	curp = path + depth;
	while (i > 0 && !EXT_HAS_FREE_INDEX(curp)) {
		i--;
		curp--;
	}


	if (EXT_HAS_FREE_INDEX(curp)) {


		err = ext4_ext_split(handle, inode, mb_flags, path, newext, i);
		if (err)
			goto errout;


		path = ext4_find_extent(inode, ee_block, path, gb_flags);
		return path;
	}


	err = ext4_ext_grow_indepth(handle, inode, mb_flags);
	if (err)
		goto errout;


	path = ext4_find_extent(inode, ee_block, path, gb_flags);
	if (IS_ERR(path))
		return path;


	depth = ext_depth(inode);
	if (path[depth].p_hdr->eh_entries == path[depth].p_hdr->eh_max) {

		goto repeat;
	}

	return path;

errout:
	ext4_free_ext_path(path);
	return ERR_PTR(err);
}


/**
 * ext4_ext_search_left - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_ext_search_left(struct inode *inode,
				struct ext4_ext_path *path,
				ext4_lblk_t *logical, ext4_fsblk_t *phys)
{
	struct ext4_extent_idx *ix;
	struct ext4_extent *ex;
	int depth, ee_len;

	if (unlikely(path == NULL)) {
		EXT4_ERROR_INODE(inode, "path == NULL *logical %d!", *logical);
		return -EFSCORRUPTED;
	}
	depth = path->p_depth;
	*phys = 0;

	if (depth == 0 && path->p_ext == NULL)
		return 0;


	ex = path[depth].p_ext;
	ee_len = ext4_ext_get_actual_len(ex);
	if (*logical < le32_to_cpu(ex->ee_block)) {
		if (unlikely(EXT_FIRST_EXTENT(path[depth].p_hdr) != ex)) {
			EXT4_ERROR_INODE(inode,
					 "EXT_FIRST_EXTENT != ex *logical %d ee_block %d!",
					 *logical, le32_to_cpu(ex->ee_block));
			return -EFSCORRUPTED;
		}
		while (--depth >= 0) {
			ix = path[depth].p_idx;
			if (unlikely(ix != EXT_FIRST_INDEX(path[depth].p_hdr))) {
				EXT4_ERROR_INODE(inode,
				  "ix (%d) != EXT_FIRST_INDEX (%d) (depth %d)!",
				  ix != NULL ? le32_to_cpu(ix->ei_block) : 0,
				  le32_to_cpu(EXT_FIRST_INDEX(path[depth].p_hdr)->ei_block),
				  depth);
				return -EFSCORRUPTED;
			}
		}
		return 0;
	}

	if (unlikely(*logical < (le32_to_cpu(ex->ee_block) + ee_len))) {
		EXT4_ERROR_INODE(inode,
				 "logical %d < ee_block %d + ee_len %d!",
				 *logical, le32_to_cpu(ex->ee_block), ee_len);
		return -EFSCORRUPTED;
	}

	*logical = le32_to_cpu(ex->ee_block) + ee_len - 1;
	*phys = ext4_ext_pblock(ex) + ee_len - 1;
	return 0;
}


/**
 * ext4_ext_search_right - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_ext_search_right(struct inode *inode,
				 struct ext4_ext_path *path,
				 ext4_lblk_t *logical, ext4_fsblk_t *phys,
				 struct ext4_extent *ret_ex)
{
	struct buffer_head *bh = NULL;
	struct ext4_extent_header *eh;
	struct ext4_extent_idx *ix;
	struct ext4_extent *ex;
	int depth;
	int ee_len;

	if (unlikely(path == NULL)) {
		EXT4_ERROR_INODE(inode, "path == NULL *logical %d!", *logical);
		return -EFSCORRUPTED;
	}
	depth = path->p_depth;
	*phys = 0;

	if (depth == 0 && path->p_ext == NULL)
		return 0;


	ex = path[depth].p_ext;
	ee_len = ext4_ext_get_actual_len(ex);
	if (*logical < le32_to_cpu(ex->ee_block)) {
		if (unlikely(EXT_FIRST_EXTENT(path[depth].p_hdr) != ex)) {
			EXT4_ERROR_INODE(inode,
					 "first_extent(path[%d].p_hdr) != ex",
					 depth);
			return -EFSCORRUPTED;
		}
		while (--depth >= 0) {
			ix = path[depth].p_idx;
			if (unlikely(ix != EXT_FIRST_INDEX(path[depth].p_hdr))) {
				EXT4_ERROR_INODE(inode,
						 "ix != EXT_FIRST_INDEX *logical %d!",
						 *logical);
				return -EFSCORRUPTED;
			}
		}
		goto found_extent;
	}

	if (unlikely(*logical < (le32_to_cpu(ex->ee_block) + ee_len))) {
		EXT4_ERROR_INODE(inode,
				 "logical %d < ee_block %d + ee_len %d!",
				 *logical, le32_to_cpu(ex->ee_block), ee_len);
		return -EFSCORRUPTED;
	}

	if (ex != EXT_LAST_EXTENT(path[depth].p_hdr)) {

		ex++;
		goto found_extent;
	}


	while (--depth >= 0) {
		ix = path[depth].p_idx;
		if (ix != EXT_LAST_INDEX(path[depth].p_hdr))
			goto got_index;
	}


	return 0;

got_index:


	ix++;
	while (++depth < path->p_depth) {

		bh = read_extent_tree_block(inode, ix, path->p_depth - depth, 0);
		if (IS_ERR(bh))
			return PTR_ERR(bh);
		eh = ext_block_hdr(bh);
		ix = EXT_FIRST_INDEX(eh);
		put_bh(bh);
	}

	bh = read_extent_tree_block(inode, ix, path->p_depth - depth, 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	eh = ext_block_hdr(bh);
	ex = EXT_FIRST_EXTENT(eh);
found_extent:
	*logical = le32_to_cpu(ex->ee_block);
	*phys = ext4_ext_pblock(ex);
	if (ret_ex)
		*ret_ex = *ex;
	if (bh)
		put_bh(bh);
	return 1;
}


/**
 * ext4_ext_next_allocated_block - Implements the ext next allocated block operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
ext4_lblk_t
ext4_ext_next_allocated_block(struct ext4_ext_path *path)
{
	int depth;

	BUG_ON(path == NULL);
	depth = path->p_depth;

	if (depth == 0 && path->p_ext == NULL)
		return EXT_MAX_BLOCKS;

	while (depth >= 0) {
		struct ext4_ext_path *p = &path[depth];

		if (depth == path->p_depth) {

			if (p->p_ext && p->p_ext != EXT_LAST_EXTENT(p->p_hdr))
				return le32_to_cpu(p->p_ext[1].ee_block);
		} else {

			if (p->p_idx != EXT_LAST_INDEX(p->p_hdr))
				return le32_to_cpu(p->p_idx[1].ei_block);
		}
		depth--;
	}

	return EXT_MAX_BLOCKS;
}


/**
 * ext4_ext_next_leaf_block - Implements the ext next leaf block operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ext4_lblk_t ext4_ext_next_leaf_block(struct ext4_ext_path *path)
{
	int depth;

	BUG_ON(path == NULL);
	depth = path->p_depth;


	if (depth == 0)
		return EXT_MAX_BLOCKS;


	depth--;

	while (depth >= 0) {
		if (path[depth].p_idx !=
				EXT_LAST_INDEX(path[depth].p_hdr))
			return (ext4_lblk_t)
				le32_to_cpu(path[depth].p_idx[1].ei_block);
		depth--;
	}

	return EXT_MAX_BLOCKS;
}


/**
 * ext4_ext_correct_indexes - Implements the ext correct indexes operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_ext_correct_indexes(handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path)
{
	struct ext4_extent_header *eh;
	int depth = ext_depth(inode);
	struct ext4_extent *ex;
	__le32 border;
	int k, err = 0;

	eh = path[depth].p_hdr;
	ex = path[depth].p_ext;

	if (unlikely(ex == NULL || eh == NULL)) {
		EXT4_ERROR_INODE(inode,
				 "ex %p == NULL or eh %p == NULL", ex, eh);
		return -EFSCORRUPTED;
	}

	if (depth == 0) {

		return 0;
	}

	if (ex != EXT_FIRST_EXTENT(eh)) {

		return 0;
	}


	k = depth - 1;
	border = path[depth].p_ext->ee_block;
	err = ext4_ext_get_access(handle, inode, path + k);
	if (err)
		return err;
	if (unlikely(path[k].p_idx > EXT_LAST_INDEX(path[k].p_hdr))) {
		EXT4_ERROR_INODE(inode,
				 "path[%d].p_idx %p > EXT_LAST_INDEX %p",
				 k, path[k].p_idx,
				 EXT_LAST_INDEX(path[k].p_hdr));
		return -EFSCORRUPTED;
	}
	path[k].p_idx->ei_block = border;
	err = ext4_ext_dirty(handle, inode, path + k);
	if (err)
		return err;

	while (k--) {

		if (path[k+1].p_idx != EXT_FIRST_INDEX(path[k+1].p_hdr))
			break;
		err = ext4_ext_get_access(handle, inode, path + k);
		if (err)
			goto clean;
		if (unlikely(path[k].p_idx > EXT_LAST_INDEX(path[k].p_hdr))) {
			EXT4_ERROR_INODE(inode,
					 "path[%d].p_idx %p > EXT_LAST_INDEX %p",
					 k, path[k].p_idx,
					 EXT_LAST_INDEX(path[k].p_hdr));
			err = -EFSCORRUPTED;
			goto clean;
		}
		path[k].p_idx->ei_block = border;
		err = ext4_ext_dirty(handle, inode, path + k);
		if (err)
			goto clean;
	}
	return 0;

clean:


	while (++k < depth)
		clear_buffer_verified(path[k].p_bh);

	return err;
}

/**
 * ext4_can_extents_be_merged - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_can_extents_be_merged(struct inode *inode,
				      struct ext4_extent *ex1,
				      struct ext4_extent *ex2)
{
	unsigned short ext1_ee_len, ext2_ee_len;

	if (ext4_ext_is_unwritten(ex1) != ext4_ext_is_unwritten(ex2))
		return 0;

	ext1_ee_len = ext4_ext_get_actual_len(ex1);
	ext2_ee_len = ext4_ext_get_actual_len(ex2);

	if (le32_to_cpu(ex1->ee_block) + ext1_ee_len !=
			le32_to_cpu(ex2->ee_block))
		return 0;

	if (ext1_ee_len + ext2_ee_len > EXT_INIT_MAX_LEN)
		return 0;

	if (ext4_ext_is_unwritten(ex1) &&
	    ext1_ee_len + ext2_ee_len > EXT_UNWRITTEN_MAX_LEN)
		return 0;
#ifdef AGGRESSIVE_TEST
	if (ext1_ee_len >= 4)
		return 0;
#endif

	if (ext4_ext_pblock(ex1) + ext1_ee_len == ext4_ext_pblock(ex2))
		return 1;
	return 0;
}


/**
 * ext4_ext_try_to_merge_right - Implements the ext try to merge right operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_ext_try_to_merge_right(struct inode *inode,
				 struct ext4_ext_path *path,
				 struct ext4_extent *ex)
{
	struct ext4_extent_header *eh;
	unsigned int depth, len;
	int merge_done = 0, unwritten;

	depth = ext_depth(inode);
	BUG_ON(path[depth].p_hdr == NULL);
	eh = path[depth].p_hdr;

	while (ex < EXT_LAST_EXTENT(eh)) {
		if (!ext4_can_extents_be_merged(inode, ex, ex + 1))
			break;

		unwritten = ext4_ext_is_unwritten(ex);
		ex->ee_len = cpu_to_le16(ext4_ext_get_actual_len(ex)
				+ ext4_ext_get_actual_len(ex + 1));
		if (unwritten)
			ext4_ext_mark_unwritten(ex);

		if (ex + 1 < EXT_LAST_EXTENT(eh)) {
			len = (EXT_LAST_EXTENT(eh) - ex - 1)
				* sizeof(struct ext4_extent);
			memmove(ex + 1, ex + 2, len);
		}
		le16_add_cpu(&eh->eh_entries, -1);
		merge_done = 1;
		WARN_ON(eh->eh_entries == 0);
		if (!eh->eh_entries)
			EXT4_ERROR_INODE(inode, "eh->eh_entries = 0!");
	}

	return merge_done;
}


/**
 * ext4_ext_try_to_merge_up - Implements the ext try to merge up operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_ext_try_to_merge_up(handle_t *handle,
				     struct inode *inode,
				     struct ext4_ext_path *path)
{
	size_t s;
	unsigned max_root = ext4_ext_space_root(inode, 0);
	ext4_fsblk_t blk;

	if ((path[0].p_depth != 1) ||
	    (le16_to_cpu(path[0].p_hdr->eh_entries) != 1) ||
	    (le16_to_cpu(path[1].p_hdr->eh_entries) > max_root))
		return;


	if (ext4_journal_extend(handle, 2,
			ext4_free_metadata_revoke_credits(inode->i_sb, 1)))
		return;


	blk = ext4_idx_pblock(path[0].p_idx);
	s = le16_to_cpu(path[1].p_hdr->eh_entries) *
		sizeof(struct ext4_extent_idx);
	s += sizeof(struct ext4_extent_header);

	path[1].p_maxdepth = path[0].p_maxdepth;
	memcpy(path[0].p_hdr, path[1].p_hdr, s);
	path[0].p_depth = 0;
	path[0].p_ext = EXT_FIRST_EXTENT(path[0].p_hdr) +
		(path[1].p_ext - EXT_FIRST_EXTENT(path[1].p_hdr));
	path[0].p_hdr->eh_max = cpu_to_le16(max_root);

	ext4_ext_path_brelse(path + 1);
	ext4_free_blocks(handle, inode, NULL, blk, 1,
			 EXT4_FREE_BLOCKS_METADATA | EXT4_FREE_BLOCKS_FORGET);
}


/**
 * ext4_ext_try_to_merge - Implements the ext try to merge operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_ext_try_to_merge(handle_t *handle,
				  struct inode *inode,
				  struct ext4_ext_path *path,
				  struct ext4_extent *ex)
{
	struct ext4_extent_header *eh;
	unsigned int depth;
	int merge_done = 0;

	depth = ext_depth(inode);
	BUG_ON(path[depth].p_hdr == NULL);
	eh = path[depth].p_hdr;

	if (ex > EXT_FIRST_EXTENT(eh))
		merge_done = ext4_ext_try_to_merge_right(inode, path, ex - 1);

	if (!merge_done)
		(void) ext4_ext_try_to_merge_right(inode, path, ex);

	ext4_ext_try_to_merge_up(handle, inode, path);
}


/**
 * ext4_ext_check_overlap - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static unsigned int ext4_ext_check_overlap(struct ext4_sb_info *sbi,
					   struct inode *inode,
					   struct ext4_extent *newext,
					   struct ext4_ext_path *path)
{
	ext4_lblk_t b1, b2;
	unsigned int depth, len1;
	unsigned int ret = 0;

	b1 = le32_to_cpu(newext->ee_block);
	len1 = ext4_ext_get_actual_len(newext);
	depth = ext_depth(inode);
	if (!path[depth].p_ext)
		goto out;
	b2 = EXT4_LBLK_CMASK(sbi, le32_to_cpu(path[depth].p_ext->ee_block));


	if (b2 < b1) {
		b2 = ext4_ext_next_allocated_block(path);
		if (b2 == EXT_MAX_BLOCKS)
			goto out;
		b2 = EXT4_LBLK_CMASK(sbi, b2);
	}


	if (b1 + len1 < b1) {
		len1 = EXT_MAX_BLOCKS - b1;
		newext->ee_len = cpu_to_le16(len1);
		ret = 1;
	}


	if (b1 + len1 > b2) {
		newext->ee_len = cpu_to_le16(b2 - b1);
		ret = 1;
	}
out:
	return ret;
}


/**
 * ext4_ext_insert_extent - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct ext4_ext_path *
ext4_ext_insert_extent(handle_t *handle, struct inode *inode,
		       struct ext4_ext_path *path,
		       struct ext4_extent *newext, int gb_flags)
{
	struct ext4_extent_header *eh;
	struct ext4_extent *ex, *fex;
	struct ext4_extent *nearex;
	int depth, len, err = 0;
	ext4_lblk_t next;
	int mb_flags = 0, unwritten;

	if (gb_flags & EXT4_GET_BLOCKS_DELALLOC_RESERVE)
		mb_flags |= EXT4_MB_DELALLOC_RESERVED;
	if (unlikely(ext4_ext_get_actual_len(newext) == 0)) {
		EXT4_ERROR_INODE(inode, "ext4_ext_get_actual_len(newext) == 0");
		err = -EFSCORRUPTED;
		goto errout;
	}
	depth = ext_depth(inode);
	ex = path[depth].p_ext;
	eh = path[depth].p_hdr;
	if (unlikely(path[depth].p_hdr == NULL)) {
		EXT4_ERROR_INODE(inode, "path[%d].p_hdr == NULL", depth);
		err = -EFSCORRUPTED;
		goto errout;
	}


	if (ex && !(gb_flags & EXT4_GET_BLOCKS_PRE_IO)) {


		if (ex < EXT_LAST_EXTENT(eh) &&
		    (le32_to_cpu(ex->ee_block) +
		    ext4_ext_get_actual_len(ex) <
		    le32_to_cpu(newext->ee_block))) {
			ex += 1;
			goto prepend;
		} else if ((ex > EXT_FIRST_EXTENT(eh)) &&
			   (le32_to_cpu(newext->ee_block) +
			   ext4_ext_get_actual_len(newext) <
			   le32_to_cpu(ex->ee_block)))
			ex -= 1;


		if (ext4_can_extents_be_merged(inode, ex, newext)) {
			ext_debug(inode, "append [%d]%d block to %u:[%d]%d"
				  "(from %llu)\n",
				  ext4_ext_is_unwritten(newext),
				  ext4_ext_get_actual_len(newext),
				  le32_to_cpu(ex->ee_block),
				  ext4_ext_is_unwritten(ex),
				  ext4_ext_get_actual_len(ex),
				  ext4_ext_pblock(ex));
			err = ext4_ext_get_access(handle, inode,
						  path + depth);
			if (err)
				goto errout;
			unwritten = ext4_ext_is_unwritten(ex);
			ex->ee_len = cpu_to_le16(ext4_ext_get_actual_len(ex)
					+ ext4_ext_get_actual_len(newext));
			if (unwritten)
				ext4_ext_mark_unwritten(ex);
			nearex = ex;
			goto merge;
		}

prepend:

		if (ext4_can_extents_be_merged(inode, newext, ex)) {
			ext_debug(inode, "prepend %u[%d]%d block to %u:[%d]%d"
				  "(from %llu)\n",
				  le32_to_cpu(newext->ee_block),
				  ext4_ext_is_unwritten(newext),
				  ext4_ext_get_actual_len(newext),
				  le32_to_cpu(ex->ee_block),
				  ext4_ext_is_unwritten(ex),
				  ext4_ext_get_actual_len(ex),
				  ext4_ext_pblock(ex));
			err = ext4_ext_get_access(handle, inode,
						  path + depth);
			if (err)
				goto errout;

			unwritten = ext4_ext_is_unwritten(ex);
			ex->ee_block = newext->ee_block;
			ext4_ext_store_pblock(ex, ext4_ext_pblock(newext));
			ex->ee_len = cpu_to_le16(ext4_ext_get_actual_len(ex)
					+ ext4_ext_get_actual_len(newext));
			if (unwritten)
				ext4_ext_mark_unwritten(ex);
			nearex = ex;
			goto merge;
		}
	}

	depth = ext_depth(inode);
	eh = path[depth].p_hdr;
	if (le16_to_cpu(eh->eh_entries) < le16_to_cpu(eh->eh_max))
		goto has_space;


	fex = EXT_LAST_EXTENT(eh);
	next = EXT_MAX_BLOCKS;
	if (le32_to_cpu(newext->ee_block) > le32_to_cpu(fex->ee_block))
		next = ext4_ext_next_leaf_block(path);
	if (next != EXT_MAX_BLOCKS) {
		struct ext4_ext_path *npath;

		ext_debug(inode, "next leaf block - %u\n", next);
		npath = ext4_find_extent(inode, next, NULL, gb_flags);
		if (IS_ERR(npath)) {
			err = PTR_ERR(npath);
			goto errout;
		}
		BUG_ON(npath->p_depth != path->p_depth);
		eh = npath[depth].p_hdr;
		if (le16_to_cpu(eh->eh_entries) < le16_to_cpu(eh->eh_max)) {
			ext_debug(inode, "next leaf isn't full(%d)\n",
				  le16_to_cpu(eh->eh_entries));
			ext4_free_ext_path(path);
			path = npath;
			goto has_space;
		}
		ext_debug(inode, "next leaf has no free space(%d,%d)\n",
			  le16_to_cpu(eh->eh_entries), le16_to_cpu(eh->eh_max));
		ext4_free_ext_path(npath);
	}


	if (gb_flags & EXT4_GET_BLOCKS_METADATA_NOFAIL)
		mb_flags |= EXT4_MB_USE_RESERVED;
	path = ext4_ext_create_new_leaf(handle, inode, mb_flags, gb_flags,
					path, newext);
	if (IS_ERR(path))
		return path;
	depth = ext_depth(inode);
	eh = path[depth].p_hdr;

has_space:
	nearex = path[depth].p_ext;

	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto errout;

	if (!nearex) {

		ext_debug(inode, "first extent in the leaf: %u:%llu:[%d]%d\n",
				le32_to_cpu(newext->ee_block),
				ext4_ext_pblock(newext),
				ext4_ext_is_unwritten(newext),
				ext4_ext_get_actual_len(newext));
		nearex = EXT_FIRST_EXTENT(eh);
	} else {
		if (le32_to_cpu(newext->ee_block)
			   > le32_to_cpu(nearex->ee_block)) {

			ext_debug(inode, "insert %u:%llu:[%d]%d before: "
					"nearest %p\n",
					le32_to_cpu(newext->ee_block),
					ext4_ext_pblock(newext),
					ext4_ext_is_unwritten(newext),
					ext4_ext_get_actual_len(newext),
					nearex);
			nearex++;
		} else {

			BUG_ON(newext->ee_block == nearex->ee_block);
			ext_debug(inode, "insert %u:%llu:[%d]%d after: "
					"nearest %p\n",
					le32_to_cpu(newext->ee_block),
					ext4_ext_pblock(newext),
					ext4_ext_is_unwritten(newext),
					ext4_ext_get_actual_len(newext),
					nearex);
		}
		len = EXT_LAST_EXTENT(eh) - nearex + 1;
		if (len > 0) {
			ext_debug(inode, "insert %u:%llu:[%d]%d: "
					"move %d extents from 0x%p to 0x%p\n",
					le32_to_cpu(newext->ee_block),
					ext4_ext_pblock(newext),
					ext4_ext_is_unwritten(newext),
					ext4_ext_get_actual_len(newext),
					len, nearex, nearex + 1);
			memmove(nearex + 1, nearex,
				len * sizeof(struct ext4_extent));
		}
	}

	le16_add_cpu(&eh->eh_entries, 1);
	path[depth].p_ext = nearex;
	nearex->ee_block = newext->ee_block;
	ext4_ext_store_pblock(nearex, ext4_ext_pblock(newext));
	nearex->ee_len = newext->ee_len;

merge:

	if (!(gb_flags & EXT4_GET_BLOCKS_PRE_IO))
		ext4_ext_try_to_merge(handle, inode, path, nearex);


	err = ext4_ext_correct_indexes(handle, inode, path);
	if (err)
		goto errout;

	err = ext4_ext_dirty(handle, inode, path + path->p_depth);
	if (err)
		goto errout;

	return path;

errout:
	ext4_free_ext_path(path);
	return ERR_PTR(err);
}

/**
 * ext4_fill_es_cache_info - Implements the fill es cache info operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fill_es_cache_info(struct inode *inode,
				   ext4_lblk_t block, ext4_lblk_t num,
				   struct fiemap_extent_info *fieinfo)
{
	ext4_lblk_t next, end = block + num - 1;
	struct extent_status es;
	unsigned char blksize_bits = inode->i_sb->s_blocksize_bits;
	unsigned int flags;
	int err;

	while (block <= end) {
		next = 0;
		flags = 0;
		if (!ext4_es_lookup_extent(inode, block, &next, &es))
			break;
		if (ext4_es_is_unwritten(&es))
			flags |= FIEMAP_EXTENT_UNWRITTEN;
		if (ext4_es_is_delayed(&es))
			flags |= (FIEMAP_EXTENT_DELALLOC |
				  FIEMAP_EXTENT_UNKNOWN);
		if (ext4_es_is_hole(&es))
			flags |= EXT4_FIEMAP_EXTENT_HOLE;
		if (next == 0)
			flags |= FIEMAP_EXTENT_LAST;
		if (flags & (FIEMAP_EXTENT_DELALLOC|
			     EXT4_FIEMAP_EXTENT_HOLE))
			es.es_pblk = 0;
		else
			es.es_pblk = ext4_es_pblock(&es);
		err = fiemap_fill_next_extent(fieinfo,
				(__u64)es.es_lblk << blksize_bits,
				(__u64)es.es_pblk << blksize_bits,
				(__u64)es.es_len << blksize_bits,
				flags);
		if (next == 0)
			break;
		block = next;
		if (err < 0)
			return err;
		if (err == 1)
			return 0;
	}
	return 0;
}


/**
 * ext4_ext_find_hole - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ext4_lblk_t ext4_ext_find_hole(struct inode *inode,
				      struct ext4_ext_path *path,
				      ext4_lblk_t *lblk)
{
	int depth = ext_depth(inode);
	struct ext4_extent *ex;
	ext4_lblk_t len;

	ex = path[depth].p_ext;
	if (ex == NULL) {

		*lblk = 0;
		len = EXT_MAX_BLOCKS;
	} else if (*lblk < le32_to_cpu(ex->ee_block)) {
		len = le32_to_cpu(ex->ee_block) - *lblk;
	} else if (*lblk >= le32_to_cpu(ex->ee_block)
			+ ext4_ext_get_actual_len(ex)) {
		ext4_lblk_t next;

		*lblk = le32_to_cpu(ex->ee_block) + ext4_ext_get_actual_len(ex);
		next = ext4_ext_next_allocated_block(path);
		BUG_ON(next == *lblk);
		len = next - *lblk;
	} else {
		BUG();
	}
	return len;
}


/**
 * ext4_ext_rm_idx - Implements the ext rm idx operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_ext_rm_idx(handle_t *handle, struct inode *inode,
			struct ext4_ext_path *path, int depth)
{
	int err;
	ext4_fsblk_t leaf;
	int k = depth - 1;


	leaf = ext4_idx_pblock(path[k].p_idx);
	if (unlikely(path[k].p_hdr->eh_entries == 0)) {
		EXT4_ERROR_INODE(inode, "path[%d].p_hdr->eh_entries == 0", k);
		return -EFSCORRUPTED;
	}
	err = ext4_ext_get_access(handle, inode, path + k);
	if (err)
		return err;

	if (path[k].p_idx != EXT_LAST_INDEX(path[k].p_hdr)) {
		int len = EXT_LAST_INDEX(path[k].p_hdr) - path[k].p_idx;
		len *= sizeof(struct ext4_extent_idx);
		memmove(path[k].p_idx, path[k].p_idx + 1, len);
	}

	le16_add_cpu(&path[k].p_hdr->eh_entries, -1);
	err = ext4_ext_dirty(handle, inode, path + k);
	if (err)
		return err;
	ext_debug(inode, "index is empty, remove it, free block %llu\n", leaf);
	trace_ext4_ext_rm_idx(inode, leaf);

	ext4_free_blocks(handle, inode, NULL, leaf, 1,
			 EXT4_FREE_BLOCKS_METADATA | EXT4_FREE_BLOCKS_FORGET);

	while (--k >= 0) {
		if (path[k + 1].p_idx != EXT_FIRST_INDEX(path[k + 1].p_hdr))
			break;
		err = ext4_ext_get_access(handle, inode, path + k);
		if (err)
			goto clean;
		path[k].p_idx->ei_block = path[k + 1].p_idx->ei_block;
		err = ext4_ext_dirty(handle, inode, path + k);
		if (err)
			goto clean;
	}
	return 0;

clean:


	while (++k < depth)
		clear_buffer_verified(path[k].p_bh);

	return err;
}


/**
 * ext4_ext_calc_credits_for_single_extent - Computes derived filesystem state used for validation, accounting or policy decisions.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_ext_calc_credits_for_single_extent(struct inode *inode, int nrblocks,
						struct ext4_ext_path *path)
{
	if (path) {
		int depth = ext_depth(inode);
		int ret = 0;


		if (le16_to_cpu(path[depth].p_hdr->eh_entries)
				< le16_to_cpu(path[depth].p_hdr->eh_max)) {


			ret = 2 + EXT4_META_TRANS_BLOCKS(inode->i_sb);
			return ret;
		}
	}

	return ext4_chunk_trans_blocks(inode, nrblocks);
}


/**
 * ext4_ext_index_trans_blocks - Implements the ext index trans blocks operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_ext_index_trans_blocks(struct inode *inode, int extents)
{
	int index;


	if (ext4_has_inline_data(inode))
		return 1;


	if (extents <= 1)
		index = EXT4_MAX_EXTENT_DEPTH * 2;
	else
		index = EXT4_MAX_EXTENT_DEPTH * 3;

	return index;
}

/**
 * get_default_free_blocks_flags - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int get_default_free_blocks_flags(struct inode *inode)
{
	if (S_ISDIR(inode->i_mode) || S_ISLNK(inode->i_mode) ||
	    ext4_test_inode_flag(inode, EXT4_INODE_EA_INODE))
		return EXT4_FREE_BLOCKS_METADATA | EXT4_FREE_BLOCKS_FORGET;
	else if (ext4_should_journal_data(inode))
		return EXT4_FREE_BLOCKS_FORGET;
	return 0;
}


/**
 * ext4_rereserve_cluster - Implements the rereserve cluster operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_rereserve_cluster(struct inode *inode, ext4_lblk_t lblk)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_inode_info *ei = EXT4_I(inode);

	dquot_reclaim_block(inode, EXT4_C2B(sbi, 1));

	spin_lock(&ei->i_block_reservation_lock);
	ei->i_reserved_data_blocks++;
	percpu_counter_add(&sbi->s_dirtyclusters_counter, 1);
	spin_unlock(&ei->i_block_reservation_lock);

	percpu_counter_add(&sbi->s_freeclusters_counter, 1);
	ext4_remove_pending(inode, lblk);
}

/**
 * ext4_remove_blocks - Implements the remove blocks operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_remove_blocks(handle_t *handle, struct inode *inode,
			      struct ext4_extent *ex,
			      struct partial_cluster *partial,
			      ext4_lblk_t from, ext4_lblk_t to)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	unsigned short ee_len = ext4_ext_get_actual_len(ex);
	ext4_fsblk_t last_pblk, pblk;
	ext4_lblk_t num;
	int flags;


	if (from < le32_to_cpu(ex->ee_block) ||
	    to != le32_to_cpu(ex->ee_block) + ee_len - 1) {
		ext4_error(sbi->s_sb,
			   "strange request: removal(2) %u-%u from %u:%u",
			   from, to, le32_to_cpu(ex->ee_block), ee_len);
		return 0;
	}

#ifdef EXTENTS_STATS
	spin_lock(&sbi->s_ext_stats_lock);
	sbi->s_ext_blocks += ee_len;
	sbi->s_ext_extents++;
	if (ee_len < sbi->s_ext_min)
		sbi->s_ext_min = ee_len;
	if (ee_len > sbi->s_ext_max)
		sbi->s_ext_max = ee_len;
	if (ext_depth(inode) > sbi->s_depth_max)
		sbi->s_depth_max = ext_depth(inode);
	spin_unlock(&sbi->s_ext_stats_lock);
#endif

	trace_ext4_remove_blocks(inode, ex, from, to, partial);


	last_pblk = ext4_ext_pblock(ex) + ee_len - 1;

	if (partial->state != initial &&
	    partial->pclu != EXT4_B2C(sbi, last_pblk)) {
		if (partial->state == tofree) {
			flags = get_default_free_blocks_flags(inode);
			if (ext4_is_pending(inode, partial->lblk))
				flags |= EXT4_FREE_BLOCKS_RERESERVE_CLUSTER;
			ext4_free_blocks(handle, inode, NULL,
					 EXT4_C2B(sbi, partial->pclu),
					 sbi->s_cluster_ratio, flags);
			if (flags & EXT4_FREE_BLOCKS_RERESERVE_CLUSTER)
				ext4_rereserve_cluster(inode, partial->lblk);
		}
		partial->state = initial;
	}

	num = le32_to_cpu(ex->ee_block) + ee_len - from;
	pblk = ext4_ext_pblock(ex) + ee_len - num;


	flags = get_default_free_blocks_flags(inode);


	if ((EXT4_LBLK_COFF(sbi, to) != sbi->s_cluster_ratio - 1) &&
	    (EXT4_LBLK_CMASK(sbi, to) >= from) &&
	    (partial->state != nofree)) {
		if (ext4_is_pending(inode, to))
			flags |= EXT4_FREE_BLOCKS_RERESERVE_CLUSTER;
		ext4_free_blocks(handle, inode, NULL,
				 EXT4_PBLK_CMASK(sbi, last_pblk),
				 sbi->s_cluster_ratio, flags);
		if (flags & EXT4_FREE_BLOCKS_RERESERVE_CLUSTER)
			ext4_rereserve_cluster(inode, to);
		partial->state = initial;
		flags = get_default_free_blocks_flags(inode);
	}

	flags |= EXT4_FREE_BLOCKS_NOFREE_LAST_CLUSTER;


	flags |= EXT4_FREE_BLOCKS_NOFREE_FIRST_CLUSTER;
	ext4_free_blocks(handle, inode, NULL, pblk, num, flags);


	if (partial->state != initial && partial->pclu != EXT4_B2C(sbi, pblk))
		partial->state = initial;


	if (EXT4_LBLK_COFF(sbi, from) && num == ee_len) {
		if (partial->state == initial) {
			partial->pclu = EXT4_B2C(sbi, pblk);
			partial->lblk = from;
			partial->state = tofree;
		}
	} else {
		partial->state = initial;
	}

	return 0;
}


/**
 * ext4_ext_rm_leaf - Implements the ext rm leaf operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_ext_rm_leaf(handle_t *handle, struct inode *inode,
		 struct ext4_ext_path *path,
		 struct partial_cluster *partial,
		 ext4_lblk_t start, ext4_lblk_t end)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	int err = 0, correct_index = 0;
	int depth = ext_depth(inode), credits, revoke_credits;
	struct ext4_extent_header *eh;
	ext4_lblk_t a, b;
	unsigned num;
	ext4_lblk_t ex_ee_block;
	unsigned short ex_ee_len;
	unsigned unwritten = 0;
	struct ext4_extent *ex;
	ext4_fsblk_t pblk;


	ext_debug(inode, "truncate since %u in leaf to %u\n", start, end);
	if (!path[depth].p_hdr)
		path[depth].p_hdr = ext_block_hdr(path[depth].p_bh);
	eh = path[depth].p_hdr;
	if (unlikely(path[depth].p_hdr == NULL)) {
		EXT4_ERROR_INODE(inode, "path[%d].p_hdr == NULL", depth);
		return -EFSCORRUPTED;
	}

	ex = path[depth].p_ext;
	if (!ex)
		ex = EXT_LAST_EXTENT(eh);

	ex_ee_block = le32_to_cpu(ex->ee_block);
	ex_ee_len = ext4_ext_get_actual_len(ex);

	trace_ext4_ext_rm_leaf(inode, start, ex, partial);

	while (ex >= EXT_FIRST_EXTENT(eh) &&
			ex_ee_block + ex_ee_len > start) {

		if (ext4_ext_is_unwritten(ex))
			unwritten = 1;
		else
			unwritten = 0;

		ext_debug(inode, "remove ext %u:[%d]%d\n", ex_ee_block,
			  unwritten, ex_ee_len);
		path[depth].p_ext = ex;

		a = max(ex_ee_block, start);
		b = min(ex_ee_block + ex_ee_len - 1, end);

		ext_debug(inode, "  border %u:%u\n", a, b);


		if (end < ex_ee_block) {


			if (sbi->s_cluster_ratio > 1) {
				pblk = ext4_ext_pblock(ex);
				partial->pclu = EXT4_B2C(sbi, pblk);
				partial->state = nofree;
			}
			ex--;
			ex_ee_block = le32_to_cpu(ex->ee_block);
			ex_ee_len = ext4_ext_get_actual_len(ex);
			continue;
		} else if (b != ex_ee_block + ex_ee_len - 1) {
			EXT4_ERROR_INODE(inode,
					 "can not handle truncate %u:%u "
					 "on extent %u:%u",
					 start, end, ex_ee_block,
					 ex_ee_block + ex_ee_len - 1);
			err = -EFSCORRUPTED;
			goto out;
		} else if (a != ex_ee_block) {

			num = a - ex_ee_block;
		} else {

			num = 0;
		}


		credits = 7 + 2*(ex_ee_len/EXT4_BLOCKS_PER_GROUP(inode->i_sb));
		if (ex == EXT_FIRST_EXTENT(eh)) {
			correct_index = 1;
			credits += (ext_depth(inode)) + 1;
		}
		credits += EXT4_MAXQUOTAS_TRANS_BLOCKS(inode->i_sb);


		revoke_credits =
			ext4_free_metadata_revoke_credits(inode->i_sb,
							  ext_depth(inode)) +
			ext4_free_data_revoke_credits(inode, b - a + 1);

		err = ext4_datasem_ensure_credits(handle, inode, credits,
						  credits, revoke_credits);
		if (err) {
			if (err > 0)
				err = -EAGAIN;
			goto out;
		}

		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto out;

		err = ext4_remove_blocks(handle, inode, ex, partial, a, b);
		if (err)
			goto out;

		if (num == 0)

			ext4_ext_store_pblock(ex, 0);

		ex->ee_len = cpu_to_le16(num);


		if (unwritten && num)
			ext4_ext_mark_unwritten(ex);


		if (num == 0) {
			if (end != EXT_MAX_BLOCKS - 1) {


				memmove(ex, ex+1, (EXT_LAST_EXTENT(eh) - ex) *
					sizeof(struct ext4_extent));


				memset(EXT_LAST_EXTENT(eh), 0,
					sizeof(struct ext4_extent));
			}
			le16_add_cpu(&eh->eh_entries, -1);
		}

		err = ext4_ext_dirty(handle, inode, path + depth);
		if (err)
			goto out;

		ext_debug(inode, "new extent: %u:%u:%llu\n", ex_ee_block, num,
				ext4_ext_pblock(ex));
		ex--;
		ex_ee_block = le32_to_cpu(ex->ee_block);
		ex_ee_len = ext4_ext_get_actual_len(ex);
	}

	if (correct_index && eh->eh_entries)
		err = ext4_ext_correct_indexes(handle, inode, path);


	if (partial->state == tofree && ex >= EXT_FIRST_EXTENT(eh)) {
		pblk = ext4_ext_pblock(ex) + ex_ee_len - 1;
		if (partial->pclu != EXT4_B2C(sbi, pblk)) {
			int flags = get_default_free_blocks_flags(inode);

			if (ext4_is_pending(inode, partial->lblk))
				flags |= EXT4_FREE_BLOCKS_RERESERVE_CLUSTER;
			ext4_free_blocks(handle, inode, NULL,
					 EXT4_C2B(sbi, partial->pclu),
					 sbi->s_cluster_ratio, flags);
			if (flags & EXT4_FREE_BLOCKS_RERESERVE_CLUSTER)
				ext4_rereserve_cluster(inode, partial->lblk);
		}
		partial->state = initial;
	}


	if (err == 0 && eh->eh_entries == 0 && path[depth].p_bh != NULL)
		err = ext4_ext_rm_idx(handle, inode, path, depth);

out:
	return err;
}


/**
 * ext4_ext_more_to_rm - Implements the ext more to rm operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_ext_more_to_rm(struct ext4_ext_path *path)
{
	BUG_ON(path->p_idx == NULL);

	if (path->p_idx < EXT_FIRST_INDEX(path->p_hdr))
		return 0;


	if (le16_to_cpu(path->p_hdr->eh_entries) == path->p_block)
		return 0;
	return 1;
}

/**
 * ext4_ext_remove_space - Implements the ext remove space operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_ext_remove_space(struct inode *inode, ext4_lblk_t start,
			  ext4_lblk_t end)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	int depth = ext_depth(inode);
	struct ext4_ext_path *path = NULL;
	struct partial_cluster partial;
	handle_t *handle;
	int i = 0, err = 0;

	partial.pclu = 0;
	partial.lblk = 0;
	partial.state = initial;

	ext_debug(inode, "truncate since %u to %u\n", start, end);


	handle = ext4_journal_start_with_revoke(inode, EXT4_HT_TRUNCATE,
			depth + 1,
			ext4_free_metadata_revoke_credits(inode->i_sb, depth));
	if (IS_ERR(handle))
		return PTR_ERR(handle);

again:
	trace_ext4_ext_remove_space(inode, start, end, depth);


	if (end < EXT_MAX_BLOCKS - 1) {
		struct ext4_extent *ex;
		ext4_lblk_t ee_block, ex_end, lblk;
		ext4_fsblk_t pblk;


		path = ext4_find_extent(inode, end, NULL,
					EXT4_EX_NOCACHE | EXT4_EX_NOFAIL);
		if (IS_ERR(path)) {
			ext4_journal_stop(handle);
			return PTR_ERR(path);
		}
		depth = ext_depth(inode);

		ex = path[depth].p_ext;
		if (!ex) {
			if (depth) {
				EXT4_ERROR_INODE(inode,
						 "path[%d].p_hdr == NULL",
						 depth);
				err = -EFSCORRUPTED;
			}
			goto out;
		}

		ee_block = le32_to_cpu(ex->ee_block);
		ex_end = ee_block + ext4_ext_get_actual_len(ex) - 1;


		if (end >= ee_block && end < ex_end) {


			if (sbi->s_cluster_ratio > 1) {
				pblk = ext4_ext_pblock(ex) + end - ee_block + 1;
				partial.pclu = EXT4_B2C(sbi, pblk);
				partial.state = nofree;
			}


			path = ext4_force_split_extent_at(handle, inode, path,
							  end + 1, 1);
			if (IS_ERR(path)) {
				err = PTR_ERR(path);
				goto out;
			}
		} else if (sbi->s_cluster_ratio > 1 && end >= ex_end &&
			   partial.state == initial) {


			lblk = ex_end + 1;
			err = ext4_ext_search_right(inode, path, &lblk, &pblk,
						    NULL);
			if (err < 0)
				goto out;
			if (pblk) {
				partial.pclu = EXT4_B2C(sbi, pblk);
				partial.state = nofree;
			}
		}
	}


	depth = ext_depth(inode);
	if (path) {
		int k = i = depth;
		while (--k > 0)
			path[k].p_block =
				le16_to_cpu(path[k].p_hdr->eh_entries)+1;
	} else {
		path = kcalloc(depth + 1, sizeof(struct ext4_ext_path),
			       GFP_NOFS | __GFP_NOFAIL);
		if (path == NULL) {
			ext4_journal_stop(handle);
			return -ENOMEM;
		}
		path[0].p_maxdepth = path[0].p_depth = depth;
		path[0].p_hdr = ext_inode_hdr(inode);
		i = 0;

		if (ext4_ext_check(inode, path[0].p_hdr, depth, 0)) {
			err = -EFSCORRUPTED;
			goto out;
		}
	}
	err = 0;

	while (i >= 0 && err == 0) {
		if (i == depth) {

			err = ext4_ext_rm_leaf(handle, inode, path,
					       &partial, start, end);

			ext4_ext_path_brelse(path + i);
			i--;
			continue;
		}


		if (!path[i].p_hdr) {
			ext_debug(inode, "initialize header\n");
			path[i].p_hdr = ext_block_hdr(path[i].p_bh);
		}

		if (!path[i].p_idx) {

			path[i].p_idx = EXT_LAST_INDEX(path[i].p_hdr);
			path[i].p_block = le16_to_cpu(path[i].p_hdr->eh_entries)+1;
			ext_debug(inode, "init index ptr: hdr 0x%p, num %d\n",
				  path[i].p_hdr,
				  le16_to_cpu(path[i].p_hdr->eh_entries));
		} else {

			path[i].p_idx--;
		}

		ext_debug(inode, "level %d - index, first 0x%p, cur 0x%p\n",
				i, EXT_FIRST_INDEX(path[i].p_hdr),
				path[i].p_idx);
		if (ext4_ext_more_to_rm(path + i)) {
			struct buffer_head *bh;

			ext_debug(inode, "move to level %d (block %llu)\n",
				  i + 1, ext4_idx_pblock(path[i].p_idx));
			memset(path + i + 1, 0, sizeof(*path));
			bh = read_extent_tree_block(inode, path[i].p_idx,
						    depth - i - 1,
						    EXT4_EX_NOCACHE);
			if (IS_ERR(bh)) {

				err = PTR_ERR(bh);
				break;
			}


			cond_resched();
			if (WARN_ON(i + 1 > depth)) {
				err = -EFSCORRUPTED;
				break;
			}
			path[i + 1].p_bh = bh;


			path[i].p_block = le16_to_cpu(path[i].p_hdr->eh_entries);
			i++;
		} else {

			if (path[i].p_hdr->eh_entries == 0 && i > 0) {


				err = ext4_ext_rm_idx(handle, inode, path, i);
			}

			ext4_ext_path_brelse(path + i);
			i--;
			ext_debug(inode, "return to level %d\n", i);
		}
	}

	trace_ext4_ext_remove_space_done(inode, start, end, depth, &partial,
					 path->p_hdr->eh_entries);


	if (partial.state == tofree && err == 0) {
		int flags = get_default_free_blocks_flags(inode);

		if (ext4_is_pending(inode, partial.lblk))
			flags |= EXT4_FREE_BLOCKS_RERESERVE_CLUSTER;
		ext4_free_blocks(handle, inode, NULL,
				 EXT4_C2B(sbi, partial.pclu),
				 sbi->s_cluster_ratio, flags);
		if (flags & EXT4_FREE_BLOCKS_RERESERVE_CLUSTER)
			ext4_rereserve_cluster(inode, partial.lblk);
		partial.state = initial;
	}


	if (path->p_hdr->eh_entries == 0) {


		err = ext4_ext_get_access(handle, inode, path);
		if (err == 0) {
			ext_inode_hdr(inode)->eh_depth = 0;
			ext_inode_hdr(inode)->eh_max =
				cpu_to_le16(ext4_ext_space_root(inode, 0));
			err = ext4_ext_dirty(handle, inode, path);
		}
	}
out:
	ext4_free_ext_path(path);
	path = NULL;
	if (err == -EAGAIN)
		goto again;
	ext4_journal_stop(handle);

	return err;
}


/**
 * ext4_ext_init - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_ext_init(struct super_block *sb)
{


	if (ext4_has_feature_extents(sb)) {
#if defined(AGGRESSIVE_TEST) || defined(CHECK_BINSEARCH) || defined(EXTENTS_STATS)
		printk(KERN_INFO "EXT4-fs: file extents enabled"
#ifdef AGGRESSIVE_TEST
		       ", aggressive tests"
#endif
#ifdef CHECK_BINSEARCH
		       ", check binsearch"
#endif
#ifdef EXTENTS_STATS
		       ", stats"
#endif
		       "\n");
#endif
#ifdef EXTENTS_STATS
		spin_lock_init(&EXT4_SB(sb)->s_ext_stats_lock);
		EXT4_SB(sb)->s_ext_min = 1 << 30;
		EXT4_SB(sb)->s_ext_max = 0;
#endif
	}
}


/**
 * ext4_ext_release - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_ext_release(struct super_block *sb)
{
	if (!ext4_has_feature_extents(sb))
		return;

#ifdef EXTENTS_STATS
	if (EXT4_SB(sb)->s_ext_blocks && EXT4_SB(sb)->s_ext_extents) {
		struct ext4_sb_info *sbi = EXT4_SB(sb);
		printk(KERN_ERR "EXT4-fs: %lu blocks in %lu extents (%lu ave)\n",
			sbi->s_ext_blocks, sbi->s_ext_extents,
			sbi->s_ext_blocks / sbi->s_ext_extents);
		printk(KERN_ERR "EXT4-fs: extents: %lu min, %lu max, max depth %lu\n",
			sbi->s_ext_min, sbi->s_ext_max, sbi->s_depth_max);
	}
#endif
}

/**
 * ext4_zeroout_es - Implements the zeroout es operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_zeroout_es(struct inode *inode, struct ext4_extent *ex)
{
	ext4_lblk_t  ee_block;
	ext4_fsblk_t ee_pblock;
	unsigned int ee_len;

	ee_block  = le32_to_cpu(ex->ee_block);
	ee_len    = ext4_ext_get_actual_len(ex);
	ee_pblock = ext4_ext_pblock(ex);

	if (ee_len == 0)
		return;

	ext4_es_insert_extent(inode, ee_block, ee_len, ee_pblock,
			      EXTENT_STATUS_WRITTEN, 0);
}


/**
 * ext4_ext_zeroout - Implements the ext zeroout operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_ext_zeroout(struct inode *inode, struct ext4_extent *ex)
{
	ext4_fsblk_t ee_pblock;
	unsigned int ee_len;

	ee_len    = ext4_ext_get_actual_len(ex);
	ee_pblock = ext4_ext_pblock(ex);
	return ext4_issue_zeroout(inode, le32_to_cpu(ex->ee_block), ee_pblock,
				  ee_len);
}


/**
 * ext4_split_extent_at - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct ext4_ext_path *ext4_split_extent_at(handle_t *handle,
						  struct inode *inode,
						  struct ext4_ext_path *path,
						  ext4_lblk_t split,
						  int split_flag, int flags)
{
	ext4_fsblk_t newblock;
	ext4_lblk_t ee_block;
	struct ext4_extent *ex, newex, orig_ex, zero_ex;
	struct ext4_extent *ex2 = NULL;
	unsigned int ee_len, depth;
	int err = 0;

	BUG_ON((split_flag & EXT4_EXT_DATA_VALID1) == EXT4_EXT_DATA_VALID1);
	BUG_ON((split_flag & EXT4_EXT_DATA_VALID1) &&
	       (split_flag & EXT4_EXT_DATA_VALID2));


	flags |= EXT4_EX_NOCACHE;

	ext_debug(inode, "logical block %llu\n", (unsigned long long)split);

	ext4_ext_show_leaf(inode, path);

	depth = ext_depth(inode);
	ex = path[depth].p_ext;
	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);
	newblock = split - ee_block + ext4_ext_pblock(ex);

	BUG_ON(split < ee_block || split >= (ee_block + ee_len));
	BUG_ON(!ext4_ext_is_unwritten(ex) &&
	       split_flag & (EXT4_EXT_MAY_ZEROOUT |
			     EXT4_EXT_MARK_UNWRIT1 |
			     EXT4_EXT_MARK_UNWRIT2));

	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto out;

	if (split == ee_block) {


		if (split_flag & EXT4_EXT_MARK_UNWRIT2)
			ext4_ext_mark_unwritten(ex);
		else
			ext4_ext_mark_initialized(ex);

		if (!(flags & EXT4_GET_BLOCKS_PRE_IO))
			ext4_ext_try_to_merge(handle, inode, path, ex);

		err = ext4_ext_dirty(handle, inode, path + path->p_depth);
		goto out;
	}


	memcpy(&orig_ex, ex, sizeof(orig_ex));
	ex->ee_len = cpu_to_le16(split - ee_block);
	if (split_flag & EXT4_EXT_MARK_UNWRIT1)
		ext4_ext_mark_unwritten(ex);


	err = ext4_ext_dirty(handle, inode, path + depth);
	if (err)
		goto fix_extent_len;

	ex2 = &newex;
	ex2->ee_block = cpu_to_le32(split);
	ex2->ee_len   = cpu_to_le16(ee_len - (split - ee_block));
	ext4_ext_store_pblock(ex2, newblock);
	if (split_flag & EXT4_EXT_MARK_UNWRIT2)
		ext4_ext_mark_unwritten(ex2);

	path = ext4_ext_insert_extent(handle, inode, path, &newex, flags);
	if (!IS_ERR(path))
		goto out;

	err = PTR_ERR(path);
	if (err != -ENOSPC && err != -EDQUOT && err != -ENOMEM)
		goto out_path;


	path = ext4_find_extent(inode, ee_block, NULL, flags | EXT4_EX_NOFAIL);
	if (IS_ERR(path)) {
		EXT4_ERROR_INODE(inode, "Failed split extent on %u, err %ld",
				 split, PTR_ERR(path));
		goto out_path;
	}
	depth = ext_depth(inode);
	ex = path[depth].p_ext;

	if (EXT4_EXT_MAY_ZEROOUT & split_flag) {
		if (split_flag & (EXT4_EXT_DATA_VALID1|EXT4_EXT_DATA_VALID2)) {
			if (split_flag & EXT4_EXT_DATA_VALID1) {
				err = ext4_ext_zeroout(inode, ex2);
				zero_ex.ee_block = ex2->ee_block;
				zero_ex.ee_len = cpu_to_le16(
						ext4_ext_get_actual_len(ex2));
				ext4_ext_store_pblock(&zero_ex,
						      ext4_ext_pblock(ex2));
			} else {
				err = ext4_ext_zeroout(inode, ex);
				zero_ex.ee_block = ex->ee_block;
				zero_ex.ee_len = cpu_to_le16(
						ext4_ext_get_actual_len(ex));
				ext4_ext_store_pblock(&zero_ex,
						      ext4_ext_pblock(ex));
			}
		} else {
			err = ext4_ext_zeroout(inode, &orig_ex);
			zero_ex.ee_block = orig_ex.ee_block;
			zero_ex.ee_len = cpu_to_le16(
						ext4_ext_get_actual_len(&orig_ex));
			ext4_ext_store_pblock(&zero_ex,
					      ext4_ext_pblock(&orig_ex));
		}

		if (!err) {


			if (split_flag & EXT4_EXT_DATA_PARTIAL_VALID1) {


				ext4_es_remove_extent(inode,
					le32_to_cpu(zero_ex.ee_block),
					ext4_ext_get_actual_len(&zero_ex));
				goto fix_extent_len;
			}


			ex->ee_len = cpu_to_le16(ee_len);
			ext4_ext_try_to_merge(handle, inode, path, ex);
			err = ext4_ext_dirty(handle, inode, path + path->p_depth);
			if (!err)

				ext4_zeroout_es(inode, &zero_ex);


			goto out;
		}
	}

fix_extent_len:
	ex->ee_len = orig_ex.ee_len;


	ext4_ext_dirty(handle, inode, path + path->p_depth);
out:
	if (err) {
		ext4_free_ext_path(path);
		path = ERR_PTR(err);
	}
out_path:
	if (IS_ERR(path))

		ext4_es_remove_extent(inode, ee_block, ee_len);
	ext4_ext_show_leaf(inode, path);
	return path;
}


/**
 * ext4_split_extent - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct ext4_ext_path *ext4_split_extent(handle_t *handle,
					       struct inode *inode,
					       struct ext4_ext_path *path,
					       struct ext4_map_blocks *map,
					       int split_flag, int flags,
					       unsigned int *allocated)
{
	ext4_lblk_t ee_block;
	struct ext4_extent *ex;
	unsigned int ee_len, depth;
	int unwritten;
	int split_flag1, flags1;

	depth = ext_depth(inode);
	ex = path[depth].p_ext;
	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);
	unwritten = ext4_ext_is_unwritten(ex);


	flags |= EXT4_EX_NOCACHE;

	if (map->m_lblk + map->m_len < ee_block + ee_len) {
		split_flag1 = split_flag & EXT4_EXT_MAY_ZEROOUT;
		flags1 = flags | EXT4_GET_BLOCKS_PRE_IO;
		if (unwritten)
			split_flag1 |= EXT4_EXT_MARK_UNWRIT1 |
				       EXT4_EXT_MARK_UNWRIT2;
		if (split_flag & EXT4_EXT_DATA_VALID2)
			split_flag1 |= map->m_lblk > ee_block ?
				       EXT4_EXT_DATA_PARTIAL_VALID1 :
				       EXT4_EXT_DATA_ENTIRE_VALID1;
		path = ext4_split_extent_at(handle, inode, path,
				map->m_lblk + map->m_len, split_flag1, flags1);
		if (IS_ERR(path))
			return path;


		path = ext4_find_extent(inode, map->m_lblk, path, flags);
		if (IS_ERR(path))
			return path;
		depth = ext_depth(inode);
		ex = path[depth].p_ext;
		if (!ex) {
			EXT4_ERROR_INODE(inode, "unexpected hole at %lu",
					(unsigned long) map->m_lblk);
			ext4_free_ext_path(path);
			return ERR_PTR(-EFSCORRUPTED);
		}
		unwritten = ext4_ext_is_unwritten(ex);
	}

	if (map->m_lblk >= ee_block) {
		split_flag1 = split_flag & EXT4_EXT_DATA_VALID2;
		if (unwritten) {
			split_flag1 |= EXT4_EXT_MARK_UNWRIT1;
			split_flag1 |= split_flag & (EXT4_EXT_MAY_ZEROOUT |
						     EXT4_EXT_MARK_UNWRIT2);
		}
		path = ext4_split_extent_at(handle, inode, path,
				map->m_lblk, split_flag1, flags);
		if (IS_ERR(path))
			return path;
	}

	if (allocated) {
		if (map->m_lblk + map->m_len > ee_block + ee_len)
			*allocated = ee_len - (map->m_lblk - ee_block);
		else
			*allocated = map->m_len;
	}
	ext4_ext_show_leaf(inode, path);
	return path;
}


/**
 * ext4_ext_convert_to_initialized - Implements the ext convert to initialized operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct ext4_ext_path *
ext4_ext_convert_to_initialized(handle_t *handle, struct inode *inode,
			struct ext4_map_blocks *map, struct ext4_ext_path *path,
			int flags, unsigned int *allocated)
{
	struct ext4_sb_info *sbi;
	struct ext4_extent_header *eh;
	struct ext4_map_blocks split_map;
	struct ext4_extent zero_ex1, zero_ex2;
	struct ext4_extent *ex, *abut_ex;
	ext4_lblk_t ee_block, eof_block;
	unsigned int ee_len, depth, map_len = map->m_len;
	int err = 0;
	int split_flag = EXT4_EXT_DATA_VALID2;
	unsigned int max_zeroout = 0;

	ext_debug(inode, "logical block %llu, max_blocks %u\n",
		  (unsigned long long)map->m_lblk, map_len);

	sbi = EXT4_SB(inode->i_sb);
	eof_block = (EXT4_I(inode)->i_disksize + inode->i_sb->s_blocksize - 1)
			>> inode->i_sb->s_blocksize_bits;
	if (eof_block < map->m_lblk + map_len)
		eof_block = map->m_lblk + map_len;

	depth = ext_depth(inode);
	eh = path[depth].p_hdr;
	ex = path[depth].p_ext;
	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);
	zero_ex1.ee_len = 0;
	zero_ex2.ee_len = 0;

	trace_ext4_ext_convert_to_initialized_enter(inode, map, ex);


	BUG_ON(!ext4_ext_is_unwritten(ex));
	BUG_ON(!in_range(map->m_lblk, ee_block, ee_len));


	*allocated = 0;
	if ((map->m_lblk == ee_block) &&

		(map_len < ee_len) &&
		(ex > EXT_FIRST_EXTENT(eh))) {
		ext4_lblk_t prev_lblk;
		ext4_fsblk_t prev_pblk, ee_pblk;
		unsigned int prev_len;

		abut_ex = ex - 1;
		prev_lblk = le32_to_cpu(abut_ex->ee_block);
		prev_len = ext4_ext_get_actual_len(abut_ex);
		prev_pblk = ext4_ext_pblock(abut_ex);
		ee_pblk = ext4_ext_pblock(ex);


		if ((!ext4_ext_is_unwritten(abut_ex)) &&
			((prev_lblk + prev_len) == ee_block) &&
			((prev_pblk + prev_len) == ee_pblk) &&
			(prev_len < (EXT_INIT_MAX_LEN - map_len))) {
			err = ext4_ext_get_access(handle, inode, path + depth);
			if (err)
				goto errout;

			trace_ext4_ext_convert_to_initialized_fastpath(inode,
				map, ex, abut_ex);


			ex->ee_block = cpu_to_le32(ee_block + map_len);
			ext4_ext_store_pblock(ex, ee_pblk + map_len);
			ex->ee_len = cpu_to_le16(ee_len - map_len);
			ext4_ext_mark_unwritten(ex);


			abut_ex->ee_len = cpu_to_le16(prev_len + map_len);


			*allocated = map_len;
		}
	} else if (((map->m_lblk + map_len) == (ee_block + ee_len)) &&
		   (map_len < ee_len) &&
		   ex < EXT_LAST_EXTENT(eh)) {

		ext4_lblk_t next_lblk;
		ext4_fsblk_t next_pblk, ee_pblk;
		unsigned int next_len;

		abut_ex = ex + 1;
		next_lblk = le32_to_cpu(abut_ex->ee_block);
		next_len = ext4_ext_get_actual_len(abut_ex);
		next_pblk = ext4_ext_pblock(abut_ex);
		ee_pblk = ext4_ext_pblock(ex);


		if ((!ext4_ext_is_unwritten(abut_ex)) &&
		    ((map->m_lblk + map_len) == next_lblk) &&
		    ((ee_pblk + ee_len) == next_pblk) &&
		    (next_len < (EXT_INIT_MAX_LEN - map_len))) {
			err = ext4_ext_get_access(handle, inode, path + depth);
			if (err)
				goto errout;

			trace_ext4_ext_convert_to_initialized_fastpath(inode,
				map, ex, abut_ex);


			abut_ex->ee_block = cpu_to_le32(next_lblk - map_len);
			ext4_ext_store_pblock(abut_ex, next_pblk - map_len);
			ex->ee_len = cpu_to_le16(ee_len - map_len);
			ext4_ext_mark_unwritten(ex);


			abut_ex->ee_len = cpu_to_le16(next_len + map_len);


			*allocated = map_len;
		}
	}
	if (*allocated) {

		err = ext4_ext_dirty(handle, inode, path + depth);


		path[depth].p_ext = abut_ex;
		if (err)
			goto errout;
		goto out;
	} else
		*allocated = ee_len - (map->m_lblk - ee_block);

	WARN_ON(map->m_lblk < ee_block);


	split_flag |= ee_block + ee_len <= eof_block ? EXT4_EXT_MAY_ZEROOUT : 0;

	if (EXT4_EXT_MAY_ZEROOUT & split_flag)
		max_zeroout = sbi->s_extent_max_zeroout_kb >>
			(inode->i_sb->s_blocksize_bits - 10);


	split_map.m_lblk = map->m_lblk;
	split_map.m_len = map->m_len;

	if (max_zeroout && (*allocated > split_map.m_len)) {
		if (*allocated <= max_zeroout) {

			zero_ex1.ee_block =
				 cpu_to_le32(split_map.m_lblk +
					     split_map.m_len);
			zero_ex1.ee_len =
				cpu_to_le16(*allocated - split_map.m_len);
			ext4_ext_store_pblock(&zero_ex1,
				ext4_ext_pblock(ex) + split_map.m_lblk +
				split_map.m_len - ee_block);
			err = ext4_ext_zeroout(inode, &zero_ex1);
			if (err)
				goto fallback;
			split_map.m_len = *allocated;
		}
		if (split_map.m_lblk - ee_block + split_map.m_len <
								max_zeroout) {

			if (split_map.m_lblk != ee_block) {
				zero_ex2.ee_block = ex->ee_block;
				zero_ex2.ee_len = cpu_to_le16(split_map.m_lblk -
							ee_block);
				ext4_ext_store_pblock(&zero_ex2,
						      ext4_ext_pblock(ex));
				err = ext4_ext_zeroout(inode, &zero_ex2);
				if (err)
					goto fallback;
			}

			split_map.m_len += split_map.m_lblk - ee_block;
			split_map.m_lblk = ee_block;
			*allocated = map->m_len;
		}
	}

fallback:
	path = ext4_split_extent(handle, inode, path, &split_map, split_flag,
				 flags, NULL);
	if (IS_ERR(path))
		return path;
out:

	ext4_zeroout_es(inode, &zero_ex1);
	ext4_zeroout_es(inode, &zero_ex2);
	return path;

errout:
	ext4_free_ext_path(path);
	return ERR_PTR(err);
}


/**
 * ext4_split_convert_extents - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct ext4_ext_path *ext4_split_convert_extents(handle_t *handle,
					struct inode *inode,
					struct ext4_map_blocks *map,
					struct ext4_ext_path *path,
					int flags, unsigned int *allocated)
{
	ext4_lblk_t eof_block;
	ext4_lblk_t ee_block;
	struct ext4_extent *ex;
	unsigned int ee_len;
	int split_flag = 0, depth;

	ext_debug(inode, "logical block %llu, max_blocks %u\n",
		  (unsigned long long)map->m_lblk, map->m_len);

	eof_block = (EXT4_I(inode)->i_disksize + inode->i_sb->s_blocksize - 1)
			>> inode->i_sb->s_blocksize_bits;
	if (eof_block < map->m_lblk + map->m_len)
		eof_block = map->m_lblk + map->m_len;
	depth = ext_depth(inode);
	ex = path[depth].p_ext;
	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);


	if (flags & EXT4_GET_BLOCKS_CONVERT_UNWRITTEN) {
		split_flag |= EXT4_EXT_DATA_ENTIRE_VALID1;

	} else if (flags & (EXT4_GET_BLOCKS_UNWRIT_EXT |
			    EXT4_GET_BLOCKS_CONVERT)) {


		split_flag |= ee_block + ee_len <= eof_block ?
			      EXT4_EXT_MAY_ZEROOUT : 0;
		split_flag |= EXT4_EXT_MARK_UNWRIT2;

		if (flags & EXT4_GET_BLOCKS_CONVERT)
			split_flag |= EXT4_EXT_DATA_VALID2;
	}
	flags |= EXT4_GET_BLOCKS_PRE_IO;
	return ext4_split_extent(handle, inode, path, map, split_flag, flags,
				 allocated);
}

/**
 * ext4_convert_unwritten_extents_endio - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct ext4_ext_path *
ext4_convert_unwritten_extents_endio(handle_t *handle, struct inode *inode,
				     struct ext4_map_blocks *map,
				     struct ext4_ext_path *path)
{
	struct ext4_extent *ex;
	ext4_lblk_t ee_block;
	unsigned int ee_len;
	int depth;
	int err = 0;

	depth = ext_depth(inode);
	ex = path[depth].p_ext;
	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);

	ext_debug(inode, "logical block %llu, max_blocks %u\n",
		  (unsigned long long)ee_block, ee_len);


	if (ee_block != map->m_lblk || ee_len > map->m_len) {
		int flags = EXT4_GET_BLOCKS_CONVERT |
			    EXT4_GET_BLOCKS_METADATA_NOFAIL;
#ifdef CONFIG_EXT4_DEBUG
		ext4_warning(inode->i_sb, "Inode (%ld) finished: extent logical block %llu,"
			     " len %u; IO logical block %llu, len %u",
			     inode->i_ino, (unsigned long long)ee_block, ee_len,
			     (unsigned long long)map->m_lblk, map->m_len);
#endif
		path = ext4_split_convert_extents(handle, inode, map, path,
						  flags, NULL);
		if (IS_ERR(path))
			return path;

		path = ext4_find_extent(inode, map->m_lblk, path, 0);
		if (IS_ERR(path))
			return path;
		depth = ext_depth(inode);
		ex = path[depth].p_ext;
	}

	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto errout;

	ext4_ext_mark_initialized(ex);


	ext4_ext_try_to_merge(handle, inode, path, ex);


	err = ext4_ext_dirty(handle, inode, path + path->p_depth);
	if (err)
		goto errout;

	ext4_ext_show_leaf(inode, path);
	return path;

errout:
	ext4_free_ext_path(path);
	return ERR_PTR(err);
}

/**
 * convert_initialized_extent - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct ext4_ext_path *
convert_initialized_extent(handle_t *handle, struct inode *inode,
			   struct ext4_map_blocks *map,
			   struct ext4_ext_path *path,
			   int flags,
			   unsigned int *allocated)
{
	struct ext4_extent *ex;
	ext4_lblk_t ee_block;
	unsigned int ee_len;
	int depth;
	int err = 0;


	if (map->m_len > EXT_UNWRITTEN_MAX_LEN)
		map->m_len = EXT_UNWRITTEN_MAX_LEN / 2;

	depth = ext_depth(inode);
	ex = path[depth].p_ext;
	ee_block = le32_to_cpu(ex->ee_block);
	ee_len = ext4_ext_get_actual_len(ex);

	ext_debug(inode, "logical block %llu, max_blocks %u\n",
		  (unsigned long long)ee_block, ee_len);

	if (ee_block != map->m_lblk || ee_len > map->m_len) {
		path = ext4_split_convert_extents(handle, inode, map, path,
						  flags, NULL);
		if (IS_ERR(path))
			return path;

		path = ext4_find_extent(inode, map->m_lblk, path, flags);
		if (IS_ERR(path))
			return path;
		depth = ext_depth(inode);
		ex = path[depth].p_ext;
		if (!ex) {
			EXT4_ERROR_INODE(inode, "unexpected hole at %lu",
					 (unsigned long) map->m_lblk);
			err = -EFSCORRUPTED;
			goto errout;
		}
	}

	err = ext4_ext_get_access(handle, inode, path + depth);
	if (err)
		goto errout;

	ext4_ext_mark_unwritten(ex);


	ext4_ext_try_to_merge(handle, inode, path, ex);


	err = ext4_ext_dirty(handle, inode, path + path->p_depth);
	if (err)
		goto errout;
	ext4_ext_show_leaf(inode, path);

	ext4_update_inode_fsync_trans(handle, inode, 1);

	map->m_flags |= EXT4_MAP_UNWRITTEN;
	if (*allocated > map->m_len)
		*allocated = map->m_len;
	map->m_len = *allocated;
	return path;

errout:
	ext4_free_ext_path(path);
	return ERR_PTR(err);
}

/**
 * ext4_ext_handle_unwritten_extents - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct ext4_ext_path *
ext4_ext_handle_unwritten_extents(handle_t *handle, struct inode *inode,
			struct ext4_map_blocks *map,
			struct ext4_ext_path *path, int flags,
			unsigned int *allocated, ext4_fsblk_t newblock)
{
	int err = 0;

	ext_debug(inode, "logical block %llu, max_blocks %u, flags 0x%x, allocated %u\n",
		  (unsigned long long)map->m_lblk, map->m_len, flags,
		  *allocated);
	ext4_ext_show_leaf(inode, path);


	flags |= EXT4_GET_BLOCKS_METADATA_NOFAIL;

	trace_ext4_ext_handle_unwritten_extents(inode, map, flags,
						*allocated, newblock);


	if (flags & EXT4_GET_BLOCKS_PRE_IO) {
		path = ext4_split_convert_extents(handle, inode, map, path,
						  flags, allocated);
		if (IS_ERR(path))
			return path;


		if (unlikely(*allocated == 0)) {
			EXT4_ERROR_INODE(inode,
					 "unexpected allocated == 0, m_len = %u",
					 map->m_len);
			err = -EFSCORRUPTED;
			goto errout;
		}
		map->m_flags |= EXT4_MAP_UNWRITTEN;
		goto out;
	}

	if (flags & EXT4_GET_BLOCKS_CONVERT) {
		path = ext4_convert_unwritten_extents_endio(handle, inode,
							    map, path);
		if (IS_ERR(path))
			return path;
		ext4_update_inode_fsync_trans(handle, inode, 1);
		goto map_out;
	}


	if (flags & EXT4_GET_BLOCKS_UNWRIT_EXT) {
		map->m_flags |= EXT4_MAP_UNWRITTEN;
		goto map_out;
	}


	if ((flags & EXT4_GET_BLOCKS_CREATE) == 0) {


		map->m_flags |= EXT4_MAP_UNWRITTEN;
		goto out1;
	}


	path = ext4_ext_convert_to_initialized(handle, inode, map, path,
					       flags, allocated);
	if (IS_ERR(path))
		return path;
	ext4_update_inode_fsync_trans(handle, inode, 1);


	if (unlikely(*allocated == 0)) {
		EXT4_ERROR_INODE(inode, "unexpected allocated == 0, m_len = %u",
				 map->m_len);
		err = -EFSCORRUPTED;
		goto errout;
	}

out:
	map->m_flags |= EXT4_MAP_NEW;
map_out:
	map->m_flags |= EXT4_MAP_MAPPED;
out1:
	map->m_pblk = newblock;
	if (*allocated > map->m_len)
		*allocated = map->m_len;
	map->m_len = *allocated;
	ext4_ext_show_leaf(inode, path);
	return path;

errout:
	ext4_free_ext_path(path);
	return ERR_PTR(err);
}


/**
 * get_implied_cluster_alloc - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int get_implied_cluster_alloc(struct super_block *sb,
				     struct ext4_map_blocks *map,
				     struct ext4_extent *ex,
				     struct ext4_ext_path *path)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_lblk_t c_offset = EXT4_LBLK_COFF(sbi, map->m_lblk);
	ext4_lblk_t ex_cluster_start, ex_cluster_end;
	ext4_lblk_t rr_cluster_start;
	ext4_lblk_t ee_block = le32_to_cpu(ex->ee_block);
	ext4_fsblk_t ee_start = ext4_ext_pblock(ex);
	unsigned short ee_len = ext4_ext_get_actual_len(ex);


	ex_cluster_start = EXT4_B2C(sbi, ee_block);
	ex_cluster_end = EXT4_B2C(sbi, ee_block + ee_len - 1);


	rr_cluster_start = EXT4_B2C(sbi, map->m_lblk);

	if ((rr_cluster_start == ex_cluster_end) ||
	    (rr_cluster_start == ex_cluster_start)) {
		if (rr_cluster_start == ex_cluster_end)
			ee_start += ee_len - 1;
		map->m_pblk = EXT4_PBLK_CMASK(sbi, ee_start) + c_offset;
		map->m_len = min(map->m_len,
				 (unsigned) sbi->s_cluster_ratio - c_offset);


		if (map->m_lblk < ee_block)
			map->m_len = min(map->m_len, ee_block - map->m_lblk);


		if (map->m_lblk > ee_block) {
			ext4_lblk_t next = ext4_ext_next_allocated_block(path);
			map->m_len = min(map->m_len, next - map->m_lblk);
		}

		trace_ext4_get_implied_cluster_alloc_exit(sb, map, 1);
		return 1;
	}

	trace_ext4_get_implied_cluster_alloc_exit(sb, map, 0);
	return 0;
}


/**
 * ext4_ext_determine_insert_hole - Implements the ext determine insert hole operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ext4_lblk_t ext4_ext_determine_insert_hole(struct inode *inode,
						  struct ext4_ext_path *path,
						  ext4_lblk_t lblk)
{
	ext4_lblk_t hole_start, len;
	struct extent_status es;

	hole_start = lblk;
	len = ext4_ext_find_hole(inode, path, &hole_start);
again:
	ext4_es_find_extent_range(inode, &ext4_es_is_delayed, hole_start,
				  hole_start + len - 1, &es);
	if (!es.es_len)
		goto insert_hole;


	if (lblk >= es.es_lblk + es.es_len) {


		len -= lblk - hole_start;
		hole_start = lblk;
		goto again;
	} else if (in_range(lblk, es.es_lblk, es.es_len)) {


		len = es.es_lblk + es.es_len - lblk;
		return len;
	} else {


		len = min(es.es_lblk - hole_start, len);
	}

insert_hole:

	ext_debug(inode, " -> %u:%u\n", hole_start, len);
	ext4_es_insert_extent(inode, hole_start, len, ~0,
			      EXTENT_STATUS_HOLE, 0);


	if (hole_start != lblk)
		len -= lblk - hole_start;

	return len;
}


/**
 * ext4_ext_map_blocks - Implements the ext map blocks operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_ext_map_blocks(handle_t *handle, struct inode *inode,
			struct ext4_map_blocks *map, int flags)
{
	struct ext4_ext_path *path = NULL;
	struct ext4_extent newex, *ex, ex2;
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	ext4_fsblk_t newblock = 0, pblk;
	int err = 0, depth;
	unsigned int allocated = 0, offset = 0;
	unsigned int allocated_clusters = 0;
	struct ext4_allocation_request ar;
	ext4_lblk_t cluster_offset;

	ext_debug(inode, "blocks %u/%u requested\n", map->m_lblk, map->m_len);
	trace_ext4_ext_map_blocks_enter(inode, map->m_lblk, map->m_len, flags);


	path = ext4_find_extent(inode, map->m_lblk, NULL, 0);
	if (IS_ERR(path)) {
		err = PTR_ERR(path);
		goto out;
	}

	depth = ext_depth(inode);


	if (unlikely(path[depth].p_ext == NULL && depth != 0)) {
		EXT4_ERROR_INODE(inode, "bad extent address "
				 "lblock: %lu, depth: %d pblock %lld",
				 (unsigned long) map->m_lblk, depth,
				 path[depth].p_block);
		err = -EFSCORRUPTED;
		goto out;
	}

	ex = path[depth].p_ext;
	if (ex) {
		ext4_lblk_t ee_block = le32_to_cpu(ex->ee_block);
		ext4_fsblk_t ee_start = ext4_ext_pblock(ex);
		unsigned short ee_len;


		ee_len = ext4_ext_get_actual_len(ex);

		trace_ext4_ext_show_extent(inode, ee_block, ee_start, ee_len);


		if (in_range(map->m_lblk, ee_block, ee_len)) {
			newblock = map->m_lblk - ee_block + ee_start;

			allocated = ee_len - (map->m_lblk - ee_block);
			ext_debug(inode, "%u fit into %u:%d -> %llu\n",
				  map->m_lblk, ee_block, ee_len, newblock);


			if ((!ext4_ext_is_unwritten(ex)) &&
			    (flags & EXT4_GET_BLOCKS_CONVERT_UNWRITTEN)) {
				path = convert_initialized_extent(handle,
					inode, map, path, flags, &allocated);
				if (IS_ERR(path))
					err = PTR_ERR(path);
				goto out;
			} else if (!ext4_ext_is_unwritten(ex)) {
				map->m_flags |= EXT4_MAP_MAPPED;
				map->m_pblk = newblock;
				if (allocated > map->m_len)
					allocated = map->m_len;
				map->m_len = allocated;
				ext4_ext_show_leaf(inode, path);
				goto out;
			}

			path = ext4_ext_handle_unwritten_extents(
				handle, inode, map, path, flags,
				&allocated, newblock);
			if (IS_ERR(path))
				err = PTR_ERR(path);
			goto out;
		}
	}


	if ((flags & EXT4_GET_BLOCKS_CREATE) == 0) {
		ext4_lblk_t len;

		len = ext4_ext_determine_insert_hole(inode, path, map->m_lblk);

		map->m_pblk = 0;
		map->m_len = min_t(unsigned int, map->m_len, len);
		goto out;
	}


	newex.ee_block = cpu_to_le32(map->m_lblk);
	cluster_offset = EXT4_LBLK_COFF(sbi, map->m_lblk);


	if (cluster_offset && ex &&
	    get_implied_cluster_alloc(inode->i_sb, map, ex, path)) {
		ar.len = allocated = map->m_len;
		newblock = map->m_pblk;
		goto got_allocated_blocks;
	}


	ar.lleft = map->m_lblk;
	err = ext4_ext_search_left(inode, path, &ar.lleft, &ar.pleft);
	if (err)
		goto out;
	ar.lright = map->m_lblk;
	err = ext4_ext_search_right(inode, path, &ar.lright, &ar.pright, &ex2);
	if (err < 0)
		goto out;


	if ((sbi->s_cluster_ratio > 1) && err &&
	    get_implied_cluster_alloc(inode->i_sb, map, &ex2, path)) {
		ar.len = allocated = map->m_len;
		newblock = map->m_pblk;
		err = 0;
		goto got_allocated_blocks;
	}


	if (map->m_len > EXT_INIT_MAX_LEN &&
	    !(flags & EXT4_GET_BLOCKS_UNWRIT_EXT))
		map->m_len = EXT_INIT_MAX_LEN;
	else if (map->m_len > EXT_UNWRITTEN_MAX_LEN &&
		 (flags & EXT4_GET_BLOCKS_UNWRIT_EXT))
		map->m_len = EXT_UNWRITTEN_MAX_LEN;


	newex.ee_len = cpu_to_le16(map->m_len);
	err = ext4_ext_check_overlap(sbi, inode, &newex, path);
	if (err)
		allocated = ext4_ext_get_actual_len(&newex);
	else
		allocated = map->m_len;


	ar.inode = inode;
	ar.goal = ext4_ext_find_goal(inode, path, map->m_lblk);
	ar.logical = map->m_lblk;


	offset = EXT4_LBLK_COFF(sbi, map->m_lblk);
	ar.len = EXT4_NUM_B2C(sbi, offset+allocated);
	ar.goal -= offset;
	ar.logical -= offset;
	if (S_ISREG(inode->i_mode))
		ar.flags = EXT4_MB_HINT_DATA;
	else

		ar.flags = 0;
	if (flags & EXT4_GET_BLOCKS_NO_NORMALIZE)
		ar.flags |= EXT4_MB_HINT_NOPREALLOC;
	if (flags & EXT4_GET_BLOCKS_DELALLOC_RESERVE)
		ar.flags |= EXT4_MB_DELALLOC_RESERVED;
	if (flags & EXT4_GET_BLOCKS_METADATA_NOFAIL)
		ar.flags |= EXT4_MB_USE_RESERVED;
	newblock = ext4_mb_new_blocks(handle, &ar, &err);
	if (!newblock)
		goto out;
	allocated_clusters = ar.len;
	ar.len = EXT4_C2B(sbi, ar.len) - offset;
	ext_debug(inode, "allocate new block: goal %llu, found %llu/%u, requested %u\n",
		  ar.goal, newblock, ar.len, allocated);
	if (ar.len > allocated)
		ar.len = allocated;

got_allocated_blocks:

	pblk = newblock + offset;
	ext4_ext_store_pblock(&newex, pblk);
	newex.ee_len = cpu_to_le16(ar.len);

	if (flags & EXT4_GET_BLOCKS_UNWRIT_EXT) {
		ext4_ext_mark_unwritten(&newex);
		map->m_flags |= EXT4_MAP_UNWRITTEN;
	}

	path = ext4_ext_insert_extent(handle, inode, path, &newex, flags);
	if (IS_ERR(path)) {
		err = PTR_ERR(path);


		if (allocated_clusters && (err == -EDQUOT || err == -ENOSPC)) {
			int fb_flags = 0;


			ext4_discard_preallocations(inode);
			if (flags & EXT4_GET_BLOCKS_DELALLOC_RESERVE)
				fb_flags = EXT4_FREE_BLOCKS_NO_QUOT_UPDATE;
			ext4_free_blocks(handle, inode, NULL, newblock,
					 EXT4_C2B(sbi, allocated_clusters),
					 fb_flags);
		}
		goto out;
	}


	if ((flags & EXT4_GET_BLOCKS_UNWRIT_EXT) == 0)
		ext4_update_inode_fsync_trans(handle, inode, 1);
	else
		ext4_update_inode_fsync_trans(handle, inode, 0);

	map->m_flags |= (EXT4_MAP_NEW | EXT4_MAP_MAPPED);
	map->m_pblk = pblk;
	map->m_len = ar.len;
	allocated = map->m_len;
	ext4_ext_show_leaf(inode, path);
out:
	ext4_free_ext_path(path);

	trace_ext4_ext_map_blocks_exit(inode, flags, map,
				       err ? err : allocated);
	return err ? err : allocated;
}

/**
 * ext4_ext_truncate - Implements the ext truncate operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_ext_truncate(handle_t *handle, struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	ext4_lblk_t last_block;
	int err = 0;


	EXT4_I(inode)->i_disksize = inode->i_size;
	err = ext4_mark_inode_dirty(handle, inode);
	if (err)
		return err;

	last_block = (inode->i_size + sb->s_blocksize - 1)
			>> EXT4_BLOCK_SIZE_BITS(sb);
	ext4_es_remove_extent(inode, last_block, EXT_MAX_BLOCKS - last_block);

retry_remove_space:
	err = ext4_ext_remove_space(inode, last_block, EXT_MAX_BLOCKS - 1);
	if (err == -ENOMEM) {
		memalloc_retry_wait(GFP_ATOMIC);
		goto retry_remove_space;
	}
	return err;
}

/**
 * ext4_alloc_file_blocks - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_alloc_file_blocks(struct file *file, ext4_lblk_t offset,
				  ext4_lblk_t len, loff_t new_size,
				  int flags)
{
	struct inode *inode = file_inode(file);
	handle_t *handle;
	int ret = 0, ret2 = 0, ret3 = 0;
	int retries = 0;
	int depth = 0;
	struct ext4_map_blocks map;
	unsigned int credits;
	loff_t epos, old_size = i_size_read(inode);

	BUG_ON(!ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS));
	map.m_lblk = offset;
	map.m_len = len;


	if (len <= EXT_UNWRITTEN_MAX_LEN)
		flags |= EXT4_GET_BLOCKS_NO_NORMALIZE;


	credits = ext4_chunk_trans_blocks(inode, len);
	depth = ext_depth(inode);

retry:
	while (len) {


		if (depth != ext_depth(inode)) {
			credits = ext4_chunk_trans_blocks(inode, len);
			depth = ext_depth(inode);
		}

		handle = ext4_journal_start(inode, EXT4_HT_MAP_BLOCKS,
					    credits);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			break;
		}
		ret = ext4_map_blocks(handle, inode, &map, flags);
		if (ret <= 0) {
			ext4_debug("inode #%lu: block %u: len %u: "
				   "ext4_ext_map_blocks returned %d",
				   inode->i_ino, map.m_lblk,
				   map.m_len, ret);
			ext4_mark_inode_dirty(handle, inode);
			ext4_journal_stop(handle);
			break;
		}


		retries = 0;
		map.m_lblk += ret;
		map.m_len = len = len - ret;
		epos = (loff_t)map.m_lblk << inode->i_blkbits;
		inode_set_ctime_current(inode);
		if (new_size) {
			if (epos > new_size)
				epos = new_size;
			if (ext4_update_inode_size(inode, epos) & 0x1)
				inode_set_mtime_to_ts(inode,
						      inode_get_ctime(inode));
			if (epos > old_size) {
				pagecache_isize_extended(inode, old_size, epos);
				ext4_zero_partial_blocks(handle, inode,
						     old_size, epos - old_size);
			}
		}
		ret2 = ext4_mark_inode_dirty(handle, inode);
		ext4_update_inode_fsync_trans(handle, inode, 1);
		ret3 = ext4_journal_stop(handle);
		ret2 = ret3 ? ret3 : ret2;
		if (unlikely(ret2))
			break;
	}
	if (ret == -ENOSPC && ext4_should_retry_alloc(inode->i_sb, &retries))
		goto retry;

	return ret > 0 ? ret2 : ret;
}

static int ext4_collapse_range(struct file *file, loff_t offset, loff_t len);

static int ext4_insert_range(struct file *file, loff_t offset, loff_t len);

/**
 * ext4_zero_range - Implements the zero range operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static long ext4_zero_range(struct file *file, loff_t offset,
			    loff_t len, int mode)
{
	struct inode *inode = file_inode(file);
	handle_t *handle = NULL;
	loff_t new_size = 0;
	loff_t end = offset + len;
	ext4_lblk_t start_lblk, end_lblk;
	unsigned int blocksize = i_blocksize(inode);
	unsigned int blkbits = inode->i_blkbits;
	int ret, flags, credits;

	trace_ext4_zero_range(inode, offset, len, mode);
	WARN_ON_ONCE(!inode_is_locked(inode));


	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)))
		return -EOPNOTSUPP;

	if (!(mode & FALLOC_FL_KEEP_SIZE) &&
	    (end > inode->i_size || end > EXT4_I(inode)->i_disksize)) {
		new_size = end;
		ret = inode_newsize_ok(inode, new_size);
		if (ret)
			return ret;
	}

	flags = EXT4_GET_BLOCKS_CREATE_UNWRIT_EXT;

	if (!IS_ALIGNED(offset | end, blocksize)) {
		ext4_lblk_t alloc_lblk = offset >> blkbits;
		ext4_lblk_t len_lblk = EXT4_MAX_BLOCKS(len, offset, blkbits);

		ret = ext4_alloc_file_blocks(file, alloc_lblk, len_lblk,
					     new_size, flags);
		if (ret)
			return ret;
	}

	ret = ext4_update_disksize_before_punch(inode, offset, len);
	if (ret)
		return ret;


	ret = ext4_truncate_page_cache_block_range(inode, offset, end);
	if (ret)
		return ret;


	start_lblk = EXT4_B_TO_LBLK(inode, offset);
	end_lblk = end >> blkbits;
	if (end_lblk > start_lblk) {
		ext4_lblk_t zero_blks = end_lblk - start_lblk;

		flags |= (EXT4_GET_BLOCKS_CONVERT_UNWRITTEN | EXT4_EX_NOCACHE);
		ret = ext4_alloc_file_blocks(file, start_lblk, zero_blks,
					     new_size, flags);
		if (ret)
			return ret;
	}

	if (IS_ALIGNED(offset | end, blocksize))
		return ret;


	credits = (2 * ext4_ext_index_trans_blocks(inode, 2)) + 1;
	if (ext4_should_journal_data(inode))
		credits += 2;
	handle = ext4_journal_start(inode, EXT4_HT_MISC, credits);
	if (IS_ERR(handle)) {
		ret = PTR_ERR(handle);
		ext4_std_error(inode->i_sb, ret);
		return ret;
	}


	ret = ext4_zero_partial_blocks(handle, inode, offset, len);
	if (ret)
		goto out_handle;

	if (new_size)
		ext4_update_inode_size(inode, new_size);
	ret = ext4_mark_inode_dirty(handle, inode);
	if (unlikely(ret))
		goto out_handle;

	ext4_update_inode_fsync_trans(handle, inode, 1);
	if (file->f_flags & O_SYNC)
		ext4_handle_sync(handle);

out_handle:
	ext4_journal_stop(handle);
	return ret;
}

/**
 * ext4_do_fallocate - Implements the do fallocate operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static long ext4_do_fallocate(struct file *file, loff_t offset,
			      loff_t len, int mode)
{
	struct inode *inode = file_inode(file);
	loff_t end = offset + len;
	loff_t new_size = 0;
	ext4_lblk_t start_lblk, len_lblk;
	int ret;

	trace_ext4_fallocate_enter(inode, offset, len, mode);
	WARN_ON_ONCE(!inode_is_locked(inode));

	start_lblk = offset >> inode->i_blkbits;
	len_lblk = EXT4_MAX_BLOCKS(len, offset, inode->i_blkbits);


	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))) {
		ret = -EOPNOTSUPP;
		goto out;
	}

	if (!(mode & FALLOC_FL_KEEP_SIZE) &&
	    (end > inode->i_size || end > EXT4_I(inode)->i_disksize)) {
		new_size = end;
		ret = inode_newsize_ok(inode, new_size);
		if (ret)
			goto out;
	}

	ret = ext4_alloc_file_blocks(file, start_lblk, len_lblk, new_size,
				     EXT4_GET_BLOCKS_CREATE_UNWRIT_EXT);
	if (ret)
		goto out;

	if (file->f_flags & O_SYNC && EXT4_SB(inode->i_sb)->s_journal) {
		ret = ext4_fc_commit(EXT4_SB(inode->i_sb)->s_journal,
					EXT4_I(inode)->i_sync_tid);
	}
out:
	trace_ext4_fallocate_exit(inode, offset, len_lblk, ret);
	return ret;
}


/**
 * ext4_fallocate - Implements the fallocate operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
long ext4_fallocate(struct file *file, int mode, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct address_space *mapping = file->f_mapping;
	int ret;


	if (IS_ENCRYPTED(inode) &&
	    (mode & (FALLOC_FL_COLLAPSE_RANGE | FALLOC_FL_INSERT_RANGE)))
		return -EOPNOTSUPP;


	if (mode & ~(FALLOC_FL_KEEP_SIZE | FALLOC_FL_PUNCH_HOLE |
		     FALLOC_FL_COLLAPSE_RANGE | FALLOC_FL_ZERO_RANGE |
		     FALLOC_FL_INSERT_RANGE))
		return -EOPNOTSUPP;

	inode_lock(inode);
	ret = ext4_convert_inline_data(inode);
	if (ret)
		goto out_inode_lock;


	inode_dio_wait(inode);

	ret = file_modified(file);
	if (ret)
		goto out_inode_lock;

	if ((mode & FALLOC_FL_MODE_MASK) == FALLOC_FL_ALLOCATE_RANGE) {
		ret = ext4_do_fallocate(file, offset, len, mode);
		goto out_inode_lock;
	}


	filemap_invalidate_lock(mapping);

	ret = ext4_break_layouts(inode);
	if (ret)
		goto out_invalidate_lock;

	if (mode & FALLOC_FL_PUNCH_HOLE)
		ret = ext4_punch_hole(file, offset, len);
	else if (mode & FALLOC_FL_COLLAPSE_RANGE)
		ret = ext4_collapse_range(file, offset, len);
	else if (mode & FALLOC_FL_INSERT_RANGE)
		ret = ext4_insert_range(file, offset, len);
	else if (mode & FALLOC_FL_ZERO_RANGE)
		ret = ext4_zero_range(file, offset, len, mode);
	else
		ret = -EOPNOTSUPP;

out_invalidate_lock:
	filemap_invalidate_unlock(mapping);
out_inode_lock:
	inode_unlock(inode);
	return ret;
}


/**
 * ext4_convert_unwritten_extents - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_convert_unwritten_extents(handle_t *handle, struct inode *inode,
				   loff_t offset, ssize_t len)
{
	unsigned int max_blocks;
	int ret = 0, ret2 = 0, ret3 = 0;
	struct ext4_map_blocks map;
	unsigned int blkbits = inode->i_blkbits;
	unsigned int credits = 0;

	map.m_lblk = offset >> blkbits;
	max_blocks = EXT4_MAX_BLOCKS(len, offset, blkbits);

	if (!handle) {


		credits = ext4_chunk_trans_blocks(inode, max_blocks);
	}
	while (ret >= 0 && ret < max_blocks) {
		map.m_lblk += ret;
		map.m_len = (max_blocks -= ret);
		if (credits) {
			handle = ext4_journal_start(inode, EXT4_HT_MAP_BLOCKS,
						    credits);
			if (IS_ERR(handle)) {
				ret = PTR_ERR(handle);
				break;
			}
		}
		ret = ext4_map_blocks(handle, inode, &map,
				      EXT4_GET_BLOCKS_IO_CONVERT_EXT);
		if (ret <= 0)
			ext4_warning(inode->i_sb,
				     "inode #%lu: block %u: len %u: "
				     "ext4_ext_map_blocks returned %d",
				     inode->i_ino, map.m_lblk,
				     map.m_len, ret);
		ret2 = ext4_mark_inode_dirty(handle, inode);
		if (credits) {
			ret3 = ext4_journal_stop(handle);
			if (unlikely(ret3))
				ret2 = ret3;
		}

		if (ret <= 0 || ret2)
			break;
	}
	return ret > 0 ? ret2 : ret;
}

/**
 * ext4_convert_unwritten_io_end_vec - Implements the convert unwritten io end vec operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_convert_unwritten_io_end_vec(handle_t *handle, ext4_io_end_t *io_end)
{
	int ret = 0, err = 0;
	struct ext4_io_end_vec *io_end_vec;


	if (handle) {
		handle = ext4_journal_start_reserved(handle,
						     EXT4_HT_EXT_CONVERT);
		if (IS_ERR(handle))
			return PTR_ERR(handle);
	}

	list_for_each_entry(io_end_vec, &io_end->list_vec, list) {
		ret = ext4_convert_unwritten_extents(handle, io_end->inode,
						     io_end_vec->offset,
						     io_end_vec->size);
		if (ret)
			break;
	}

	if (handle)
		err = ext4_journal_stop(handle);

	return ret < 0 ? ret : err;
}

/**
 * ext4_iomap_xattr_fiemap - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_iomap_xattr_fiemap(struct inode *inode, struct iomap *iomap)
{
	__u64 physical = 0;
	__u64 length = 0;
	int blockbits = inode->i_sb->s_blocksize_bits;
	int error = 0;
	u16 iomap_type;


	if (ext4_test_inode_state(inode, EXT4_STATE_XATTR)) {
		struct ext4_iloc iloc;
		int offset;

		error = ext4_get_inode_loc(inode, &iloc);
		if (error)
			return error;
		physical = (__u64)iloc.bh->b_blocknr << blockbits;
		offset = EXT4_GOOD_OLD_INODE_SIZE +
				EXT4_I(inode)->i_extra_isize;
		physical += offset;
		length = EXT4_SB(inode->i_sb)->s_inode_size - offset;
		brelse(iloc.bh);
		iomap_type = IOMAP_INLINE;
	} else if (EXT4_I(inode)->i_file_acl) {
		physical = (__u64)EXT4_I(inode)->i_file_acl << blockbits;
		length = inode->i_sb->s_blocksize;
		iomap_type = IOMAP_MAPPED;
	} else {

		error = -ENOENT;
		goto out;
	}

	iomap->addr = physical;
	iomap->offset = 0;
	iomap->length = length;
	iomap->type = iomap_type;
	iomap->flags = 0;
out:
	return error;
}

/**
 * ext4_iomap_xattr_begin - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_iomap_xattr_begin(struct inode *inode, loff_t offset,
				  loff_t length, unsigned flags,
				  struct iomap *iomap, struct iomap *srcmap)
{
	int error;

	error = ext4_iomap_xattr_fiemap(inode, iomap);
	if (error == 0 && (offset >= iomap->length))
		error = -ENOENT;
	return error;
}

static const struct iomap_ops ext4_iomap_xattr_ops = {
	.iomap_begin		= ext4_iomap_xattr_begin,
};

/**
 * ext4_fiemap_check_ranges - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fiemap_check_ranges(struct inode *inode, u64 start, u64 *len)
{
	u64 maxbytes = ext4_get_maxbytes(inode);

	if (*len == 0)
		return -EINVAL;
	if (start > maxbytes)
		return -EFBIG;


	if (*len > maxbytes || (maxbytes - *len) < start)
		*len = maxbytes - start;
	return 0;
}

/**
 * ext4_fiemap - Implements the fiemap operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		u64 start, u64 len)
{
	int error = 0;

	if (fieinfo->fi_flags & FIEMAP_FLAG_CACHE) {
		error = ext4_ext_precache(inode);
		if (error)
			return error;
		fieinfo->fi_flags &= ~FIEMAP_FLAG_CACHE;
	}


	error = ext4_fiemap_check_ranges(inode, start, &len);
	if (error)
		return error;

	if (fieinfo->fi_flags & FIEMAP_FLAG_XATTR) {
		fieinfo->fi_flags &= ~FIEMAP_FLAG_XATTR;
		return iomap_fiemap(inode, fieinfo, start, len,
				    &ext4_iomap_xattr_ops);
	}

	return iomap_fiemap(inode, fieinfo, start, len, &ext4_iomap_report_ops);
}

/**
 * ext4_get_es_cache - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_get_es_cache(struct inode *inode, struct fiemap_extent_info *fieinfo,
		      __u64 start, __u64 len)
{
	ext4_lblk_t start_blk, len_blks;
	__u64 last_blk;
	int error = 0;

	if (ext4_has_inline_data(inode)) {
		int has_inline;

		down_read(&EXT4_I(inode)->xattr_sem);
		has_inline = ext4_has_inline_data(inode);
		up_read(&EXT4_I(inode)->xattr_sem);
		if (has_inline)
			return 0;
	}

	if (fieinfo->fi_flags & FIEMAP_FLAG_CACHE) {
		error = ext4_ext_precache(inode);
		if (error)
			return error;
		fieinfo->fi_flags &= ~FIEMAP_FLAG_CACHE;
	}

	error = fiemap_prep(inode, fieinfo, start, &len, 0);
	if (error)
		return error;

	error = ext4_fiemap_check_ranges(inode, start, &len);
	if (error)
		return error;

	start_blk = start >> inode->i_sb->s_blocksize_bits;
	last_blk = (start + len - 1) >> inode->i_sb->s_blocksize_bits;
	if (last_blk >= EXT_MAX_BLOCKS)
		last_blk = EXT_MAX_BLOCKS-1;
	len_blks = ((ext4_lblk_t) last_blk) - start_blk + 1;


	return ext4_fill_es_cache_info(inode, start_blk, len_blks, fieinfo);
}


/**
 * ext4_ext_shift_path_extents - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_ext_shift_path_extents(struct ext4_ext_path *path, ext4_lblk_t shift,
			    struct inode *inode, handle_t *handle,
			    enum SHIFT_DIRECTION SHIFT)
{
	int depth, err = 0;
	struct ext4_extent *ex_start, *ex_last;
	bool update = false;
	int credits, restart_credits;
	depth = path->p_depth;

	while (depth >= 0) {
		if (depth == path->p_depth) {
			ex_start = path[depth].p_ext;
			if (!ex_start)
				return -EFSCORRUPTED;

			ex_last = EXT_LAST_EXTENT(path[depth].p_hdr);

			credits = 3;
			if (ex_start == EXT_FIRST_EXTENT(path[depth].p_hdr)) {
				update = true;

				credits = depth + 2;
			}

			restart_credits = ext4_writepage_trans_blocks(inode);
			err = ext4_datasem_ensure_credits(handle, inode, credits,
					restart_credits, 0);
			if (err) {
				if (err > 0)
					err = -EAGAIN;
				goto out;
			}

			err = ext4_ext_get_access(handle, inode, path + depth);
			if (err)
				goto out;

			while (ex_start <= ex_last) {
				if (SHIFT == SHIFT_LEFT) {
					le32_add_cpu(&ex_start->ee_block,
						-shift);

					if ((ex_start >
					    EXT_FIRST_EXTENT(path[depth].p_hdr))
					    &&
					    ext4_ext_try_to_merge_right(inode,
					    path, ex_start - 1))
						ex_last--;
					else
						ex_start++;
				} else {
					le32_add_cpu(&ex_last->ee_block, shift);
					ext4_ext_try_to_merge_right(inode, path,
						ex_last);
					ex_last--;
				}
			}
			err = ext4_ext_dirty(handle, inode, path + depth);
			if (err)
				goto out;

			if (--depth < 0 || !update)
				break;
		}


		err = ext4_ext_get_access(handle, inode, path + depth);
		if (err)
			goto out;

		if (SHIFT == SHIFT_LEFT)
			le32_add_cpu(&path[depth].p_idx->ei_block, -shift);
		else
			le32_add_cpu(&path[depth].p_idx->ei_block, shift);
		err = ext4_ext_dirty(handle, inode, path + depth);
		if (err)
			goto out;


		if (path[depth].p_idx != EXT_FIRST_INDEX(path[depth].p_hdr))
			break;

		depth--;
	}

out:
	return err;
}


/**
 * ext4_ext_shift_extents - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_ext_shift_extents(struct inode *inode, handle_t *handle,
		       ext4_lblk_t start, ext4_lblk_t shift,
		       enum SHIFT_DIRECTION SHIFT)
{
	struct ext4_ext_path *path;
	int ret = 0, depth;
	struct ext4_extent *extent;
	ext4_lblk_t stop, *iterator, ex_start, ex_end;
	ext4_lblk_t tmp = EXT_MAX_BLOCKS;


	path = ext4_find_extent(inode, EXT_MAX_BLOCKS - 1, NULL,
				EXT4_EX_NOCACHE);
	if (IS_ERR(path))
		return PTR_ERR(path);

	depth = path->p_depth;
	extent = path[depth].p_ext;
	if (!extent)
		goto out;

	stop = le32_to_cpu(extent->ee_block);


	if (SHIFT == SHIFT_LEFT) {
		path = ext4_find_extent(inode, start - 1, path,
					EXT4_EX_NOCACHE);
		if (IS_ERR(path))
			return PTR_ERR(path);
		depth = path->p_depth;
		extent =  path[depth].p_ext;
		if (extent) {
			ex_start = le32_to_cpu(extent->ee_block);
			ex_end = le32_to_cpu(extent->ee_block) +
				ext4_ext_get_actual_len(extent);
		} else {
			ex_start = 0;
			ex_end = 0;
		}

		if ((start == ex_start && shift > ex_start) ||
		    (shift > start - ex_end)) {
			ret = -EINVAL;
			goto out;
		}
	} else {
		if (shift > EXT_MAX_BLOCKS -
		    (stop + ext4_ext_get_actual_len(extent))) {
			ret = -EINVAL;
			goto out;
		}
	}


again:
	ret = 0;
	if (SHIFT == SHIFT_LEFT)
		iterator = &start;
	else
		iterator = &stop;

	if (tmp != EXT_MAX_BLOCKS)
		*iterator = tmp;


	while (iterator && start <= stop) {
		path = ext4_find_extent(inode, *iterator, path,
					EXT4_EX_NOCACHE);
		if (IS_ERR(path))
			return PTR_ERR(path);
		depth = path->p_depth;
		extent = path[depth].p_ext;
		if (!extent) {
			EXT4_ERROR_INODE(inode, "unexpected hole at %lu",
					 (unsigned long) *iterator);
			ret = -EFSCORRUPTED;
			goto out;
		}
		if (SHIFT == SHIFT_LEFT && *iterator >
		    le32_to_cpu(extent->ee_block)) {

			if (extent < EXT_LAST_EXTENT(path[depth].p_hdr)) {
				path[depth].p_ext++;
			} else {
				*iterator = ext4_ext_next_allocated_block(path);
				continue;
			}
		}

		tmp = *iterator;
		if (SHIFT == SHIFT_LEFT) {
			extent = EXT_LAST_EXTENT(path[depth].p_hdr);
			*iterator = le32_to_cpu(extent->ee_block) +
					ext4_ext_get_actual_len(extent);
		} else {
			extent = EXT_FIRST_EXTENT(path[depth].p_hdr);
			if (le32_to_cpu(extent->ee_block) > start)
				*iterator = le32_to_cpu(extent->ee_block) - 1;
			else if (le32_to_cpu(extent->ee_block) == start)
				iterator = NULL;
			else {
				extent = EXT_LAST_EXTENT(path[depth].p_hdr);
				while (le32_to_cpu(extent->ee_block) >= start)
					extent--;

				if (extent == EXT_LAST_EXTENT(path[depth].p_hdr))
					break;

				extent++;
				iterator = NULL;
			}
			path[depth].p_ext = extent;
		}
		ret = ext4_ext_shift_path_extents(path, shift, inode,
				handle, SHIFT);

		if (ret == -EAGAIN)
			goto again;
		if (ret)
			break;
	}
out:
	ext4_free_ext_path(path);
	return ret;
}


/**
 * ext4_collapse_range - Implements the collapse range operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_collapse_range(struct file *file, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct address_space *mapping = inode->i_mapping;
	loff_t end = offset + len;
	ext4_lblk_t start_lblk, end_lblk;
	handle_t *handle;
	unsigned int credits;
	loff_t start, new_size;
	int ret;

	trace_ext4_collapse_range(inode, offset, len);
	WARN_ON_ONCE(!inode_is_locked(inode));


	if (!ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		return -EOPNOTSUPP;

	if (!IS_ALIGNED(offset | len, EXT4_CLUSTER_SIZE(sb)))
		return -EINVAL;


	if (end >= inode->i_size)
		return -EINVAL;


	start = round_down(offset, PAGE_SIZE);
	ret = filemap_write_and_wait_range(mapping, start, offset);
	if (!ret)
		ret = filemap_write_and_wait_range(mapping, end, LLONG_MAX);
	if (ret)
		return ret;

	truncate_pagecache(inode, start);

	credits = ext4_writepage_trans_blocks(inode);
	handle = ext4_journal_start(inode, EXT4_HT_TRUNCATE, credits);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	ext4_fc_mark_ineligible(sb, EXT4_FC_REASON_FALLOC_RANGE, handle);

	start_lblk = offset >> inode->i_blkbits;
	end_lblk = (offset + len) >> inode->i_blkbits;

	down_write(&EXT4_I(inode)->i_data_sem);
	ext4_discard_preallocations(inode);
	ext4_es_remove_extent(inode, start_lblk, EXT_MAX_BLOCKS - start_lblk);

	ret = ext4_ext_remove_space(inode, start_lblk, end_lblk - 1);
	if (ret) {
		up_write(&EXT4_I(inode)->i_data_sem);
		goto out_handle;
	}
	ext4_discard_preallocations(inode);

	ret = ext4_ext_shift_extents(inode, handle, end_lblk,
				     end_lblk - start_lblk, SHIFT_LEFT);
	if (ret) {
		up_write(&EXT4_I(inode)->i_data_sem);
		goto out_handle;
	}

	new_size = inode->i_size - len;
	i_size_write(inode, new_size);
	EXT4_I(inode)->i_disksize = new_size;

	up_write(&EXT4_I(inode)->i_data_sem);
	ret = ext4_mark_inode_dirty(handle, inode);
	if (ret)
		goto out_handle;

	ext4_update_inode_fsync_trans(handle, inode, 1);
	if (IS_SYNC(inode))
		ext4_handle_sync(handle);

out_handle:
	ext4_journal_stop(handle);
	return ret;
}


/**
 * ext4_insert_range - Implements the insert range operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_insert_range(struct file *file, loff_t offset, loff_t len)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct address_space *mapping = inode->i_mapping;
	handle_t *handle;
	struct ext4_ext_path *path;
	struct ext4_extent *extent;
	ext4_lblk_t start_lblk, len_lblk, ee_start_lblk = 0;
	unsigned int credits, ee_len;
	int ret, depth, split_flag = 0;
	loff_t start;

	trace_ext4_insert_range(inode, offset, len);
	WARN_ON_ONCE(!inode_is_locked(inode));


	if (!ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		return -EOPNOTSUPP;

	if (!IS_ALIGNED(offset | len, EXT4_CLUSTER_SIZE(sb)))
		return -EINVAL;

	if (offset >= inode->i_size)
		return -EINVAL;

	if (len > inode->i_sb->s_maxbytes - inode->i_size)
		return -EFBIG;


	start = round_down(offset, PAGE_SIZE);
	ret = filemap_write_and_wait_range(mapping, start, LLONG_MAX);
	if (ret)
		return ret;

	truncate_pagecache(inode, start);

	credits = ext4_writepage_trans_blocks(inode);
	handle = ext4_journal_start(inode, EXT4_HT_TRUNCATE, credits);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	ext4_fc_mark_ineligible(sb, EXT4_FC_REASON_FALLOC_RANGE, handle);


	inode->i_size += len;
	EXT4_I(inode)->i_disksize += len;
	ret = ext4_mark_inode_dirty(handle, inode);
	if (ret)
		goto out_handle;

	start_lblk = offset >> inode->i_blkbits;
	len_lblk = len >> inode->i_blkbits;

	down_write(&EXT4_I(inode)->i_data_sem);
	ext4_discard_preallocations(inode);

	path = ext4_find_extent(inode, start_lblk, NULL, 0);
	if (IS_ERR(path)) {
		up_write(&EXT4_I(inode)->i_data_sem);
		ret = PTR_ERR(path);
		goto out_handle;
	}

	depth = ext_depth(inode);
	extent = path[depth].p_ext;
	if (extent) {
		ee_start_lblk = le32_to_cpu(extent->ee_block);
		ee_len = ext4_ext_get_actual_len(extent);


		if ((start_lblk > ee_start_lblk) &&
				(start_lblk < (ee_start_lblk + ee_len))) {
			if (ext4_ext_is_unwritten(extent))
				split_flag = EXT4_EXT_MARK_UNWRIT1 |
					EXT4_EXT_MARK_UNWRIT2;
			path = ext4_split_extent_at(handle, inode, path,
					start_lblk, split_flag,
					EXT4_EX_NOCACHE |
					EXT4_GET_BLOCKS_PRE_IO |
					EXT4_GET_BLOCKS_METADATA_NOFAIL);
		}

		if (IS_ERR(path)) {
			up_write(&EXT4_I(inode)->i_data_sem);
			ret = PTR_ERR(path);
			goto out_handle;
		}
	}

	ext4_free_ext_path(path);
	ext4_es_remove_extent(inode, start_lblk, EXT_MAX_BLOCKS - start_lblk);


	ret = ext4_ext_shift_extents(inode, handle,
		max(ee_start_lblk, start_lblk), len_lblk, SHIFT_RIGHT);
	up_write(&EXT4_I(inode)->i_data_sem);
	if (ret)
		goto out_handle;

	ext4_update_inode_fsync_trans(handle, inode, 1);
	if (IS_SYNC(inode))
		ext4_handle_sync(handle);

out_handle:
	ext4_journal_stop(handle);
	return ret;
}


/**
 * ext4_swap_extents - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int
ext4_swap_extents(handle_t *handle, struct inode *inode1,
		  struct inode *inode2, ext4_lblk_t lblk1, ext4_lblk_t lblk2,
		  ext4_lblk_t count, int unwritten, int *erp)
{
	struct ext4_ext_path *path1 = NULL;
	struct ext4_ext_path *path2 = NULL;
	int replaced_count = 0;

	BUG_ON(!rwsem_is_locked(&EXT4_I(inode1)->i_data_sem));
	BUG_ON(!rwsem_is_locked(&EXT4_I(inode2)->i_data_sem));
	BUG_ON(!inode_is_locked(inode1));
	BUG_ON(!inode_is_locked(inode2));

	ext4_es_remove_extent(inode1, lblk1, count);
	ext4_es_remove_extent(inode2, lblk2, count);

	while (count) {
		struct ext4_extent *ex1, *ex2, tmp_ex;
		ext4_lblk_t e1_blk, e2_blk;
		int e1_len, e2_len, len;
		int split = 0;

		path1 = ext4_find_extent(inode1, lblk1, path1, EXT4_EX_NOCACHE);
		if (IS_ERR(path1)) {
			*erp = PTR_ERR(path1);
			goto errout;
		}
		path2 = ext4_find_extent(inode2, lblk2, path2, EXT4_EX_NOCACHE);
		if (IS_ERR(path2)) {
			*erp = PTR_ERR(path2);
			goto errout;
		}
		ex1 = path1[path1->p_depth].p_ext;
		ex2 = path2[path2->p_depth].p_ext;

		if (unlikely(!ex2 || !ex1))
			goto errout;

		e1_blk = le32_to_cpu(ex1->ee_block);
		e2_blk = le32_to_cpu(ex2->ee_block);
		e1_len = ext4_ext_get_actual_len(ex1);
		e2_len = ext4_ext_get_actual_len(ex2);


		if (!in_range(lblk1, e1_blk, e1_len) ||
		    !in_range(lblk2, e2_blk, e2_len)) {
			ext4_lblk_t next1, next2;


			next1 = ext4_ext_next_allocated_block(path1);
			next2 = ext4_ext_next_allocated_block(path2);

			if (e1_blk > lblk1)
				next1 = e1_blk;
			if (e2_blk > lblk2)
				next2 = e2_blk;

			if (next1 == EXT_MAX_BLOCKS || next2 == EXT_MAX_BLOCKS)
				goto errout;

			len = next1 - lblk1;
			if (len < next2 - lblk2)
				len = next2 - lblk2;
			if (len > count)
				len = count;
			lblk1 += len;
			lblk2 += len;
			count -= len;
			continue;
		}


		if (e1_blk < lblk1) {
			split = 1;
			path1 = ext4_force_split_extent_at(handle, inode1,
							   path1, lblk1, 0);
			if (IS_ERR(path1)) {
				*erp = PTR_ERR(path1);
				goto errout;
			}
		}
		if (e2_blk < lblk2) {
			split = 1;
			path2 = ext4_force_split_extent_at(handle, inode2,
							   path2, lblk2, 0);
			if (IS_ERR(path2)) {
				*erp = PTR_ERR(path2);
				goto errout;
			}
		}


		if (split)
			continue;


		len = count;
		if (len > e1_blk + e1_len - lblk1)
			len = e1_blk + e1_len - lblk1;
		if (len > e2_blk + e2_len - lblk2)
			len = e2_blk + e2_len - lblk2;

		if (len != e1_len) {
			split = 1;
			path1 = ext4_force_split_extent_at(handle, inode1,
							path1, lblk1 + len, 0);
			if (IS_ERR(path1)) {
				*erp = PTR_ERR(path1);
				goto errout;
			}
		}
		if (len != e2_len) {
			split = 1;
			path2 = ext4_force_split_extent_at(handle, inode2,
							path2, lblk2 + len, 0);
			if (IS_ERR(path2)) {
				*erp = PTR_ERR(path2);
				goto errout;
			}
		}


		if (split)
			continue;

		BUG_ON(e2_len != e1_len);
		*erp = ext4_ext_get_access(handle, inode1, path1 + path1->p_depth);
		if (unlikely(*erp))
			goto errout;
		*erp = ext4_ext_get_access(handle, inode2, path2 + path2->p_depth);
		if (unlikely(*erp))
			goto errout;


		tmp_ex = *ex1;
		ext4_ext_store_pblock(ex1, ext4_ext_pblock(ex2));
		ext4_ext_store_pblock(ex2, ext4_ext_pblock(&tmp_ex));
		ex1->ee_len = cpu_to_le16(e2_len);
		ex2->ee_len = cpu_to_le16(e1_len);
		if (unwritten)
			ext4_ext_mark_unwritten(ex2);
		if (ext4_ext_is_unwritten(&tmp_ex))
			ext4_ext_mark_unwritten(ex1);

		ext4_ext_try_to_merge(handle, inode2, path2, ex2);
		ext4_ext_try_to_merge(handle, inode1, path1, ex1);
		*erp = ext4_ext_dirty(handle, inode2, path2 +
				      path2->p_depth);
		if (unlikely(*erp))
			goto errout;
		*erp = ext4_ext_dirty(handle, inode1, path1 +
				      path1->p_depth);


		if (unlikely(*erp))
			goto errout;

		lblk1 += len;
		lblk2 += len;
		replaced_count += len;
		count -= len;
	}

errout:
	ext4_free_ext_path(path1);
	ext4_free_ext_path(path2);
	return replaced_count;
}


/**
 * ext4_clu_mapped - Implements the clu mapped operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_clu_mapped(struct inode *inode, ext4_lblk_t lclu)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_ext_path *path;
	int depth, mapped = 0, err = 0;
	struct ext4_extent *extent;
	ext4_lblk_t first_lblk, first_lclu, last_lclu;


	if (ext4_test_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA) ||
	    ext4_has_inline_data(inode))
		return 0;


	path = ext4_find_extent(inode, EXT4_C2B(sbi, lclu), NULL, 0);
	if (IS_ERR(path))
		return PTR_ERR(path);

	depth = ext_depth(inode);


	if (unlikely(path[depth].p_ext == NULL && depth != 0)) {
		EXT4_ERROR_INODE(inode,
		    "bad extent address - lblock: %lu, depth: %d, pblock: %lld",
				 (unsigned long) EXT4_C2B(sbi, lclu),
				 depth, path[depth].p_block);
		err = -EFSCORRUPTED;
		goto out;
	}

	extent = path[depth].p_ext;


	if (extent == NULL)
		goto out;

	first_lblk = le32_to_cpu(extent->ee_block);
	first_lclu = EXT4_B2C(sbi, first_lblk);


	if (lclu >= first_lclu) {
		last_lclu = EXT4_B2C(sbi, first_lblk +
				     ext4_ext_get_actual_len(extent) - 1);
		if (lclu <= last_lclu) {
			mapped = 1;
		} else {
			first_lblk = ext4_ext_next_allocated_block(path);
			first_lclu = EXT4_B2C(sbi, first_lblk);
			if (lclu == first_lclu)
				mapped = 1;
		}
	}

out:
	ext4_free_ext_path(path);

	return err ? err : mapped;
}


/**
 * ext4_ext_replay_update_ex - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_ext_replay_update_ex(struct inode *inode, ext4_lblk_t start,
			      int len, int unwritten, ext4_fsblk_t pblk)
{
	struct ext4_ext_path *path;
	struct ext4_extent *ex;
	int ret;

	path = ext4_find_extent(inode, start, NULL, 0);
	if (IS_ERR(path))
		return PTR_ERR(path);
	ex = path[path->p_depth].p_ext;
	if (!ex) {
		ret = -EFSCORRUPTED;
		goto out;
	}

	if (le32_to_cpu(ex->ee_block) != start ||
		ext4_ext_get_actual_len(ex) != len) {

		down_write(&EXT4_I(inode)->i_data_sem);
		path = ext4_force_split_extent_at(NULL, inode, path, start, 1);
		up_write(&EXT4_I(inode)->i_data_sem);
		if (IS_ERR(path)) {
			ret = PTR_ERR(path);
			goto out;
		}

		path = ext4_find_extent(inode, start, path, 0);
		if (IS_ERR(path))
			return PTR_ERR(path);

		ex = path[path->p_depth].p_ext;
		WARN_ON(le32_to_cpu(ex->ee_block) != start);

		if (ext4_ext_get_actual_len(ex) != len) {
			down_write(&EXT4_I(inode)->i_data_sem);
			path = ext4_force_split_extent_at(NULL, inode, path,
							  start + len, 1);
			up_write(&EXT4_I(inode)->i_data_sem);
			if (IS_ERR(path)) {
				ret = PTR_ERR(path);
				goto out;
			}

			path = ext4_find_extent(inode, start, path, 0);
			if (IS_ERR(path))
				return PTR_ERR(path);
			ex = path[path->p_depth].p_ext;
		}
	}
	if (unwritten)
		ext4_ext_mark_unwritten(ex);
	else
		ext4_ext_mark_initialized(ex);
	ext4_ext_store_pblock(ex, pblk);
	down_write(&EXT4_I(inode)->i_data_sem);
	ret = ext4_ext_dirty(NULL, inode, &path[path->p_depth]);
	up_write(&EXT4_I(inode)->i_data_sem);
out:
	ext4_free_ext_path(path);
	ext4_mark_inode_dirty(NULL, inode);
	return ret;
}


/**
 * ext4_ext_replay_shrink_inode - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_ext_replay_shrink_inode(struct inode *inode, ext4_lblk_t end)
{
	struct ext4_ext_path *path = NULL;
	struct ext4_extent *ex;
	ext4_lblk_t old_cur, cur = 0;

	while (cur < end) {
		path = ext4_find_extent(inode, cur, NULL, 0);
		if (IS_ERR(path))
			return;
		ex = path[path->p_depth].p_ext;
		if (!ex) {
			ext4_free_ext_path(path);
			ext4_mark_inode_dirty(NULL, inode);
			return;
		}
		old_cur = cur;
		cur = le32_to_cpu(ex->ee_block) + ext4_ext_get_actual_len(ex);
		if (cur <= old_cur)
			cur = old_cur + 1;
		ext4_ext_try_to_merge(NULL, inode, path, ex);
		down_write(&EXT4_I(inode)->i_data_sem);
		ext4_ext_dirty(NULL, inode, &path[path->p_depth]);
		up_write(&EXT4_I(inode)->i_data_sem);
		ext4_mark_inode_dirty(NULL, inode);
		ext4_free_ext_path(path);
	}
}


/**
 * skip_hole - Implements the skip hole operation within the extent-tree engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int skip_hole(struct inode *inode, ext4_lblk_t *cur)
{
	int ret;
	struct ext4_map_blocks map;

	map.m_lblk = *cur;
	map.m_len = ((inode->i_size) >> inode->i_sb->s_blocksize_bits) - *cur;

	ret = ext4_map_blocks(NULL, inode, &map, 0);
	if (ret < 0)
		return ret;
	if (ret != 0)
		return 0;
	*cur = *cur + map.m_len;
	return 0;
}


/**
 * ext4_ext_replay_set_iblocks - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_ext_replay_set_iblocks(struct inode *inode)
{
	struct ext4_ext_path *path = NULL, *path2 = NULL;
	struct ext4_extent *ex;
	ext4_lblk_t cur = 0, end;
	int numblks = 0, i, ret = 0;
	ext4_fsblk_t cmp1, cmp2;
	struct ext4_map_blocks map;


	path = ext4_find_extent(inode, EXT_MAX_BLOCKS - 1, NULL,
					EXT4_EX_NOCACHE);
	if (IS_ERR(path))
		return PTR_ERR(path);
	ex = path[path->p_depth].p_ext;
	if (!ex)
		goto out;
	end = le32_to_cpu(ex->ee_block) + ext4_ext_get_actual_len(ex);


	cur = 0;
	while (cur < end) {
		map.m_lblk = cur;
		map.m_len = end - cur;
		ret = ext4_map_blocks(NULL, inode, &map, 0);
		if (ret < 0)
			break;
		if (ret > 0)
			numblks += ret;
		cur = cur + map.m_len;
	}


	cur = 0;
	ret = skip_hole(inode, &cur);
	if (ret < 0)
		goto out;
	path = ext4_find_extent(inode, cur, path, 0);
	if (IS_ERR(path))
		goto out;
	numblks += path->p_depth;
	while (cur < end) {
		path = ext4_find_extent(inode, cur, path, 0);
		if (IS_ERR(path))
			break;
		ex = path[path->p_depth].p_ext;
		if (!ex)
			goto cleanup;

		cur = max(cur + 1, le32_to_cpu(ex->ee_block) +
					ext4_ext_get_actual_len(ex));
		ret = skip_hole(inode, &cur);
		if (ret < 0)
			break;

		path2 = ext4_find_extent(inode, cur, path2, 0);
		if (IS_ERR(path2))
			break;

		for (i = 0; i <= max(path->p_depth, path2->p_depth); i++) {
			cmp1 = cmp2 = 0;
			if (i <= path->p_depth)
				cmp1 = path[i].p_bh ?
					path[i].p_bh->b_blocknr : 0;
			if (i <= path2->p_depth)
				cmp2 = path2[i].p_bh ?
					path2[i].p_bh->b_blocknr : 0;
			if (cmp1 != cmp2 && cmp2 != 0)
				numblks++;
		}
	}

out:
	inode->i_blocks = numblks << (inode->i_sb->s_blocksize_bits - 9);
	ext4_mark_inode_dirty(NULL, inode);
cleanup:
	ext4_free_ext_path(path);
	ext4_free_ext_path(path2);
	return 0;
}

/**
 * ext4_ext_clear_bb - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_ext_clear_bb(struct inode *inode)
{
	struct ext4_ext_path *path = NULL;
	struct ext4_extent *ex;
	ext4_lblk_t cur = 0, end;
	int j, ret = 0;
	struct ext4_map_blocks map;

	if (ext4_test_inode_flag(inode, EXT4_INODE_INLINE_DATA))
		return 0;


	path = ext4_find_extent(inode, EXT_MAX_BLOCKS - 1, NULL,
					EXT4_EX_NOCACHE);
	if (IS_ERR(path))
		return PTR_ERR(path);
	ex = path[path->p_depth].p_ext;
	if (!ex)
		goto out;
	end = le32_to_cpu(ex->ee_block) + ext4_ext_get_actual_len(ex);

	cur = 0;
	while (cur < end) {
		map.m_lblk = cur;
		map.m_len = end - cur;
		ret = ext4_map_blocks(NULL, inode, &map, 0);
		if (ret < 0)
			break;
		if (ret > 0) {
			path = ext4_find_extent(inode, map.m_lblk, path, 0);
			if (!IS_ERR(path)) {
				for (j = 0; j < path->p_depth; j++) {
					ext4_mb_mark_bb(inode->i_sb,
							path[j].p_block, 1, false);
					ext4_fc_record_regions(inode->i_sb, inode->i_ino,
							0, path[j].p_block, 1, 1);
				}
			} else {
				path = NULL;
			}
			ext4_mb_mark_bb(inode->i_sb, map.m_pblk, map.m_len, false);
			ext4_fc_record_regions(inode->i_sb, inode->i_ino,
					map.m_lblk, map.m_pblk, map.m_len, 1);
		}
		cur = cur + map.m_len;
	}

out:
	ext4_free_ext_path(path);
	return 0;
}
