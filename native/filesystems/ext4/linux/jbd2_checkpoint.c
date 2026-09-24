/*
 * Filesystem Support EXT4 embedded journal checkpoint engine.
 *
 * Checkpointing writes committed metadata to its home location and retires
 * journal transactions only after recovery no longer depends on them.
 */

#include <linux/blkdev.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/sched.h>
#include <trace/events/jbd2.h>

static void ifs_jbd2_unlink_checkpoint_head(struct journal_head *jh)
{
	transaction_t *transaction = jh->b_cp_transaction;
	struct journal_head *next = jh->b_cpnext;
	struct journal_head *prev = jh->b_cpprev;

	prev->b_cpnext = next;
	next->b_cpprev = prev;

	if (transaction->t_checkpoint_list != jh)
		return;

	transaction->t_checkpoint_list = next == jh ? NULL : next;
}

static void ifs_jbd2_flush_checkpoint_batch(journal_t *journal,
					    int *batch_count)
{
	struct blk_plug plug;
	int index;

	if (*batch_count == 0)
		return;

	blk_start_plug(&plug);
	for (index = 0; index < *batch_count; ++index)
		write_dirty_buffer(journal->j_chkpt_bhs[index], REQ_SYNC);
	blk_finish_plug(&plug);

	for (index = 0; index < *batch_count; ++index) {
		struct buffer_head *bh = journal->j_chkpt_bhs[index];

		journal->j_chkpt_bhs[index] = NULL;
		__brelse(bh);
	}

	*batch_count = 0;
}

void __jbd2_log_wait_for_space(journal_t *journal)
__acquires(&journal->j_state_lock)
__releases(&journal->j_state_lock)
{
	const int required = journal->j_max_transaction_buffers;

	while (jbd2_log_space_left(journal) < required) {
		bool have_checkpoint;
		bool have_commit = false;
		tid_t commit_tid = 0;
		int available;

		write_unlock(&journal->j_state_lock);
		mutex_lock_io(&journal->j_checkpoint_mutex);
		write_lock(&journal->j_state_lock);

		if (journal->j_flags & JBD2_ABORT) {
			mutex_unlock(&journal->j_checkpoint_mutex);
			return;
		}

		spin_lock(&journal->j_list_lock);
		available = jbd2_log_space_left(journal);
		if (available >= required) {
			spin_unlock(&journal->j_list_lock);
			mutex_unlock(&journal->j_checkpoint_mutex);
			continue;
		}

		have_checkpoint = journal->j_checkpoint_transactions != NULL;
		if (journal->j_committing_transaction) {
			have_commit = true;
			commit_tid = journal->j_committing_transaction->t_tid;
		}
		spin_unlock(&journal->j_list_lock);
		write_unlock(&journal->j_state_lock);

		if (have_checkpoint) {
			jbd2_log_do_checkpoint(journal);
		} else {
			int tail_result = jbd2_cleanup_journal_tail(journal);

			if (tail_result > 0 && have_commit) {
				mutex_unlock(&journal->j_checkpoint_mutex);
				jbd2_log_wait_commit(journal, commit_tid);
				write_lock(&journal->j_state_lock);
				continue;
			}

			if (tail_result > 0 && !have_commit) {
				pr_err("JBD2: %s needs %d journal blocks but only %d remain\n",
				       journal->j_devname, required, available);
				jbd2_journal_abort(journal, -EIO);
			}
		}

		write_lock(&journal->j_state_lock);
		mutex_unlock(&journal->j_checkpoint_mutex);
	}
}

static bool ifs_jbd2_checkpoint_transaction_changed(
	const journal_t *journal,
	const transaction_t *transaction,
	const tid_t tid)
{
	return journal->j_checkpoint_transactions != transaction ||
	       transaction->t_tid != tid;
}

static int ifs_jbd2_checkpoint_queue_buffer(
	journal_t *journal,
	transaction_t *transaction,
	struct journal_head *jh,
	int *batch_count)
{
	struct buffer_head *bh = jh2bh(jh);

	if (WARN_ON_ONCE(buffer_jwrite(bh)))
		return -EFSCORRUPTED;

	get_bh(bh);
	journal->j_chkpt_bhs[*batch_count] = bh;
	(*batch_count)++;
	transaction->t_chp_stats.cs_written++;
	transaction->t_checkpoint_list = jh->b_cpnext;
	return 0;
}

int jbd2_log_do_checkpoint(journal_t *journal)
{
	transaction_t *transaction;
	tid_t transaction_tid;
	int batch_count = 0;
	int result;

	result = jbd2_cleanup_journal_tail(journal);
	trace_jbd2_checkpoint(journal, result);
	if (result <= 0)
		return result;

	spin_lock(&journal->j_list_lock);
	transaction = journal->j_checkpoint_transactions;
	if (!transaction)
		goto out_unlock;

	if (transaction->t_chp_stats.cs_chp_time == 0)
		transaction->t_chp_stats.cs_chp_time = jiffies;
	transaction_tid = transaction->t_tid;

	for (;;) {
		struct journal_head *jh;
		struct buffer_head *bh;

		if (ifs_jbd2_checkpoint_transaction_changed(
			    journal, transaction, transaction_tid))
			break;

		jh = transaction->t_checkpoint_list;
		if (!jh)
			break;
		bh = jh2bh(jh);

		if (jh->b_transaction) {
			const tid_t wait_tid = jh->b_transaction->t_tid;

			transaction->t_chp_stats.cs_forced_to_close++;
			spin_unlock(&journal->j_list_lock);
			if (batch_count)
				ifs_jbd2_flush_checkpoint_batch(
					journal, &batch_count);

			jbd2_log_start_commit(journal, wait_tid);
			mutex_unlock(&journal->j_checkpoint_mutex);
			jbd2_log_wait_commit(journal, wait_tid);
			mutex_lock_io(&journal->j_checkpoint_mutex);
			spin_lock(&journal->j_list_lock);
			continue;
		}

		if (!trylock_buffer(bh)) {
			get_bh(bh);
			spin_unlock(&journal->j_list_lock);
			if (batch_count)
				ifs_jbd2_flush_checkpoint_batch(
					journal, &batch_count);
			wait_on_buffer(bh);
			__brelse(bh);
			cond_resched();
			spin_lock(&journal->j_list_lock);
			continue;
		}

		if (!buffer_dirty(bh)) {
			unlock_buffer(bh);
			if (__jbd2_journal_remove_checkpoint(jh) ||
			    !transaction->t_checkpoint_list)
				break;
			continue;
		}

		unlock_buffer(bh);
		result = ifs_jbd2_checkpoint_queue_buffer(
			journal, transaction, jh, &batch_count);
		if (result) {
			spin_unlock(&journal->j_list_lock);
			ifs_jbd2_flush_checkpoint_batch(
				journal, &batch_count);
			jbd2_journal_abort(journal, result);
			return result;
		}

		if (batch_count == JBD2_NR_BATCH ||
		    need_resched() ||
		    spin_needbreak(&journal->j_list_lock) ||
		    jh2bh(transaction->t_checkpoint_list) ==
			    journal->j_chkpt_bhs[0]) {
			spin_unlock(&journal->j_list_lock);
			ifs_jbd2_flush_checkpoint_batch(
				journal, &batch_count);
			cond_resched();
			spin_lock(&journal->j_list_lock);
		}
	}

out_unlock:
	spin_unlock(&journal->j_list_lock);
	if (batch_count)
		ifs_jbd2_flush_checkpoint_batch(journal, &batch_count);

	result = jbd2_cleanup_journal_tail(journal);
	return result < 0 ? result : 0;
}

int jbd2_cleanup_journal_tail(journal_t *journal)
{
	tid_t oldest_tid;
	unsigned long oldest_block;

	if (is_journal_aborted(journal))
		return -EIO;

	if (!jbd2_journal_get_log_tail(
		    journal, &oldest_tid, &oldest_block))
		return 1;

	if (WARN_ON_ONCE(oldest_block == 0)) {
		jbd2_journal_abort(journal, -EFSCORRUPTED);
		return -EFSCORRUPTED;
	}

	if (journal->j_flags & JBD2_BARRIER)
		blkdev_issue_flush(journal->j_fs_dev);

	return __jbd2_update_log_tail(
		journal, oldest_tid, oldest_block);
}

static unsigned long ifs_jbd2_shrink_checkpoint_ring(
	struct journal_head *first,
	enum jbd2_shrink_type type,
	bool *transaction_released)
{
	struct journal_head *last;
	struct journal_head *cursor;
	unsigned long removed = 0;

	*transaction_released = false;
	if (!first)
		return 0;

	last = first->b_cpprev;
	cursor = first;

	for (;;) {
		struct journal_head *next = cursor->b_cpnext;
		int result;

		if (type == JBD2_SHRINK_DESTROY)
			result = __jbd2_journal_remove_checkpoint(cursor);
		else
			result = jbd2_journal_try_remove_checkpoint(cursor);

		if (result < 0) {
			if (type != JBD2_SHRINK_BUSY_SKIP)
				break;
		} else {
			removed++;
			if (result > 0) {
				*transaction_released = true;
				break;
			}
		}

		if (cursor == last || need_resched())
			break;
		cursor = next;
	}

	return removed;
}

unsigned long jbd2_journal_shrink_checkpoint_list(
	journal_t *journal,
	unsigned long *nr_to_scan)
{
	transaction_t *transaction;
	transaction_t *last;
	unsigned long removed_total = 0;
	tid_t first_tid = 0;
	tid_t current_tid = 0;
	tid_t last_tid = 0;
	tid_t next_tid = 0;
	bool first_seen = false;

	for (;;) {
		spin_lock(&journal->j_list_lock);
		if (!journal->j_checkpoint_transactions) {
			spin_unlock(&journal->j_list_lock);
			break;
		}

		transaction = journal->j_shrink_transaction ?
			journal->j_shrink_transaction :
			journal->j_checkpoint_transactions;
		last = journal->j_checkpoint_transactions->t_cpprev;

		if (!first_seen) {
			first_tid = transaction->t_tid;
			first_seen = true;
		}
		last_tid = last->t_tid;

		for (;;) {
			transaction_t *next = transaction->t_cpnext;
			bool released;
			unsigned long removed;

			current_tid = transaction->t_tid;
			removed = ifs_jbd2_shrink_checkpoint_ring(
				transaction->t_checkpoint_list,
				JBD2_SHRINK_BUSY_SKIP, &released);
			removed_total += removed;
			*nr_to_scan -= min(*nr_to_scan, removed);

			if (*nr_to_scan == 0 ||
			    transaction == last ||
			    need_resched() ||
			    spin_needbreak(&journal->j_list_lock)) {
				if (transaction != last) {
					journal->j_shrink_transaction = next;
					next_tid = next->t_tid;
				} else {
					journal->j_shrink_transaction = NULL;
					next_tid = 0;
				}
				break;
			}

			transaction = next;
		}

		spin_unlock(&journal->j_list_lock);
		cond_resched();

		if (*nr_to_scan == 0 ||
		    !journal->j_shrink_transaction)
			break;
	}

	trace_jbd2_shrink_checkpoint_list(
		journal, first_tid, current_tid, last_tid,
		removed_total, next_tid);
	return removed_total;
}

void __jbd2_journal_clean_checkpoint_list(
	journal_t *journal,
	enum jbd2_shrink_type type)
{
	transaction_t *transaction;
	transaction_t *last;

	WARN_ON_ONCE(type == JBD2_SHRINK_BUSY_SKIP);

	transaction = journal->j_checkpoint_transactions;
	if (!transaction)
		return;

	last = transaction->t_cpprev;
	for (;;) {
		transaction_t *next = transaction->t_cpnext;
		bool released;

		ifs_jbd2_shrink_checkpoint_ring(
			transaction->t_checkpoint_list, type, &released);

		if (need_resched() || !released ||
		    transaction == last)
			return;

		transaction = next;
	}
}

void jbd2_journal_destroy_checkpoint(journal_t *journal)
{
	for (;;) {
		spin_lock(&journal->j_list_lock);
		if (!journal->j_checkpoint_transactions) {
			spin_unlock(&journal->j_list_lock);
			return;
		}
		__jbd2_journal_clean_checkpoint_list(
			journal, JBD2_SHRINK_DESTROY);
		spin_unlock(&journal->j_list_lock);
		cond_resched();
	}
}

int __jbd2_journal_remove_checkpoint(struct journal_head *jh)
{
	transaction_t *transaction = jh->b_cp_transaction;
	journal_t *journal;
	struct transaction_chp_stats_s *stats;

	if (!transaction)
		return 0;

	journal = transaction->t_journal;
	ifs_jbd2_unlink_checkpoint_head(jh);
	jh->b_cp_transaction = NULL;
	percpu_counter_dec(&journal->j_checkpoint_jh_count);
	jbd2_journal_put_journal_head(jh);

	if (transaction->t_checkpoint_list ||
	    transaction->t_state != T_FINISHED)
		return 0;

	stats = &transaction->t_chp_stats;
	if (stats->cs_chp_time)
		stats->cs_chp_time =
			jbd2_time_diff(stats->cs_chp_time, jiffies);

	trace_jbd2_checkpoint_stats(
		journal->j_fs_dev->bd_dev,
		transaction->t_tid, stats);

	__jbd2_journal_drop_transaction(journal, transaction);
	jbd2_journal_free_transaction(transaction);
	return 1;
}

int jbd2_journal_try_remove_checkpoint(struct journal_head *jh)
{
	struct buffer_head *bh;

	if (jh->b_transaction)
		return -EBUSY;

	bh = jh2bh(jh);
	if (!trylock_buffer(bh))
		return -EBUSY;
	if (buffer_dirty(bh)) {
		unlock_buffer(bh);
		return -EBUSY;
	}
	unlock_buffer(bh);

	return __jbd2_journal_remove_checkpoint(jh);
}

void __jbd2_journal_insert_checkpoint(
	struct journal_head *jh,
	transaction_t *transaction)
{
	struct journal_head *head;

	J_ASSERT_JH(jh,
		buffer_dirty(jh2bh(jh)) ||
		buffer_jbddirty(jh2bh(jh)));
	J_ASSERT_JH(jh, jh->b_cp_transaction == NULL);

	jbd2_journal_grab_journal_head(jh2bh(jh));
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
	percpu_counter_inc(
		&transaction->t_journal->j_checkpoint_jh_count);
}

void __jbd2_journal_drop_transaction(
	journal_t *journal,
	transaction_t *transaction)
{
	assert_spin_locked(&journal->j_list_lock);

	journal->j_shrink_transaction = NULL;
	if (transaction->t_cpnext) {
		transaction->t_cpnext->t_cpprev = transaction->t_cpprev;
		transaction->t_cpprev->t_cpnext = transaction->t_cpnext;

		if (journal->j_checkpoint_transactions == transaction) {
			journal->j_checkpoint_transactions =
				transaction->t_cpnext;
			if (journal->j_checkpoint_transactions ==
			    transaction)
				journal->j_checkpoint_transactions = NULL;
		}
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
}
