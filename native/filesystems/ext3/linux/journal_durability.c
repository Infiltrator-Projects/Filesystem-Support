/* Infiltrator Filesystem Support — EXT3 journal durability engine.
 * Checkpoint, commit, recovery and revoke handling are one crash-consistency
 * responsibility. Journal core/thread ownership remains separately isolated
 * while inherited units complete their replacement.
 */

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


/* ===== commit ===== */
/*
 * Filesystem Support EXT3 embedded journal commit engine.
 *
 * A commit has four durable ordering boundaries:
 *   1. stop new handles and drain users of the running transaction;
 *   2. finish ordered-data writeback before metadata can be committed;
 *   3. write descriptor, revoke and metadata log records and wait for them;
 *   4. publish the commit record before checkpoint ownership is exposed.
 *
 * The implementation is private to ext3.ko.  Its list manipulation follows
 * the ownership rules declared in journal.h; no separately deployed journal
 * module is required.
 */

#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/time.h>

#include "journal.h"

struct ifs_ext3_log_batch {
	struct journal_head *descriptor;
	journal_block_tag_t *last_tag;
	char *cursor;
	int bytes_left;
	int count;
	bool first_tag;
};

static void ifs_ext3_log_end_io(
	struct buffer_head *bh, int uptodate)
{
	if (uptodate)
		set_buffer_uptodate(bh);
	else
		clear_buffer_uptodate(bh);
	unlock_buffer(bh);
}

static void ifs_ext3_release_detached_page(
	struct buffer_head *bh)
{
	struct folio *folio;

	if (buffer_dirty(bh) ||
	    atomic_read(&bh->b_count) != 1) {
		__brelse(bh);
		return;
	}

	folio = bh->b_folio;
	if (folio->mapping || !folio_trylock(folio)) {
		__brelse(bh);
		return;
	}

	folio_get(folio);
	__brelse(bh);
	try_to_free_buffers(folio);
	folio_unlock(folio);
	folio_put(folio);
}

static void ifs_ext3_release_data_ref(
	struct buffer_head *bh)
{
	if (!buffer_freed(bh)) {
		put_bh(bh);
		return;
	}

	WARN_ON_ONCE(buffer_dirty(bh));
	clear_buffer_freed(bh);
	clear_buffer_mapped(bh);
	clear_buffer_new(bh);
	clear_buffer_req(bh);
	bh->b_bdev = NULL;
	ifs_ext3_release_detached_page(bh);
}

/*
 * j_list_lock is deliberately dropped while waiting for the per-buffer state
 * bit.  Taking those locks in the opposite order would deadlock transaction
 * list walkers against buffer users.
 */
static bool ifs_ext3_try_state_from_list(
	journal_t *journal, struct buffer_head *bh)
{
	if (jbd_trylock_bh_state(bh))
		return true;

	spin_unlock(&journal->j_list_lock);
	cond_resched();
	return false;
}

static int ifs_ext3_write_commit_block(
	journal_t *journal, transaction_t *transaction)
{
	struct journal_head *jh;
	struct buffer_head *bh;
	journal_header_t *header;
	int error;

	if (is_journal_aborted(journal))
		return 0;

	jh = journal_get_descriptor_buffer(journal);
	if (!jh)
		return -EIO;

	bh = jh2bh(jh);
	header = (journal_header_t *)bh->b_data;
	header->h_magic = cpu_to_be32(JFS_MAGIC_NUMBER);
	header->h_blocktype = cpu_to_be32(JFS_COMMIT_BLOCK);
	header->h_sequence = cpu_to_be32(transaction->t_tid);
	set_buffer_dirty(bh);

	if (journal->j_flags & JFS_BARRIER)
		error = __sync_dirty_buffer(
			bh, REQ_SYNC | REQ_PREFLUSH | REQ_FUA);
	else
		error = sync_dirty_buffer(bh);

	put_bh(bh);
	journal_put_journal_head(jh);
	return error;
}

static void ifs_ext3_submit_data_batch(
	struct buffer_head **buffers,
	int count,
	blk_opf_t write_flags)
{
	int index;

	for (index = 0; index < count; ++index) {
		buffers[index]->b_end_io =
			end_buffer_write_sync;
		submit_bh(
			REQ_OP_WRITE | write_flags,
			buffers[index]);
	}
}

static int ifs_ext3_flush_ordered_data(
	journal_t *journal,
	transaction_t *transaction,
	blk_opf_t write_flags)
{
	struct buffer_head **batch = journal->j_wbuf;
	int batch_count = 0;
	int result = 0;

	for (;;) {
		struct journal_head *jh;
		struct buffer_head *bh;
		bool locked_here = false;

		cond_resched();
		spin_lock(&journal->j_list_lock);

		jh = transaction->t_sync_datalist;
		if (!jh) {
			spin_unlock(&journal->j_list_lock);
			break;
		}

		bh = jh2bh(jh);
		get_bh(bh);

		if (buffer_dirty(bh)) {
			if (!trylock_buffer(bh)) {
				spin_unlock(&journal->j_list_lock);
				ifs_ext3_submit_data_batch(
					batch, batch_count,
					write_flags);
				batch_count = 0;
				lock_buffer(bh);
				spin_lock(&journal->j_list_lock);
			}
			locked_here = true;
		}

		if (!ifs_ext3_try_state_from_list(
			    journal, bh)) {
			jbd_lock_bh_state(bh);
			spin_lock(&journal->j_list_lock);
		}

		if (!buffer_jbd(bh) ||
		    bh2jh(bh) != jh ||
		    jh->b_transaction != transaction ||
		    jh->b_jlist != BJ_SyncData) {
			jbd_unlock_bh_state(bh);
			spin_unlock(&journal->j_list_lock);
			if (locked_here)
				unlock_buffer(bh);
			ifs_ext3_release_data_ref(bh);
			continue;
		}

		if (locked_here &&
		    test_clear_buffer_dirty(bh)) {
			batch[batch_count++] = bh;
			__journal_file_buffer(
				jh, transaction, BJ_Locked);
			jbd_unlock_bh_state(bh);
			spin_unlock(&journal->j_list_lock);

			if (batch_count ==
			    journal->j_wbufsize) {
				ifs_ext3_submit_data_batch(
					batch, batch_count,
					write_flags);
				batch_count = 0;
			}
			continue;
		}

		if (!locked_here && buffer_locked(bh)) {
			__journal_file_buffer(
				jh, transaction, BJ_Locked);
			jbd_unlock_bh_state(bh);
			spin_unlock(&journal->j_list_lock);
			put_bh(bh);
			continue;
		}

		if (unlikely(!buffer_uptodate(bh)))
			result = -EIO;
		__journal_unfile_buffer(jh);
		jbd_unlock_bh_state(bh);
		spin_unlock(&journal->j_list_lock);

		if (locked_here)
			unlock_buffer(bh);
		ifs_ext3_release_data_ref(bh);
	}

	ifs_ext3_submit_data_batch(
		batch, batch_count, write_flags);
	return result;
}

static int ifs_ext3_wait_ordered_data(
	journal_t *journal, transaction_t *transaction)
{
	int result = 0;

	for (;;) {
		struct journal_head *jh;
		struct buffer_head *bh;

		spin_lock(&journal->j_list_lock);
		jh = transaction->t_locked_list;
		if (!jh) {
			spin_unlock(&journal->j_list_lock);
			break;
		}

		jh = jh->b_tprev;
		bh = jh2bh(jh);
		get_bh(bh);

		if (buffer_locked(bh)) {
			spin_unlock(&journal->j_list_lock);
			wait_on_buffer(bh);
			put_bh(bh);
			continue;
		}

		if (unlikely(!buffer_uptodate(bh))) {
			struct folio *folio = bh->b_folio;

			if (!folio_trylock(folio)) {
				spin_unlock(
					&journal->j_list_lock);
				folio_lock(folio);
				spin_lock(
					&journal->j_list_lock);
			}
			if (folio->mapping)
				mapping_set_error(
					folio->mapping, -EIO);
			folio_unlock(folio);
			result = -EIO;
		}

		if (!ifs_ext3_try_state_from_list(
			    journal, bh)) {
			put_bh(bh);
			continue;
		}

		if (buffer_jbd(bh) &&
		    bh2jh(bh) == jh &&
		    jh->b_transaction == transaction &&
		    jh->b_jlist == BJ_Locked)
			__journal_unfile_buffer(jh);

		jbd_unlock_bh_state(bh);
		spin_unlock(&journal->j_list_lock);
		ifs_ext3_release_data_ref(bh);
		cond_resched();
	}

	return result;
}

static transaction_t *ifs_ext3_lock_running_transaction(
	journal_t *journal, ktime_t *started)
{
	transaction_t *transaction;

	if (journal->j_flags & JFS_FLUSHED) {
		mutex_lock(&journal->j_checkpoint_mutex);
		journal_update_sb_log_tail(
			journal,
			journal->j_tail_sequence,
			journal->j_tail,
			REQ_SYNC);
		mutex_unlock(&journal->j_checkpoint_mutex);
	}

	spin_lock(&journal->j_state_lock);
	J_ASSERT(journal->j_running_transaction != NULL);
	J_ASSERT(journal->j_committing_transaction == NULL);

	transaction = journal->j_running_transaction;
	J_ASSERT(transaction->t_state == T_RUNNING);
	transaction->t_state = T_LOCKED;

	for (;;) {
		DEFINE_WAIT(wait);

		spin_lock(&transaction->t_handle_lock);
		if (!transaction->t_updates) {
			spin_unlock(
				&transaction->t_handle_lock);
			break;
		}

		prepare_to_wait(
			&journal->j_wait_updates, &wait,
			TASK_UNINTERRUPTIBLE);
		spin_unlock(&transaction->t_handle_lock);
		spin_unlock(&journal->j_state_lock);
		schedule();
		finish_wait(&journal->j_wait_updates, &wait);
		spin_lock(&journal->j_state_lock);
	}

	J_ASSERT(
		transaction->t_outstanding_credits <=
		journal->j_max_transaction_buffers);

	while (transaction->t_reserved_list) {
		struct journal_head *jh =
			transaction->t_reserved_list;

		if (jh->b_committed_data) {
			struct buffer_head *bh = jh2bh(jh);

			jbd_lock_bh_state(bh);
			jbd_free(
				jh->b_committed_data,
				bh->b_size);
			jh->b_committed_data = NULL;
			jbd_unlock_bh_state(bh);
		}
		journal_refile_buffer(journal, jh);
	}

	spin_lock(&journal->j_list_lock);
	__journal_clean_checkpoint_list(journal);
	spin_unlock(&journal->j_list_lock);

	journal_clear_buffer_revoked_flags(journal);
	journal_switch_revoke_table(journal);

	transaction->t_state = T_FLUSH;
	journal->j_committing_transaction = transaction;
	journal->j_running_transaction = NULL;
	transaction->t_log_start = journal->j_head;
	*started = ktime_get();

	wake_up(&journal->j_wait_transaction_locked);
	spin_unlock(&journal->j_state_lock);
	return transaction;
}

static int ifs_ext3_open_descriptor(
	journal_t *journal,
	transaction_t *transaction,
	struct ifs_ext3_log_batch *batch)
{
	struct buffer_head *bh;
	journal_header_t *header;

	batch->descriptor =
		journal_get_descriptor_buffer(journal);
	if (!batch->descriptor)
		return -EIO;

	bh = jh2bh(batch->descriptor);
	header = (journal_header_t *)bh->b_data;
	header->h_magic = cpu_to_be32(JFS_MAGIC_NUMBER);
	header->h_blocktype =
		cpu_to_be32(JFS_DESCRIPTOR_BLOCK);
	header->h_sequence =
		cpu_to_be32(transaction->t_tid);

	batch->cursor =
		bh->b_data + sizeof(journal_header_t);
	batch->bytes_left =
		bh->b_size - sizeof(journal_header_t);
	batch->first_tag = true;
	batch->last_tag = NULL;

	set_buffer_jwrite(bh);
	set_buffer_dirty(bh);
	journal->j_wbuf[batch->count++] = bh;
	journal_file_buffer(
		batch->descriptor,
		transaction,
		BJ_LogCtl);
	return 0;
}

static int ifs_ext3_append_metadata(
	journal_t *journal,
	transaction_t *transaction,
	struct ifs_ext3_log_batch *batch)
{
	struct journal_head *source =
		transaction->t_buffers;
	struct journal_head *logged;
	struct buffer_head *source_bh;
	unsigned int log_block;
	unsigned int tag_flags = 0U;
	int transform;
	int error;

	if (!batch->descriptor) {
		error = ifs_ext3_open_descriptor(
			journal, transaction, batch);
		if (error)
			return error;
	}

	error = journal_next_log_block(
		journal, &log_block);
	if (error)
		return error;

	transaction->t_outstanding_credits--;
	source_bh = jh2bh(source);
	get_bh(source_bh);
	set_buffer_jwrite(source_bh);

	transform = journal_write_metadata_buffer(
		transaction, source, &logged, log_block);
	set_buffer_jwrite(jh2bh(logged));
	journal->j_wbuf[batch->count++] =
		jh2bh(logged);

	if (transform & 1)
		tag_flags |= JFS_FLAG_ESCAPE;
	if (!batch->first_tag)
		tag_flags |= JFS_FLAG_SAME_UUID;

	batch->last_tag =
		(journal_block_tag_t *)batch->cursor;
	batch->last_tag->t_blocknr =
		cpu_to_be32(source_bh->b_blocknr);
	batch->last_tag->t_flags =
		cpu_to_be32(tag_flags);
	batch->cursor += sizeof(journal_block_tag_t);
	batch->bytes_left -=
		sizeof(journal_block_tag_t);

	if (batch->first_tag) {
		memcpy(batch->cursor, journal->j_uuid, 16);
		batch->cursor += 16;
		batch->bytes_left -= 16;
		batch->first_tag = false;
	}

	return 0;
}

static void ifs_ext3_submit_log_batch(
	journal_t *journal,
	struct ifs_ext3_log_batch *batch,
	blk_opf_t write_flags)
{
	int index;

	if (!batch->count)
		return;

	if (batch->last_tag)
		batch->last_tag->t_flags |=
			cpu_to_be32(JFS_FLAG_LAST_TAG);

	for (index = 0; index < batch->count; ++index) {
		struct buffer_head *bh =
			journal->j_wbuf[index];

		lock_buffer(bh);
		clear_buffer_dirty(bh);
		set_buffer_uptodate(bh);
		bh->b_end_io = ifs_ext3_log_end_io;
		submit_bh(
			REQ_OP_WRITE | write_flags, bh);
	}

	batch->descriptor = NULL;
	batch->last_tag = NULL;
	batch->cursor = NULL;
	batch->bytes_left = 0;
	batch->count = 0;
	batch->first_tag = true;
	cond_resched();
}

static int ifs_ext3_write_metadata_log(
	journal_t *journal,
	transaction_t *transaction,
	blk_opf_t write_flags)
{
	struct ifs_ext3_log_batch batch = { 0 };
	int result = 0;

	while (transaction->t_buffers) {
		int error;

		if (is_journal_aborted(journal)) {
			struct journal_head *jh =
				transaction->t_buffers;

			clear_buffer_jbddirty(jh2bh(jh));
			journal_refile_buffer(journal, jh);
			continue;
		}

		error = ifs_ext3_append_metadata(
			journal, transaction, &batch);
		if (error) {
			journal_abort(journal, error);
			result = error;
			continue;
		}

		if (batch.count == journal->j_wbufsize ||
		    !transaction->t_buffers ||
		    batch.bytes_left <
			(int)(sizeof(journal_block_tag_t) + 16))
			ifs_ext3_submit_log_batch(
				journal, &batch, write_flags);
	}

	ifs_ext3_submit_log_batch(
		journal, &batch, write_flags);
	return result;
}

static int ifs_ext3_wait_metadata_io(
	journal_t *journal, transaction_t *transaction)
{
	int result = 0;

	while (transaction->t_iobuf_list) {
		struct journal_head *logged =
			transaction->t_iobuf_list->b_tprev;
		struct buffer_head *logged_bh =
			jh2bh(logged);
		struct journal_head *shadow;
		struct buffer_head *shadow_bh;

		if (buffer_locked(logged_bh)) {
			wait_on_buffer(logged_bh);
			continue;
		}
		if (cond_resched())
			continue;

		if (unlikely(!buffer_uptodate(logged_bh)))
			result = -EIO;

		clear_buffer_jwrite(logged_bh);
		journal_unfile_buffer(journal, logged);
		journal_put_journal_head(logged);
		__brelse(logged_bh);
		J_ASSERT_BH(
			logged_bh,
			atomic_read(&logged_bh->b_count) == 0);
		free_buffer_head(logged_bh);

		shadow = transaction->t_shadow_list;
		J_ASSERT(shadow != NULL);
		shadow = shadow->b_tprev;
		shadow_bh = jh2bh(shadow);
		clear_buffer_jwrite(shadow_bh);
		J_ASSERT_BH(
			shadow_bh,
			buffer_jbddirty(shadow_bh));

		journal_file_buffer(
			shadow, transaction, BJ_Forget);
		smp_mb();
		wake_up_bit(
			&shadow_bh->b_state, BH_Unshadow);
		__brelse(shadow_bh);
	}

	J_ASSERT(transaction->t_shadow_list == NULL);
	return result;
}

static int ifs_ext3_wait_control_io(
	journal_t *journal, transaction_t *transaction)
{
	int result = 0;

	while (transaction->t_log_list) {
		struct journal_head *jh =
			transaction->t_log_list->b_tprev;
		struct buffer_head *bh = jh2bh(jh);

		if (buffer_locked(bh)) {
			wait_on_buffer(bh);
			continue;
		}
		if (cond_resched())
			continue;

		if (unlikely(!buffer_uptodate(bh)))
			result = -EIO;

		clear_buffer_jwrite(bh);
		journal_unfile_buffer(journal, jh);
		journal_put_journal_head(jh);
		__brelse(bh);
	}

	return result;
}

static void ifs_ext3_retire_forget_list(
	journal_t *journal, transaction_t *transaction)
{
	for (;;) {
		struct journal_head *jh;
		struct buffer_head *bh;
		bool release_page = false;

		spin_lock(&journal->j_list_lock);
		jh = transaction->t_forget;
		spin_unlock(&journal->j_list_lock);
		if (!jh)
			return;

		bh = jh2bh(jh);
		get_bh(bh);
		jbd_lock_bh_state(bh);

		J_ASSERT_JH(
			jh,
			jh->b_transaction == transaction ||
			jh->b_transaction ==
				journal->j_running_transaction);

		if (jh->b_committed_data) {
			jbd_free(
				jh->b_committed_data,
				bh->b_size);
			jh->b_committed_data = NULL;
			if (jh->b_frozen_data) {
				jh->b_committed_data =
					jh->b_frozen_data;
				jh->b_frozen_data = NULL;
			}
		} else if (jh->b_frozen_data) {
			jbd_free(
				jh->b_frozen_data,
				bh->b_size);
			jh->b_frozen_data = NULL;
		}

		spin_lock(&journal->j_list_lock);

		if (jh->b_cp_transaction)
			__journal_remove_checkpoint(jh);

		if (buffer_freed(bh)) {
			jh->b_modified = 0;
			if (!jh->b_next_transaction) {
				clear_buffer_freed(bh);
				clear_buffer_jbddirty(bh);
				clear_buffer_mapped(bh);
				clear_buffer_new(bh);
				clear_buffer_req(bh);
				bh->b_bdev = NULL;
			}
		}

		if (buffer_jbddirty(bh)) {
			__journal_insert_checkpoint(
				jh, transaction);
			if (is_journal_aborted(journal))
				clear_buffer_jbddirty(bh);
		} else {
			J_ASSERT_BH(bh, !buffer_dirty(bh));
			release_page =
				!jh->b_next_transaction;
		}

		__journal_refile_buffer(jh);
		spin_unlock(&journal->j_list_lock);
		jbd_unlock_bh_state(bh);

		if (release_page)
			ifs_ext3_release_detached_page(bh);
		else
			__brelse(bh);
		cond_resched();
	}
}

static void ifs_ext3_link_checkpoint_transaction(
	journal_t *journal, transaction_t *transaction)
{
	if (!transaction->t_checkpoint_list &&
	    !transaction->t_checkpoint_io_list) {
		__journal_drop_transaction(
			journal, transaction);
		return;
	}

	if (!journal->j_checkpoint_transactions) {
		journal->j_checkpoint_transactions =
			transaction;
		transaction->t_cpnext = transaction;
		transaction->t_cpprev = transaction;
		return;
	}

	transaction->t_cpnext =
		journal->j_checkpoint_transactions;
	transaction->t_cpprev =
		transaction->t_cpnext->t_cpprev;
	transaction->t_cpnext->t_cpprev =
		transaction;
	transaction->t_cpprev->t_cpnext =
		transaction;
}

static void ifs_ext3_publish_finished_transaction(
	journal_t *journal,
	transaction_t *transaction,
	ktime_t started)
{
	u64 elapsed;

	for (;;) {
		ifs_ext3_retire_forget_list(
			journal, transaction);

		spin_lock(&journal->j_state_lock);
		spin_lock(&journal->j_list_lock);
		if (transaction->t_forget) {
			spin_unlock(&journal->j_list_lock);
			spin_unlock(&journal->j_state_lock);
			continue;
		}
		break;
	}

	J_ASSERT(
		transaction->t_state == T_COMMIT_RECORD);
	transaction->t_state = T_FINISHED;
	J_ASSERT(
		transaction ==
		journal->j_committing_transaction);

	journal->j_commit_sequence = transaction->t_tid;
	journal->j_committing_transaction = NULL;

	elapsed = ktime_to_ns(
		ktime_sub(ktime_get(), started));
	if (likely(journal->j_average_commit_time))
		journal->j_average_commit_time =
			(elapsed * 3 +
			 journal->j_average_commit_time) / 4;
	else
		journal->j_average_commit_time = elapsed;

	spin_unlock(&journal->j_state_lock);
	ifs_ext3_link_checkpoint_transaction(
		journal, transaction);
	spin_unlock(&journal->j_list_lock);

	wake_up(&journal->j_wait_done_commit);
}

void journal_commit_transaction(journal_t *journal)
{
	transaction_t *transaction;
	ktime_t started;
	blk_opf_t write_flags = 0;
	struct blk_plug plug;
	int error;
	int secondary;

	transaction = ifs_ext3_lock_running_transaction(
		journal, &started);

	if (tid_geq(
		    journal->j_commit_waited,
		    transaction->t_tid))
		write_flags = REQ_SYNC;

	blk_start_plug(&plug);
	error = ifs_ext3_flush_ordered_data(
		journal, transaction, write_flags);
	blk_finish_plug(&plug);

	secondary = ifs_ext3_wait_ordered_data(
		journal, transaction);
	if (!error)
		error = secondary;

	if (error) {
		pr_warn(
			"EXT3: ordered data writeback failed on %pg\n",
			journal->j_fs_dev);
		if (journal->j_flags &
		    JFS_ABORT_ON_SYNCDATA_ERR)
			journal_abort(journal, error);
		error = 0;
	}

	blk_start_plug(&plug);
	journal_write_revoke_records(
		journal, transaction, write_flags);

	J_ASSERT(transaction->t_sync_datalist == NULL);

	spin_lock(&journal->j_state_lock);
	transaction->t_state = T_COMMIT;
	spin_unlock(&journal->j_state_lock);

	J_ASSERT(
		transaction->t_nr_buffers <=
		transaction->t_outstanding_credits);

	secondary = ifs_ext3_write_metadata_log(
		journal, transaction, write_flags);
	if (!error)
		error = secondary;
	blk_finish_plug(&plug);

	secondary = ifs_ext3_wait_metadata_io(
		journal, transaction);
	if (!error)
		error = secondary;

	secondary = ifs_ext3_wait_control_io(
		journal, transaction);
	if (!error)
		error = secondary;

	if (error)
		journal_abort(journal, error);

	spin_lock(&journal->j_state_lock);
	J_ASSERT(transaction->t_state == T_COMMIT);
	transaction->t_state = T_COMMIT_RECORD;
	spin_unlock(&journal->j_state_lock);

	secondary = ifs_ext3_write_commit_block(
		journal, transaction);
	if (secondary)
		journal_abort(journal, secondary);

	J_ASSERT(transaction->t_sync_datalist == NULL);
	J_ASSERT(transaction->t_buffers == NULL);
	J_ASSERT(transaction->t_checkpoint_list == NULL);
	J_ASSERT(transaction->t_iobuf_list == NULL);
	J_ASSERT(transaction->t_shadow_list == NULL);
	J_ASSERT(transaction->t_log_list == NULL);

	ifs_ext3_publish_finished_transaction(
		journal, transaction, started);
}


/* ===== recovery ===== */
/*
 * Filesystem Support EXT3 embedded journal recovery engine.
 *
 * Recovery walks the EXT3 JBD log in three passes: discover committed
 * transactions, record revocations, then replay surviving metadata. Persistent
 * journal blocks are treated as untrusted input and all variable-length
 * records are bounds checked before use.
 */

#ifndef __KERNEL__
#include "jfs_user.h"
#else
#include <linux/blkdev.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include "journal.h"
#endif

struct ifs_ext3_recovery_info {
	tid_t start_transaction;
	tid_t end_transaction;
	int nr_replays;
	int nr_revokes;
	int nr_revoke_hits;
};

enum ifs_ext3_recovery_pass {
	IFS_EXT3_PASS_SCAN,
	IFS_EXT3_PASS_REVOKE,
	IFS_EXT3_PASS_REPLAY,
};

static unsigned int ifs_ext3_wrap_log_block(
	journal_t *journal, unsigned int block)
{
	if (block >= journal->j_last)
		block -= journal->j_last - journal->j_first;
	return block;
}

#ifdef __KERNEL__
static void ifs_ext3_readahead(journal_t *journal, unsigned int start)
{
	unsigned int limit;
	unsigned int offset;

	limit = start + (128U * 1024U / journal->j_blocksize);
	if (limit > journal->j_maxlen)
		limit = journal->j_maxlen;

	for (offset = start; offset < limit; ++offset) {
		unsigned int physical;
		struct buffer_head *bh;

		if (journal_bmap(journal, offset, &physical))
			break;

		bh = __getblk(journal->j_dev, physical, journal->j_blocksize);
		if (!bh)
			break;

		if (!buffer_uptodate(bh) && !buffer_locked(bh))
			bh_readahead(bh, REQ_RAHEAD);
		brelse(bh);
	}
}
#endif

static int ifs_ext3_read_log_block(
	journal_t *journal,
	unsigned int offset,
	struct buffer_head **result)
{
	unsigned int physical;
	struct buffer_head *bh;
	int error;

	*result = NULL;
	if (offset >= journal->j_maxlen)
		return -EUCLEAN;

	error = journal_bmap(journal, offset, &physical);
	if (error)
		return error;

	bh = __getblk(journal->j_dev, physical, journal->j_blocksize);
	if (!bh)
		return -ENOMEM;

	if (!buffer_uptodate(bh)) {
#ifdef __KERNEL__
		if (!buffer_req(bh))
			ifs_ext3_readahead(journal, offset);
#endif
		wait_on_buffer(bh);
	}

	if (!buffer_uptodate(bh)) {
		brelse(bh);
		return -EIO;
	}

	*result = bh;
	return 0;
}

static int ifs_ext3_descriptor_tag_count(
	struct buffer_head *bh, unsigned int block_size)
{
	char *cursor = bh->b_data + sizeof(journal_header_t);
	char *limit = bh->b_data + block_size;
	int count = 0;

	while (cursor + sizeof(journal_block_tag_t) <= limit) {
		journal_block_tag_t tag;
		unsigned int flags;

		memcpy(&tag, cursor, sizeof(tag));
		flags = be32_to_cpu(tag.t_flags);
		cursor += sizeof(tag);
		count++;

		if (!(flags & JFS_FLAG_SAME_UUID)) {
			if (cursor + 16 > limit)
				return -EUCLEAN;
			cursor += 16;
		}

		if (flags & JFS_FLAG_LAST_TAG)
			return count;
	}

	return count;
}

static int ifs_ext3_scan_revoke_block(
	journal_t *journal,
	struct buffer_head *bh,
	tid_t sequence,
	struct ifs_ext3_recovery_info *info)
{
	journal_revoke_header_t *header =
		(journal_revoke_header_t *)bh->b_data;
	unsigned int offset = sizeof(*header);
	unsigned int end = be32_to_cpu(header->r_count);

	if (end < sizeof(*header) || end > journal->j_blocksize)
		return -EUCLEAN;
	if ((end - sizeof(*header)) & 3U)
		return -EUCLEAN;

	while (offset < end) {
		unsigned int block =
			be32_to_cpu(*(__be32 *)(bh->b_data + offset));
		int error;

		error = journal_set_revoke(journal, block, sequence);
		if (error)
			return error;

		info->nr_revokes++;
		offset += sizeof(__be32);
	}

	return 0;
}

static int ifs_ext3_replay_descriptor(
	journal_t *journal,
	struct ifs_ext3_recovery_info *info,
	struct buffer_head *descriptor,
	unsigned int *next_log_block,
	tid_t sequence)
{
	char *cursor =
		descriptor->b_data + sizeof(journal_header_t);
	char *limit =
		descriptor->b_data + journal->j_blocksize;
	int status = 0;

	while (cursor + sizeof(journal_block_tag_t) <= limit) {
		journal_block_tag_t tag;
		struct buffer_head *logged = NULL;
		struct buffer_head *home = NULL;
		unsigned int flags;
		unsigned int block;
		unsigned int log_block;
		int error;

		memcpy(&tag, cursor, sizeof(tag));
		flags = be32_to_cpu(tag.t_flags);
		block = be32_to_cpu(tag.t_blocknr);
		cursor += sizeof(tag);

		if (!(flags & JFS_FLAG_SAME_UUID)) {
			if (cursor + 16 > limit)
				return -EUCLEAN;
			cursor += 16;
		}

		log_block = *next_log_block;
		*next_log_block = ifs_ext3_wrap_log_block(
			journal, log_block + 1U);

		error = ifs_ext3_read_log_block(
			journal, log_block, &logged);
		if (error) {
			if (!status)
				status = error;
			goto next_tag;
		}

		if (journal_test_revoke(journal, block, sequence)) {
			info->nr_revoke_hits++;
			goto next_tag;
		}

		home = __getblk(
			journal->j_fs_dev, block, journal->j_blocksize);
		if (!home) {
			if (!status)
				status = -ENOMEM;
			goto next_tag;
		}

		lock_buffer(home);
		memcpy(home->b_data, logged->b_data, journal->j_blocksize);
		if (flags & JFS_FLAG_ESCAPE)
			*(__be32 *)home->b_data =
				cpu_to_be32(JFS_MAGIC_NUMBER);
		set_buffer_uptodate(home);
		mark_buffer_dirty(home);
		unlock_buffer(home);
		info->nr_replays++;

next_tag:
		brelse(home);
		brelse(logged);
		if (flags & JFS_FLAG_LAST_TAG)
			break;
	}

	return status;
}

static int ifs_ext3_recovery_pass(
	journal_t *journal,
	struct ifs_ext3_recovery_info *info,
	enum ifs_ext3_recovery_pass pass)
{
	journal_superblock_t *super = journal->j_superblock;
	tid_t transaction = be32_to_cpu(super->s_sequence);
	unsigned int next = be32_to_cpu(super->s_start);
	int replay_error = 0;

	if (pass == IFS_EXT3_PASS_SCAN)
		info->start_transaction = transaction;

	for (;;) {
		struct buffer_head *bh;
		journal_header_t *header;
		unsigned int sequence;
		unsigned int type;
		int error;

		cond_resched();

		if (pass != IFS_EXT3_PASS_SCAN &&
		    tid_geq(transaction, info->end_transaction))
			break;

		error = ifs_ext3_read_log_block(journal, next, &bh);
		if (error)
			return error;

		next = ifs_ext3_wrap_log_block(journal, next + 1U);
		header = (journal_header_t *)bh->b_data;

		if (header->h_magic != cpu_to_be32(JFS_MAGIC_NUMBER)) {
			brelse(bh);
			break;
		}

		type = be32_to_cpu(header->h_blocktype);
		sequence = be32_to_cpu(header->h_sequence);
		if (sequence != transaction) {
			brelse(bh);
			break;
		}

		switch (type) {
		case JFS_DESCRIPTOR_BLOCK:
			if (pass == IFS_EXT3_PASS_REPLAY) {
				error = ifs_ext3_replay_descriptor(
					journal, info, bh, &next,
					transaction);
				if (error && !replay_error)
					replay_error = error;
			} else {
				int tags =
					ifs_ext3_descriptor_tag_count(
						bh, journal->j_blocksize);

				if (tags < 0) {
					brelse(bh);
					return tags;
				}
				next = ifs_ext3_wrap_log_block(
					journal, next + (unsigned int)tags);
			}
			brelse(bh);
			continue;

		case JFS_REVOKE_BLOCK:
			if (pass == IFS_EXT3_PASS_REVOKE) {
				error = ifs_ext3_scan_revoke_block(
					journal, bh, transaction, info);
				brelse(bh);
				if (error)
					return error;
			} else {
				brelse(bh);
			}
			continue;

		case JFS_COMMIT_BLOCK:
			brelse(bh);
			transaction++;
			continue;

		default:
			brelse(bh);
			goto done;
		}
	}

done:
	if (pass == IFS_EXT3_PASS_SCAN)
		info->end_transaction = transaction;
	else if (info->end_transaction != transaction && !replay_error)
		replay_error = -EIO;

	return replay_error;
}

int journal_recover(journal_t *journal)
{
	struct ifs_ext3_recovery_info info;
	int error;
	int secondary;

	memset(&info, 0, sizeof(info));

	if (!journal->j_superblock->s_start) {
		journal->j_transaction_sequence =
			be32_to_cpu(
				journal->j_superblock->s_sequence) + 1U;
		return 0;
	}

	error = ifs_ext3_recovery_pass(
		journal, &info, IFS_EXT3_PASS_SCAN);
	if (!error)
		error = ifs_ext3_recovery_pass(
			journal, &info, IFS_EXT3_PASS_REVOKE);
	if (!error)
		error = ifs_ext3_recovery_pass(
			journal, &info, IFS_EXT3_PASS_REPLAY);

	journal->j_transaction_sequence =
		info.end_transaction + 1U;
	journal_clear_revoke(journal);

	secondary = sync_blockdev(journal->j_fs_dev);
	if (!error)
		error = secondary;

	if (journal->j_flags & JFS_BARRIER) {
		secondary = blkdev_issue_flush(journal->j_fs_dev);
		if (!error)
			error = secondary;
	}

	return error;
}

int journal_skip_recovery(journal_t *journal)
{
	struct ifs_ext3_recovery_info info;
	int error;

	memset(&info, 0, sizeof(info));
	error = ifs_ext3_recovery_pass(
		journal, &info, IFS_EXT3_PASS_SCAN);

	if (error)
		journal->j_transaction_sequence++;
	else
		journal->j_transaction_sequence =
			info.end_transaction + 1U;

	journal->j_tail = 0;
	return error;
}


/* ===== revoke ===== */
/*
 * Filesystem Support EXT3 journal revoke engine.
 *
 * A revoke prevents an older journal image from being replayed over a block
 * whose meaning changed later.  Running and committing transactions use
 * separate hash tables so commit serialization never races transaction-side
 * insertion.  Recovery reuses the same table API while the filesystem is
 * still offline.
 */

#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/slab.h>

#include "journal.h"

struct ifs_ext3_revoke_record {
	struct list_head link;
	tid_t sequence;
	unsigned int block;
};

struct jbd_revoke_table_s {
	unsigned int bucket_count;
	unsigned int hash_shift;
	struct list_head *buckets;
};

static struct kmem_cache *ifs_ext3_revoke_record_cache;
static struct kmem_cache *ifs_ext3_revoke_table_cache;

static unsigned int ifs_ext3_revoke_bucket(
	const struct jbd_revoke_table_s *table,
	unsigned int block)
{
	return hash_32(block, table->hash_shift);
}

static struct ifs_ext3_revoke_record *
ifs_ext3_find_revoke_locked(
	struct jbd_revoke_table_s *table,
	unsigned int block)
{
	struct ifs_ext3_revoke_record *record;
	const unsigned int bucket =
		ifs_ext3_revoke_bucket(table, block);

	list_for_each_entry(
		record, &table->buckets[bucket], link) {
		if (record->block == block)
			return record;
	}
	return NULL;
}

static int ifs_ext3_add_revoke(
	journal_t *journal,
	unsigned int block,
	tid_t sequence)
{
	struct ifs_ext3_revoke_record *record;

	for (;;) {
		record = kmem_cache_alloc(
			ifs_ext3_revoke_record_cache, GFP_NOFS);
		if (record)
			break;
		if (!journal_oom_retry)
			return -ENOMEM;
		cond_resched();
	}

	record->block = block;
	record->sequence = sequence;
	INIT_LIST_HEAD(&record->link);

	spin_lock(&journal->j_revoke_lock);
	if (ifs_ext3_find_revoke_locked(
		    journal->j_revoke, block)) {
		spin_unlock(&journal->j_revoke_lock);
		kmem_cache_free(
			ifs_ext3_revoke_record_cache, record);
		return -EEXIST;
	}
	list_add(
		&record->link,
		&journal->j_revoke->buckets[
			ifs_ext3_revoke_bucket(
				journal->j_revoke, block)]);
	spin_unlock(&journal->j_revoke_lock);
	return 0;
}

static void ifs_ext3_clear_revoke_table(
	struct jbd_revoke_table_s *table)
{
	unsigned int bucket;

	if (!table)
		return;

	for (bucket = 0U;
	     bucket < table->bucket_count;
	     ++bucket) {
		struct ifs_ext3_revoke_record *record;
		struct ifs_ext3_revoke_record *next;

		list_for_each_entry_safe(
			record, next, &table->buckets[bucket], link) {
			list_del(&record->link);
			kmem_cache_free(
				ifs_ext3_revoke_record_cache, record);
		}
	}
}

void journal_destroy_revoke_caches(void)
{
	if (ifs_ext3_revoke_record_cache) {
		kmem_cache_destroy(
			ifs_ext3_revoke_record_cache);
		ifs_ext3_revoke_record_cache = NULL;
	}
	if (ifs_ext3_revoke_table_cache) {
		kmem_cache_destroy(
			ifs_ext3_revoke_table_cache);
		ifs_ext3_revoke_table_cache = NULL;
	}
}

int __init journal_init_revoke_caches(void)
{
	if (ifs_ext3_revoke_record_cache ||
	    ifs_ext3_revoke_table_cache)
		return -EBUSY;

	ifs_ext3_revoke_record_cache =
		kmem_cache_create(
			"ext3_revoke_record",
			sizeof(struct ifs_ext3_revoke_record),
			0U, SLAB_HWCACHE_ALIGN, NULL);
	if (!ifs_ext3_revoke_record_cache)
		return -ENOMEM;

	ifs_ext3_revoke_table_cache =
		kmem_cache_create(
			"ext3_revoke_table",
			sizeof(struct jbd_revoke_table_s),
			0U, 0U, NULL);
	if (!ifs_ext3_revoke_table_cache) {
		journal_destroy_revoke_caches();
		return -ENOMEM;
	}

	return 0;
}

static struct jbd_revoke_table_s *
ifs_ext3_create_revoke_table(unsigned int bucket_count)
{
	struct jbd_revoke_table_s *table;
	unsigned int bucket;

	if (bucket_count == 0U ||
	    !is_power_of_2(bucket_count))
		return NULL;

	table = kmem_cache_zalloc(
		ifs_ext3_revoke_table_cache, GFP_KERNEL);
	if (!table)
		return NULL;

	table->buckets = kmalloc_array(
		bucket_count,
		sizeof(*table->buckets),
		GFP_KERNEL);
	if (!table->buckets) {
		kmem_cache_free(
			ifs_ext3_revoke_table_cache, table);
		return NULL;
	}

	table->bucket_count = bucket_count;
	table->hash_shift = ilog2(bucket_count);
	for (bucket = 0U;
	     bucket < bucket_count;
	     ++bucket)
		INIT_LIST_HEAD(&table->buckets[bucket]);

	return table;
}

static void ifs_ext3_destroy_revoke_table(
	struct jbd_revoke_table_s *table)
{
	if (!table)
		return;

	ifs_ext3_clear_revoke_table(table);
	kfree(table->buckets);
	kmem_cache_free(
		ifs_ext3_revoke_table_cache, table);
}

int journal_init_revoke(
	journal_t *journal, int hash_size)
{
	if (!journal || hash_size <= 0 ||
	    !is_power_of_2((unsigned int)hash_size))
		return -EINVAL;
	if (journal->j_revoke_table[0] ||
	    journal->j_revoke_table[1])
		return -EBUSY;

	journal->j_revoke_table[0] =
		ifs_ext3_create_revoke_table(
			(unsigned int)hash_size);
	if (!journal->j_revoke_table[0])
		return -ENOMEM;

	journal->j_revoke_table[1] =
		ifs_ext3_create_revoke_table(
			(unsigned int)hash_size);
	if (!journal->j_revoke_table[1]) {
		ifs_ext3_destroy_revoke_table(
			journal->j_revoke_table[0]);
		journal->j_revoke_table[0] = NULL;
		return -ENOMEM;
	}

	spin_lock_init(&journal->j_revoke_lock);
	journal->j_revoke =
		journal->j_revoke_table[1];
	return 0;
}

void journal_destroy_revoke(journal_t *journal)
{
	if (!journal)
		return;

	journal->j_revoke = NULL;
	ifs_ext3_destroy_revoke_table(
		journal->j_revoke_table[0]);
	ifs_ext3_destroy_revoke_table(
		journal->j_revoke_table[1]);
	journal->j_revoke_table[0] = NULL;
	journal->j_revoke_table[1] = NULL;
}

int journal_revoke(
	handle_t *handle,
	unsigned int block,
	struct buffer_head *supplied)
{
	journal_t *journal;
	struct buffer_head *bh = supplied;
	bool borrowed = supplied != NULL;
	int error;

	if (!handle || !handle->h_transaction)
		return -EINVAL;

	journal = handle->h_transaction->t_journal;
	if (!journal_set_features(
		    journal, 0U, 0U,
		    JFS_FEATURE_INCOMPAT_REVOKE))
		return -EINVAL;

	if (!bh)
		bh = __find_get_block(
			journal->j_fs_dev,
			block,
			journal->j_blocksize);

	if (bh) {
		if (buffer_revoked(bh)) {
			if (!borrowed)
				brelse(bh);
			return -EIO;
		}

		set_buffer_revoked(bh);
		set_buffer_revokevalid(bh);
		if (borrowed)
			error = journal_forget(
				handle, supplied);
		else {
			brelse(bh);
			error = 0;
		}
		if (error)
			return error;
	}

	error = ifs_ext3_add_revoke(
		journal, block,
		handle->h_transaction->t_tid);
	if (error == -EEXIST)
		return 0;
	return error;
}

int journal_cancel_revoke(
	handle_t *handle,
	struct journal_head *jh)
{
	journal_t *journal;
	struct buffer_head *bh;
	struct ifs_ext3_revoke_record *record = NULL;
	bool lookup;

	if (!handle || !handle->h_transaction || !jh)
		return 0;

	journal = handle->h_transaction->t_journal;
	bh = jh2bh(jh);

	if (test_set_buffer_revokevalid(bh))
		lookup = test_clear_buffer_revoked(bh);
	else {
		clear_buffer_revoked(bh);
		lookup = true;
	}

	if (lookup) {
		spin_lock(&journal->j_revoke_lock);
		record = ifs_ext3_find_revoke_locked(
			journal->j_revoke,
			(unsigned int)bh->b_blocknr);
		if (record)
			list_del(&record->link);
		spin_unlock(&journal->j_revoke_lock);

		if (record)
			kmem_cache_free(
				ifs_ext3_revoke_record_cache, record);

		{
			struct buffer_head *alias =
				__find_get_block(
					bh->b_bdev,
					bh->b_blocknr,
					bh->b_size);
			if (alias) {
				if (alias != bh)
					clear_buffer_revoked(alias);
				brelse(alias);
			}
		}
	}

	return record != NULL;
}

void journal_clear_buffer_revoked_flags(
	journal_t *journal)
{
	struct jbd_revoke_table_s *table;
	unsigned int bucket;

	if (!journal || !journal->j_revoke)
		return;

	table = journal->j_revoke;
	for (bucket = 0U;
	     bucket < table->bucket_count;
	     ++bucket) {
		struct ifs_ext3_revoke_record *record;

		list_for_each_entry(
			record, &table->buckets[bucket], link) {
			struct buffer_head *bh =
				__find_get_block(
					journal->j_fs_dev,
					record->block,
					journal->j_blocksize);
			if (!bh)
				continue;
			clear_buffer_revoked(bh);
			brelse(bh);
		}
	}
}

void journal_switch_revoke_table(journal_t *journal)
{
	struct jbd_revoke_table_s *next;

	if (!journal)
		return;

	next = journal->j_revoke ==
		journal->j_revoke_table[0] ?
		journal->j_revoke_table[1] :
		journal->j_revoke_table[0];

	ifs_ext3_clear_revoke_table(next);
	journal->j_revoke = next;
}

static struct jbd_revoke_table_s *
ifs_ext3_committing_revoke_table(journal_t *journal)
{
	return journal->j_revoke ==
		journal->j_revoke_table[0] ?
		journal->j_revoke_table[1] :
		journal->j_revoke_table[0];
}

static int ifs_ext3_flush_revoke_descriptor(
	journal_t *journal,
	struct journal_head *descriptor,
	unsigned int used,
	int write_op)
{
	struct buffer_head *bh;
	journal_revoke_header_t *header;

	if (!descriptor)
		return 0;

	bh = jh2bh(descriptor);
	if (is_journal_aborted(journal)) {
		put_bh(bh);
		return -EROFS;
	}

	header =
		(journal_revoke_header_t *)bh->b_data;
	header->r_count = cpu_to_be32(used);
	set_buffer_jwrite(bh);
	set_buffer_dirty(bh);
	write_dirty_buffer(bh, write_op);
	return 0;
}

static int ifs_ext3_append_revoke(
	journal_t *journal,
	transaction_t *transaction,
	struct journal_head **descriptor,
	unsigned int *used,
	unsigned int block,
	int write_op)
{
	struct buffer_head *bh;
	journal_header_t *header;

	if (is_journal_aborted(journal))
		return -EROFS;

	if (*descriptor &&
	    *used + sizeof(__be32) >
		journal->j_blocksize) {
		int error =
			ifs_ext3_flush_revoke_descriptor(
				journal, *descriptor,
				*used, write_op);
		if (error)
			return error;
		*descriptor = NULL;
		*used = 0U;
	}

	if (!*descriptor) {
		*descriptor =
			journal_get_descriptor_buffer(journal);
		if (!*descriptor) {
			journal_abort(journal, -ENOMEM);
			return -ENOMEM;
		}

		bh = jh2bh(*descriptor);
		memset(bh->b_data, 0, journal->j_blocksize);
		header = (journal_header_t *)bh->b_data;
		header->h_magic =
			cpu_to_be32(JFS_MAGIC_NUMBER);
		header->h_blocktype =
			cpu_to_be32(JFS_REVOKE_BLOCK);
		header->h_sequence =
			cpu_to_be32(transaction->t_tid);
		journal_file_buffer(
			*descriptor,
			transaction,
			BJ_LogCtl);
		*used = sizeof(
			journal_revoke_header_t);
	}

	bh = jh2bh(*descriptor);
	*(__be32 *)(bh->b_data + *used) =
		cpu_to_be32(block);
	*used += sizeof(__be32);
	return 0;
}

void journal_write_revoke_records(
	journal_t *journal,
	transaction_t *transaction,
	int write_op)
{
	struct jbd_revoke_table_s *table;
	struct journal_head *descriptor = NULL;
	unsigned int used = 0U;
	unsigned int bucket;
	int error = 0;

	if (!journal || !transaction)
		return;

	table =
		ifs_ext3_committing_revoke_table(journal);
	for (bucket = 0U;
	     bucket < table->bucket_count && !error;
	     ++bucket) {
		struct ifs_ext3_revoke_record *record;
		struct ifs_ext3_revoke_record *next;

		list_for_each_entry_safe(
			record, next,
			&table->buckets[bucket], link) {
			error = ifs_ext3_append_revoke(
				journal, transaction,
				&descriptor, &used,
				record->block, write_op);
			list_del(&record->link);
			kmem_cache_free(
				ifs_ext3_revoke_record_cache,
				record);
			if (error)
				break;
		}
	}

	if (!error && descriptor)
		error = ifs_ext3_flush_revoke_descriptor(
			journal, descriptor,
			used, write_op);

	if (error && !is_journal_aborted(journal))
		journal_abort(journal, error);
}

int journal_set_revoke(
	journal_t *journal,
	unsigned int block,
	tid_t sequence)
{
	struct ifs_ext3_revoke_record *record;
	int error;

	if (!journal || !journal->j_revoke)
		return -EINVAL;

	spin_lock(&journal->j_revoke_lock);
	record = ifs_ext3_find_revoke_locked(
		journal->j_revoke, block);
	if (record) {
		if (tid_gt(sequence, record->sequence))
			record->sequence = sequence;
		spin_unlock(&journal->j_revoke_lock);
		return 0;
	}
	spin_unlock(&journal->j_revoke_lock);

	error = ifs_ext3_add_revoke(
		journal, block, sequence);
	return error == -EEXIST ? 0 : error;
}

int journal_test_revoke(
	journal_t *journal,
	unsigned int block,
	tid_t sequence)
{
	struct ifs_ext3_revoke_record *record;
	int revoked = 0;

	if (!journal || !journal->j_revoke)
		return 0;

	spin_lock(&journal->j_revoke_lock);
	record = ifs_ext3_find_revoke_locked(
		journal->j_revoke, block);
	if (record && !tid_gt(sequence, record->sequence))
		revoked = 1;
	spin_unlock(&journal->j_revoke_lock);
	return revoked;
}

void journal_clear_revoke(journal_t *journal)
{
	if (!journal || !journal->j_revoke)
		return;

	spin_lock(&journal->j_revoke_lock);
	ifs_ext3_clear_revoke_table(
		journal->j_revoke);
	spin_unlock(&journal->j_revoke_lock);
}

