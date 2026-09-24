/*
 * Filesystem Support EXT3 embedded journal checkpoint engine.
 *
 * Checkpointing writes committed metadata to its home location, waits for the
 * writeback to finish, and only then retires the transaction from the JBD log.
 * The two JBD checkpoint rings are treated as ownership lists: pending buffers
 * live on t_checkpoint_list and submitted I/O lives on t_checkpoint_io_list.
 */

#include <linux/blkdev.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include "journal.h"

#define IFS_EXT3_CHECKPOINT_BATCH 64

static void ifs_ext3_unlink_checkpoint_head(struct journal_head *jh)
{
	transaction_t *transaction = jh->b_cp_transaction;
	struct journal_head *next = jh->b_cpnext;
	struct journal_head *prev = jh->b_cpprev;

	prev->b_cpnext = next;
	next->b_cpprev = prev;

	if (transaction->t_checkpoint_list == jh)
		transaction->t_checkpoint_list =
			next == jh ? NULL : next;
	if (transaction->t_checkpoint_io_list == jh)
		transaction->t_checkpoint_io_list =
			next == jh ? NULL : next;
}

static void ifs_ext3_move_checkpoint_to_io(struct journal_head *jh)
{
	transaction_t *transaction = jh->b_cp_transaction;
	struct journal_head *head;

	ifs_ext3_unlink_checkpoint_head(jh);

	head = transaction->t_checkpoint_io_list;
	if (!head) {
		jh->b_cpnext = jh;
		jh->b_cpprev = jh;
	} else {
		jh->b_cpnext = head;
		jh->b_cpprev = head->b_cpprev;
		head->b_cpprev->b_cpnext = jh;
		head->b_cpprev = jh;
	}
	transaction->t_checkpoint_io_list = jh;
}

static void ifs_ext3_flush_checkpoint_batch(
	struct buffer_head **buffers, int *count)
{
	struct blk_plug plug;
	int index;

	if (*count == 0)
		return;

	blk_start_plug(&plug);
	for (index = 0; index < *count; ++index)
		write_dirty_buffer(buffers[index], REQ_SYNC);
	blk_finish_plug(&plug);

	for (index = 0; index < *count; ++index) {
		clear_buffer_jwrite(buffers[index]);
		__brelse(buffers[index]);
		buffers[index] = NULL;
	}

	*count = 0;
}

void __log_wait_for_space(journal_t *journal)
{
	int required;

	assert_spin_locked(&journal->j_state_lock);
	required = jbd_space_needed(journal);

	while (__log_space_left(journal) < required) {
		bool have_checkpoint;
		tid_t commit_tid = 0;
		int available;

		if (journal->j_flags & JFS_ABORT)
			return;

		spin_unlock(&journal->j_state_lock);
		mutex_lock(&journal->j_checkpoint_mutex);
		spin_lock(&journal->j_state_lock);
		spin_lock(&journal->j_list_lock);

		required = jbd_space_needed(journal);
		available = __log_space_left(journal);
		have_checkpoint =
			journal->j_checkpoint_transactions != NULL;
		if (journal->j_committing_transaction)
			commit_tid =
				journal->j_committing_transaction->t_tid;

		spin_unlock(&journal->j_list_lock);

		if (available >= required) {
			mutex_unlock(&journal->j_checkpoint_mutex);
			continue;
		}

		spin_unlock(&journal->j_state_lock);

		if (have_checkpoint) {
			log_do_checkpoint(journal);
		} else if (cleanup_journal_tail(journal) != 0) {
			if (commit_tid) {
				log_wait_commit(journal, commit_tid);
			} else {
				pr_err(
					"JBD: %s needs %d blocks but only %d remain\n",
					journal->j_devname,
					required, available);
				journal_abort(journal, -ENOSPC);
			}
		}

		spin_lock(&journal->j_state_lock);
		mutex_unlock(&journal->j_checkpoint_mutex);
	}
}

static int ifs_ext3_wait_checkpoint_io(
	journal_t *journal, transaction_t *transaction)
{
	const tid_t tid = transaction->t_tid;
	int result = 0;

	for (;;) {
		struct journal_head *jh;
		struct buffer_head *bh;

		if (journal->j_checkpoint_transactions != transaction ||
		    transaction->t_tid != tid)
			return result;

		jh = transaction->t_checkpoint_io_list;
		if (!jh)
			return result;

		bh = jh2bh(jh);
		if (!jbd_trylock_bh_state(bh)) {
			get_bh(bh);
			spin_unlock(&journal->j_list_lock);
			jbd_lock_bh_state(bh);
			jbd_unlock_bh_state(bh);
			__brelse(bh);
			spin_lock(&journal->j_list_lock);
			continue;
		}

		get_bh(bh);
		if (buffer_locked(bh)) {
			spin_unlock(&journal->j_list_lock);
			jbd_unlock_bh_state(bh);
			wait_on_buffer(bh);
			__brelse(bh);
			spin_lock(&journal->j_list_lock);
			continue;
		}

		if (buffer_write_io_error(bh) && !result)
			result = -EIO;

		__journal_remove_checkpoint(jh);
		jbd_unlock_bh_state(bh);
		__brelse(bh);
	}
}

static int ifs_ext3_checkpoint_pending(
	journal_t *journal,
	transaction_t *transaction)
{
	struct buffer_head *batch[IFS_EXT3_CHECKPOINT_BATCH];
	const tid_t tid = transaction->t_tid;
	int batch_count = 0;
	int result = 0;

	for (;;) {
		struct journal_head *jh;
		struct buffer_head *bh;

		if (journal->j_checkpoint_transactions != transaction ||
		    transaction->t_tid != tid)
			break;

		jh = transaction->t_checkpoint_list;
		if (!jh)
			break;

		bh = jh2bh(jh);
		if (!jbd_trylock_bh_state(bh)) {
			get_bh(bh);
			spin_unlock(&journal->j_list_lock);
			ifs_ext3_flush_checkpoint_batch(
				batch, &batch_count);
			jbd_lock_bh_state(bh);
			jbd_unlock_bh_state(bh);
			__brelse(bh);
			cond_resched();
			spin_lock(&journal->j_list_lock);
			continue;
		}

		if (buffer_locked(bh)) {
			get_bh(bh);
			spin_unlock(&journal->j_list_lock);
			jbd_unlock_bh_state(bh);
			ifs_ext3_flush_checkpoint_batch(
				batch, &batch_count);
			wait_on_buffer(bh);
			__brelse(bh);
			cond_resched();
			spin_lock(&journal->j_list_lock);
			continue;
		}

		if (jh->b_transaction) {
			const tid_t wait_tid =
				jh->b_transaction->t_tid;

			spin_unlock(&journal->j_list_lock);
			jbd_unlock_bh_state(bh);
			ifs_ext3_flush_checkpoint_batch(
				batch, &batch_count);
			log_start_commit(journal, wait_tid);
			log_wait_commit(journal, wait_tid);
			cond_resched();
			spin_lock(&journal->j_list_lock);
			continue;
		}

		if (!buffer_dirty(bh)) {
			if (buffer_write_io_error(bh) && !result)
				result = -EIO;
			get_bh(bh);
			__journal_remove_checkpoint(jh);
			jbd_unlock_bh_state(bh);
			__brelse(bh);
			continue;
		}

		get_bh(bh);
		set_buffer_jwrite(bh);
		batch[batch_count++] = bh;
		ifs_ext3_move_checkpoint_to_io(jh);
		jbd_unlock_bh_state(bh);

		if (batch_count == IFS_EXT3_CHECKPOINT_BATCH ||
		    need_resched() ||
		    spin_needbreak(&journal->j_list_lock)) {
			spin_unlock(&journal->j_list_lock);
			ifs_ext3_flush_checkpoint_batch(
				batch, &batch_count);
			cond_resched();
			spin_lock(&journal->j_list_lock);
		}
	}

	if (batch_count) {
		spin_unlock(&journal->j_list_lock);
		ifs_ext3_flush_checkpoint_batch(batch, &batch_count);
		spin_lock(&journal->j_list_lock);
	}

	return result;
}

int log_do_checkpoint(journal_t *journal)
{
	transaction_t *transaction;
	int result;
	int wait_result;

	result = cleanup_journal_tail(journal);
	if (result <= 0)
		return result;

	spin_lock(&journal->j_list_lock);
	transaction = journal->j_checkpoint_transactions;
	if (!transaction) {
		spin_unlock(&journal->j_list_lock);
		return 0;
	}

	result = ifs_ext3_checkpoint_pending(journal, transaction);
	wait_result =
		ifs_ext3_wait_checkpoint_io(journal, transaction);
	if (!result)
		result = wait_result;
	spin_unlock(&journal->j_list_lock);

	if (result < 0) {
		journal_abort(journal, result);
		return result;
	}

	result = cleanup_journal_tail(journal);
	return result < 0 ? result : 0;
}

int cleanup_journal_tail(journal_t *journal)
{
	transaction_t *transaction;
	tid_t first_tid;
	unsigned int block;
	unsigned int freed;

	if (is_journal_aborted(journal))
		return 1;

	spin_lock(&journal->j_state_lock);
	spin_lock(&journal->j_list_lock);

	transaction = journal->j_checkpoint_transactions;
	if (transaction) {
		first_tid = transaction->t_tid;
		block = transaction->t_log_start;
	} else if (journal->j_committing_transaction) {
		transaction = journal->j_committing_transaction;
		first_tid = transaction->t_tid;
		block = transaction->t_log_start;
	} else if (journal->j_running_transaction) {
		transaction = journal->j_running_transaction;
		first_tid = transaction->t_tid;
		block = journal->j_head;
	} else {
		first_tid = journal->j_transaction_sequence;
		block = journal->j_head;
	}

	spin_unlock(&journal->j_list_lock);

	if (!block) {
		spin_unlock(&journal->j_state_lock);
		journal_abort(journal, -EFSCORRUPTED);
		return -EFSCORRUPTED;
	}

	if (journal->j_tail_sequence == first_tid) {
		spin_unlock(&journal->j_state_lock);
		return 1;
	}
	spin_unlock(&journal->j_state_lock);

	journal_update_sb_log_tail(
		journal, first_tid, block,
		REQ_PREFLUSH | REQ_FUA);

	spin_lock(&journal->j_state_lock);
	freed = block - journal->j_tail;
	if (block < journal->j_tail)
		freed += journal->j_last - journal->j_first;

	journal->j_free += freed;
	journal->j_tail_sequence = first_tid;
	journal->j_tail = block;
	spin_unlock(&journal->j_state_lock);
	return 0;
}

static int ifs_ext3_try_remove_checkpoint(struct journal_head *jh)
{
	struct buffer_head *bh = jh2bh(jh);
	int released;

	if (!jbd_trylock_bh_state(bh))
		return 0;

	if (jh->b_jlist != BJ_None ||
	    buffer_locked(bh) ||
	    buffer_dirty(bh) ||
	    buffer_write_io_error(bh)) {
		jbd_unlock_bh_state(bh);
		return 0;
	}

	get_bh(bh);
	released = __journal_remove_checkpoint(jh);
	jbd_unlock_bh_state(bh);
	__brelse(bh);
	return released ? 2 : 1;
}

static int ifs_ext3_clean_checkpoint_ring(
	struct journal_head *first, int *transaction_released)
{
	struct journal_head *last;
	struct journal_head *current;
	int cleaned = 0;

	*transaction_released = 0;
	if (!first)
		return 0;

	last = first->b_cpprev;
	current = first;
	for (;;) {
		struct journal_head *next = current->b_cpnext;
		int result =
			ifs_ext3_try_remove_checkpoint(current);

		if (result) {
			cleaned++;
			if (result == 2) {
				*transaction_released = 1;
				break;
			}
		}

		if (current == last || need_resched())
			break;
		current = next;
	}

	return cleaned;
}

int __journal_clean_checkpoint_list(journal_t *journal)
{
	transaction_t *transaction;
	transaction_t *last;
	int cleaned = 0;

	transaction = journal->j_checkpoint_transactions;
	if (!transaction)
		return 0;

	last = transaction->t_cpprev;
	for (;;) {
		transaction_t *next = transaction->t_cpnext;
		int released;

		cleaned += ifs_ext3_clean_checkpoint_ring(
			transaction->t_checkpoint_list, &released);
		if (!released)
			cleaned += ifs_ext3_clean_checkpoint_ring(
				transaction->t_checkpoint_io_list,
				&released);

		if (need_resched() || transaction == last)
			break;
		transaction = next;
	}

	return cleaned;
}

int __journal_remove_checkpoint(struct journal_head *jh)
{
	transaction_t *transaction = jh->b_cp_transaction;
	journal_t *journal;

	if (!transaction)
		return 0;

	journal = transaction->t_journal;
	ifs_ext3_unlink_checkpoint_head(jh);
	jh->b_cp_transaction = NULL;
	journal_put_journal_head(jh);

	if (transaction->t_checkpoint_list ||
	    transaction->t_checkpoint_io_list ||
	    transaction->t_state != T_FINISHED)
		return 0;

	__journal_drop_transaction(journal, transaction);
	wake_up(&journal->j_wait_logspace);
	return 1;
}

void __journal_insert_checkpoint(
	struct journal_head *jh, transaction_t *transaction)
{
	struct journal_head *head;

	J_ASSERT_JH(jh,
		buffer_dirty(jh2bh(jh)) ||
		buffer_jbddirty(jh2bh(jh)));
	J_ASSERT_JH(jh, jh->b_cp_transaction == NULL);

	journal_grab_journal_head(jh2bh(jh));
	jh->b_cp_transaction = transaction;

	head = transaction->t_checkpoint_list;
	if (!head) {
		jh->b_cpnext = jh;
		jh->b_cpprev = jh;
	} else {
		jh->b_cpnext = head;
		jh->b_cpprev = head->b_cpprev;
		head->b_cpprev->b_cpnext = jh;
		head->b_cpprev = jh;
	}
	transaction->t_checkpoint_list = jh;
}

void __journal_drop_transaction(
	journal_t *journal, transaction_t *transaction)
{
	assert_spin_locked(&journal->j_list_lock);

	if (transaction->t_cpnext) {
		transaction->t_cpnext->t_cpprev =
			transaction->t_cpprev;
		transaction->t_cpprev->t_cpnext =
			transaction->t_cpnext;

		if (journal->j_checkpoint_transactions ==
		    transaction) {
			journal->j_checkpoint_transactions =
				transaction->t_cpnext;
			if (journal->j_checkpoint_transactions ==
			    transaction)
				journal->j_checkpoint_transactions = NULL;
		}
	}

	J_ASSERT(transaction->t_state == T_FINISHED);
	J_ASSERT(transaction->t_buffers == NULL);
	J_ASSERT(transaction->t_sync_datalist == NULL);
	J_ASSERT(transaction->t_forget == NULL);
	J_ASSERT(transaction->t_iobuf_list == NULL);
	J_ASSERT(transaction->t_shadow_list == NULL);
	J_ASSERT(transaction->t_log_list == NULL);
	J_ASSERT(transaction->t_checkpoint_list == NULL);
	J_ASSERT(transaction->t_checkpoint_io_list == NULL);
	J_ASSERT(transaction->t_updates == 0);
	J_ASSERT(journal->j_committing_transaction != transaction);
	J_ASSERT(journal->j_running_transaction != transaction);

	kfree(transaction);
}
