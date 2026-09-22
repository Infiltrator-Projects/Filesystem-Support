// SPDX-License-Identifier: GPL-2.0+
/*
 * ext4_jbd2.h
 *
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1999
 *
 * Copyright 1998--1999 Red Hat corp --- All Rights Reserved
 *
 * Ext4-specific journaling extensions.
 */

/*
 * EXT4 — EXT4 journaling contract
 *
 * Purpose:
 *   Defines EXT4 journal credit calculations, handle helpers and private interfaces used to coordinate metadata updates with JBD2.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Credit estimates are correctness bounds as well as performance estimates: under-accounting can deadlock or force unexpected transaction restarts.
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

#ifndef _EXT4_JBD2_H
#define _EXT4_JBD2_H

#include <linux/fs.h>
#include <linux/jbd2.h>
#include "ext4.h"

#define EXT4_JOURNAL(inode)	(EXT4_SB((inode)->i_sb)->s_journal)


#define EXT4_SINGLEDATA_TRANS_BLOCKS(sb)				\
	(ext4_has_feature_extents(sb) ? 20U : 8U)


#define EXT4_XATTR_TRANS_BLOCKS		6U


#define EXT4_DATA_TRANS_BLOCKS(sb)	(EXT4_SINGLEDATA_TRANS_BLOCKS(sb) + \
					 EXT4_XATTR_TRANS_BLOCKS - 2 + \
					 EXT4_MAXQUOTAS_TRANS_BLOCKS(sb))


#define EXT4_META_TRANS_BLOCKS(sb)	(EXT4_XATTR_TRANS_BLOCKS + \
					EXT4_MAXQUOTAS_TRANS_BLOCKS(sb))


#define EXT4_MAX_TRANS_DATA		64U


#define EXT4_RESERVE_TRANS_BLOCKS	12U


#define EXT4_INDEX_EXTRA_TRANS_BLOCKS	12U

#ifdef CONFIG_QUOTA


#define EXT4_QUOTA_TRANS_BLOCKS(sb) ((ext4_quota_capable(sb)) ? 1 : 0)


#define EXT4_QUOTA_INIT_BLOCKS(sb) ((ext4_quota_capable(sb)) ?\
		(DQUOT_INIT_ALLOC*(EXT4_SINGLEDATA_TRANS_BLOCKS(sb)-3)\
		 +3+DQUOT_INIT_REWRITE) : 0)

#define EXT4_QUOTA_DEL_BLOCKS(sb) ((ext4_quota_capable(sb)) ?\
		(DQUOT_DEL_ALLOC*(EXT4_SINGLEDATA_TRANS_BLOCKS(sb)-3)\
		 +3+DQUOT_DEL_REWRITE) : 0)
#else
#define EXT4_QUOTA_TRANS_BLOCKS(sb) 0
#define EXT4_QUOTA_INIT_BLOCKS(sb) 0
#define EXT4_QUOTA_DEL_BLOCKS(sb) 0
#endif
#define EXT4_MAXQUOTAS_TRANS_BLOCKS(sb) (EXT4_MAXQUOTAS*EXT4_QUOTA_TRANS_BLOCKS(sb))
#define EXT4_MAXQUOTAS_INIT_BLOCKS(sb) (EXT4_MAXQUOTAS*EXT4_QUOTA_INIT_BLOCKS(sb))
#define EXT4_MAXQUOTAS_DEL_BLOCKS(sb) (EXT4_MAXQUOTAS*EXT4_QUOTA_DEL_BLOCKS(sb))


#define EXT4_HT_MISC             0
#define EXT4_HT_INODE            1
#define EXT4_HT_WRITE_PAGE       2
#define EXT4_HT_MAP_BLOCKS       3
#define EXT4_HT_DIR              4
#define EXT4_HT_TRUNCATE         5
#define EXT4_HT_QUOTA            6
#define EXT4_HT_RESIZE           7
#define EXT4_HT_MIGRATE          8
#define EXT4_HT_MOVE_EXTENTS     9
#define EXT4_HT_XATTR           10
#define EXT4_HT_EXT_CONVERT     11
#define EXT4_HT_MAX             12


struct ext4_journal_cb_entry {

	struct list_head jce_list;


	void (*jce_func)(struct super_block *sb,
			 struct ext4_journal_cb_entry *jce, int error);


};


/**
 * _ext4_journal_callback_add - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void _ext4_journal_callback_add(handle_t *handle,
			struct ext4_journal_cb_entry *jce)
{

	list_add_tail(&jce->jce_list, &handle->h_transaction->t_private_list);
}

/**
 * ext4_journal_callback_add - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_journal_callback_add(handle_t *handle,
			void (*func)(struct super_block *sb,
				     struct ext4_journal_cb_entry *jce,
				     int rc),
			struct ext4_journal_cb_entry *jce)
{
	struct ext4_sb_info *sbi =
			EXT4_SB(handle->h_transaction->t_journal->j_private);


	jce->jce_func = func;
	spin_lock(&sbi->s_md_lock);
	_ext4_journal_callback_add(handle, jce);
	spin_unlock(&sbi->s_md_lock);
}


/**
 * ext4_journal_callback_try_del - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline bool ext4_journal_callback_try_del(handle_t *handle,
					     struct ext4_journal_cb_entry *jce)
{
	bool deleted;
	struct ext4_sb_info *sbi =
			EXT4_SB(handle->h_transaction->t_journal->j_private);

	spin_lock(&sbi->s_md_lock);
	deleted = !list_empty(&jce->jce_list);
	list_del_init(&jce->jce_list);
	spin_unlock(&sbi->s_md_lock);
	return deleted;
}

int
ext4_mark_iloc_dirty(handle_t *handle,
		     struct inode *inode,
		     struct ext4_iloc *iloc);


int ext4_reserve_inode_write(handle_t *handle, struct inode *inode,
			struct ext4_iloc *iloc);

#define ext4_mark_inode_dirty(__h, __i)					\
		__ext4_mark_inode_dirty((__h), (__i), __func__, __LINE__)
int __ext4_mark_inode_dirty(handle_t *handle, struct inode *inode,
				const char *func, unsigned int line);

int ext4_expand_extra_isize(struct inode *inode,
			    unsigned int new_extra_isize,
			    struct ext4_iloc *iloc);


int __ext4_journal_get_write_access(const char *where, unsigned int line,
				    handle_t *handle, struct super_block *sb,
				    struct buffer_head *bh,
				    enum ext4_journal_trigger_type trigger_type);

int __ext4_forget(const char *where, unsigned int line, handle_t *handle,
		  int is_metadata, struct inode *inode,
		  struct buffer_head *bh, ext4_fsblk_t blocknr);

int __ext4_journal_get_create_access(const char *where, unsigned int line,
				handle_t *handle, struct super_block *sb,
				struct buffer_head *bh,
				enum ext4_journal_trigger_type trigger_type);

int __ext4_handle_dirty_metadata(const char *where, unsigned int line,
				 handle_t *handle, struct inode *inode,
				 struct buffer_head *bh);

#define ext4_journal_get_write_access(handle, sb, bh, trigger_type) \
	__ext4_journal_get_write_access(__func__, __LINE__, (handle), (sb), \
					(bh), (trigger_type))
#define ext4_forget(handle, is_metadata, inode, bh, block_nr) \
	__ext4_forget(__func__, __LINE__, (handle), (is_metadata), (inode), \
		      (bh), (block_nr))
#define ext4_journal_get_create_access(handle, sb, bh, trigger_type) \
	__ext4_journal_get_create_access(__func__, __LINE__, (handle), (sb), \
					 (bh), (trigger_type))
#define ext4_handle_dirty_metadata(handle, inode, bh) \
	__ext4_handle_dirty_metadata(__func__, __LINE__, (handle), (inode), \
				     (bh))

handle_t *__ext4_journal_start_sb(struct inode *inode, struct super_block *sb,
				  unsigned int line, int type, int blocks,
				  int rsv_blocks, int revoke_creds);
int __ext4_journal_stop(const char *where, unsigned int line, handle_t *handle);

#define EXT4_NOJOURNAL_MAX_REF_COUNT ((unsigned long) 4096)


/**
 * ext4_handle_valid - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_handle_valid(handle_t *handle)
{
	if ((unsigned long)handle < EXT4_NOJOURNAL_MAX_REF_COUNT)
		return 0;
	return 1;
}

/**
 * ext4_handle_sync - Drives pending state toward the durability guarantee required by the calling VFS or journal interface.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_handle_sync(handle_t *handle)
{
	if (ext4_handle_valid(handle))
		handle->h_sync = 1;
}

/**
 * ext4_handle_is_aborted - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_handle_is_aborted(handle_t *handle)
{
	if (ext4_handle_valid(handle))
		return is_handle_aborted(handle);
	return 0;
}

/**
 * ext4_free_metadata_revoke_credits - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_free_metadata_revoke_credits(struct super_block *sb,
						    int blocks)
{

	return blocks * EXT4_SB(sb)->s_cluster_ratio;
}

/**
 * ext4_trans_default_revoke_credits - Implements the trans default revoke credits operation within the ext4 journaling contract subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_trans_default_revoke_credits(struct super_block *sb)
{
	return ext4_free_metadata_revoke_credits(sb, 8);
}

#define ext4_journal_start_sb(sb, type, nblocks)			\
	__ext4_journal_start_sb(NULL, (sb), __LINE__, (type), (nblocks), 0,\
				ext4_trans_default_revoke_credits(sb))

#define ext4_journal_start(inode, type, nblocks)			\
	__ext4_journal_start((inode), __LINE__, (type), (nblocks), 0,	\
			     ext4_trans_default_revoke_credits((inode)->i_sb))

#define ext4_journal_start_with_reserve(inode, type, blocks, rsv_blocks)\
	__ext4_journal_start((inode), __LINE__, (type), (blocks), (rsv_blocks),\
			     ext4_trans_default_revoke_credits((inode)->i_sb))

#define ext4_journal_start_with_revoke(inode, type, blocks, revoke_creds) \
	__ext4_journal_start((inode), __LINE__, (type), (blocks), 0,	\
			     (revoke_creds))

/**
 * __ext4_journal_start - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline handle_t *__ext4_journal_start(struct inode *inode,
					     unsigned int line, int type,
					     int blocks, int rsv_blocks,
					     int revoke_creds)
{
	return __ext4_journal_start_sb(inode, inode->i_sb, line, type, blocks,
				       rsv_blocks, revoke_creds);
}

#define ext4_journal_stop(handle) \
	__ext4_journal_stop(__func__, __LINE__, (handle))

#define ext4_journal_start_reserved(handle, type) \
	__ext4_journal_start_reserved((handle), __LINE__, (type))

handle_t *__ext4_journal_start_reserved(handle_t *handle, unsigned int line,
					int type);

/**
 * ext4_journal_current_handle - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline handle_t *ext4_journal_current_handle(void)
{
	return journal_current_handle();
}

/**
 * ext4_journal_extend - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_journal_extend(handle_t *handle, int nblocks, int revoke)
{
	if (ext4_handle_valid(handle))
		return jbd2_journal_extend(handle, nblocks, revoke);
	return 0;
}

/**
 * ext4_journal_restart - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_journal_restart(handle_t *handle, int nblocks,
				       int revoke)
{
	if (ext4_handle_valid(handle))
		return jbd2__journal_restart(handle, nblocks, revoke, GFP_NOFS);
	return 0;
}

int __ext4_journal_ensure_credits(handle_t *handle, int check_cred,
				  int extend_cred, int revoke_cred);


#define ext4_journal_ensure_credits_fn(handle, check_cred, extend_cred,	\
				       revoke_cred, fn) \
({									\
	__label__ __ensure_end;						\
	int err = __ext4_journal_ensure_credits((handle), (check_cred),	\
					(extend_cred), (revoke_cred));	\
									\
	if (err <= 0)							\
		goto __ensure_end;					\
	err = (fn);							\
	if (err < 0)							\
		goto __ensure_end;					\
	err = ext4_journal_restart((handle), (extend_cred), (revoke_cred)); \
	if (err == 0)							\
		err = 1;						\
__ensure_end:								\
	err;								\
})


/**
 * ext4_journal_ensure_credits - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_journal_ensure_credits(handle_t *handle, int credits,
					      int revoke_creds)
{
	return ext4_journal_ensure_credits_fn(handle, credits, credits,
				revoke_creds, 0);
}

/**
 * ext4_journal_blocks_per_page - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_journal_blocks_per_page(struct inode *inode)
{
	if (EXT4_JOURNAL(inode) != NULL)
		return jbd2_journal_blocks_per_page(inode);
	return 0;
}

/**
 * ext4_journal_force_commit - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_journal_force_commit(journal_t *journal)
{
	if (journal)
		return jbd2_journal_force_commit(journal);
	return 0;
}

/**
 * ext4_jbd2_inode_add_write - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_jbd2_inode_add_write(handle_t *handle,
		struct inode *inode, loff_t start_byte, loff_t length)
{
	if (ext4_handle_valid(handle))
		return jbd2_journal_inode_ranged_write(handle,
				EXT4_I(inode)->jinode, start_byte, length);
	return 0;
}

/**
 * ext4_jbd2_inode_add_wait - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_jbd2_inode_add_wait(handle_t *handle,
		struct inode *inode, loff_t start_byte, loff_t length)
{
	if (ext4_handle_valid(handle))
		return jbd2_journal_inode_ranged_wait(handle,
				EXT4_I(inode)->jinode, start_byte, length);
	return 0;
}

/**
 * ext4_update_inode_fsync_trans - Drives pending state toward the durability guarantee required by the calling VFS or journal interface.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_update_inode_fsync_trans(handle_t *handle,
						 struct inode *inode,
						 int datasync)
{
	struct ext4_inode_info *ei = EXT4_I(inode);

	if (ext4_handle_valid(handle) && !is_handle_aborted(handle)) {
		ei->i_sync_tid = handle->h_transaction->t_tid;
		if (datasync)
			ei->i_datasync_tid = handle->h_transaction->t_tid;
	}
}


int ext4_force_commit(struct super_block *sb);


#define EXT4_INODE_JOURNAL_DATA_MODE	0x01
#define EXT4_INODE_ORDERED_DATA_MODE	0x02
#define EXT4_INODE_WRITEBACK_DATA_MODE	0x04

int ext4_inode_journal_mode(struct inode *inode);

/**
 * ext4_should_journal_data - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_should_journal_data(struct inode *inode)
{
	return ext4_inode_journal_mode(inode) & EXT4_INODE_JOURNAL_DATA_MODE;
}

/**
 * ext4_should_order_data - Implements the should order data operation within the ext4 journaling contract subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_should_order_data(struct inode *inode)
{
	return ext4_inode_journal_mode(inode) & EXT4_INODE_ORDERED_DATA_MODE;
}

/**
 * ext4_should_writeback_data - Implements the should writeback data operation within the ext4 journaling contract subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_should_writeback_data(struct inode *inode)
{
	return ext4_inode_journal_mode(inode) & EXT4_INODE_WRITEBACK_DATA_MODE;
}

/**
 * ext4_free_data_revoke_credits - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_free_data_revoke_credits(struct inode *inode, int blocks)
{
	if (test_opt(inode->i_sb, DATA_FLAGS) == EXT4_MOUNT_JOURNAL_DATA)
		return 0;
	if (!ext4_should_journal_data(inode))
		return 0;


	return blocks + 2*(EXT4_SB(inode->i_sb)->s_cluster_ratio - 1);
}


/**
 * ext4_should_dioread_nolock - Implements the should dioread nolock operation within the ext4 journaling contract subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_should_dioread_nolock(struct inode *inode)
{
	if (!test_opt(inode->i_sb, DIOREAD_NOLOCK))
		return 0;
	if (!S_ISREG(inode->i_mode))
		return 0;
	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)))
		return 0;
	if (ext4_should_journal_data(inode))
		return 0;

	if (!test_opt(inode->i_sb, DELALLOC))
		return 0;
	return 1;
}


/**
 * ext4_journal_destroy - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_journal_destroy(struct ext4_sb_info *sbi, journal_t *journal)
{
	int err = 0;


	ext4_set_mount_flag(sbi->s_sb, EXT4_MF_JOURNAL_DESTROY);

	ext4_force_commit(sbi->s_sb);
	flush_work(&sbi->s_sb_upd_work);

	err = jbd2_journal_destroy(journal);
	sbi->s_journal = NULL;

	return err;
}

#endif
