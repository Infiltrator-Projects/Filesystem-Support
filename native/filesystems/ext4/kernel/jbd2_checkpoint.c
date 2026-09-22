// SPDX-License-Identifier: GPL-2.0+
/*
 * linux/fs/jbd2/checkpoint.c
 *
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1999
 *
 * Copyright 1999 Red Hat Software --- All Rights Reserved
 *
 * Checkpoint routines for the generic filesystem journaling code.
 * Part of the ext2fs journaling system.
 *
 * Checkpointing is the process of ensuring that a section of the log is
 * committed fully to disk, so that that portion of the log can be
 * reused.
 */

/*
 * EXT4 — JBD2 checkpointing
 *
 * Purpose:
 *   Moves committed metadata toward home locations and advances the recoverable journal tail.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Journal space can be reclaimed only after recovery no longer needs the corresponding logged metadata.
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

#include <linux/time.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/blkdev.h>
#include <trace/events/jbd2.h>


/**
 * __buffer_unlink - Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void __buffer_unlink(struct journal_head *jh)
{
	transaction_t *transaction = jh->b_cp_transaction;

	jh->b_cpnext->b_cpprev = jh->b_cpprev;
	jh->b_cpprev->b_cpnext = jh->b_cpnext;
	if (transaction->t_checkpoint_list == jh) {
		transaction->t_checkpoint_list = jh->b_cpnext;
		if (transaction->t_checkpoint_list == jh)
			transaction->t_checkpoint_list = NULL;
	}
}


/**
 * __releases - Implements the releases operation within the jbd2 checkpointing subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void __jbd2_log_wait_for_space(journal_t *journal)
__acquires(&journal->j_state_lock)
__releases(&journal->j_state_lock)
{
	int nblocks, space_left;


	nblocks = journal->j_max_transaction_buffers;
	while (jbd2_log_space_left(journal) < nblocks) {
		write_unlock(&journal->j_state_lock);
		mutex_lock_io(&journal->j_checkpoint_mutex);


		write_lock(&journal->j_state_lock);
		if (journal->j_flags & JBD2_ABORT) {
			mutex_unlock(&journal->j_checkpoint_mutex);
			return;
		}
		spin_lock(&journal->j_list_lock);
		space_left = jbd2_log_space_left(journal);
		if (space_left < nblocks) {
			int chkpt = journal->j_checkpoint_transactions != NULL;
			tid_t tid = 0;
			bool has_transaction = false;

			if (journal->j_committing_transaction) {
				tid = journal->j_committing_transaction->t_tid;
				has_transaction = true;
			}
			spin_unlock(&journal->j_list_lock);
			write_unlock(&journal->j_state_lock);
			if (chkpt) {
				jbd2_log_do_checkpoint(journal);
			} else if (jbd2_cleanup_journal_tail(journal) <= 0) {


				;
			} else if (has_transaction) {


				mutex_unlock(&journal->j_checkpoint_mutex);
				jbd2_log_wait_commit(journal, tid);
				write_lock(&journal->j_state_lock);
				continue;
			} else {
				printk(KERN_ERR "%s: needed %d blocks and "
				       "only had %d space available\n",
				       __func__, nblocks, space_left);
				printk(KERN_ERR "%s: no way to get more "
				       "journal space in %s\n", __func__,
				       journal->j_devname);
				WARN_ON(1);
				jbd2_journal_abort(journal, -EIO);
			}
			write_lock(&journal->j_state_lock);
		} else {
			spin_unlock(&journal->j_list_lock);
		}
		mutex_unlock(&journal->j_checkpoint_mutex);
	}
}

/**
 * __flush_batch - Drives pending state toward the durability guarantee required by the calling VFS or journal interface.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void
__flush_batch(journal_t *journal, int *batch_count)
{
	int i;
	struct blk_plug plug;

	blk_start_plug(&plug);
	for (i = 0; i < *batch_count; i++)
		write_dirty_buffer(journal->j_chkpt_bhs[i], REQ_SYNC);
	blk_finish_plug(&plug);

	for (i = 0; i < *batch_count; i++) {
		struct buffer_head *bh = journal->j_chkpt_bhs[i];
		BUFFER_TRACE(bh, "brelse");
		__brelse(bh);
		journal->j_chkpt_bhs[i] = NULL;
	}
	*batch_count = 0;
}


/**
 * jbd2_log_do_checkpoint - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_log_do_checkpoint(journal_t *journal)
{
	struct journal_head	*jh;
	struct buffer_head	*bh;
	transaction_t		*transaction;
	tid_t			this_tid;
	int			result, batch_count = 0;

	jbd2_debug(1, "Start checkpoint\n");


	result = jbd2_cleanup_journal_tail(journal);
	trace_jbd2_checkpoint(journal, result);
	jbd2_debug(1, "cleanup_journal_tail returned %d\n", result);
	if (result <= 0)
		return result;


	spin_lock(&journal->j_list_lock);
	if (!journal->j_checkpoint_transactions)
		goto out;
	transaction = journal->j_checkpoint_transactions;
	if (transaction->t_chp_stats.cs_chp_time == 0)
		transaction->t_chp_stats.cs_chp_time = jiffies;
	this_tid = transaction->t_tid;
restart:


	if (journal->j_checkpoint_transactions != transaction ||
	    transaction->t_tid != this_tid)
		goto out;


	while (transaction->t_checkpoint_list) {
		jh = transaction->t_checkpoint_list;
		bh = jh2bh(jh);

		if (jh->b_transaction != NULL) {
			transaction_t *t = jh->b_transaction;
			tid_t tid = t->t_tid;

			transaction->t_chp_stats.cs_forced_to_close++;
			spin_unlock(&journal->j_list_lock);
			if (unlikely(journal->j_flags & JBD2_UNMOUNT))


				printk(KERN_ERR
		"JBD2: %s: Waiting for Godot: block %llu\n",
		journal->j_devname, (unsigned long long) bh->b_blocknr);

			if (batch_count)
				__flush_batch(journal, &batch_count);
			jbd2_log_start_commit(journal, tid);


			mutex_unlock(&journal->j_checkpoint_mutex);
			jbd2_log_wait_commit(journal, tid);
			mutex_lock_io(&journal->j_checkpoint_mutex);
			spin_lock(&journal->j_list_lock);
			goto restart;
		}
		if (!trylock_buffer(bh)) {


			get_bh(bh);
			spin_unlock(&journal->j_list_lock);
			wait_on_buffer(bh);

			BUFFER_TRACE(bh, "brelse");
			__brelse(bh);
			goto retry;
		} else if (!buffer_dirty(bh)) {
			unlock_buffer(bh);
			BUFFER_TRACE(bh, "remove from checkpoint");


			if (__jbd2_journal_remove_checkpoint(jh) ||
			    !transaction->t_checkpoint_list)
				goto out;
		} else {
			unlock_buffer(bh);


			BUFFER_TRACE(bh, "queue");
			get_bh(bh);
			if (WARN_ON_ONCE(buffer_jwrite(bh))) {
				put_bh(bh);
				spin_unlock(&journal->j_list_lock);

				if (batch_count)
					__flush_batch(journal, &batch_count);
				jbd2_journal_abort(journal, -EFSCORRUPTED);
				return -EFSCORRUPTED;
			}
			journal->j_chkpt_bhs[batch_count++] = bh;
			transaction->t_chp_stats.cs_written++;
			transaction->t_checkpoint_list = jh->b_cpnext;
		}

		if ((batch_count == JBD2_NR_BATCH) ||
		    need_resched() || spin_needbreak(&journal->j_list_lock) ||
		    jh2bh(transaction->t_checkpoint_list) == journal->j_chkpt_bhs[0])
			goto unlock_and_flush;
	}

	if (batch_count) {
		unlock_and_flush:
			spin_unlock(&journal->j_list_lock);
		retry:
			if (batch_count)
				__flush_batch(journal, &batch_count);
			cond_resched();
			spin_lock(&journal->j_list_lock);
			goto restart;
	}

out:
	spin_unlock(&journal->j_list_lock);
	result = jbd2_cleanup_journal_tail(journal);

	return (result < 0) ? result : 0;
}


/**
 * jbd2_cleanup_journal_tail - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_cleanup_journal_tail(journal_t *journal)
{
	tid_t		first_tid;
	unsigned long	blocknr;

	if (is_journal_aborted(journal))
		return -EIO;

	if (!jbd2_journal_get_log_tail(journal, &first_tid, &blocknr))
		return 1;
	if (WARN_ON_ONCE(blocknr == 0)) {
		jbd2_journal_abort(journal, -EFSCORRUPTED);
		return -EFSCORRUPTED;
	}


	if (journal->j_flags & JBD2_BARRIER)
		blkdev_issue_flush(journal->j_fs_dev);

	return __jbd2_update_log_tail(journal, first_tid, blocknr);
}


/**
 * journal_shrink_one_cp_list - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static unsigned long journal_shrink_one_cp_list(struct journal_head *jh,
						enum jbd2_shrink_type type,
						bool *released)
{
	struct journal_head *last_jh;
	struct journal_head *next_jh = jh;
	unsigned long nr_freed = 0;
	int ret;

	*released = false;
	if (!jh)
		return 0;

	last_jh = jh->b_cpprev;
	do {
		jh = next_jh;
		next_jh = jh->b_cpnext;

		if (type == JBD2_SHRINK_DESTROY) {
			ret = __jbd2_journal_remove_checkpoint(jh);
		} else {
			ret = jbd2_journal_try_remove_checkpoint(jh);
			if (ret < 0) {
				if (type == JBD2_SHRINK_BUSY_SKIP)
					continue;
				break;
			}
		}

		nr_freed++;
		if (ret) {
			*released = true;
			break;
		}

		if (need_resched())
			break;
	} while (jh != last_jh);

	return nr_freed;
}


/**
 * jbd2_journal_shrink_checkpoint_list - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
unsigned long jbd2_journal_shrink_checkpoint_list(journal_t *journal,
						  unsigned long *nr_to_scan)
{
	transaction_t *transaction, *last_transaction, *next_transaction;
	bool __maybe_unused released;
	tid_t first_tid = 0, last_tid = 0, next_tid = 0;
	tid_t tid = 0;
	unsigned long nr_freed = 0;
	unsigned long freed;
	bool first_set = false;

again:
	spin_lock(&journal->j_list_lock);
	if (!journal->j_checkpoint_transactions) {
		spin_unlock(&journal->j_list_lock);
		goto out;
	}


	if (journal->j_shrink_transaction)
		transaction = journal->j_shrink_transaction;
	else
		transaction = journal->j_checkpoint_transactions;

	if (!first_set) {
		first_tid = transaction->t_tid;
		first_set = true;
	}
	last_transaction = journal->j_checkpoint_transactions->t_cpprev;
	next_transaction = transaction;
	last_tid = last_transaction->t_tid;
	do {
		transaction = next_transaction;
		next_transaction = transaction->t_cpnext;
		tid = transaction->t_tid;

		freed = journal_shrink_one_cp_list(transaction->t_checkpoint_list,
						   JBD2_SHRINK_BUSY_SKIP, &released);
		nr_freed += freed;
		(*nr_to_scan) -= min(*nr_to_scan, freed);
		if (*nr_to_scan == 0)
			break;
		if (need_resched() || spin_needbreak(&journal->j_list_lock))
			break;
	} while (transaction != last_transaction);

	if (transaction != last_transaction) {
		journal->j_shrink_transaction = next_transaction;
		next_tid = next_transaction->t_tid;
	} else {
		journal->j_shrink_transaction = NULL;
		next_tid = 0;
	}

	spin_unlock(&journal->j_list_lock);
	cond_resched();

	if (*nr_to_scan && journal->j_shrink_transaction)
		goto again;
out:
	trace_jbd2_shrink_checkpoint_list(journal, first_tid, tid, last_tid,
					  nr_freed, next_tid);

	return nr_freed;
}


/**
 * __jbd2_journal_clean_checkpoint_list - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void __jbd2_journal_clean_checkpoint_list(journal_t *journal,
					  enum jbd2_shrink_type type)
{
	transaction_t *transaction, *last_transaction, *next_transaction;
	bool released;

	WARN_ON_ONCE(type == JBD2_SHRINK_BUSY_SKIP);

	transaction = journal->j_checkpoint_transactions;
	if (!transaction)
		return;

	last_transaction = transaction->t_cpprev;
	next_transaction = transaction;
	do {
		transaction = next_transaction;
		next_transaction = transaction->t_cpnext;
		journal_shrink_one_cp_list(transaction->t_checkpoint_list,
					   type, &released);


		if (need_resched())
			return;


		if (!released)
			return;
	} while (transaction != last_transaction);
}


/**
 * jbd2_journal_destroy_checkpoint - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void jbd2_journal_destroy_checkpoint(journal_t *journal)
{


	while (1) {
		spin_lock(&journal->j_list_lock);
		if (!journal->j_checkpoint_transactions) {
			spin_unlock(&journal->j_list_lock);
			break;
		}
		__jbd2_journal_clean_checkpoint_list(journal, JBD2_SHRINK_DESTROY);
		spin_unlock(&journal->j_list_lock);
		cond_resched();
	}
}


/**
 * __jbd2_journal_remove_checkpoint - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int __jbd2_journal_remove_checkpoint(struct journal_head *jh)
{
	struct transaction_chp_stats_s *stats;
	transaction_t *transaction;
	journal_t *journal;

	JBUFFER_TRACE(jh, "entry");

	transaction = jh->b_cp_transaction;
	if (!transaction) {
		JBUFFER_TRACE(jh, "not on transaction");
		return 0;
	}
	journal = transaction->t_journal;

	JBUFFER_TRACE(jh, "removing from transaction");

	__buffer_unlink(jh);
	jh->b_cp_transaction = NULL;
	percpu_counter_dec(&journal->j_checkpoint_jh_count);
	jbd2_journal_put_journal_head(jh);


	if (transaction->t_checkpoint_list)
		return 0;


	if (transaction->t_state != T_FINISHED)
		return 0;


	stats = &transaction->t_chp_stats;
	if (stats->cs_chp_time)
		stats->cs_chp_time = jbd2_time_diff(stats->cs_chp_time,
						    jiffies);
	trace_jbd2_checkpoint_stats(journal->j_fs_dev->bd_dev,
				    transaction->t_tid, stats);

	__jbd2_journal_drop_transaction(journal, transaction);
	jbd2_journal_free_transaction(transaction);
	return 1;
}


/**
 * jbd2_journal_try_remove_checkpoint - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_try_remove_checkpoint(struct journal_head *jh)
{
	struct buffer_head *bh = jh2bh(jh);

	if (jh->b_transaction)
		return -EBUSY;
	if (!trylock_buffer(bh))
		return -EBUSY;
	if (buffer_dirty(bh)) {
		unlock_buffer(bh);
		return -EBUSY;
	}
	unlock_buffer(bh);


	JBUFFER_TRACE(jh, "remove from checkpoint list");
	return __jbd2_journal_remove_checkpoint(jh);
}


/**
 * __jbd2_journal_insert_checkpoint - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void __jbd2_journal_insert_checkpoint(struct journal_head *jh,
			       transaction_t *transaction)
{
	JBUFFER_TRACE(jh, "entry");
	J_ASSERT_JH(jh, buffer_dirty(jh2bh(jh)) || buffer_jbddirty(jh2bh(jh)));
	J_ASSERT_JH(jh, jh->b_cp_transaction == NULL);


	jbd2_journal_grab_journal_head(jh2bh(jh));
	jh->b_cp_transaction = transaction;

	if (!transaction->t_checkpoint_list) {
		jh->b_cpnext = jh->b_cpprev = jh;
	} else {
		jh->b_cpnext = transaction->t_checkpoint_list;
		jh->b_cpprev = transaction->t_checkpoint_list->b_cpprev;
		jh->b_cpprev->b_cpnext = jh;
		jh->b_cpnext->b_cpprev = jh;
	}
	transaction->t_checkpoint_list = jh;
	percpu_counter_inc(&transaction->t_journal->j_checkpoint_jh_count);
}


/**
 * __jbd2_journal_drop_transaction - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void __jbd2_journal_drop_transaction(journal_t *journal, transaction_t *transaction)
{
	assert_spin_locked(&journal->j_list_lock);

	journal->j_shrink_transaction = NULL;
	if (transaction->t_cpnext) {
		transaction->t_cpnext->t_cpprev = transaction->t_cpprev;
		transaction->t_cpprev->t_cpnext = transaction->t_cpnext;
		if (journal->j_checkpoint_transactions == transaction)
			journal->j_checkpoint_transactions =
				transaction->t_cpnext;
		if (journal->j_checkpoint_transactions == transaction)
			journal->j_checkpoint_transactions = NULL;
	}

	J_ASSERT(transaction->t_state == T_FINISHED);
	J_ASSERT(transaction->t_buffers == NULL);
	J_ASSERT(transaction->t_forget == NULL);
	J_ASSERT(transaction->t_shadow_list == NULL);
	J_ASSERT(transaction->t_checkpoint_list == NULL);
	J_ASSERT(atomic_read(&transaction->t_updates) == 0);
	J_ASSERT(journal->j_committing_transaction != transaction);
	J_ASSERT(journal->j_running_transaction != transaction);

	trace_jbd2_drop_transaction(journal, transaction);

	jbd2_debug(1, "Dropping transaction %d, all done\n", transaction->t_tid);
}
