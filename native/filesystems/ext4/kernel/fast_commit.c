// SPDX-License-Identifier: GPL-2.0

/*
 * EXT4 — Fast-commit engine
 *
 * Purpose:
 *   Implements EXT4 fast-commit tracking, compact log emission and replay for eligible metadata changes.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Fast commit is an optimisation over full JBD2 commit: ineligibility or uncertainty must fall back to the full journal rather than weaken recovery.
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

#include "ext4.h"
#include "ext4_jbd2.h"
#include "ext4_extents.h"
#include "mballoc.h"


#include <trace/events/ext4.h>
static struct kmem_cache *ext4_fc_dentry_cachep;


/**
 * ext4_end_buffer_io_sync - Drives pending state toward the durability guarantee required by the calling VFS or journal interface.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_end_buffer_io_sync(struct buffer_head *bh, int uptodate)
{
	BUFFER_TRACE(bh, "");
	if (uptodate) {
		ext4_debug("%s: Block %lld up-to-date",
			   __func__, bh->b_blocknr);
		set_buffer_uptodate(bh);
	} else {
		ext4_debug("%s: Block %lld not up-to-date",
			   __func__, bh->b_blocknr);
		clear_buffer_uptodate(bh);
	}

	unlock_buffer(bh);
}


/**
 * ext4_fc_reset_inode - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_fc_reset_inode(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);

	ei->i_fc_lblk_start = 0;
	ei->i_fc_lblk_len = 0;
}


/**
 * ext4_fc_init_inode - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_init_inode(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);

	ext4_fc_reset_inode(inode);
	ext4_clear_inode_state(inode, EXT4_STATE_FC_COMMITTING);
	INIT_LIST_HEAD(&ei->i_fc_list);
	INIT_LIST_HEAD(&ei->i_fc_dilist);
	init_waitqueue_head(&ei->i_fc_wait);
	atomic_set(&ei->i_fc_updates, 0);
}


/**
 * __releases - Implements the releases operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_fc_wait_committing_inode(struct inode *inode)
__releases(&EXT4_SB(inode->i_sb)->s_fc_lock)
{
	wait_queue_head_t *wq;
	struct ext4_inode_info *ei = EXT4_I(inode);

#if (BITS_PER_LONG < 64)
	DEFINE_WAIT_BIT(wait, &ei->i_state_flags,
			EXT4_STATE_FC_COMMITTING);
	wq = bit_waitqueue(&ei->i_state_flags,
				EXT4_STATE_FC_COMMITTING);
#else
	DEFINE_WAIT_BIT(wait, &ei->i_flags,
			EXT4_STATE_FC_COMMITTING);
	wq = bit_waitqueue(&ei->i_flags,
				EXT4_STATE_FC_COMMITTING);
#endif
	lockdep_assert_held(&EXT4_SB(inode->i_sb)->s_fc_lock);
	prepare_to_wait(wq, &wait.wq_entry, TASK_UNINTERRUPTIBLE);
	spin_unlock(&EXT4_SB(inode->i_sb)->s_fc_lock);
	schedule();
	finish_wait(wq, &wait.wq_entry);
}


/**
 * ext4_fc_disabled - Implements the fc disabled operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool ext4_fc_disabled(struct super_block *sb)
{
	return (!test_opt2(sb, JOURNAL_FAST_COMMIT) ||
		(EXT4_SB(sb)->s_mount_state & EXT4_FC_REPLAY));
}


/**
 * ext4_fc_start_update - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_start_update(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);

	if (ext4_fc_disabled(inode->i_sb))
		return;

restart:
	spin_lock(&EXT4_SB(inode->i_sb)->s_fc_lock);
	if (list_empty(&ei->i_fc_list))
		goto out;

	if (ext4_test_inode_state(inode, EXT4_STATE_FC_COMMITTING)) {
		ext4_fc_wait_committing_inode(inode);
		goto restart;
	}
out:
	atomic_inc(&ei->i_fc_updates);
	spin_unlock(&EXT4_SB(inode->i_sb)->s_fc_lock);
}


/**
 * ext4_fc_stop_update - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_stop_update(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);

	if (ext4_fc_disabled(inode->i_sb))
		return;

	if (atomic_dec_and_test(&ei->i_fc_updates))
		wake_up_all(&ei->i_fc_wait);
}


/**
 * ext4_fc_del - Implements the fc del operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_del(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_fc_dentry_update *fc_dentry;

	if (ext4_fc_disabled(inode->i_sb))
		return;

restart:
	spin_lock(&EXT4_SB(inode->i_sb)->s_fc_lock);
	if (list_empty(&ei->i_fc_list) && list_empty(&ei->i_fc_dilist)) {
		spin_unlock(&EXT4_SB(inode->i_sb)->s_fc_lock);
		return;
	}

	if (ext4_test_inode_state(inode, EXT4_STATE_FC_COMMITTING)) {
		ext4_fc_wait_committing_inode(inode);
		goto restart;
	}

	if (!list_empty(&ei->i_fc_list))
		list_del_init(&ei->i_fc_list);


	if (list_empty(&ei->i_fc_dilist)) {
		spin_unlock(&sbi->s_fc_lock);
		return;
	}

	fc_dentry = list_first_entry(&ei->i_fc_dilist, struct ext4_fc_dentry_update, fcd_dilist);
	WARN_ON(fc_dentry->fcd_op != EXT4_FC_TAG_CREAT);
	list_del_init(&fc_dentry->fcd_list);
	list_del_init(&fc_dentry->fcd_dilist);

	WARN_ON(!list_empty(&ei->i_fc_dilist));
	spin_unlock(&sbi->s_fc_lock);

	if (fc_dentry->fcd_name.name &&
		fc_dentry->fcd_name.len > DNAME_INLINE_LEN)
		kfree(fc_dentry->fcd_name.name);
	kmem_cache_free(ext4_fc_dentry_cachep, fc_dentry);

	return;
}


/**
 * ext4_fc_mark_ineligible - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_mark_ineligible(struct super_block *sb, int reason, handle_t *handle)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	tid_t tid;
	bool has_transaction = true;
	bool is_ineligible;

	if (ext4_fc_disabled(sb))
		return;

	if (handle && !IS_ERR(handle))
		tid = handle->h_transaction->t_tid;
	else {
		read_lock(&sbi->s_journal->j_state_lock);
		if (sbi->s_journal->j_running_transaction)
			tid = sbi->s_journal->j_running_transaction->t_tid;
		else
			has_transaction = false;
		read_unlock(&sbi->s_journal->j_state_lock);
	}
	spin_lock(&sbi->s_fc_lock);
	is_ineligible = ext4_test_mount_flag(sb, EXT4_MF_FC_INELIGIBLE);
	if (has_transaction &&
	    (!is_ineligible ||
	     (is_ineligible && tid_gt(tid, sbi->s_fc_ineligible_tid))))
		sbi->s_fc_ineligible_tid = tid;
	ext4_set_mount_flag(sb, EXT4_MF_FC_INELIGIBLE);
	spin_unlock(&sbi->s_fc_lock);
	WARN_ON(reason >= EXT4_FC_REASON_MAX);
	sbi->s_fc_stats.fc_ineligible_reason_count[reason]++;
}


/**
 * ext4_fc_track_template - Implements the fc track template operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_track_template(
	handle_t *handle, struct inode *inode,
	int (*__fc_track_fn)(handle_t *handle, struct inode *, void *, bool),
	void *args, int enqueue)
{
	bool update = false;
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	tid_t tid = 0;
	int ret;

	tid = handle->h_transaction->t_tid;
	mutex_lock(&ei->i_fc_lock);
	if (tid == ei->i_sync_tid) {
		update = true;
	} else {
		ext4_fc_reset_inode(inode);
		ei->i_sync_tid = tid;
	}
	ret = __fc_track_fn(handle, inode, args, update);
	mutex_unlock(&ei->i_fc_lock);

	if (!enqueue)
		return ret;

	spin_lock(&sbi->s_fc_lock);
	if (list_empty(&EXT4_I(inode)->i_fc_list))
		list_add_tail(&EXT4_I(inode)->i_fc_list,
				(sbi->s_journal->j_flags & JBD2_FULL_COMMIT_ONGOING ||
				 sbi->s_journal->j_flags & JBD2_FAST_COMMIT_ONGOING) ?
				&sbi->s_fc_q[FC_Q_STAGING] :
				&sbi->s_fc_q[FC_Q_MAIN]);
	spin_unlock(&sbi->s_fc_lock);

	return ret;
}


/**
 * struct __track_dentry_update_args - Private EXT4 state/data structure used by fast-commit engine.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct __track_dentry_update_args {
	struct dentry *dentry;
	int op;
};


/**
 * __track_dentry_update - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __track_dentry_update(handle_t *handle, struct inode *inode,
				 void *arg, bool update)
{
	struct ext4_fc_dentry_update *node;
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct __track_dentry_update_args *dentry_update =
		(struct __track_dentry_update_args *)arg;
	struct dentry *dentry = dentry_update->dentry;
	struct inode *dir = dentry->d_parent->d_inode;
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	mutex_unlock(&ei->i_fc_lock);

	if (IS_ENCRYPTED(dir)) {
		ext4_fc_mark_ineligible(sb, EXT4_FC_REASON_ENCRYPTED_FILENAME,
					handle);
		mutex_lock(&ei->i_fc_lock);
		return -EOPNOTSUPP;
	}

	node = kmem_cache_alloc(ext4_fc_dentry_cachep, GFP_NOFS);
	if (!node) {
		ext4_fc_mark_ineligible(sb, EXT4_FC_REASON_NOMEM, handle);
		mutex_lock(&ei->i_fc_lock);
		return -ENOMEM;
	}

	node->fcd_op = dentry_update->op;
	node->fcd_parent = dir->i_ino;
	node->fcd_ino = inode->i_ino;
	if (dentry->d_name.len > DNAME_INLINE_LEN) {
		node->fcd_name.name = kmalloc(dentry->d_name.len, GFP_NOFS);
		if (!node->fcd_name.name) {
			kmem_cache_free(ext4_fc_dentry_cachep, node);
			ext4_fc_mark_ineligible(sb, EXT4_FC_REASON_NOMEM, handle);
			mutex_lock(&ei->i_fc_lock);
			return -ENOMEM;
		}
		memcpy((u8 *)node->fcd_name.name, dentry->d_name.name,
			dentry->d_name.len);
	} else {
		memcpy(node->fcd_iname, dentry->d_name.name,
			dentry->d_name.len);
		node->fcd_name.name = node->fcd_iname;
	}
	node->fcd_name.len = dentry->d_name.len;
	INIT_LIST_HEAD(&node->fcd_dilist);
	spin_lock(&sbi->s_fc_lock);
	if (sbi->s_journal->j_flags & JBD2_FULL_COMMIT_ONGOING ||
		sbi->s_journal->j_flags & JBD2_FAST_COMMIT_ONGOING)
		list_add_tail(&node->fcd_list,
				&sbi->s_fc_dentry_q[FC_Q_STAGING]);
	else
		list_add_tail(&node->fcd_list, &sbi->s_fc_dentry_q[FC_Q_MAIN]);


	if (dentry_update->op == EXT4_FC_TAG_CREAT) {
		WARN_ON(!list_empty(&ei->i_fc_dilist));
		list_add_tail(&node->fcd_dilist, &ei->i_fc_dilist);
	}
	spin_unlock(&sbi->s_fc_lock);
	mutex_lock(&ei->i_fc_lock);

	return 0;
}


/**
 * __ext4_fc_track_unlink - Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void __ext4_fc_track_unlink(handle_t *handle,
		struct inode *inode, struct dentry *dentry)
{
	struct __track_dentry_update_args args;
	int ret;

	args.dentry = dentry;
	args.op = EXT4_FC_TAG_UNLINK;

	ret = ext4_fc_track_template(handle, inode, __track_dentry_update,
					(void *)&args, 0);
	trace_ext4_fc_track_unlink(handle, inode, dentry, ret);
}


/**
 * ext4_fc_track_unlink - Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_track_unlink(handle_t *handle, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);

	if (ext4_fc_disabled(inode->i_sb))
		return;

	if (ext4_test_mount_flag(inode->i_sb, EXT4_MF_FC_INELIGIBLE))
		return;

	__ext4_fc_track_unlink(handle, inode, dentry);
}


/**
 * __ext4_fc_track_link - Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void __ext4_fc_track_link(handle_t *handle,
	struct inode *inode, struct dentry *dentry)
{
	struct __track_dentry_update_args args;
	int ret;

	args.dentry = dentry;
	args.op = EXT4_FC_TAG_LINK;

	ret = ext4_fc_track_template(handle, inode, __track_dentry_update,
					(void *)&args, 0);
	trace_ext4_fc_track_link(handle, inode, dentry, ret);
}


/**
 * ext4_fc_track_link - Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_track_link(handle_t *handle, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);

	if (ext4_fc_disabled(inode->i_sb))
		return;

	if (ext4_test_mount_flag(inode->i_sb, EXT4_MF_FC_INELIGIBLE))
		return;

	__ext4_fc_track_link(handle, inode, dentry);
}


/**
 * __ext4_fc_track_create - Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void __ext4_fc_track_create(handle_t *handle, struct inode *inode,
			  struct dentry *dentry)
{
	struct __track_dentry_update_args args;
	int ret;

	args.dentry = dentry;
	args.op = EXT4_FC_TAG_CREAT;

	ret = ext4_fc_track_template(handle, inode, __track_dentry_update,
					(void *)&args, 0);
	trace_ext4_fc_track_create(handle, inode, dentry, ret);
}


/**
 * ext4_fc_track_create - Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_track_create(handle_t *handle, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);

	if (ext4_fc_disabled(inode->i_sb))
		return;

	if (ext4_test_mount_flag(inode->i_sb, EXT4_MF_FC_INELIGIBLE))
		return;

	__ext4_fc_track_create(handle, inode, dentry);
}


/**
 * __track_inode - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __track_inode(handle_t *handle, struct inode *inode, void *arg,
			 bool update)
{
	if (update)
		return -EEXIST;

	EXT4_I(inode)->i_fc_lblk_len = 0;

	return 0;
}


/**
 * ext4_fc_track_inode - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_track_inode(handle_t *handle, struct inode *inode)
{
	int ret;

	if (S_ISDIR(inode->i_mode))
		return;

	if (ext4_fc_disabled(inode->i_sb))
		return;

	if (ext4_should_journal_data(inode)) {
		ext4_fc_mark_ineligible(inode->i_sb,
					EXT4_FC_REASON_INODE_JOURNAL_DATA, handle);
		return;
	}

	if (ext4_test_mount_flag(inode->i_sb, EXT4_MF_FC_INELIGIBLE))
		return;

	ret = ext4_fc_track_template(handle, inode, __track_inode, NULL, 1);
	trace_ext4_fc_track_inode(handle, inode, ret);
}


/**
 * struct __track_range_args - Private EXT4 state/data structure used by fast-commit engine.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct __track_range_args {
	ext4_lblk_t start, end;
};


/**
 * __track_range - Implements the track range operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __track_range(handle_t *handle, struct inode *inode, void *arg,
			 bool update)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	ext4_lblk_t oldstart;
	struct __track_range_args *__arg =
		(struct __track_range_args *)arg;

	if (inode->i_ino < EXT4_FIRST_INO(inode->i_sb)) {
		ext4_debug("Special inode %ld being modified\n", inode->i_ino);
		return -ECANCELED;
	}

	oldstart = ei->i_fc_lblk_start;

	if (update && ei->i_fc_lblk_len > 0) {
		ei->i_fc_lblk_start = min(ei->i_fc_lblk_start, __arg->start);
		ei->i_fc_lblk_len =
			max(oldstart + ei->i_fc_lblk_len - 1, __arg->end) -
				ei->i_fc_lblk_start + 1;
	} else {
		ei->i_fc_lblk_start = __arg->start;
		ei->i_fc_lblk_len = __arg->end - __arg->start + 1;
	}

	return 0;
}


/**
 * ext4_fc_track_range - Implements the fc track range operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_track_range(handle_t *handle, struct inode *inode, ext4_lblk_t start,
			 ext4_lblk_t end)
{
	struct __track_range_args args;
	int ret;

	if (S_ISDIR(inode->i_mode))
		return;

	if (ext4_fc_disabled(inode->i_sb))
		return;

	if (ext4_test_mount_flag(inode->i_sb, EXT4_MF_FC_INELIGIBLE))
		return;

	if (ext4_has_inline_data(inode)) {
		ext4_fc_mark_ineligible(inode->i_sb, EXT4_FC_REASON_XATTR,
					handle);
		return;
	}

	args.start = start;
	args.end = end;

	ret = ext4_fc_track_template(handle, inode,  __track_range, &args, 1);

	trace_ext4_fc_track_range(handle, inode, start, end, ret);
}


/**
 * ext4_fc_submit_bh - Implements the fc submit bh operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_fc_submit_bh(struct super_block *sb, bool is_tail)
{
	blk_opf_t write_flags = JBD2_JOURNAL_REQ_FLAGS;
	struct buffer_head *bh = EXT4_SB(sb)->s_fc_bh;


	if (test_opt(sb, BARRIER) && is_tail)
		write_flags |= REQ_FUA | REQ_PREFLUSH;
	lock_buffer(bh);
	set_buffer_dirty(bh);
	set_buffer_uptodate(bh);
	bh->b_end_io = ext4_end_buffer_io_sync;
	submit_bh(REQ_OP_WRITE | write_flags, bh);
	EXT4_SB(sb)->s_fc_bh = NULL;
}


/**
 * ext4_fc_reserve_space - Implements the fc reserve space operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static u8 *ext4_fc_reserve_space(struct super_block *sb, int len, u32 *crc)
{
	struct ext4_fc_tl tl;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct buffer_head *bh;
	int bsize = sbi->s_journal->j_blocksize;
	int ret, off = sbi->s_fc_bytes % bsize;
	int remaining;
	u8 *dst;


	if (len > bsize - EXT4_FC_TAG_BASE_LEN)
		return NULL;

	if (!sbi->s_fc_bh) {
		ret = jbd2_fc_get_buf(EXT4_SB(sb)->s_journal, &bh);
		if (ret)
			return NULL;
		sbi->s_fc_bh = bh;
	}
	dst = sbi->s_fc_bh->b_data + off;


	remaining = bsize - EXT4_FC_TAG_BASE_LEN - off;
	if (len <= remaining) {
		sbi->s_fc_bytes += len;
		return dst;
	}


	tl.fc_tag = cpu_to_le16(EXT4_FC_TAG_PAD);
	tl.fc_len = cpu_to_le16(remaining);
	memcpy(dst, &tl, EXT4_FC_TAG_BASE_LEN);
	memset(dst + EXT4_FC_TAG_BASE_LEN, 0, remaining);
	*crc = ext4_chksum(sbi, *crc, sbi->s_fc_bh->b_data, bsize);

	ext4_fc_submit_bh(sb, false);

	ret = jbd2_fc_get_buf(EXT4_SB(sb)->s_journal, &bh);
	if (ret)
		return NULL;
	sbi->s_fc_bh = bh;
	sbi->s_fc_bytes += bsize - off + len;
	return sbi->s_fc_bh->b_data;
}


/**
 * ext4_fc_write_tail - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_write_tail(struct super_block *sb, u32 crc)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_fc_tl tl;
	struct ext4_fc_tail tail;
	int off, bsize = sbi->s_journal->j_blocksize;
	u8 *dst;


	dst = ext4_fc_reserve_space(sb, EXT4_FC_TAG_BASE_LEN + sizeof(tail), &crc);
	if (!dst)
		return -ENOSPC;

	off = sbi->s_fc_bytes % bsize;

	tl.fc_tag = cpu_to_le16(EXT4_FC_TAG_TAIL);
	tl.fc_len = cpu_to_le16(bsize - off + sizeof(struct ext4_fc_tail));
	sbi->s_fc_bytes = round_up(sbi->s_fc_bytes, bsize);

	memcpy(dst, &tl, EXT4_FC_TAG_BASE_LEN);
	dst += EXT4_FC_TAG_BASE_LEN;
	tail.fc_tid = cpu_to_le32(sbi->s_journal->j_running_transaction->t_tid);
	memcpy(dst, &tail.fc_tid, sizeof(tail.fc_tid));
	dst += sizeof(tail.fc_tid);
	crc = ext4_chksum(sbi, crc, sbi->s_fc_bh->b_data,
			  dst - (u8 *)sbi->s_fc_bh->b_data);
	tail.fc_crc = cpu_to_le32(crc);
	memcpy(dst, &tail.fc_crc, sizeof(tail.fc_crc));
	dst += sizeof(tail.fc_crc);
	memset(dst, 0, bsize - off);

	ext4_fc_submit_bh(sb, true);

	return 0;
}


/**
 * ext4_fc_add_tlv - Implements the fc add tlv operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool ext4_fc_add_tlv(struct super_block *sb, u16 tag, u16 len, u8 *val,
			   u32 *crc)
{
	struct ext4_fc_tl tl;
	u8 *dst;

	dst = ext4_fc_reserve_space(sb, EXT4_FC_TAG_BASE_LEN + len, crc);
	if (!dst)
		return false;

	tl.fc_tag = cpu_to_le16(tag);
	tl.fc_len = cpu_to_le16(len);

	memcpy(dst, &tl, EXT4_FC_TAG_BASE_LEN);
	memcpy(dst + EXT4_FC_TAG_BASE_LEN, val, len);

	return true;
}


/**
 * ext4_fc_add_dentry_tlv - Implements the fc add dentry tlv operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool ext4_fc_add_dentry_tlv(struct super_block *sb, u32 *crc,
				   struct ext4_fc_dentry_update *fc_dentry)
{
	struct ext4_fc_dentry_info fcd;
	struct ext4_fc_tl tl;
	int dlen = fc_dentry->fcd_name.len;
	u8 *dst = ext4_fc_reserve_space(sb,
			EXT4_FC_TAG_BASE_LEN + sizeof(fcd) + dlen, crc);

	if (!dst)
		return false;

	fcd.fc_parent_ino = cpu_to_le32(fc_dentry->fcd_parent);
	fcd.fc_ino = cpu_to_le32(fc_dentry->fcd_ino);
	tl.fc_tag = cpu_to_le16(fc_dentry->fcd_op);
	tl.fc_len = cpu_to_le16(sizeof(fcd) + dlen);
	memcpy(dst, &tl, EXT4_FC_TAG_BASE_LEN);
	dst += EXT4_FC_TAG_BASE_LEN;
	memcpy(dst, &fcd, sizeof(fcd));
	dst += sizeof(fcd);
	memcpy(dst, fc_dentry->fcd_name.name, dlen);

	return true;
}


/**
 * ext4_fc_write_inode - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_write_inode(struct inode *inode, u32 *crc)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	int inode_len = EXT4_GOOD_OLD_INODE_SIZE;
	int ret;
	struct ext4_iloc iloc;
	struct ext4_fc_inode fc_inode;
	struct ext4_fc_tl tl;
	u8 *dst;

	ret = ext4_get_inode_loc(inode, &iloc);
	if (ret)
		return ret;

	if (ext4_test_inode_flag(inode, EXT4_INODE_INLINE_DATA))
		inode_len = EXT4_INODE_SIZE(inode->i_sb);
	else if (EXT4_INODE_SIZE(inode->i_sb) > EXT4_GOOD_OLD_INODE_SIZE)
		inode_len += ei->i_extra_isize;

	fc_inode.fc_ino = cpu_to_le32(inode->i_ino);
	tl.fc_tag = cpu_to_le16(EXT4_FC_TAG_INODE);
	tl.fc_len = cpu_to_le16(inode_len + sizeof(fc_inode.fc_ino));

	ret = -ECANCELED;
	dst = ext4_fc_reserve_space(inode->i_sb,
		EXT4_FC_TAG_BASE_LEN + inode_len + sizeof(fc_inode.fc_ino), crc);
	if (!dst)
		goto err;

	memcpy(dst, &tl, EXT4_FC_TAG_BASE_LEN);
	dst += EXT4_FC_TAG_BASE_LEN;
	memcpy(dst, &fc_inode, sizeof(fc_inode));
	dst += sizeof(fc_inode);
	memcpy(dst, (u8 *)ext4_raw_inode(&iloc), inode_len);
	ret = 0;
err:
	brelse(iloc.bh);
	return ret;
}


/**
 * ext4_fc_write_inode_data - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_write_inode_data(struct inode *inode, u32 *crc)
{
	ext4_lblk_t old_blk_size, cur_lblk_off, new_blk_size;
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_map_blocks map;
	struct ext4_fc_add_range fc_ext;
	struct ext4_fc_del_range lrange;
	struct ext4_extent *ex;
	int ret;

	mutex_lock(&ei->i_fc_lock);
	if (ei->i_fc_lblk_len == 0) {
		mutex_unlock(&ei->i_fc_lock);
		return 0;
	}
	old_blk_size = ei->i_fc_lblk_start;
	new_blk_size = ei->i_fc_lblk_start + ei->i_fc_lblk_len - 1;
	ei->i_fc_lblk_len = 0;
	mutex_unlock(&ei->i_fc_lock);

	cur_lblk_off = old_blk_size;
	ext4_debug("will try writing %d to %d for inode %ld\n",
		   cur_lblk_off, new_blk_size, inode->i_ino);

	while (cur_lblk_off <= new_blk_size) {
		map.m_lblk = cur_lblk_off;
		map.m_len = new_blk_size - cur_lblk_off + 1;
		ret = ext4_map_blocks(NULL, inode, &map, 0);
		if (ret < 0)
			return -ECANCELED;

		if (map.m_len == 0) {
			cur_lblk_off++;
			continue;
		}

		if (ret == 0) {
			lrange.fc_ino = cpu_to_le32(inode->i_ino);
			lrange.fc_lblk = cpu_to_le32(map.m_lblk);
			lrange.fc_len = cpu_to_le32(map.m_len);
			if (!ext4_fc_add_tlv(inode->i_sb, EXT4_FC_TAG_DEL_RANGE,
					    sizeof(lrange), (u8 *)&lrange, crc))
				return -ENOSPC;
		} else {
			unsigned int max = (map.m_flags & EXT4_MAP_UNWRITTEN) ?
				EXT_UNWRITTEN_MAX_LEN : EXT_INIT_MAX_LEN;


			map.m_len = min(max, map.m_len);

			fc_ext.fc_ino = cpu_to_le32(inode->i_ino);
			ex = (struct ext4_extent *)&fc_ext.fc_ex;
			ex->ee_block = cpu_to_le32(map.m_lblk);
			ex->ee_len = cpu_to_le16(map.m_len);
			ext4_ext_store_pblock(ex, map.m_pblk);
			if (map.m_flags & EXT4_MAP_UNWRITTEN)
				ext4_ext_mark_unwritten(ex);
			else
				ext4_ext_mark_initialized(ex);
			if (!ext4_fc_add_tlv(inode->i_sb, EXT4_FC_TAG_ADD_RANGE,
					    sizeof(fc_ext), (u8 *)&fc_ext, crc))
				return -ENOSPC;
		}

		cur_lblk_off += map.m_len;
	}

	return 0;
}


/**
 * ext4_fc_submit_inode_data_all - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_submit_inode_data_all(journal_t *journal)
{
	struct super_block *sb = journal->j_private;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_inode_info *ei;
	int ret = 0;

	spin_lock(&sbi->s_fc_lock);
	list_for_each_entry(ei, &sbi->s_fc_q[FC_Q_MAIN], i_fc_list) {
		ext4_set_inode_state(&ei->vfs_inode, EXT4_STATE_FC_COMMITTING);
		while (atomic_read(&ei->i_fc_updates)) {
			DEFINE_WAIT(wait);

			prepare_to_wait(&ei->i_fc_wait, &wait,
						TASK_UNINTERRUPTIBLE);
			if (atomic_read(&ei->i_fc_updates)) {
				spin_unlock(&sbi->s_fc_lock);
				schedule();
				spin_lock(&sbi->s_fc_lock);
			}
			finish_wait(&ei->i_fc_wait, &wait);
		}
		spin_unlock(&sbi->s_fc_lock);
		ret = jbd2_submit_inode_data(journal, READ_ONCE(ei->jinode));
		if (ret)
			return ret;
		spin_lock(&sbi->s_fc_lock);
	}
	spin_unlock(&sbi->s_fc_lock);

	return ret;
}


/**
 * ext4_fc_wait_inode_data_all - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_wait_inode_data_all(journal_t *journal)
{
	struct super_block *sb = journal->j_private;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_inode_info *pos, *n;
	int ret = 0;

	spin_lock(&sbi->s_fc_lock);
	list_for_each_entry_safe(pos, n, &sbi->s_fc_q[FC_Q_MAIN], i_fc_list) {
		if (!ext4_test_inode_state(&pos->vfs_inode,
					   EXT4_STATE_FC_COMMITTING))
			continue;
		spin_unlock(&sbi->s_fc_lock);

		ret = jbd2_wait_inode_data(journal, READ_ONCE(pos->jinode));
		if (ret)
			return ret;
		spin_lock(&sbi->s_fc_lock);
	}
	spin_unlock(&sbi->s_fc_lock);

	return 0;
}


/**
 * __releases - Implements the releases operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_commit_dentry_updates(journal_t *journal, u32 *crc)
__acquires(&sbi->s_fc_lock)
__releases(&sbi->s_fc_lock)
{
	struct super_block *sb = journal->j_private;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_fc_dentry_update *fc_dentry, *fc_dentry_n;
	struct inode *inode;
	struct ext4_inode_info *ei;
	int ret;

	if (list_empty(&sbi->s_fc_dentry_q[FC_Q_MAIN]))
		return 0;
	list_for_each_entry_safe(fc_dentry, fc_dentry_n,
				 &sbi->s_fc_dentry_q[FC_Q_MAIN], fcd_list) {
		if (fc_dentry->fcd_op != EXT4_FC_TAG_CREAT) {
			spin_unlock(&sbi->s_fc_lock);
			if (!ext4_fc_add_dentry_tlv(sb, crc, fc_dentry)) {
				ret = -ENOSPC;
				goto lock_and_exit;
			}
			spin_lock(&sbi->s_fc_lock);
			continue;
		}


		WARN_ON(list_empty(&fc_dentry->fcd_dilist));
		ei = list_first_entry(&fc_dentry->fcd_dilist,
				struct ext4_inode_info, i_fc_dilist);
		inode = &ei->vfs_inode;
		WARN_ON(inode->i_ino != fc_dentry->fcd_ino);

		spin_unlock(&sbi->s_fc_lock);


		ret = ext4_fc_write_inode(inode, crc);
		if (ret)
			goto lock_and_exit;

		ret = ext4_fc_write_inode_data(inode, crc);
		if (ret)
			goto lock_and_exit;

		if (!ext4_fc_add_dentry_tlv(sb, crc, fc_dentry)) {
			ret = -ENOSPC;
			goto lock_and_exit;
		}

		spin_lock(&sbi->s_fc_lock);
	}
	return 0;
lock_and_exit:
	spin_lock(&sbi->s_fc_lock);
	return ret;
}


/**
 * ext4_fc_perform_commit - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_perform_commit(journal_t *journal)
{
	struct super_block *sb = journal->j_private;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_inode_info *iter;
	struct ext4_fc_head head;
	struct inode *inode;
	struct blk_plug plug;
	int ret = 0;
	u32 crc = 0;

	ret = ext4_fc_submit_inode_data_all(journal);
	if (ret)
		return ret;

	ret = ext4_fc_wait_inode_data_all(journal);
	if (ret)
		return ret;


	if (journal->j_fs_dev != journal->j_dev)
		blkdev_issue_flush(journal->j_fs_dev);

	blk_start_plug(&plug);
	if (sbi->s_fc_bytes == 0) {


		head.fc_features = cpu_to_le32(EXT4_FC_SUPPORTED_FEATURES);
		head.fc_tid = cpu_to_le32(
			sbi->s_journal->j_running_transaction->t_tid);
		if (!ext4_fc_add_tlv(sb, EXT4_FC_TAG_HEAD, sizeof(head),
			(u8 *)&head, &crc)) {
			ret = -ENOSPC;
			goto out;
		}
	}

	spin_lock(&sbi->s_fc_lock);
	ret = ext4_fc_commit_dentry_updates(journal, &crc);
	if (ret) {
		spin_unlock(&sbi->s_fc_lock);
		goto out;
	}

	list_for_each_entry(iter, &sbi->s_fc_q[FC_Q_MAIN], i_fc_list) {
		inode = &iter->vfs_inode;
		if (!ext4_test_inode_state(inode, EXT4_STATE_FC_COMMITTING))
			continue;

		spin_unlock(&sbi->s_fc_lock);
		ret = ext4_fc_write_inode_data(inode, &crc);
		if (ret)
			goto out;
		ret = ext4_fc_write_inode(inode, &crc);
		if (ret)
			goto out;
		spin_lock(&sbi->s_fc_lock);
	}
	spin_unlock(&sbi->s_fc_lock);

	ret = ext4_fc_write_tail(sb, crc);

out:
	blk_finish_plug(&plug);
	return ret;
}


/**
 * ext4_fc_update_stats - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_fc_update_stats(struct super_block *sb, int status,
				 u64 commit_time, int nblks, tid_t commit_tid)
{
	struct ext4_fc_stats *stats = &EXT4_SB(sb)->s_fc_stats;

	ext4_debug("Fast commit ended with status = %d for tid %u",
			status, commit_tid);
	if (status == EXT4_FC_STATUS_OK) {
		stats->fc_num_commits++;
		stats->fc_numblks += nblks;
		if (likely(stats->s_fc_avg_commit_time))
			stats->s_fc_avg_commit_time =
				(commit_time +
				 stats->s_fc_avg_commit_time * 3) / 4;
		else
			stats->s_fc_avg_commit_time = commit_time;
	} else if (status == EXT4_FC_STATUS_FAILED ||
		   status == EXT4_FC_STATUS_INELIGIBLE) {
		if (status == EXT4_FC_STATUS_FAILED)
			stats->fc_failed_commits++;
		stats->fc_ineligible_commits++;
	} else {
		stats->fc_skipped_commits++;
	}
	trace_ext4_fc_commit_stop(sb, nblks, status, commit_tid);
}


/**
 * ext4_fc_commit - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_fc_commit(journal_t *journal, tid_t commit_tid)
{
	struct super_block *sb = journal->j_private;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int nblks = 0, ret, bsize = journal->j_blocksize;
	int subtid = atomic_read(&sbi->s_fc_subtid);
	int status = EXT4_FC_STATUS_OK, fc_bufs_before = 0;
	ktime_t start_time, commit_time;

	if (!test_opt2(sb, JOURNAL_FAST_COMMIT))
		return jbd2_complete_transaction(journal, commit_tid);

	trace_ext4_fc_commit_start(sb, commit_tid);

	start_time = ktime_get();

restart_fc:
	ret = jbd2_fc_begin_commit(journal, commit_tid);
	if (ret == -EALREADY) {

		if (atomic_read(&sbi->s_fc_subtid) <= subtid &&
		    tid_gt(commit_tid, journal->j_commit_sequence))
			goto restart_fc;
		ext4_fc_update_stats(sb, EXT4_FC_STATUS_SKIPPED, 0, 0,
				commit_tid);
		return 0;
	} else if (ret) {


		ext4_fc_update_stats(sb, EXT4_FC_STATUS_FAILED, 0, 0,
				commit_tid);
		return jbd2_complete_transaction(journal, commit_tid);
	}


	if (ext4_test_mount_flag(sb, EXT4_MF_FC_INELIGIBLE)) {
		status = EXT4_FC_STATUS_INELIGIBLE;
		goto fallback;
	}

	fc_bufs_before = (sbi->s_fc_bytes + bsize - 1) / bsize;
	ret = ext4_fc_perform_commit(journal);
	if (ret < 0) {
		status = EXT4_FC_STATUS_FAILED;
		goto fallback;
	}
	nblks = (sbi->s_fc_bytes + bsize - 1) / bsize - fc_bufs_before;
	ret = jbd2_fc_wait_bufs(journal, nblks);
	if (ret < 0) {
		status = EXT4_FC_STATUS_FAILED;
		goto fallback;
	}
	atomic_inc(&sbi->s_fc_subtid);
	ret = jbd2_fc_end_commit(journal);


	commit_time = ktime_to_ns(ktime_sub(ktime_get(), start_time));
	ext4_fc_update_stats(sb, status, commit_time, nblks, commit_tid);
	return ret;

fallback:
	ret = jbd2_fc_end_commit_fallback(journal);
	ext4_fc_update_stats(sb, status, 0, 0, commit_tid);
	return ret;
}


/**
 * ext4_fc_cleanup - Implements the fc cleanup operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_fc_cleanup(journal_t *journal, int full, tid_t tid)
{
	struct super_block *sb = journal->j_private;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_inode_info *iter, *iter_n;
	struct ext4_fc_dentry_update *fc_dentry;

	if (full && sbi->s_fc_bh)
		sbi->s_fc_bh = NULL;

	trace_ext4_fc_cleanup(journal, full, tid);
	jbd2_fc_release_bufs(journal);

	spin_lock(&sbi->s_fc_lock);
	list_for_each_entry_safe(iter, iter_n, &sbi->s_fc_q[FC_Q_MAIN],
				 i_fc_list) {
		list_del_init(&iter->i_fc_list);
		ext4_clear_inode_state(&iter->vfs_inode,
				       EXT4_STATE_FC_COMMITTING);
		if (tid_geq(tid, iter->i_sync_tid)) {
			ext4_fc_reset_inode(&iter->vfs_inode);
		} else if (full) {


			list_add_tail(&EXT4_I(&iter->vfs_inode)->i_fc_list,
				      &sbi->s_fc_q[FC_Q_STAGING]);
		}

		smp_mb();
#if (BITS_PER_LONG < 64)
		wake_up_bit(&iter->i_state_flags, EXT4_STATE_FC_COMMITTING);
#else
		wake_up_bit(&iter->i_flags, EXT4_STATE_FC_COMMITTING);
#endif
	}

	while (!list_empty(&sbi->s_fc_dentry_q[FC_Q_MAIN])) {
		fc_dentry = list_first_entry(&sbi->s_fc_dentry_q[FC_Q_MAIN],
					     struct ext4_fc_dentry_update,
					     fcd_list);
		list_del_init(&fc_dentry->fcd_list);
		list_del_init(&fc_dentry->fcd_dilist);
		spin_unlock(&sbi->s_fc_lock);

		if (fc_dentry->fcd_name.name &&
			fc_dentry->fcd_name.len > DNAME_INLINE_LEN)
			kfree(fc_dentry->fcd_name.name);
		kmem_cache_free(ext4_fc_dentry_cachep, fc_dentry);
		spin_lock(&sbi->s_fc_lock);
	}

	list_splice_init(&sbi->s_fc_dentry_q[FC_Q_STAGING],
				&sbi->s_fc_dentry_q[FC_Q_MAIN]);
	list_splice_init(&sbi->s_fc_q[FC_Q_STAGING],
				&sbi->s_fc_q[FC_Q_MAIN]);

	if (tid_geq(tid, sbi->s_fc_ineligible_tid)) {
		sbi->s_fc_ineligible_tid = 0;
		ext4_clear_mount_flag(sb, EXT4_MF_FC_INELIGIBLE);
	}

	if (full)
		sbi->s_fc_bytes = 0;
	spin_unlock(&sbi->s_fc_lock);
	trace_ext4_fc_stats(sb);
}


/**
 * struct dentry_info_args - Private EXT4 state/data structure used by fast-commit engine.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct dentry_info_args {
	int parent_ino, dname_len, ino, inode_len;
	char *dname;
};


/**
 * struct ext4_fc_tl_mem - Private EXT4 state/data structure used by fast-commit engine.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fc_tl_mem {
	u16 fc_tag;
	u16 fc_len;
};


/**
 * tl_to_darg - Implements the tl to darg operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void tl_to_darg(struct dentry_info_args *darg,
			      struct ext4_fc_tl_mem *tl, u8 *val)
{
	struct ext4_fc_dentry_info fcd;

	memcpy(&fcd, val, sizeof(fcd));

	darg->parent_ino = le32_to_cpu(fcd.fc_parent_ino);
	darg->ino = le32_to_cpu(fcd.fc_ino);
	darg->dname = val + offsetof(struct ext4_fc_dentry_info, fc_dname);
	darg->dname_len = tl->fc_len - sizeof(struct ext4_fc_dentry_info);
}


/**
 * ext4_fc_get_tl - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_fc_get_tl(struct ext4_fc_tl_mem *tl, u8 *val)
{
	struct ext4_fc_tl tl_disk;

	memcpy(&tl_disk, val, EXT4_FC_TAG_BASE_LEN);
	tl->fc_len = le16_to_cpu(tl_disk.fc_len);
	tl->fc_tag = le16_to_cpu(tl_disk.fc_tag);
}


/**
 * ext4_fc_replay_unlink - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_replay_unlink(struct super_block *sb,
				 struct ext4_fc_tl_mem *tl, u8 *val)
{
	struct inode *inode, *old_parent;
	struct qstr entry;
	struct dentry_info_args darg;
	int ret = 0;

	tl_to_darg(&darg, tl, val);

	trace_ext4_fc_replay(sb, EXT4_FC_TAG_UNLINK, darg.ino,
			darg.parent_ino, darg.dname_len);

	entry.name = darg.dname;
	entry.len = darg.dname_len;
	inode = ext4_iget(sb, darg.ino, EXT4_IGET_NORMAL);

	if (IS_ERR(inode)) {
		ext4_debug("Inode %d not found", darg.ino);
		return 0;
	}

	old_parent = ext4_iget(sb, darg.parent_ino,
				EXT4_IGET_NORMAL);
	if (IS_ERR(old_parent)) {
		ext4_debug("Dir with inode %d not found", darg.parent_ino);
		iput(inode);
		return 0;
	}

	ret = __ext4_unlink(old_parent, &entry, inode, NULL);

	if (ret == -ENOENT)
		ret = 0;
	iput(old_parent);
	iput(inode);
	return ret;
}


/**
 * ext4_fc_replay_link_internal - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_replay_link_internal(struct super_block *sb,
				struct dentry_info_args *darg,
				struct inode *inode)
{
	struct inode *dir = NULL;
	struct dentry *dentry_dir = NULL, *dentry_inode = NULL;
	struct qstr qstr_dname = QSTR_INIT(darg->dname, darg->dname_len);
	int ret = 0;

	dir = ext4_iget(sb, darg->parent_ino, EXT4_IGET_NORMAL);
	if (IS_ERR(dir)) {
		ext4_debug("Dir with inode %d not found.", darg->parent_ino);
		dir = NULL;
		goto out;
	}

	dentry_dir = d_obtain_alias(dir);
	if (IS_ERR(dentry_dir)) {
		ext4_debug("Failed to obtain dentry");
		dentry_dir = NULL;
		goto out;
	}

	dentry_inode = d_alloc(dentry_dir, &qstr_dname);
	if (!dentry_inode) {
		ext4_debug("Inode dentry not created.");
		ret = -ENOMEM;
		goto out;
	}

	ret = __ext4_link(dir, inode, dentry_inode);


	if (ret && ret != -EEXIST) {
		ext4_debug("Failed to link\n");
		goto out;
	}

	ret = 0;
out:
	if (dentry_dir) {
		d_drop(dentry_dir);
		dput(dentry_dir);
	} else if (dir) {
		iput(dir);
	}
	if (dentry_inode) {
		d_drop(dentry_inode);
		dput(dentry_inode);
	}

	return ret;
}


/**
 * ext4_fc_replay_link - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_replay_link(struct super_block *sb,
			       struct ext4_fc_tl_mem *tl, u8 *val)
{
	struct inode *inode;
	struct dentry_info_args darg;
	int ret = 0;

	tl_to_darg(&darg, tl, val);
	trace_ext4_fc_replay(sb, EXT4_FC_TAG_LINK, darg.ino,
			darg.parent_ino, darg.dname_len);

	inode = ext4_iget(sb, darg.ino, EXT4_IGET_NORMAL);
	if (IS_ERR(inode)) {
		ext4_debug("Inode not found.");
		return 0;
	}

	ret = ext4_fc_replay_link_internal(sb, &darg, inode);
	iput(inode);
	return ret;
}


/**
 * ext4_fc_record_modified_inode - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_record_modified_inode(struct super_block *sb, int ino)
{
	struct ext4_fc_replay_state *state;
	int i;

	state = &EXT4_SB(sb)->s_fc_replay_state;
	for (i = 0; i < state->fc_modified_inodes_used; i++)
		if (state->fc_modified_inodes[i] == ino)
			return 0;
	if (state->fc_modified_inodes_used == state->fc_modified_inodes_size) {
		int *fc_modified_inodes;

		fc_modified_inodes = krealloc(state->fc_modified_inodes,
				sizeof(int) * (state->fc_modified_inodes_size +
				EXT4_FC_REPLAY_REALLOC_INCREMENT),
				GFP_KERNEL);
		if (!fc_modified_inodes)
			return -ENOMEM;
		state->fc_modified_inodes = fc_modified_inodes;
		state->fc_modified_inodes_size +=
			EXT4_FC_REPLAY_REALLOC_INCREMENT;
	}
	state->fc_modified_inodes[state->fc_modified_inodes_used++] = ino;
	return 0;
}


/**
 * ext4_fc_replay_inode - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_replay_inode(struct super_block *sb,
				struct ext4_fc_tl_mem *tl, u8 *val)
{
	struct ext4_fc_inode fc_inode;
	struct ext4_inode *raw_inode;
	struct ext4_inode *raw_fc_inode;
	struct inode *inode = NULL;
	struct ext4_iloc iloc;
	int inode_len, ino, ret, tag = tl->fc_tag;
	struct ext4_extent_header *eh;
	size_t off_gen = offsetof(struct ext4_inode, i_generation);

	memcpy(&fc_inode, val, sizeof(fc_inode));

	ino = le32_to_cpu(fc_inode.fc_ino);
	trace_ext4_fc_replay(sb, tag, ino, 0, 0);

	inode = ext4_iget(sb, ino, EXT4_IGET_NORMAL);
	if (!IS_ERR(inode)) {
		ext4_ext_clear_bb(inode);
		iput(inode);
	}
	inode = NULL;

	ret = ext4_fc_record_modified_inode(sb, ino);
	if (ret)
		goto out;

	raw_fc_inode = (struct ext4_inode *)
		(val + offsetof(struct ext4_fc_inode, fc_raw_inode));
	ret = ext4_get_fc_inode_loc(sb, ino, &iloc);
	if (ret)
		goto out;

	inode_len = tl->fc_len - sizeof(struct ext4_fc_inode);
	raw_inode = ext4_raw_inode(&iloc);

	memcpy(raw_inode, raw_fc_inode, offsetof(struct ext4_inode, i_block));
	memcpy((u8 *)raw_inode + off_gen, (u8 *)raw_fc_inode + off_gen,
	       inode_len - off_gen);
	if (le32_to_cpu(raw_inode->i_flags) & EXT4_EXTENTS_FL) {
		eh = (struct ext4_extent_header *)(&raw_inode->i_block[0]);
		if (eh->eh_magic != EXT4_EXT_MAGIC) {
			memset(eh, 0, sizeof(*eh));
			eh->eh_magic = EXT4_EXT_MAGIC;
			eh->eh_max = cpu_to_le16(
				(sizeof(raw_inode->i_block) -
				 sizeof(struct ext4_extent_header))
				 / sizeof(struct ext4_extent));
		}
	} else if (le32_to_cpu(raw_inode->i_flags) & EXT4_INLINE_DATA_FL) {
		memcpy(raw_inode->i_block, raw_fc_inode->i_block,
			sizeof(raw_inode->i_block));
	}


	ret = ext4_handle_dirty_metadata(NULL, NULL, iloc.bh);
	if (ret)
		goto out_brelse;
	ret = sync_dirty_buffer(iloc.bh);
	if (ret)
		goto out_brelse;
	ret = ext4_mark_inode_used(sb, ino);
	if (ret)
		goto out_brelse;


	inode = ext4_iget(sb, ino, EXT4_IGET_NORMAL);
	if (IS_ERR(inode)) {
		ext4_debug("Inode not found.");
		inode = NULL;
		ret = -EFSCORRUPTED;
		goto out_brelse;
	}


	if (!ext4_test_inode_flag(inode, EXT4_INODE_INLINE_DATA))
		ext4_ext_replay_set_iblocks(inode);

	inode->i_generation = le32_to_cpu(ext4_raw_inode(&iloc)->i_generation);
	ext4_reset_inode_seed(inode);

	ext4_inode_csum_set(inode, ext4_raw_inode(&iloc), EXT4_I(inode));
	ret = ext4_handle_dirty_metadata(NULL, NULL, iloc.bh);
	sync_dirty_buffer(iloc.bh);
out_brelse:
	brelse(iloc.bh);
out:
	iput(inode);
	if (!ret)
		blkdev_issue_flush(sb->s_bdev);

	return ret;
}


/**
 * ext4_fc_replay_create - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_replay_create(struct super_block *sb,
				 struct ext4_fc_tl_mem *tl, u8 *val)
{
	int ret = 0;
	struct inode *inode = NULL;
	struct inode *dir = NULL;
	struct dentry_info_args darg;

	tl_to_darg(&darg, tl, val);

	trace_ext4_fc_replay(sb, EXT4_FC_TAG_CREAT, darg.ino,
			darg.parent_ino, darg.dname_len);


	ret = ext4_mark_inode_used(sb, darg.ino);
	if (ret)
		goto out;

	inode = ext4_iget(sb, darg.ino, EXT4_IGET_NORMAL);
	if (IS_ERR(inode)) {
		ext4_debug("inode %d not found.", darg.ino);
		inode = NULL;
		ret = -EINVAL;
		goto out;
	}

	if (S_ISDIR(inode->i_mode)) {


		dir = ext4_iget(sb, darg.parent_ino, EXT4_IGET_NORMAL);
		if (IS_ERR(dir)) {
			ext4_debug("Dir %d not found.", darg.ino);
			goto out;
		}
		ret = ext4_init_new_dir(NULL, dir, inode);
		iput(dir);
		if (ret) {
			ret = 0;
			goto out;
		}
	}
	ret = ext4_fc_replay_link_internal(sb, &darg, inode);
	if (ret)
		goto out;
	set_nlink(inode, 1);
	ext4_mark_inode_dirty(NULL, inode);
out:
	iput(inode);
	return ret;
}


/**
 * ext4_fc_record_regions - Implements the fc record regions operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_fc_record_regions(struct super_block *sb, int ino,
		ext4_lblk_t lblk, ext4_fsblk_t pblk, int len, int replay)
{
	struct ext4_fc_replay_state *state;
	struct ext4_fc_alloc_region *region;

	state = &EXT4_SB(sb)->s_fc_replay_state;


	if (replay && state->fc_regions_used != state->fc_regions_valid)
		state->fc_regions_used = state->fc_regions_valid;
	if (state->fc_regions_used == state->fc_regions_size) {
		struct ext4_fc_alloc_region *fc_regions;

		fc_regions = krealloc(state->fc_regions,
				      sizeof(struct ext4_fc_alloc_region) *
				      (state->fc_regions_size +
				       EXT4_FC_REPLAY_REALLOC_INCREMENT),
				      GFP_KERNEL);
		if (!fc_regions)
			return -ENOMEM;
		state->fc_regions_size +=
			EXT4_FC_REPLAY_REALLOC_INCREMENT;
		state->fc_regions = fc_regions;
	}
	region = &state->fc_regions[state->fc_regions_used++];
	region->ino = ino;
	region->lblk = lblk;
	region->pblk = pblk;
	region->len = len;

	if (replay)
		state->fc_regions_valid++;

	return 0;
}


/**
 * ext4_fc_replay_add_range - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_replay_add_range(struct super_block *sb,
				    struct ext4_fc_tl_mem *tl, u8 *val)
{
	struct ext4_fc_add_range fc_add_ex;
	struct ext4_extent newex, *ex;
	struct inode *inode;
	ext4_lblk_t start, cur;
	int remaining, len;
	ext4_fsblk_t start_pblk;
	struct ext4_map_blocks map;
	struct ext4_ext_path *path = NULL;
	int ret;

	memcpy(&fc_add_ex, val, sizeof(fc_add_ex));
	ex = (struct ext4_extent *)&fc_add_ex.fc_ex;

	trace_ext4_fc_replay(sb, EXT4_FC_TAG_ADD_RANGE,
		le32_to_cpu(fc_add_ex.fc_ino), le32_to_cpu(ex->ee_block),
		ext4_ext_get_actual_len(ex));

	inode = ext4_iget(sb, le32_to_cpu(fc_add_ex.fc_ino), EXT4_IGET_NORMAL);
	if (IS_ERR(inode)) {
		ext4_debug("Inode not found.");
		return 0;
	}

	ret = ext4_fc_record_modified_inode(sb, inode->i_ino);
	if (ret)
		goto out;

	start = le32_to_cpu(ex->ee_block);
	start_pblk = ext4_ext_pblock(ex);
	len = ext4_ext_get_actual_len(ex);

	cur = start;
	remaining = len;
	ext4_debug("ADD_RANGE, lblk %d, pblk %lld, len %d, unwritten %d, inode %ld\n",
		  start, start_pblk, len, ext4_ext_is_unwritten(ex),
		  inode->i_ino);

	while (remaining > 0) {
		map.m_lblk = cur;
		map.m_len = remaining;
		map.m_pblk = 0;
		ret = ext4_map_blocks(NULL, inode, &map, 0);

		if (ret < 0)
			goto out;

		if (ret == 0) {

			path = ext4_find_extent(inode, cur, path, 0);
			if (IS_ERR(path)) {
				ret = PTR_ERR(path);
				path = NULL;
				goto out;
			}
			memset(&newex, 0, sizeof(newex));
			newex.ee_block = cpu_to_le32(cur);
			ext4_ext_store_pblock(
				&newex, start_pblk + cur - start);
			newex.ee_len = cpu_to_le16(map.m_len);
			if (ext4_ext_is_unwritten(ex))
				ext4_ext_mark_unwritten(&newex);
			down_write(&EXT4_I(inode)->i_data_sem);
			path = ext4_ext_insert_extent(NULL, inode,
						      path, &newex, 0);
			up_write((&EXT4_I(inode)->i_data_sem));
			if (IS_ERR(path)) {
				ret = PTR_ERR(path);
				path = NULL;
				goto out;
			}
			goto next;
		}

		if (start_pblk + cur - start != map.m_pblk) {


			ret = ext4_ext_replay_update_ex(inode, cur, map.m_len,
					ext4_ext_is_unwritten(ex),
					start_pblk + cur - start);
			if (ret)
				goto out;


			ext4_mb_mark_bb(inode->i_sb, map.m_pblk, map.m_len, false);
			goto next;
		}


		ext4_debug("Converting from %ld to %d %lld",
				map.m_flags & EXT4_MAP_UNWRITTEN,
			ext4_ext_is_unwritten(ex), map.m_pblk);
		ret = ext4_ext_replay_update_ex(inode, cur, map.m_len,
					ext4_ext_is_unwritten(ex), map.m_pblk);
		if (ret)
			goto out;


		ext4_ext_replay_shrink_inode(inode, start + len);
next:
		cur += map.m_len;
		remaining -= map.m_len;
	}
	ext4_ext_replay_shrink_inode(inode, i_size_read(inode) >>
					sb->s_blocksize_bits);
	ret = 0;
out:
	ext4_free_ext_path(path);
	iput(inode);
	return ret;
}


/**
 * ext4_fc_replay_del_range - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_fc_replay_del_range(struct super_block *sb,
			 struct ext4_fc_tl_mem *tl, u8 *val)
{
	struct inode *inode;
	struct ext4_fc_del_range lrange;
	struct ext4_map_blocks map;
	ext4_lblk_t cur, remaining;
	int ret;

	memcpy(&lrange, val, sizeof(lrange));
	cur = le32_to_cpu(lrange.fc_lblk);
	remaining = le32_to_cpu(lrange.fc_len);

	trace_ext4_fc_replay(sb, EXT4_FC_TAG_DEL_RANGE,
		le32_to_cpu(lrange.fc_ino), cur, remaining);

	inode = ext4_iget(sb, le32_to_cpu(lrange.fc_ino), EXT4_IGET_NORMAL);
	if (IS_ERR(inode)) {
		ext4_debug("Inode %d not found", le32_to_cpu(lrange.fc_ino));
		return 0;
	}

	ret = ext4_fc_record_modified_inode(sb, inode->i_ino);
	if (ret)
		goto out;

	ext4_debug("DEL_RANGE, inode %ld, lblk %d, len %d\n",
			inode->i_ino, le32_to_cpu(lrange.fc_lblk),
			le32_to_cpu(lrange.fc_len));
	while (remaining > 0) {
		map.m_lblk = cur;
		map.m_len = remaining;

		ret = ext4_map_blocks(NULL, inode, &map, 0);
		if (ret < 0)
			goto out;
		if (ret > 0) {
			remaining -= ret;
			cur += ret;
			ext4_mb_mark_bb(inode->i_sb, map.m_pblk, map.m_len, false);
		} else {
			remaining -= map.m_len;
			cur += map.m_len;
		}
	}

	down_write(&EXT4_I(inode)->i_data_sem);
	ret = ext4_ext_remove_space(inode, le32_to_cpu(lrange.fc_lblk),
				le32_to_cpu(lrange.fc_lblk) +
				le32_to_cpu(lrange.fc_len) - 1);
	up_write(&EXT4_I(inode)->i_data_sem);
	if (ret)
		goto out;
	ext4_ext_replay_shrink_inode(inode,
		i_size_read(inode) >> sb->s_blocksize_bits);
	ext4_mark_inode_dirty(NULL, inode);
	ret = 0;
out:
	iput(inode);
	return ret;
}


/**
 * ext4_fc_set_bitmaps_and_counters - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_fc_set_bitmaps_and_counters(struct super_block *sb)
{
	struct ext4_fc_replay_state *state;
	struct inode *inode;
	struct ext4_ext_path *path = NULL;
	struct ext4_map_blocks map;
	int i, ret, j;
	ext4_lblk_t cur, end;

	state = &EXT4_SB(sb)->s_fc_replay_state;
	for (i = 0; i < state->fc_modified_inodes_used; i++) {
		inode = ext4_iget(sb, state->fc_modified_inodes[i],
			EXT4_IGET_NORMAL);
		if (IS_ERR(inode)) {
			ext4_debug("Inode %d not found.",
				state->fc_modified_inodes[i]);
			continue;
		}
		cur = 0;
		end = EXT_MAX_BLOCKS;
		if (ext4_test_inode_flag(inode, EXT4_INODE_INLINE_DATA)) {
			iput(inode);
			continue;
		}
		while (cur < end) {
			map.m_lblk = cur;
			map.m_len = end - cur;

			ret = ext4_map_blocks(NULL, inode, &map, 0);
			if (ret < 0)
				break;

			if (ret > 0) {
				path = ext4_find_extent(inode, map.m_lblk, path, 0);
				if (!IS_ERR(path)) {
					for (j = 0; j < path->p_depth; j++)
						ext4_mb_mark_bb(inode->i_sb,
							path[j].p_block, 1, true);
				} else {
					path = NULL;
				}
				cur += ret;
				ext4_mb_mark_bb(inode->i_sb, map.m_pblk,
							map.m_len, true);
			} else {
				cur = cur + (map.m_len ? map.m_len : 1);
			}
		}
		iput(inode);
	}

	ext4_free_ext_path(path);
}


/**
 * ext4_fc_replay_check_excluded - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
bool ext4_fc_replay_check_excluded(struct super_block *sb, ext4_fsblk_t blk)
{
	int i;
	struct ext4_fc_replay_state *state;

	state = &EXT4_SB(sb)->s_fc_replay_state;
	for (i = 0; i < state->fc_regions_valid; i++) {
		if (state->fc_regions[i].ino == 0 ||
			state->fc_regions[i].len == 0)
			continue;
		if (in_range(blk, state->fc_regions[i].pblk,
					state->fc_regions[i].len))
			return true;
	}
	return false;
}


/**
 * ext4_fc_replay_cleanup - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_replay_cleanup(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	sbi->s_mount_state &= ~EXT4_FC_REPLAY;
	kfree(sbi->s_fc_replay_state.fc_regions);
	kfree(sbi->s_fc_replay_state.fc_modified_inodes);
}


/**
 * ext4_fc_value_len_isvalid - Implements the fc value len isvalid operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool ext4_fc_value_len_isvalid(struct ext4_sb_info *sbi,
				      int tag, int len)
{
	switch (tag) {
	case EXT4_FC_TAG_ADD_RANGE:
		return len == sizeof(struct ext4_fc_add_range);
	case EXT4_FC_TAG_DEL_RANGE:
		return len == sizeof(struct ext4_fc_del_range);
	case EXT4_FC_TAG_CREAT:
	case EXT4_FC_TAG_LINK:
	case EXT4_FC_TAG_UNLINK:
		len -= sizeof(struct ext4_fc_dentry_info);
		return len >= 1 && len <= EXT4_NAME_LEN;
	case EXT4_FC_TAG_INODE:
		len -= sizeof(struct ext4_fc_inode);
		return len >= EXT4_GOOD_OLD_INODE_SIZE &&
			len <= sbi->s_inode_size;
	case EXT4_FC_TAG_PAD:
		return true;
	case EXT4_FC_TAG_TAIL:
		return len >= sizeof(struct ext4_fc_tail);
	case EXT4_FC_TAG_HEAD:
		return len == sizeof(struct ext4_fc_head);
	}
	return false;
}


/**
 * ext4_fc_replay_scan - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_replay_scan(journal_t *journal,
				struct buffer_head *bh, int off,
				tid_t expected_tid)
{
	struct super_block *sb = journal->j_private;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_fc_replay_state *state;
	int ret = JBD2_FC_REPLAY_CONTINUE;
	struct ext4_fc_add_range ext;
	struct ext4_fc_tl_mem tl;
	struct ext4_fc_tail tail;
	__u8 *start, *end, *cur, *val;
	struct ext4_fc_head head;
	struct ext4_extent *ex;

	state = &sbi->s_fc_replay_state;

	start = (u8 *)bh->b_data;
	end = start + journal->j_blocksize;

	if (state->fc_replay_expected_off == 0) {
		state->fc_cur_tag = 0;
		state->fc_replay_num_tags = 0;
		state->fc_crc = 0;
		state->fc_regions = NULL;
		state->fc_regions_valid = state->fc_regions_used =
			state->fc_regions_size = 0;

		if (le16_to_cpu(((struct ext4_fc_tl *)start)->fc_tag)
			!= EXT4_FC_TAG_HEAD)
			return 0;
	}

	if (off != state->fc_replay_expected_off) {
		ret = -EFSCORRUPTED;
		goto out_err;
	}

	state->fc_replay_expected_off++;
	for (cur = start; cur <= end - EXT4_FC_TAG_BASE_LEN;
	     cur = cur + EXT4_FC_TAG_BASE_LEN + tl.fc_len) {
		ext4_fc_get_tl(&tl, cur);
		val = cur + EXT4_FC_TAG_BASE_LEN;
		if (tl.fc_len > end - val ||
		    !ext4_fc_value_len_isvalid(sbi, tl.fc_tag, tl.fc_len)) {
			ret = state->fc_replay_num_tags ?
				JBD2_FC_REPLAY_STOP : -ECANCELED;
			goto out_err;
		}
		ext4_debug("Scan phase, tag:%s, blk %lld\n",
			   tag2str(tl.fc_tag), bh->b_blocknr);
		switch (tl.fc_tag) {
		case EXT4_FC_TAG_ADD_RANGE:
			memcpy(&ext, val, sizeof(ext));
			ex = (struct ext4_extent *)&ext.fc_ex;
			ret = ext4_fc_record_regions(sb,
				le32_to_cpu(ext.fc_ino),
				le32_to_cpu(ex->ee_block), ext4_ext_pblock(ex),
				ext4_ext_get_actual_len(ex), 0);
			if (ret < 0)
				break;
			ret = JBD2_FC_REPLAY_CONTINUE;
			fallthrough;
		case EXT4_FC_TAG_DEL_RANGE:
		case EXT4_FC_TAG_LINK:
		case EXT4_FC_TAG_UNLINK:
		case EXT4_FC_TAG_CREAT:
		case EXT4_FC_TAG_INODE:
		case EXT4_FC_TAG_PAD:
			state->fc_cur_tag++;
			state->fc_crc = ext4_chksum(sbi, state->fc_crc, cur,
				EXT4_FC_TAG_BASE_LEN + tl.fc_len);
			break;
		case EXT4_FC_TAG_TAIL:
			state->fc_cur_tag++;
			memcpy(&tail, val, sizeof(tail));
			state->fc_crc = ext4_chksum(sbi, state->fc_crc, cur,
						EXT4_FC_TAG_BASE_LEN +
						offsetof(struct ext4_fc_tail,
						fc_crc));
			if (le32_to_cpu(tail.fc_tid) == expected_tid &&
				le32_to_cpu(tail.fc_crc) == state->fc_crc) {
				state->fc_replay_num_tags = state->fc_cur_tag;
				state->fc_regions_valid =
					state->fc_regions_used;
			} else {
				ret = state->fc_replay_num_tags ?
					JBD2_FC_REPLAY_STOP : -EFSBADCRC;
			}
			state->fc_crc = 0;
			break;
		case EXT4_FC_TAG_HEAD:
			memcpy(&head, val, sizeof(head));
			if (le32_to_cpu(head.fc_features) &
				~EXT4_FC_SUPPORTED_FEATURES) {
				ret = -EOPNOTSUPP;
				break;
			}
			if (le32_to_cpu(head.fc_tid) != expected_tid) {
				ret = JBD2_FC_REPLAY_STOP;
				break;
			}
			state->fc_cur_tag++;
			state->fc_crc = ext4_chksum(sbi, state->fc_crc, cur,
				EXT4_FC_TAG_BASE_LEN + tl.fc_len);
			break;
		default:
			ret = state->fc_replay_num_tags ?
				JBD2_FC_REPLAY_STOP : -ECANCELED;
		}
		if (ret < 0 || ret == JBD2_FC_REPLAY_STOP)
			break;
	}

out_err:
	trace_ext4_fc_replay_scan(sb, ret, off);
	return ret;
}


/**
 * ext4_fc_replay - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fc_replay(journal_t *journal, struct buffer_head *bh,
				enum passtype pass, int off, tid_t expected_tid)
{
	struct super_block *sb = journal->j_private;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_fc_tl_mem tl;
	__u8 *start, *end, *cur, *val;
	int ret = JBD2_FC_REPLAY_CONTINUE;
	struct ext4_fc_replay_state *state = &sbi->s_fc_replay_state;
	struct ext4_fc_tail tail;

	if (pass == PASS_SCAN) {
		state->fc_current_pass = PASS_SCAN;
		return ext4_fc_replay_scan(journal, bh, off, expected_tid);
	}

	if (state->fc_current_pass != pass) {
		state->fc_current_pass = pass;
		sbi->s_mount_state |= EXT4_FC_REPLAY;
	}
	if (!sbi->s_fc_replay_state.fc_replay_num_tags) {
		ext4_debug("Replay stops\n");
		ext4_fc_set_bitmaps_and_counters(sb);
		return 0;
	}

#ifdef CONFIG_EXT4_DEBUG
	if (sbi->s_fc_debug_max_replay && off >= sbi->s_fc_debug_max_replay) {
		pr_warn("Dropping fc block %d because max_replay set\n", off);
		return JBD2_FC_REPLAY_STOP;
	}
#endif

	start = (u8 *)bh->b_data;
	end = start + journal->j_blocksize;

	for (cur = start; cur <= end - EXT4_FC_TAG_BASE_LEN;
	     cur = cur + EXT4_FC_TAG_BASE_LEN + tl.fc_len) {
		ext4_fc_get_tl(&tl, cur);
		val = cur + EXT4_FC_TAG_BASE_LEN;

		if (state->fc_replay_num_tags == 0) {
			ret = JBD2_FC_REPLAY_STOP;
			ext4_fc_set_bitmaps_and_counters(sb);
			break;
		}

		ext4_debug("Replay phase, tag:%s\n", tag2str(tl.fc_tag));
		state->fc_replay_num_tags--;
		switch (tl.fc_tag) {
		case EXT4_FC_TAG_LINK:
			ret = ext4_fc_replay_link(sb, &tl, val);
			break;
		case EXT4_FC_TAG_UNLINK:
			ret = ext4_fc_replay_unlink(sb, &tl, val);
			break;
		case EXT4_FC_TAG_ADD_RANGE:
			ret = ext4_fc_replay_add_range(sb, &tl, val);
			break;
		case EXT4_FC_TAG_CREAT:
			ret = ext4_fc_replay_create(sb, &tl, val);
			break;
		case EXT4_FC_TAG_DEL_RANGE:
			ret = ext4_fc_replay_del_range(sb, &tl, val);
			break;
		case EXT4_FC_TAG_INODE:
			ret = ext4_fc_replay_inode(sb, &tl, val);
			break;
		case EXT4_FC_TAG_PAD:
			trace_ext4_fc_replay(sb, EXT4_FC_TAG_PAD, 0,
					     tl.fc_len, 0);
			break;
		case EXT4_FC_TAG_TAIL:
			trace_ext4_fc_replay(sb, EXT4_FC_TAG_TAIL,
					     0, tl.fc_len, 0);
			memcpy(&tail, val, sizeof(tail));
			WARN_ON(le32_to_cpu(tail.fc_tid) != expected_tid);
			break;
		case EXT4_FC_TAG_HEAD:
			break;
		default:
			trace_ext4_fc_replay(sb, tl.fc_tag, 0, tl.fc_len, 0);
			ret = -ECANCELED;
			break;
		}
		if (ret < 0)
			break;
		ret = JBD2_FC_REPLAY_CONTINUE;
	}
	return ret;
}


/**
 * ext4_fc_init - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_init(struct super_block *sb, journal_t *journal)
{


	journal->j_fc_replay_callback = ext4_fc_replay;
	if (!test_opt2(sb, JOURNAL_FAST_COMMIT))
		return;
	journal->j_fc_cleanup_callback = ext4_fc_cleanup;
}

static const char * const fc_ineligible_reasons[] = {
	[EXT4_FC_REASON_XATTR] = "Extended attributes changed",
	[EXT4_FC_REASON_CROSS_RENAME] = "Cross rename",
	[EXT4_FC_REASON_JOURNAL_FLAG_CHANGE] = "Journal flag changed",
	[EXT4_FC_REASON_NOMEM] = "Insufficient memory",
	[EXT4_FC_REASON_SWAP_BOOT] = "Swap boot",
	[EXT4_FC_REASON_RESIZE] = "Resize",
	[EXT4_FC_REASON_RENAME_DIR] = "Dir renamed",
	[EXT4_FC_REASON_FALLOC_RANGE] = "Falloc range op",
	[EXT4_FC_REASON_INODE_JOURNAL_DATA] = "Data journalling",
	[EXT4_FC_REASON_ENCRYPTED_FILENAME] = "Encrypted filename",
};


/**
 * ext4_fc_info_show - Implements the fc info show operation within the fast-commit engine subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_fc_info_show(struct seq_file *seq, void *v)
{
	struct ext4_sb_info *sbi = EXT4_SB((struct super_block *)seq->private);
	struct ext4_fc_stats *stats = &sbi->s_fc_stats;
	int i;

	if (v != SEQ_START_TOKEN)
		return 0;

	seq_printf(seq,
		"fc stats:\n%ld commits\n%ld ineligible\n%ld numblks\n%lluus avg_commit_time\n",
		   stats->fc_num_commits, stats->fc_ineligible_commits,
		   stats->fc_numblks,
		   div_u64(stats->s_fc_avg_commit_time, 1000));
	seq_puts(seq, "Ineligible reasons:\n");
	for (i = 0; i < EXT4_FC_REASON_MAX; i++)
		seq_printf(seq, "\"%s\":\t%d\n", fc_ineligible_reasons[i],
			stats->fc_ineligible_reason_count[i]);

	return 0;
}


/**
 * ext4_fc_init_dentry_cache - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int __init ext4_fc_init_dentry_cache(void)
{
	ext4_fc_dentry_cachep = KMEM_CACHE(ext4_fc_dentry_update,
					   SLAB_RECLAIM_ACCOUNT);

	if (ext4_fc_dentry_cachep == NULL)
		return -ENOMEM;

	return 0;
}


/**
 * ext4_fc_destroy_dentry_cache - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fc_destroy_dentry_cache(void)
{
	kmem_cache_destroy(ext4_fc_dentry_cachep);
}
