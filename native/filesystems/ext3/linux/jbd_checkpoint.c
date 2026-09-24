/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Filesystem Support EXT3 embedded checkpoint engine.
 *
 * A committed transaction remains journal-owned until every checkpointed
 * metadata buffer has reached its home block.  Pending checkpoint buffers and
 * submitted checkpoint I/O are maintained as two independent circular rings.
 *
 * Locking:
 *   - j_list_lock protects checkpoint transaction/ring membership.
 *   - buffer journal state is protected by the per-buffer JBD state lock.
 *   - j_state_lock protects log head/tail accounting.
 *
 * The implementation is intentionally local to EXT3.  It implements the JBD
 * contract required by this module without depending on an external jbd.ko.
 */

#include <linux/blkdev.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/slab.h>

#include "journal.h"

#define IFS_EXT3_CP_BATCH_LIMIT 64U

struct ifs_ext3_cp_batch {
	struct buffer_head *items[IFS_EXT3_CP_BATCH_LIMIT];
	unsigned int count;
};

static void ifs_ext3_cp_ring_detach(struct journal_head *jh)
{
	transaction_t *transaction = jh->b_cp_transaction;
	struct journal_head *next = jh->b_cpnext;
	struct journal_head *previous = jh->b_cpprev;

	previous->b_cpnext = next;
	next->b_cpprev = previous;

	if (transaction->t_checkpoint_list == jh)
		transaction->t_checkpoint_list =
			next == jh ? NULL : next;
	if (transaction->t_checkpoint_io_list == jh)
		transaction->t_checkpoint_io_list =
			next == jh ? NULL : next;

	jh->b_cpnext = NULL;
	jh->b_cpprev = NULL;
}

static void ifs_ext3_cp_ring_append(struct journal_head **head,
				    struct journal_head *jh)
{
	if (!*head) {
		jh->b_cpnext = jh;
		jh->b_cpprev = jh;
		*head = jh;
		return;
	}

	jh->b_cpnext = *head;
	jh->b_cpprev = (*head)->b_cpprev;
	(*head)->b_cpprev->b_cpnext = jh;
	(*head)->b_cpprev = jh;
}

static void ifs_ext3_cp_move_to_io(struct journal_head *jh)
{
	transaction_t *transaction = jh->b_cp_transaction;

	ifs_ext3_cp_ring_detach(jh);
	ifs_ext3_cp_ring_append(&transaction->t_checkpoint_io_list, jh);
}

static void ifs_ext3_cp_batch_reset(struct ifs_ext3_cp_batch *batch)
{
	unsigned int index;

	for (index = 0; index < batch->count; ++index)
		batch->items[index] = NULL;
	batch->count = 0;
}

static void ifs_ext3_cp_batch_submit(struct ifs_ext3_cp_batch *batch)
{
	struct blk_plug plug;
	unsigned int index;

	if (!batch->count)
		return;

	blk_start_plug(&plug);
	for (index = 0; index < batch->count; ++index)
		write_dirty_buffer(batch->items[index], REQ_SYNC);
	blk_finish_plug(&plug);

	for (index = 0; index < batch->count; ++index) {
		clear_buffer_jwrite(batch->items[index]);
		__brelse(batch->items[index]);
	}

	ifs_ext3_cp_batch_reset(batch);
}

static bool ifs_ext3_cp_transaction_still_current(
	journal_t *journal, transaction_t *transaction, tid_t tid)
{
	return journal->j_checkpoint_transactions == transaction &&
	       transaction->t_tid == tid;
}

static void ifs_ext3_cp_wait_for_state_lock(
	journal_t *journal, struct buffer_head *bh,
	struct ifs_ext3_cp_batch *batch)
{
	get_bh(bh);
	spin_unlock(&journal->j_list_lock);
	ifs_ext3_cp_batch_submit(batch);
	jbd_lock_bh_state(bh);
	jbd_unlock_bh_state(bh);
	__brelse(bh);
	cond_resched();
	spin_lock(&journal->j_list_lock);
}

static void ifs_ext3_cp_wait_for_buffer(
	journal_t *journal, struct buffer_head *bh,
	struct ifs_ext3_cp_batch *batch)
{
	get_bh(bh);
	spin_unlock(&journal->j_list_lock);
	jbd_unlock_bh_state(bh);
	ifs_ext3_cp_batch_submit(batch);
	wait_on_buffer(bh);
	__brelse(bh);
	cond_resched();
	spin_lock(&journal->j_list_lock);
}

static void ifs_ext3_cp_wait_for_transaction(
	journal_t *journal, tid_t tid,
	struct buffer_head *bh,
	struct ifs_ext3_cp_batch *batch)
{
	spin_unlock(&journal->j_list_lock);
	jbd_unlock_bh_state(bh);
	ifs_ext3_cp_batch_submit(batch);
	log_start_commit(journal, tid);
	log_wait_commit(journal, tid);
	cond_resched();
	spin_lock(&journal->j_list_lock);
}

static int ifs_ext3_cp_submit_pending(
	journal_t *journal, transaction_t *transaction)
{
	struct ifs_ext3_cp_batch batch = { };
	const tid_t transaction_id = transaction->t_tid;
	int first_error = 0;

	while (ifs_ext3_cp_transaction_still_current(
		       journal, transaction, transaction_id)) {
		struct journal_head *jh;
		struct buffer_head *bh;

		jh = transaction->t_checkpoint_list;
		if (!jh)
			break;

		bh = jh2bh(jh);
		if (!jbd_trylock_bh_state(bh)) {
			ifs_ext3_cp_wait_for_state_lock(journal, bh, &batch);
			continue;
		}

		if (buffer_locked(bh)) {
			ifs_ext3_cp_wait_for_buffer(journal, bh, &batch);
			continue;
		}

		if (jh->b_transaction) {
			tid_t owner_tid = jh->b_transaction->t_tid;

			ifs_ext3_cp_wait_for_transaction(
				journal, owner_tid, bh, &batch);
			continue;
		}

		if (!buffer_dirty(bh)) {
			if (buffer_write_io_error(bh) && !first_error)
				first_error = -EIO;

			get_bh(bh);
			__journal_remove_checkpoint(jh);
			jbd_unlock_bh_state(bh);
			__brelse(bh);
			continue;
		}

		get_bh(bh);
		set_buffer_jwrite(bh);
		batch.items[batch.count++] = bh;
		ifs_ext3_cp_move_to_io(jh);
		jbd_unlock_bh_state(bh);

		if (batch.count == IFS_EXT3_CP_BATCH_LIMIT ||
		    need_resched() ||
		    spin_needbreak(&journal->j_list_lock)) {
			spin_unlock(&journal->j_list_lock);
			ifs_ext3_cp_batch_submit(&batch);
			cond_resched();
			spin_lock(&journal->j_list_lock);
		}
	}

	if (batch.count) {
		spin_unlock(&journal->j_list_lock);
		ifs_ext3_cp_batch_submit(&batch);
		spin_lock(&journal->j_list_lock);
	}

	return first_error;
}

static int ifs_ext3_cp_retire_submitted(
	journal_t *journal, transaction_t *transaction)
{
	const tid_t transaction_id = transaction->t_tid;
	int first_error = 0;

	while (ifs_ext3_cp_transaction_still_current(
		       journal, transaction, transaction_id)) {
		struct journal_head *jh;
		struct buffer_head *bh;

		jh = transaction->t_checkpoint_io_list;
		if (!jh)
			break;

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

		if (buffer_write_io_error(bh) && !first_error)
			first_error = -EIO;

		__journal_remove_checkpoint(jh);
		jbd_unlock_bh_state(bh);
		__brelse(bh);
	}

	return first_error;
}

static bool ifs_ext3_cp_log_has_space(journal_t *journal, int required)
{
	return __log_space_left(journal) >= required;
}

void __log_wait_for_space(journal_t *journal)
{
	int required;

	assert_spin_locked(&journal->j_state_lock);

	for (;;) {
		bool have_checkpoint;
		tid_t commit_tid = 0;
		int available;

		required = jbd_space_needed(journal);
		if (ifs_ext3_cp_log_has_space(journal, required))
			return;
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
					"EXT3 JBD: %pg requires %d log blocks; %d available\n",
					journal->j_dev,
					required, available);
				journal_abort(journal, -ENOSPC);
			}
		}

		spin_lock(&journal->j_state_lock);
		mutex_unlock(&journal->j_checkpoint_mutex);
	}
}

int log_do_checkpoint(journal_t *journal)
{
	transaction_t *transaction;
	int error;
	int retire_error;

	error = cleanup_journal_tail(journal);
	if (error <= 0)
		return error;

	spin_lock(&journal->j_list_lock);
	transaction = journal->j_checkpoint_transactions;
	if (!transaction) {
		spin_unlock(&journal->j_list_lock);
		return 0;
	}

	error = ifs_ext3_cp_submit_pending(journal, transaction);
	retire_error =
		ifs_ext3_cp_retire_submitted(journal, transaction);
	if (!error)
		error = retire_error;
	spin_unlock(&journal->j_list_lock);

	if (error < 0) {
		journal_abort(journal, error);
		return error;
	}

	error = cleanup_journal_tail(journal);
	return error < 0 ? error : 0;
}

static void ifs_ext3_cp_oldest_log_position(
	journal_t *journal, tid_t *tid, unsigned int *block)
{
	transaction_t *transaction;

	transaction = journal->j_checkpoint_transactions;
	if (transaction) {
		*tid = transaction->t_tid;
		*block = transaction->t_log_start;
		return;
	}

	transaction = journal->j_committing_transaction;
	if (transaction) {
		*tid = transaction->t_tid;
		*block = transaction->t_log_start;
		return;
	}

	transaction = journal->j_running_transaction;
	if (transaction) {
		*tid = transaction->t_tid;
		*block = journal->j_head;
		return;
	}

	*tid = journal->j_transaction_sequence;
	*block = journal->j_head;
}

static unsigned int ifs_ext3_cp_reclaimed_blocks(
	const journal_t *journal, unsigned int new_tail)
{
	if (new_tail >= journal->j_tail)
		return new_tail - journal->j_tail;

	return (journal->j_last - journal->j_tail) +
	       (new_tail - journal->j_first);
}

int cleanup_journal_tail(journal_t *journal)
{
	tid_t new_sequence;
	unsigned int new_tail;
	unsigned int reclaimed;

	if (is_journal_aborted(journal))
		return 1;

	spin_lock(&journal->j_state_lock);
	spin_lock(&journal->j_list_lock);
	ifs_ext3_cp_oldest_log_position(
		journal, &new_sequence, &new_tail);
	spin_unlock(&journal->j_list_lock);

	if (!new_tail) {
		spin_unlock(&journal->j_state_lock);
		journal_abort(journal, -EUCLEAN);
		return -EUCLEAN;
	}

	if (journal->j_tail_sequence == new_sequence) {
		spin_unlock(&journal->j_state_lock);
		return 1;
	}
	spin_unlock(&journal->j_state_lock);

	journal_update_sb_log_tail(
		journal, new_sequence, new_tail,
		REQ_PREFLUSH | REQ_FUA);

	spin_lock(&journal->j_state_lock);
	reclaimed = ifs_ext3_cp_reclaimed_blocks(journal, new_tail);
	journal->j_free += reclaimed;
	journal->j_tail_sequence = new_sequence;
	journal->j_tail = new_tail;
	spin_unlock(&journal->j_state_lock);

	return 0;
}

static int ifs_ext3_cp_try_prune(struct journal_head *jh)
{
	struct buffer_head *bh = jh2bh(jh);
	int transaction_dropped;

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
	transaction_dropped = __journal_remove_checkpoint(jh);
	jbd_unlock_bh_state(bh);
	__brelse(bh);

	return transaction_dropped ? 2 : 1;
}

static int ifs_ext3_cp_prune_ring(
	struct journal_head *head, int *transaction_dropped)
{
	struct journal_head *stop;
	struct journal_head *cursor;
	int removed = 0;

	*transaction_dropped = 0;
	if (!head)
		return 0;

	stop = head->b_cpprev;
	cursor = head;

	for (;;) {
		struct journal_head *next = cursor->b_cpnext;
		int status = ifs_ext3_cp_try_prune(cursor);

		if (status) {
			++removed;
			if (status == 2) {
				*transaction_dropped = 1;
				break;
			}
		}

		if (cursor == stop || need_resched())
			break;
		cursor = next;
	}

	return removed;
}

int __journal_clean_checkpoint_list(journal_t *journal)
{
	transaction_t *first;
	transaction_t *last;
	transaction_t *transaction;
	int removed = 0;

	first = journal->j_checkpoint_transactions;
	if (!first)
		return 0;

	last = first->t_cpprev;
	transaction = first;

	for (;;) {
		transaction_t *next = transaction->t_cpnext;
		int dropped;

		removed += ifs_ext3_cp_prune_ring(
			transaction->t_checkpoint_list, &dropped);
		if (!dropped)
			removed += ifs_ext3_cp_prune_ring(
				transaction->t_checkpoint_io_list, &dropped);

		if (dropped || transaction == last || need_resched())
			break;
		transaction = next;
	}

	return removed;
}

int __journal_remove_checkpoint(struct journal_head *jh)
{
	transaction_t *transaction = jh->b_cp_transaction;
	journal_t *journal;

	if (!transaction)
		return 0;

	journal = transaction->t_journal;
	ifs_ext3_cp_ring_detach(jh);
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
	J_ASSERT_JH(jh,
		buffer_dirty(jh2bh(jh)) ||
		buffer_jbddirty(jh2bh(jh)));
	J_ASSERT_JH(jh, jh->b_cp_transaction == NULL);

	journal_grab_journal_head(jh2bh(jh));
	jh->b_cp_transaction = transaction;
	ifs_ext3_cp_ring_append(&transaction->t_checkpoint_list, jh);
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
