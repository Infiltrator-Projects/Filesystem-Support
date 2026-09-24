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
