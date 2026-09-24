/* Infiltrator Filesystem Support — EXT4 journal durability adapter.
 * EXT4-to-journal glue plus checkpoint, commit, recovery and revoke semantics
 * are one crash-consistency responsibility. The journal core/transaction
 * engine remains isolated until its inherited implementation is replaced.
 */

#include "ext4_jbd2.h"

#include <trace/events/ext4.h>

int ext4_inode_journal_mode(struct inode *inode)
{
	if (!EXT4_JOURNAL(inode))
		return EXT4_INODE_WRITEBACK_DATA_MODE;

	if (!S_ISREG(inode->i_mode) ||
	    ext4_test_inode_flag(inode, EXT4_INODE_EA_INODE) ||
	    test_opt(inode->i_sb, DATA_FLAGS) == EXT4_MOUNT_JOURNAL_DATA ||
	    (ext4_test_inode_flag(inode, EXT4_INODE_JOURNAL_DATA) &&
	     !test_opt(inode->i_sb, DELALLOC))) {
		if (S_ISREG(inode->i_mode) && IS_ENCRYPTED(inode))
			return EXT4_INODE_ORDERED_DATA_MODE;
		return EXT4_INODE_JOURNAL_DATA_MODE;
	}

	if (test_opt(inode->i_sb, DATA_FLAGS) == EXT4_MOUNT_ORDERED_DATA)
		return EXT4_INODE_ORDERED_DATA_MODE;
	if (test_opt(inode->i_sb, DATA_FLAGS) == EXT4_MOUNT_WRITEBACK_DATA)
		return EXT4_INODE_WRITEBACK_DATA_MODE;

	BUG();
}

static handle_t *ext4_nojournal_get(void)
{
	unsigned long refs = (unsigned long)current->journal_info;

	BUG_ON(refs >= EXT4_NOJOURNAL_MAX_REF_COUNT);
	refs++;
	current->journal_info = (handle_t *)refs;
	return (handle_t *)refs;
}

static void ext4_nojournal_put(handle_t *handle)
{
	unsigned long refs = (unsigned long)handle;

	BUG_ON(refs == 0);
	current->journal_info = (handle_t *)(refs - 1);
}

static int ext4_journal_can_start(struct super_block *sb)
{
	journal_t *journal;

	might_sleep();
	if (unlikely(ext4_forced_shutdown(sb)))
		return -EIO;
	if (WARN_ON_ONCE(sb_rdonly(sb)))
		return -EROFS;

	WARN_ON(sb->s_writers.frozen == SB_FREEZE_COMPLETE);
	journal = EXT4_SB(sb)->s_journal;
	if (journal && is_journal_aborted(journal)) {
		ext4_abort(sb, -journal->j_errno, "aborted journal");
		return -EROFS;
	}

	return 0;
}

handle_t *__ext4_journal_start_sb(struct inode *inode,
				  struct super_block *sb,
				  unsigned int line, int type,
				  int blocks, int reserved_blocks,
				  int revoke_credits)
{
	journal_t *journal;
	int err;

	if (inode)
		trace_ext4_journal_start_inode(
			inode, blocks, reserved_blocks,
			revoke_credits, type, _RET_IP_);
	else
		trace_ext4_journal_start_sb(
			sb, blocks, reserved_blocks,
			revoke_credits, type, _RET_IP_);

	err = ext4_journal_can_start(sb);
	if (err)
		return ERR_PTR(err);

	journal = EXT4_SB(sb)->s_journal;
	if (!journal || (EXT4_SB(sb)->s_mount_state & EXT4_FC_REPLAY))
		return ext4_nojournal_get();

	return jbd2__journal_start(
		journal, blocks, reserved_blocks, revoke_credits,
		GFP_NOFS, type, line);
}

int __ext4_journal_stop(const char *where, unsigned int line,
			handle_t *handle)
{
	struct super_block *sb;
	int stored_err;
	int stop_err;

	if (!ext4_handle_valid(handle)) {
		ext4_nojournal_put(handle);
		return 0;
	}

	stored_err = handle->h_err;
	if (!handle->h_transaction) {
		stop_err = jbd2_journal_stop(handle);
		return stored_err ? stored_err : stop_err;
	}

	sb = handle->h_transaction->t_journal->j_private;
	stop_err = jbd2_journal_stop(handle);
	if (!stored_err)
		stored_err = stop_err;
	if (stored_err)
		__ext4_std_error(sb, where, line, stored_err);
	return stored_err;
}

handle_t *__ext4_journal_start_reserved(handle_t *handle,
					unsigned int line, int type)
{
	struct super_block *sb;
	int err;

	if (!ext4_handle_valid(handle))
		return ext4_nojournal_get();

	sb = handle->h_journal->j_private;
	trace_ext4_journal_start_reserved(
		sb, jbd2_handle_buffer_credits(handle), _RET_IP_);

	err = ext4_journal_can_start(sb);
	if (err) {
		jbd2_journal_free_reserved(handle);
		return ERR_PTR(err);
	}

	err = jbd2_journal_start_reserved(handle, type, line);
	return err ? ERR_PTR(err) : handle;
}

int __ext4_journal_ensure_credits(handle_t *handle, int check_credits,
				  int extend_credits, int revoke_credits)
{
	if (!ext4_handle_valid(handle))
		return 0;
	if (is_handle_aborted(handle))
		return -EROFS;
	if (jbd2_handle_buffer_credits(handle) >= check_credits &&
	    handle->h_revoke_credits >= revoke_credits)
		return 0;

	extend_credits = max(
		0, extend_credits - jbd2_handle_buffer_credits(handle));
	revoke_credits = max(
		0, revoke_credits - handle->h_revoke_credits);
	return ext4_journal_extend(
		handle, extend_credits, revoke_credits);
}

static void ext4_abort_handle(const char *where, unsigned int line,
			      const char *operation,
			      struct buffer_head *bh,
			      handle_t *handle, int err)
{
	char error_text[16];

	BUG_ON(!ext4_handle_valid(handle));
	if (bh)
		BUFFER_TRACE(bh, "abort");
	if (!handle->h_err)
		handle->h_err = err;
	if (is_handle_aborted(handle))
		return;

	pr_err("EXT4-fs: %s:%u: aborting transaction: %s in %s\n",
	       where, line,
	       ext4_decode_error(NULL, err, error_text), operation);
	jbd2_journal_abort_handle(handle);
}

static void ext4_check_device_writeback(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct address_space *mapping = sb->s_bdev->bd_mapping;
	int err;

	if (!errseq_check(&mapping->wb_err, READ_ONCE(sbi->s_bdev_wb_err)))
		return;

	spin_lock(&sbi->s_bdev_wb_lock);
	err = errseq_check_and_advance(
		&mapping->wb_err, &sbi->s_bdev_wb_err);
	spin_unlock(&sbi->s_bdev_wb_lock);

	if (err)
		ext4_error_err(
			sb, -err, "asynchronous metadata writeback failed");
}

static void ext4_set_journal_trigger(struct super_block *sb,
				     struct buffer_head *bh,
				     enum ext4_journal_trigger_type type)
{
	if (type == EXT4_JTR_NONE || !ext4_has_metadata_csum(sb))
		return;

	BUG_ON(type >= EXT4_JOURNAL_TRIGGER_COUNT);
	jbd2_journal_set_triggers(
		bh, &EXT4_SB(sb)->s_journal_triggers[type].tr_triggers);
}

int __ext4_journal_get_write_access(const char *where, unsigned int line,
				    handle_t *handle,
				    struct super_block *sb,
				    struct buffer_head *bh,
				    enum ext4_journal_trigger_type type)
{
	int err;

	might_sleep();
	if (ext4_handle_valid(handle)) {
		err = jbd2_journal_get_write_access(handle, bh);
		if (err) {
			ext4_abort_handle(
				where, line, __func__, bh, handle, err);
			return err;
		}
	} else {
		ext4_check_device_writeback(sb);
	}

	ext4_set_journal_trigger(sb, bh, type);
	return 0;
}

int __ext4_forget(const char *where, unsigned int line, handle_t *handle,
		  int metadata, struct inode *inode,
		  struct buffer_head *bh, ext4_fsblk_t block)
{
	int err;

	might_sleep();
	trace_ext4_forget(inode, metadata, block);

	if (!ext4_handle_valid(handle)) {
		if (bh) {
			clear_buffer_dirty(bh);
			wait_on_buffer(bh);
			__bforget(bh);
		}
		return 0;
	}

	if (test_opt(inode->i_sb, DATA_FLAGS) == EXT4_MOUNT_JOURNAL_DATA ||
	    (!metadata && !ext4_should_journal_data(inode))) {
		if (!bh)
			return 0;
		err = jbd2_journal_forget(handle, bh);
		if (err)
			ext4_abort_handle(
				where, line, __func__, bh, handle, err);
		return err;
	}

	err = jbd2_journal_revoke(handle, block, bh);
	if (err) {
		ext4_abort_handle(
			where, line, __func__, bh, handle, err);
		__ext4_error(
			inode->i_sb, where, line, true, -err, 0,
			"journal revoke failed: %d", err);
	}
	return err;
}

int __ext4_journal_get_create_access(
	const char *where, unsigned int line,
	handle_t *handle, struct super_block *sb,
	struct buffer_head *bh,
	enum ext4_journal_trigger_type type)
{
	int err;

	if (!ext4_handle_valid(handle))
		return 0;

	err = jbd2_journal_get_create_access(handle, bh);
	if (err) {
		ext4_abort_handle(
			where, line, __func__, bh, handle, err);
		return err;
	}

	ext4_set_journal_trigger(sb, bh, type);
	return 0;
}

int __ext4_handle_dirty_metadata(const char *where, unsigned int line,
				 handle_t *handle, struct inode *inode,
				 struct buffer_head *bh)
{
	int err = 0;

	might_sleep();
	set_buffer_meta(bh);
	set_buffer_prio(bh);
	set_buffer_uptodate(bh);

	if (ext4_handle_valid(handle)) {
		err = jbd2_journal_dirty_metadata(handle, bh);
		if (!is_handle_aborted(handle) && WARN_ON_ONCE(err)) {
			ext4_abort_handle(
				where, line, __func__, bh, handle, err);
			if (inode)
				ext4_error_inode(
					inode, where, line, bh->b_blocknr,
					"journal metadata dirty failed: %d",
					err);
			else
				pr_err(
					"EXT4: journal metadata dirty failed: %d\n",
					err);
		}
		return err;
	}

	if (inode)
		mark_buffer_dirty_inode(bh, inode);
	else
		mark_buffer_dirty(bh);

	if (inode && inode_needs_sync(inode)) {
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh)) {
			ext4_error_inode_err(
				inode, where, line, bh->b_blocknr, EIO,
				"metadata sync failed");
			err = -EIO;
		}
	}

	return err;
}


/* ===== checkpoint ===== */
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


/* ===== commit ===== */
/*
 * Filesystem Support EXT4 embedded journal commit engine.
 *
 * A full commit freezes the running transaction, flushes ordered data,
 * serialises metadata into the journal, publishes the commit record, and
 * transfers surviving buffers to checkpoint ownership.
 */

#include <linux/backing-dev.h>
#include <linux/bio.h>
#include <linux/bitops.h>
#include <linux/blkdev.h>
#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/jiffies.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/writeback.h>
#include <trace/events/jbd2.h>

static void ifs_jbd2_commit_end_io(
	struct buffer_head *bh, int uptodate)
{
	struct buffer_head *source = bh->b_private;

	if (uptodate)
		set_buffer_uptodate(bh);
	else
		clear_buffer_uptodate(bh);

	if (source) {
		clear_bit_unlock(BH_Shadow, &source->b_state);
		smp_mb__after_atomic();
		wake_up_bit(&source->b_state, BH_Shadow);
	}
	unlock_buffer(bh);
}

static void ifs_jbd2_release_buffer(struct buffer_head *bh)
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

static void ifs_jbd2_commit_checksum(
	journal_t *journal, struct buffer_head *bh)
{
	struct commit_header *header;
	__u32 checksum;

	if (!jbd2_journal_has_csum_v2or3(journal))
		return;

	header = (struct commit_header *)bh->b_data;
	header->h_chksum_type = 0;
	header->h_chksum_size = 0;
	header->h_chksum[0] = 0;
	checksum = jbd2_chksum(
		journal, journal->j_csum_seed,
		bh->b_data, journal->j_blocksize);
	header->h_chksum[0] = cpu_to_be32(checksum);
}

static int ifs_jbd2_submit_commit_record(
	journal_t *journal,
	transaction_t *transaction,
	struct buffer_head **result,
	__u32 legacy_checksum)
{
	struct buffer_head *bh;
	struct commit_header *header;
	struct timespec64 now;
	blk_opf_t flags =
		REQ_OP_WRITE | JBD2_JOURNAL_REQ_FLAGS;

	*result = NULL;
	if (is_journal_aborted(journal))
		return 0;

	bh = jbd2_journal_get_descriptor_buffer(
		transaction, JBD2_COMMIT_BLOCK);
	if (!bh)
		return -EIO;

	header = (struct commit_header *)bh->b_data;
	ktime_get_coarse_real_ts64(&now);
	header->h_commit_sec = cpu_to_be64(now.tv_sec);
	header->h_commit_nsec = cpu_to_be32(now.tv_nsec);

	if (jbd2_has_feature_checksum(journal)) {
		header->h_chksum_type = JBD2_CRC32_CHKSUM;
		header->h_chksum_size =
			JBD2_CRC32_CHKSUM_SIZE;
		header->h_chksum[0] =
			cpu_to_be32(legacy_checksum);
	}
	ifs_jbd2_commit_checksum(journal, bh);

	lock_buffer(bh);
	clear_buffer_dirty(bh);
	set_buffer_uptodate(bh);
	bh->b_end_io = ifs_jbd2_commit_end_io;

	if ((journal->j_flags & JBD2_BARRIER) &&
	    !jbd2_has_feature_async_commit(journal))
		flags |= REQ_PREFLUSH | REQ_FUA;

	submit_bh(flags, bh);
	*result = bh;
	return 0;
}

static int ifs_jbd2_wait_commit_record(
	struct buffer_head *bh)
{
	int error = 0;

	clear_buffer_dirty(bh);
	wait_on_buffer(bh);
	if (!buffer_uptodate(bh))
		error = -EIO;
	put_bh(bh);
	return error;
}

int jbd2_submit_inode_data(
	journal_t *journal, struct jbd2_inode *jinode)
{
	if (!jinode ||
	    !(jinode->i_flags & JI_WRITE_DATA) ||
	    !journal->j_submit_inode_data_buffers)
		return 0;

	trace_jbd2_submit_inode_data(jinode->i_vfs_inode);
	return journal->j_submit_inode_data_buffers(jinode);
}

int jbd2_wait_inode_data(
	journal_t *journal, struct jbd2_inode *jinode)
{
	if (!jinode ||
	    !(jinode->i_flags & JI_WAIT_DATA) ||
	    !jinode->i_vfs_inode ||
	    !jinode->i_vfs_inode->i_mapping)
		return 0;

	return filemap_fdatawait_range_keep_errors(
		jinode->i_vfs_inode->i_mapping,
		jinode->i_dirty_start,
		jinode->i_dirty_end);
}

int jbd2_journal_finish_inode_data_buffers(
	struct jbd2_inode *jinode)
{
	return filemap_fdatawait_range_keep_errors(
		jinode->i_vfs_inode->i_mapping,
		jinode->i_dirty_start,
		jinode->i_dirty_end);
}

static int ifs_jbd2_submit_transaction_data(
	journal_t *journal,
	transaction_t *transaction)
{
	struct jbd2_inode *jinode;
	int first_error = 0;

	spin_lock(&journal->j_list_lock);
	list_for_each_entry(
		jinode, &transaction->t_inode_list, i_list) {
		int error;

		if (!(jinode->i_flags & JI_WRITE_DATA))
			continue;

		jinode->i_flags |= JI_COMMIT_RUNNING;
		spin_unlock(&journal->j_list_lock);

		trace_jbd2_submit_inode_data(
			jinode->i_vfs_inode);
		error = journal->j_submit_inode_data_buffers ?
			journal->j_submit_inode_data_buffers(jinode) :
			0;
		if (!first_error)
			first_error = error;

		spin_lock(&journal->j_list_lock);
		J_ASSERT(
			jinode->i_transaction == transaction);
		jinode->i_flags &= ~JI_COMMIT_RUNNING;
		smp_mb();
		wake_up_bit(
			&jinode->i_flags,
			__JI_COMMIT_RUNNING);
	}
	spin_unlock(&journal->j_list_lock);
	return first_error;
}

static int ifs_jbd2_finish_transaction_data(
	journal_t *journal,
	transaction_t *transaction)
{
	struct jbd2_inode *jinode;
	struct jbd2_inode *next;
	int first_error = 0;

	spin_lock(&journal->j_list_lock);
	list_for_each_entry(
		jinode, &transaction->t_inode_list, i_list) {
		int error;

		if (!(jinode->i_flags & JI_WAIT_DATA))
			continue;

		jinode->i_flags |= JI_COMMIT_RUNNING;
		spin_unlock(&journal->j_list_lock);

		error = journal->j_finish_inode_data_buffers ?
			journal->j_finish_inode_data_buffers(jinode) :
			0;
		if (!first_error)
			first_error = error;
		cond_resched();

		spin_lock(&journal->j_list_lock);
		jinode->i_flags &= ~JI_COMMIT_RUNNING;
		smp_mb();
		wake_up_bit(
			&jinode->i_flags,
			__JI_COMMIT_RUNNING);
	}

	list_for_each_entry_safe(
		jinode, next,
		&transaction->t_inode_list, i_list) {
		list_del(&jinode->i_list);
		if (jinode->i_next_transaction) {
			jinode->i_transaction =
				jinode->i_next_transaction;
			jinode->i_next_transaction = NULL;
			list_add(
				&jinode->i_list,
				&jinode->i_transaction->t_inode_list);
		} else {
			jinode->i_transaction = NULL;
			jinode->i_dirty_start = 0;
			jinode->i_dirty_end = 0;
		}
	}
	spin_unlock(&journal->j_list_lock);
	return first_error;
}

static __u32 ifs_jbd2_legacy_checksum(
	__u32 checksum, struct buffer_head *bh)
{
	void *address;
	__u32 result;

	address = kmap_local_folio(
		bh->b_folio, bh_offset(bh));
	result = crc32_be(
		checksum, address, bh->b_size);
	kunmap_local(address);
	return result;
}

static void ifs_jbd2_set_tag_block(
	journal_t *journal,
	journal_block_tag_t *tag,
	unsigned long long block)
{
	tag->t_blocknr = cpu_to_be32((u32)block);
	if (jbd2_has_feature_64bit(journal))
		tag->t_blocknr_high =
			cpu_to_be32((u32)(block >> 32));
}

static void ifs_jbd2_set_tag_checksum(
	journal_t *journal,
	journal_block_tag_t *tag,
	struct buffer_head *bh,
	__u32 sequence)
{
	journal_block_tag3_t *tag3 =
		(journal_block_tag3_t *)tag;
	void *address;
	__be32 sequence_be;
	__u32 checksum;

	if (!jbd2_journal_has_csum_v2or3(journal))
		return;

	sequence_be = cpu_to_be32(sequence);
	checksum = jbd2_chksum(
		journal, journal->j_csum_seed,
		(__u8 *)&sequence_be, sizeof(sequence_be));
	address = kmap_local_folio(
		bh->b_folio, bh_offset(bh));
	checksum = jbd2_chksum(
		journal, checksum, address, bh->b_size);
	kunmap_local(address);

	if (jbd2_has_feature_csum3(journal))
		tag3->t_checksum =
			cpu_to_be32(checksum);
	else
		tag->t_checksum =
			cpu_to_be16(checksum);
}

static void ifs_jbd2_wait_fast_commit_idle(
	journal_t *journal)
{
	write_lock(&journal->j_state_lock);
	journal->j_flags |= JBD2_FULL_COMMIT_ONGOING;

	while (journal->j_flags &
	       JBD2_FAST_COMMIT_ONGOING) {
		DEFINE_WAIT(wait);

		prepare_to_wait(
			&journal->j_fc_wait, &wait,
			TASK_UNINTERRUPTIBLE);
		write_unlock(&journal->j_state_lock);
		schedule();
		write_lock(&journal->j_state_lock);
		finish_wait(&journal->j_fc_wait, &wait);
	}

	write_unlock(&journal->j_state_lock);
}

static transaction_t *ifs_jbd2_lock_running_transaction(
	journal_t *journal,
	struct transaction_stats_s *stats)
{
	transaction_t *transaction =
		journal->j_running_transaction;

	J_ASSERT(transaction);
	J_ASSERT(!journal->j_committing_transaction);

	write_lock(&journal->j_state_lock);
	journal->j_fc_off = 0;
	J_ASSERT(transaction->t_state == T_RUNNING);
	transaction->t_state = T_LOCKED;

	stats->run.rs_wait = transaction->t_max_wait;
	stats->run.rs_request_delay = 0;
	stats->run.rs_locked = jiffies;
	if (transaction->t_requested)
		stats->run.rs_request_delay =
			jbd2_time_diff(
				transaction->t_requested,
				stats->run.rs_locked);
	stats->run.rs_running =
		jbd2_time_diff(
			transaction->t_start,
			stats->run.rs_locked);

	jbd2_journal_wait_updates(journal);
	transaction->t_state = T_SWITCH;

	while (transaction->t_reserved_list) {
		struct journal_head *jh =
			transaction->t_reserved_list;

		if (jh->b_committed_data) {
			struct buffer_head *bh = jh2bh(jh);

			spin_lock(&jh->b_state_lock);
			jbd2_free(
				jh->b_committed_data,
				bh->b_size);
			jh->b_committed_data = NULL;
			spin_unlock(&jh->b_state_lock);
		}
		jbd2_journal_refile_buffer(journal, jh);
	}

	write_unlock(&journal->j_state_lock);
	return transaction;
}

static void ifs_jbd2_publish_committing_transaction(
	journal_t *journal,
	transaction_t *transaction,
	struct transaction_stats_s *stats,
	ktime_t *start_time)
{
	write_lock(&journal->j_state_lock);

	atomic_sub(
		atomic_read(&journal->j_reserved_credits),
		&transaction->t_outstanding_credits);

	stats->run.rs_flushing = jiffies;
	stats->run.rs_locked =
		jbd2_time_diff(
			stats->run.rs_locked,
			stats->run.rs_flushing);

	transaction->t_state = T_FLUSH;
	journal->j_committing_transaction = transaction;
	journal->j_running_transaction = NULL;
	*start_time = ktime_get();
	transaction->t_log_start = journal->j_head;
	wake_up_all(&journal->j_wait_transaction_locked);

	write_unlock(&journal->j_state_lock);
}

static int ifs_jbd2_submit_metadata_batch(
	journal_t *journal,
	struct buffer_head **buffers,
	int count,
	__u32 *legacy_checksum)
{
	int index;

	for (index = 0; index < count; ++index) {
		struct buffer_head *bh = buffers[index];

		if (jbd2_has_feature_checksum(journal))
			*legacy_checksum =
				ifs_jbd2_legacy_checksum(
					*legacy_checksum, bh);

		lock_buffer(bh);
		clear_buffer_dirty(bh);
		set_buffer_uptodate(bh);
		bh->b_end_io = ifs_jbd2_commit_end_io;
		submit_bh(
			REQ_OP_WRITE | JBD2_JOURNAL_REQ_FLAGS,
			bh);
	}
	return 0;
}

static int ifs_jbd2_log_transaction_buffers(
	journal_t *journal,
	transaction_t *transaction,
	struct list_head *io_buffers,
	struct list_head *log_buffers,
	__u32 *legacy_checksum)
{
	struct buffer_head **write_buffers =
		journal->j_wbuf;
	struct buffer_head *descriptor = NULL;
	char *tag_cursor = NULL;
	journal_block_tag_t *last_tag = NULL;
	const int tag_bytes = journal_tag_bytes(journal);
	const int checksum_bytes =
		jbd2_journal_has_csum_v2or3(journal) ?
		sizeof(struct jbd2_journal_block_tail) : 0;
	int space_left = 0;
	int buffer_count = 0;
	bool first_tag = true;
	int error = 0;

	while (transaction->t_buffers) {
		struct journal_head *jh =
			transaction->t_buffers;
		unsigned long long log_block;
		int escape;
		int tag_flags = 0;

		if (is_journal_aborted(journal)) {
			clear_buffer_jbddirty(jh2bh(jh));
			jbd2_buffer_abort_trigger(
				jh,
				jh->b_frozen_data ?
					jh->b_frozen_triggers :
					jh->b_triggers);
			jbd2_journal_refile_buffer(
				journal, jh);
			continue;
		}

		if (!descriptor) {
			descriptor =
				jbd2_journal_get_descriptor_buffer(
					transaction,
					JBD2_DESCRIPTOR_BLOCK);
			if (!descriptor)
				return -EIO;

			tag_cursor =
				descriptor->b_data +
				sizeof(journal_header_t);
			space_left =
				descriptor->b_size -
				sizeof(journal_header_t);
			first_tag = true;
			set_buffer_jwrite(descriptor);
			set_buffer_dirty(descriptor);
			write_buffers[buffer_count++] =
				descriptor;
			jbd2_file_log_bh(
				log_buffers, descriptor);
		}

		error = jbd2_journal_next_log_block(
			journal, &log_block);
		if (error)
			return error;

		atomic_dec(
			&transaction->t_outstanding_credits);
		atomic_inc(&jh2bh(jh)->b_count);
		set_bit(BH_JWrite, &jh2bh(jh)->b_state);

		escape = jbd2_journal_write_metadata_buffer(
			transaction, jh,
			&write_buffers[buffer_count],
			log_block);
		if (escape < 0)
			return escape;

		jbd2_file_log_bh(
			io_buffers,
			write_buffers[buffer_count]);

		if (escape)
			tag_flags |= JBD2_FLAG_ESCAPE;
		if (!first_tag)
			tag_flags |= JBD2_FLAG_SAME_UUID;

		last_tag =
			(journal_block_tag_t *)tag_cursor;
		ifs_jbd2_set_tag_block(
			journal, last_tag,
			jh2bh(jh)->b_blocknr);
		last_tag->t_flags =
			cpu_to_be16(tag_flags);
		ifs_jbd2_set_tag_checksum(
			journal, last_tag,
			write_buffers[buffer_count],
			transaction->t_tid);

		tag_cursor += tag_bytes;
		space_left -= tag_bytes;
		buffer_count++;

		if (first_tag) {
			memcpy(
				tag_cursor, journal->j_uuid, 16);
			tag_cursor += 16;
			space_left -= 16;
			first_tag = false;
		}

		if (buffer_count == journal->j_wbufsize ||
		    !transaction->t_buffers ||
		    space_left <
			    tag_bytes + 16 + checksum_bytes) {
			if (last_tag)
				last_tag->t_flags |=
					cpu_to_be16(
						JBD2_FLAG_LAST_TAG);
			jbd2_descriptor_block_csum_set(
				journal, descriptor);
			ifs_jbd2_submit_metadata_batch(
				journal, write_buffers,
				buffer_count,
				legacy_checksum);
			descriptor = NULL;
			buffer_count = 0;
			cond_resched();
		}
	}

	return error;
}

static int ifs_jbd2_wait_logged_metadata(
	transaction_t *transaction,
	struct list_head *io_buffers,
	struct transaction_stats_s *stats)
{
	int error = 0;

	while (!list_empty(io_buffers)) {
		struct buffer_head *temporary =
			list_last_entry(
				io_buffers,
				struct buffer_head,
				b_assoc_buffers);
		struct journal_head *jh;
		struct buffer_head *original;

		wait_on_buffer(temporary);
		cond_resched();
		if (!buffer_uptodate(temporary))
			error = -EIO;

		jbd2_unfile_log_bh(temporary);
		stats->run.rs_blocks_logged++;
		__brelse(temporary);
		J_ASSERT_BH(
			temporary,
			atomic_read(&temporary->b_count) == 0);
		free_buffer_head(temporary);

		jh = transaction->t_shadow_list->b_tprev;
		original = jh2bh(jh);
		clear_buffer_jwrite(original);
		J_ASSERT_BH(
			original, buffer_jbddirty(original));
		J_ASSERT_BH(
			original, !buffer_shadow(original));
		jbd2_journal_file_buffer(
			jh, transaction, BJ_Forget);
		__brelse(original);
	}

	J_ASSERT(!transaction->t_shadow_list);
	return error;
}

static int ifs_jbd2_wait_control_buffers(
	struct list_head *log_buffers,
	struct transaction_stats_s *stats)
{
	int error = 0;

	while (!list_empty(log_buffers)) {
		struct buffer_head *bh =
			list_last_entry(
				log_buffers,
				struct buffer_head,
				b_assoc_buffers);

		wait_on_buffer(bh);
		cond_resched();
		if (!buffer_uptodate(bh))
			error = -EIO;

		clear_buffer_jwrite(bh);
		jbd2_unfile_log_bh(bh);
		stats->run.rs_blocks_logged++;
		__brelse(bh);
	}

	return error;
}

static void ifs_jbd2_finish_forget_list(
	journal_t *journal,
	transaction_t *transaction)
{
restart:
	spin_lock(&journal->j_list_lock);
	while (transaction->t_forget) {
		struct journal_head *jh =
			transaction->t_forget;
		struct buffer_head *bh = jh2bh(jh);
		transaction_t *old_checkpoint;
		bool free_page = false;
		bool drop_jh;

		spin_unlock(&journal->j_list_lock);
		get_bh(bh);
		spin_lock(&jh->b_state_lock);

		J_ASSERT_JH(
			jh, jh->b_transaction == transaction);

		if (jh->b_committed_data) {
			jbd2_free(
				jh->b_committed_data,
				bh->b_size);
			jh->b_committed_data = NULL;
			if (jh->b_frozen_data) {
				jh->b_committed_data =
					jh->b_frozen_data;
				jh->b_frozen_data = NULL;
				jh->b_frozen_triggers = NULL;
			}
		} else if (jh->b_frozen_data) {
			jbd2_free(
				jh->b_frozen_data,
				bh->b_size);
			jh->b_frozen_data = NULL;
			jh->b_frozen_triggers = NULL;
		}

		spin_lock(&journal->j_list_lock);
		old_checkpoint = jh->b_cp_transaction;
		if (old_checkpoint) {
			old_checkpoint->t_chp_stats.cs_dropped++;
			__jbd2_journal_remove_checkpoint(jh);
		}

		if (buffer_freed(bh) &&
		    !jh->b_next_transaction) {
			struct address_space *mapping;

			clear_buffer_freed(bh);
			clear_buffer_jbddirty(bh);
			mapping =
				READ_ONCE(bh->b_folio->mapping);
			if (mapping &&
			    !sb_is_blkdev_sb(
				    mapping->host->i_sb)) {
				clear_buffer_mapped(bh);
				clear_buffer_new(bh);
				clear_buffer_req(bh);
				bh->b_bdev = NULL;
			}
		}

		if (buffer_jbddirty(bh)) {
			__jbd2_journal_insert_checkpoint(
				jh, transaction);
			if (is_journal_aborted(journal))
				clear_buffer_jbddirty(bh);
		} else {
			J_ASSERT_BH(bh, !buffer_dirty(bh));
			free_page =
				!jh->b_next_transaction;
		}

		drop_jh =
			__jbd2_journal_refile_buffer(jh);
		spin_unlock(&jh->b_state_lock);

		if (drop_jh)
			jbd2_journal_put_journal_head(jh);
		if (free_page)
			ifs_jbd2_release_buffer(bh);
		else
			__brelse(bh);

		cond_resched_lock(
			&journal->j_list_lock);
	}
	spin_unlock(&journal->j_list_lock);

	write_lock(&journal->j_state_lock);
	spin_lock(&journal->j_list_lock);
	if (transaction->t_forget) {
		spin_unlock(&journal->j_list_lock);
		write_unlock(&journal->j_state_lock);
		goto restart;
	}

	if (!journal->j_checkpoint_transactions) {
		journal->j_checkpoint_transactions =
			transaction;
		transaction->t_cpnext = transaction;
		transaction->t_cpprev = transaction;
	} else {
		transaction_t *head =
			journal->j_checkpoint_transactions;

		transaction->t_cpnext = head;
		transaction->t_cpprev = head->t_cpprev;
		head->t_cpprev->t_cpnext = transaction;
		head->t_cpprev = transaction;
	}
	spin_unlock(&journal->j_list_lock);
	write_unlock(&journal->j_state_lock);
}

static void ifs_jbd2_publish_commit_complete(
	journal_t *journal,
	transaction_t *transaction,
	struct transaction_stats_s *stats,
	ktime_t start_time)
{
	u64 elapsed;

	write_lock(&journal->j_state_lock);

	transaction->t_start = jiffies;
	stats->run.rs_logging =
		jbd2_time_diff(
			stats->run.rs_logging,
			transaction->t_start);
	stats->ts_tid = transaction->t_tid;
	stats->run.rs_handle_count =
		atomic_read(&transaction->t_handle_count);
	stats->ts_requested =
		transaction->t_requested ? 1 : 0;

	transaction->t_state = T_COMMIT_CALLBACK;
	J_ASSERT(
		transaction ==
		journal->j_committing_transaction);
	WRITE_ONCE(
		journal->j_commit_sequence,
		transaction->t_tid);
	journal->j_committing_transaction = NULL;

	elapsed = ktime_to_ns(
		ktime_sub(ktime_get(), start_time));
	if (journal->j_average_commit_time)
		journal->j_average_commit_time =
			(elapsed +
			 3 * journal->j_average_commit_time) / 4;
	else
		journal->j_average_commit_time = elapsed;

	write_unlock(&journal->j_state_lock);

	if (journal->j_commit_callback)
		journal->j_commit_callback(
			journal, transaction);
	if (journal->j_fc_cleanup_callback)
		journal->j_fc_cleanup_callback(
			journal, 1, transaction->t_tid);

	trace_jbd2_end_commit(journal, transaction);

	write_lock(&journal->j_state_lock);
	journal->j_flags &=
		~JBD2_FULL_COMMIT_ONGOING;
	journal->j_flags &=
		~JBD2_FAST_COMMIT_ONGOING;

	spin_lock(&journal->j_list_lock);
	transaction->t_state = T_FINISHED;
	if (!transaction->t_checkpoint_list) {
		__jbd2_journal_drop_transaction(
			journal, transaction);
		jbd2_journal_free_transaction(transaction);
	}
	spin_unlock(&journal->j_list_lock);
	write_unlock(&journal->j_state_lock);

	wake_up(&journal->j_wait_done_commit);
	wake_up(&journal->j_fc_wait);

	spin_lock(&journal->j_history_lock);
	journal->j_stats.ts_tid++;
	journal->j_stats.ts_requested +=
		stats->ts_requested;
	journal->j_stats.run.rs_wait +=
		stats->run.rs_wait;
	journal->j_stats.run.rs_request_delay +=
		stats->run.rs_request_delay;
	journal->j_stats.run.rs_running +=
		stats->run.rs_running;
	journal->j_stats.run.rs_locked +=
		stats->run.rs_locked;
	journal->j_stats.run.rs_flushing +=
		stats->run.rs_flushing;
	journal->j_stats.run.rs_logging +=
		stats->run.rs_logging;
	journal->j_stats.run.rs_handle_count +=
		stats->run.rs_handle_count;
	journal->j_stats.run.rs_blocks +=
		stats->run.rs_blocks;
	journal->j_stats.run.rs_blocks_logged +=
		stats->run.rs_blocks_logged;
	spin_unlock(&journal->j_history_lock);
}

void jbd2_journal_commit_transaction(journal_t *journal)
{
	struct transaction_stats_s stats = { 0 };
	transaction_t *transaction;
	struct buffer_head *commit_bh = NULL;
	struct blk_plug plug;
	ktime_t start_time;
	unsigned long first_block;
	tid_t first_tid;
	__u32 legacy_checksum = ~0U;
	bool update_tail;
	int error = 0;
	LIST_HEAD(io_buffers);
	LIST_HEAD(log_buffers);

	if (journal->j_flags & JBD2_FLUSHED) {
		mutex_lock_io(&journal->j_checkpoint_mutex);
		jbd2_journal_update_sb_log_tail(
			journal,
			journal->j_tail_sequence,
			journal->j_tail, 0);
		mutex_unlock(&journal->j_checkpoint_mutex);
	}

	ifs_jbd2_wait_fast_commit_idle(journal);
	transaction =
		ifs_jbd2_lock_running_transaction(
			journal, &stats);

	trace_jbd2_start_commit(journal, transaction);
	trace_jbd2_commit_locking(journal, transaction);

	spin_lock(&journal->j_list_lock);
	__jbd2_journal_clean_checkpoint_list(
		journal, JBD2_SHRINK_BUSY_STOP);
	spin_unlock(&journal->j_list_lock);

	jbd2_clear_buffer_revoked_flags(journal);
	jbd2_journal_switch_revoke_table(journal);

	ifs_jbd2_publish_committing_transaction(
		journal, transaction,
		&stats, &start_time);

	error = ifs_jbd2_submit_transaction_data(
		journal, transaction);
	if (error)
		jbd2_journal_abort(journal, error);

	blk_start_plug(&plug);
	jbd2_journal_write_revoke_records(
		transaction, &log_buffers);

	write_lock(&journal->j_state_lock);
	transaction->t_state = T_COMMIT;
	write_unlock(&journal->j_state_lock);

	trace_jbd2_commit_flushing(
		journal, transaction);
	trace_jbd2_commit_logging(
		journal, transaction);

	stats.run.rs_logging = jiffies;
	stats.run.rs_flushing =
		jbd2_time_diff(
			stats.run.rs_flushing,
			stats.run.rs_logging);
	stats.run.rs_blocks =
		transaction->t_nr_buffers;

	error = ifs_jbd2_log_transaction_buffers(
		journal, transaction,
		&io_buffers, &log_buffers,
		&legacy_checksum);
	if (error)
		jbd2_journal_abort(journal, error);

	error = ifs_jbd2_finish_transaction_data(
		journal, transaction);
	if (error &&
	    (journal->j_flags &
	     JBD2_ABORT_ON_SYNCDATA_ERR))
		jbd2_journal_abort(journal, error);

	update_tail =
		jbd2_journal_get_log_tail(
			journal, &first_tid, &first_block);

	write_lock(&journal->j_state_lock);
	if (update_tail) {
		long freed =
			first_block - journal->j_tail;

		if (first_block < journal->j_tail)
			freed +=
				journal->j_last -
				journal->j_first;
		if (freed <
		    journal->j_max_transaction_buffers)
			update_tail = false;
	}
	transaction->t_state = T_COMMIT_DFLUSH;
	write_unlock(&journal->j_state_lock);

	if ((transaction->t_need_data_flush ||
	     update_tail) &&
	    journal->j_fs_dev != journal->j_dev &&
	    (journal->j_flags & JBD2_BARRIER))
		blkdev_issue_flush(journal->j_fs_dev);

	if (jbd2_has_feature_async_commit(journal)) {
		error = ifs_jbd2_submit_commit_record(
			journal, transaction,
			&commit_bh, legacy_checksum);
		if (error)
			jbd2_journal_abort(journal, error);
	}

	blk_finish_plug(&plug);

	error = ifs_jbd2_wait_logged_metadata(
		transaction, &io_buffers, &stats);
	if (error)
		jbd2_journal_abort(journal, error);

	error = ifs_jbd2_wait_control_buffers(
		&log_buffers, &stats);
	if (error)
		jbd2_journal_abort(journal, error);

	write_lock(&journal->j_state_lock);
	transaction->t_state = T_COMMIT_JFLUSH;
	write_unlock(&journal->j_state_lock);

	if (!jbd2_has_feature_async_commit(journal)) {
		error = ifs_jbd2_submit_commit_record(
			journal, transaction,
			&commit_bh, legacy_checksum);
		if (error)
			jbd2_journal_abort(journal, error);
	}

	if (commit_bh) {
		error =
			ifs_jbd2_wait_commit_record(
				commit_bh);
		if (error)
			jbd2_journal_abort(journal, error);
		stats.run.rs_blocks_logged++;
	}

	if (jbd2_has_feature_async_commit(journal) &&
	    (journal->j_flags & JBD2_BARRIER))
		blkdev_issue_flush(journal->j_dev);

	WARN_ON_ONCE(
		atomic_read(
			&transaction->t_outstanding_credits) < 0);

	if (update_tail)
		jbd2_update_log_tail(
			journal, first_tid, first_block);

	J_ASSERT(list_empty(&transaction->t_inode_list));
	J_ASSERT(!transaction->t_buffers);
	J_ASSERT(!transaction->t_checkpoint_list);
	J_ASSERT(!transaction->t_shadow_list);

	ifs_jbd2_finish_forget_list(
		journal, transaction);
	ifs_jbd2_publish_commit_complete(
		journal, transaction,
		&stats, start_time);
}


/* ===== recovery ===== */
/*
 * Filesystem Support EXT4 embedded journal recovery engine.
 *
 * Recovery scans the log for committed transactions, records revocations,
 * and replays surviving metadata blocks. Journal data is treated as
 * untrusted persistent input throughout the scan.
 */

#ifndef __KERNEL__
#include "jfs_user.h"
#else
#include <linux/blkdev.h>
#include <linux/crc32.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/slab.h>
#include <linux/string_choices.h>
#endif

struct recovery_info {
	tid_t start_transaction;
	tid_t end_transaction;
	unsigned long head_block;
	int nr_replays;
	int nr_revokes;
	int nr_revoke_hits;
};

#define IFS_JBD2_READAHEAD_BATCH 8

static unsigned long ifs_jbd2_wrap_log_block(
	journal_t *journal, unsigned long block)
{
	if (block >= journal->j_last)
		block -= journal->j_last - journal->j_first;
	return block;
}

#ifdef __KERNEL__
static void ifs_jbd2_release_buffers(
	struct buffer_head **buffers, int count)
{
	while (count > 0)
		brelse(buffers[--count]);
}

static int ifs_jbd2_readahead(
	journal_t *journal, unsigned int start)
{
	struct buffer_head *buffers[IFS_JBD2_READAHEAD_BATCH];
	unsigned int limit;
	unsigned int offset;
	int count = 0;
	int error = 0;

	limit = start + (128U * 1024U / journal->j_blocksize);
	if (limit > journal->j_total_len)
		limit = journal->j_total_len;

	for (offset = start; offset < limit; ++offset) {
		unsigned long long physical;
		struct buffer_head *bh;

		error = jbd2_journal_bmap(journal, offset, &physical);
		if (error)
			break;

		bh = __getblk(
			journal->j_dev, physical, journal->j_blocksize);
		if (!bh) {
			error = -ENOMEM;
			break;
		}

		if (buffer_uptodate(bh) || buffer_locked(bh)) {
			brelse(bh);
			continue;
		}

		buffers[count++] = bh;
		if (count == IFS_JBD2_READAHEAD_BATCH) {
			bh_readahead_batch(count, buffers, 0);
			ifs_jbd2_release_buffers(buffers, count);
			count = 0;
		}
	}

	if (count) {
		bh_readahead_batch(count, buffers, 0);
		ifs_jbd2_release_buffers(buffers, count);
	}

	return error;
}
#endif

static int ifs_jbd2_read_log_block(
	journal_t *journal,
	unsigned int offset,
	struct buffer_head **result)
{
	unsigned long long physical;
	struct buffer_head *bh;
	int error;

	*result = NULL;
	if (offset >= journal->j_total_len)
		return -EFSCORRUPTED;

	error = jbd2_journal_bmap(journal, offset, &physical);
	if (error)
		return error;

	bh = __getblk(
		journal->j_dev, physical, journal->j_blocksize);
	if (!bh)
		return -ENOMEM;

	if (!buffer_uptodate(bh)) {
#ifdef __KERNEL__
		bool readahead = !buffer_req(bh);
#endif
		bh_read_nowait(bh, 0);
#ifdef __KERNEL__
		if (readahead)
			ifs_jbd2_readahead(journal, offset);
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

static bool ifs_jbd2_block_checksum_valid(
	journal_t *journal, void *data)
{
	struct jbd2_journal_block_tail *tail;
	__be32 stored;
	__u32 calculated;

	if (!jbd2_journal_has_csum_v2or3(journal))
		return true;

	tail = (struct jbd2_journal_block_tail *)
		((char *)data + journal->j_blocksize -
		 sizeof(*tail));
	stored = tail->t_checksum;
	tail->t_checksum = 0;
	calculated = jbd2_chksum(
		journal, journal->j_csum_seed,
		data, journal->j_blocksize);
	tail->t_checksum = stored;
	return stored == cpu_to_be32(calculated);
}

static int ifs_jbd2_descriptor_tag_count(
	journal_t *journal, struct buffer_head *bh)
{
	const int tag_bytes = journal_tag_bytes(journal);
	int usable = journal->j_blocksize;
	char *cursor;
	int count = 0;

	if (jbd2_journal_has_csum_v2or3(journal))
		usable -= sizeof(struct jbd2_journal_block_tail);

	cursor = bh->b_data + sizeof(journal_header_t);
	while (cursor - bh->b_data + tag_bytes <= usable) {
		journal_block_tag_t tag;

		memcpy(&tag, cursor, sizeof(tag));
		count++;
		cursor += tag_bytes;
		if (!(tag.t_flags & cpu_to_be16(JBD2_FLAG_SAME_UUID)))
			cursor += 16;
		if (tag.t_flags & cpu_to_be16(JBD2_FLAG_LAST_TAG))
			break;
	}

	return count;
}

static unsigned long long ifs_jbd2_tag_block(
	journal_t *journal, const journal_block_tag_t *tag)
{
	unsigned long long block = be32_to_cpu(tag->t_blocknr);

	if (jbd2_has_feature_64bit(journal))
		block |=
			(u64)be32_to_cpu(tag->t_blocknr_high) << 32;
	return block;
}

static bool ifs_jbd2_tag_checksum_valid(
	journal_t *journal,
	journal_block_tag_t *tag,
	journal_block_tag3_t *tag3,
	const void *data,
	__u32 sequence)
{
	__be32 sequence_be;
	__u32 checksum;

	if (!jbd2_journal_has_csum_v2or3(journal))
		return true;

	sequence_be = cpu_to_be32(sequence);
	checksum = jbd2_chksum(
		journal, journal->j_csum_seed,
		(__u8 *)&sequence_be, sizeof(sequence_be));
	checksum = jbd2_chksum(
		journal, checksum, data, journal->j_blocksize);

	if (jbd2_has_feature_csum3(journal))
		return tag3->t_checksum == cpu_to_be32(checksum);
	return tag->t_checksum == cpu_to_be16(checksum);
}

static bool ifs_jbd2_commit_checksum_valid(
	journal_t *journal, void *data)
{
	struct commit_header *header;
	__be32 stored;
	__u32 calculated;

	if (!jbd2_journal_has_csum_v2or3(journal))
		return true;

	header = data;
	stored = header->h_chksum[0];
	header->h_chksum[0] = 0;
	calculated = jbd2_chksum(
		journal, journal->j_csum_seed,
		data, journal->j_blocksize);
	header->h_chksum[0] = stored;
	return stored == cpu_to_be32(calculated);
}

static bool ifs_jbd2_partial_commit_checksum_valid(
	journal_t *journal, const void *data)
{
	struct commit_header *header;
	void *scratch;
	__be32 stored;
	__u32 calculated;

	scratch = kzalloc(journal->j_blocksize, GFP_KERNEL);
	if (!scratch)
		return false;

	memcpy(scratch, data, sizeof(struct commit_header));
	header = scratch;
	stored = header->h_chksum[0];
	header->h_chksum[0] = 0;
	calculated = jbd2_chksum(
		journal, journal->j_csum_seed,
		scratch, journal->j_blocksize);
	kfree(scratch);
	return stored == cpu_to_be32(calculated);
}

static int ifs_jbd2_accumulate_legacy_checksum(
	journal_t *journal,
	struct buffer_head *descriptor,
	unsigned long *next_log_block,
	__u32 *checksum)
{
	int count = ifs_jbd2_descriptor_tag_count(
		journal, descriptor);
	int index;

	*checksum = crc32_be(
		*checksum, descriptor->b_data, descriptor->b_size);

	for (index = 0; index < count; ++index) {
		struct buffer_head *data;
		unsigned long block = *next_log_block;
		int error;

		*next_log_block = ifs_jbd2_wrap_log_block(
			journal, block + 1U);
		error = ifs_jbd2_read_log_block(
			journal, block, &data);
		if (error)
			return error;

		*checksum = crc32_be(
			*checksum, data->b_data, data->b_size);
		brelse(data);
	}

	return 0;
}

static int ifs_jbd2_scan_revoke_block(
	journal_t *journal,
	struct buffer_head *bh,
	tid_t sequence,
	struct recovery_info *info)
{
	jbd2_journal_revoke_header_t *header;
	unsigned int checksum_bytes = 0;
	unsigned int offset;
	unsigned int record_bytes;
	__u32 end;

	if (jbd2_journal_has_csum_v2or3(journal))
		checksum_bytes =
			sizeof(struct jbd2_journal_block_tail);

	header = (jbd2_journal_revoke_header_t *)bh->b_data;
	end = be32_to_cpu(header->r_count);
	if (end > journal->j_blocksize - checksum_bytes)
		return -EINVAL;

	record_bytes =
		jbd2_has_feature_64bit(journal) ? 8U : 4U;
	offset = sizeof(*header);

	while (offset + record_bytes <= end) {
		unsigned long long block;
		int error;

		if (record_bytes == 8U)
			block = be64_to_cpu(
				*(__be64 *)(bh->b_data + offset));
		else
			block = be32_to_cpu(
				*(__be32 *)(bh->b_data + offset));

		error = jbd2_journal_set_revoke(
			journal, block, sequence);
		if (error)
			return error;

		info->nr_revokes++;
		offset += record_bytes;
	}

	return 0;
}

static int ifs_jbd2_replay_descriptor(
	journal_t *journal,
	struct recovery_info *info,
	struct buffer_head *descriptor,
	unsigned long *next_log_block,
	__u32 sequence)
{
	int checksum_tail = jbd2_journal_has_csum_v2or3(journal) ?
		sizeof(struct jbd2_journal_block_tail) : 0;
	const int tag_bytes = journal_tag_bytes(journal);
	char *cursor =
		descriptor->b_data + sizeof(journal_header_t);
	char *limit =
		descriptor->b_data + journal->j_blocksize -
		checksum_tail;
	int status = 0;

	while (cursor + tag_bytes <= limit) {
		journal_block_tag_t tag;
		journal_block_tag3_t *tag3 =
			(journal_block_tag3_t *)cursor;
		struct buffer_head *logged = NULL;
		struct buffer_head *home = NULL;
		unsigned long long home_block;
		unsigned long log_block;
		int flags;
		int error;

		memcpy(&tag, cursor, sizeof(tag));
		flags = be16_to_cpu(tag.t_flags);
		home_block = ifs_jbd2_tag_block(journal, &tag);

		log_block = *next_log_block;
		*next_log_block = ifs_jbd2_wrap_log_block(
			journal, log_block + 1U);

		error = ifs_jbd2_read_log_block(
			journal, log_block, &logged);
		if (error) {
			status = error;
			goto next_tag;
		}

		if (jbd2_journal_test_revoke(
			    journal, home_block, sequence)) {
			info->nr_revoke_hits++;
			goto next_tag;
		}

		if (!ifs_jbd2_tag_checksum_valid(
			    journal, &tag, tag3,
			    logged->b_data, sequence)) {
			status = -EFSBADCRC;
			goto next_tag;
		}

		home = __getblk(
			journal->j_fs_dev, home_block,
			journal->j_blocksize);
		if (!home) {
			status = -ENOMEM;
			goto next_tag;
		}

		lock_buffer(home);
		memcpy(
			home->b_data, logged->b_data,
			journal->j_blocksize);
		if (flags & JBD2_FLAG_ESCAPE)
			*(__be32 *)home->b_data =
				cpu_to_be32(JBD2_MAGIC_NUMBER);
		set_buffer_uptodate(home);
		mark_buffer_dirty(home);
		unlock_buffer(home);
		info->nr_replays++;

next_tag:
		brelse(home);
		brelse(logged);

		cursor += tag_bytes;
		if (!(flags & JBD2_FLAG_SAME_UUID))
			cursor += 16;
		if (flags & JBD2_FLAG_LAST_TAG)
			break;
	}

	return status;
}

static int ifs_jbd2_fast_commit_pass(
	journal_t *journal,
	struct recovery_info *info,
	enum passtype pass)
{
	unsigned long block;
	const unsigned int expected_commit =
		info->end_transaction;

	if (!journal->j_fc_replay_callback)
		return 0;

	for (block = journal->j_fc_first;
	     block <= journal->j_fc_last; ++block) {
		struct buffer_head *bh;
		int error;

		error = ifs_jbd2_read_log_block(
			journal, block, &bh);
		if (error)
			return error;

		error = journal->j_fc_replay_callback(
			journal, bh, pass,
			block - journal->j_fc_first,
			expected_commit);
		brelse(bh);

		if (error < 0)
			return error;
		if (error == JBD2_FC_REPLAY_STOP)
			return 0;
	}

	return 0;
}

static int ifs_jbd2_recovery_pass(
	journal_t *journal,
	struct recovery_info *info,
	enum passtype pass)
{
	journal_superblock_t *super = journal->j_superblock;
	unsigned int transaction =
		be32_to_cpu(super->s_sequence);
	const unsigned int first_transaction = transaction;
	unsigned long next =
		be32_to_cpu(super->s_start);
	unsigned long head = next;
	__u32 legacy_checksum = ~0U;
	__u64 last_commit_time = 0;
	bool descriptor_checksum_failed = false;
	int status = 0;
	int replay_error = 0;

	if (pass == PASS_SCAN)
		info->start_transaction = first_transaction;

	for (;;) {
		struct buffer_head *bh;
		journal_header_t *header;
		unsigned int sequence;
		unsigned int type;
		int error;

		cond_resched();
		if (pass != PASS_SCAN &&
		    tid_geq(transaction, info->end_transaction))
			break;

		error = ifs_jbd2_read_log_block(
			journal, next, &bh);
		if (error)
			return error;

		next = ifs_jbd2_wrap_log_block(
			journal, next + 1U);
		header = (journal_header_t *)bh->b_data;

		if (header->h_magic !=
		    cpu_to_be32(JBD2_MAGIC_NUMBER)) {
			brelse(bh);
			break;
		}

		type = be32_to_cpu(header->h_blocktype);
		sequence = be32_to_cpu(header->h_sequence);
		if (sequence != transaction) {
			brelse(bh);
			break;
		}

		if (type == JBD2_DESCRIPTOR_BLOCK) {
			const bool checksum_ok =
				ifs_jbd2_block_checksum_valid(
					journal, bh->b_data);

			if (!checksum_ok) {
				if (pass != PASS_SCAN) {
					brelse(bh);
					return -EFSBADCRC;
				}
				descriptor_checksum_failed = true;
			}

			if (pass == PASS_REPLAY) {
				error = ifs_jbd2_replay_descriptor(
					journal, info, bh, &next,
					sequence);
				if (error && !replay_error)
					replay_error = error;
				brelse(bh);
				continue;
			}

			if (pass == PASS_SCAN &&
			    jbd2_has_feature_checksum(journal) &&
			    !descriptor_checksum_failed &&
			    !info->end_transaction) {
				error =
					ifs_jbd2_accumulate_legacy_checksum(
						journal, bh, &next,
						&legacy_checksum);
				brelse(bh);
				if (error)
					return error;
				continue;
			}

			next = ifs_jbd2_wrap_log_block(
				journal,
				next +
				ifs_jbd2_descriptor_tag_count(
					journal, bh));
			brelse(bh);
			continue;
		}

		if (type == JBD2_REVOKE_BLOCK) {
			if (pass == PASS_SCAN &&
			    !ifs_jbd2_block_checksum_valid(
				    journal, bh->b_data))
				descriptor_checksum_failed = true;

			if (pass == PASS_REVOKE) {
				error = ifs_jbd2_scan_revoke_block(
					journal, bh, transaction,
					info);
				brelse(bh);
				if (error)
					return error;
			} else {
				brelse(bh);
			}
			continue;
		}

		if (type == JBD2_COMMIT_BLOCK) {
			struct commit_header *commit =
				(struct commit_header *)bh->b_data;
			const __u64 commit_time =
				be64_to_cpu(commit->h_commit_sec);
			bool valid = true;

			if (descriptor_checksum_failed) {
				if (commit_time >= last_commit_time) {
					brelse(bh);
					return -EFSBADCRC;
				}
				brelse(bh);
				break;
			}

			if (pass == PASS_SCAN &&
			    jbd2_has_feature_checksum(journal)) {
				const unsigned found =
					be32_to_cpu(
						commit->h_chksum[0]);
				const bool legacy_ok =
					(legacy_checksum == found &&
					 commit->h_chksum_type ==
						JBD2_CRC32_CHKSUM &&
					 commit->h_chksum_size ==
						JBD2_CRC32_CHKSUM_SIZE) ||
					(commit->h_chksum_type == 0 &&
					 commit->h_chksum_size == 0 &&
					 found == 0);

				if (info->end_transaction)
					valid = false;
				else if (!legacy_ok)
					valid = false;
				legacy_checksum = ~0U;
			}

			if (pass == PASS_SCAN &&
			    valid &&
			    !ifs_jbd2_commit_checksum_valid(
				    journal, bh->b_data)) {
				if (!ifs_jbd2_partial_commit_checksum_valid(
					    journal, bh->b_data))
					valid = false;
			}

			if (!valid) {
				if (commit_time < last_commit_time) {
					brelse(bh);
					break;
				}

				info->end_transaction = transaction;
				info->head_block = head;
				if (!jbd2_has_feature_async_commit(
					    journal)) {
					journal->j_failed_commit =
						transaction;
					brelse(bh);
					break;
				}
			}

			if (pass == PASS_SCAN) {
				last_commit_time = commit_time;
				head = next;
			}

			brelse(bh);
			transaction++;
			continue;
		}

		brelse(bh);
		break;
	}

	if (pass == PASS_SCAN) {
		if (!info->end_transaction)
			info->end_transaction = transaction;
		if (!info->head_block)
			info->head_block = head;
	} else if (info->end_transaction != transaction &&
		   !replay_error) {
		replay_error = -EIO;
	}

	if (jbd2_has_feature_fast_commit(journal) &&
	    pass != PASS_REVOKE) {
		int error = ifs_jbd2_fast_commit_pass(
			journal, info, pass);
		if (error)
			replay_error = error;
	}

	if (status)
		return status;
	return replay_error;
}

int jbd2_journal_recover(journal_t *journal)
{
	struct recovery_info info;
	int error;
	int secondary;

	memset(&info, 0, sizeof(info));

	if (!journal->j_tail) {
		journal_superblock_t *super =
			journal->j_superblock;

		journal->j_transaction_sequence =
			be32_to_cpu(super->s_sequence) + 1U;
		journal->j_head =
			be32_to_cpu(super->s_head);
		return 0;
	}

	error = ifs_jbd2_recovery_pass(
		journal, &info, PASS_SCAN);
	if (!error)
		error = ifs_jbd2_recovery_pass(
			journal, &info, PASS_REVOKE);
	if (!error)
		error = ifs_jbd2_recovery_pass(
			journal, &info, PASS_REPLAY);

	journal->j_transaction_sequence =
		info.end_transaction + 1U;
	journal->j_head = info.head_block;
	jbd2_journal_clear_revoke(journal);

	secondary = sync_blockdev(journal->j_fs_dev);
	if (!error)
		error = secondary;
	secondary = jbd2_check_fs_dev_write_error(journal);
	if (!error)
		error = secondary;

	if (journal->j_flags & JBD2_BARRIER) {
		secondary =
			blkdev_issue_flush(journal->j_fs_dev);
		if (!error)
			error = secondary;
	}

	return error;
}

int jbd2_journal_skip_recovery(journal_t *journal)
{
	struct recovery_info info;
	int error;

	memset(&info, 0, sizeof(info));
	error = ifs_jbd2_recovery_pass(
		journal, &info, PASS_SCAN);

	if (error) {
		journal->j_transaction_sequence++;
		journal->j_head = journal->j_first;
	} else {
		journal->j_transaction_sequence =
			info.end_transaction + 1U;
		journal->j_head = info.head_block;
	}

	journal->j_tail = 0;
	return error;
}


/* ===== revoke ===== */
/*
 * Filesystem Support EXT4 embedded journal revoke engine.
 *
 * Revoke records prevent stale journal images from being replayed over newer
 * block contents. One table belongs to the running transaction and the other
 * to the committing transaction.
 */

#ifndef __KERNEL__
#include "jfs_user.h"
#else
#include <linux/bio.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/init.h>
#include <linux/jbd2.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/slab.h>
#endif

static struct kmem_cache *ifs_jbd2_revoke_record_cache;
static struct kmem_cache *ifs_jbd2_revoke_table_cache;

struct jbd2_revoke_record_s {
	struct list_head hash;
	tid_t sequence;
	unsigned long long blocknr;
};

struct jbd2_revoke_table_s {
	int hash_size;
	int hash_shift;
	struct list_head *hash_table;
};

static unsigned int ifs_jbd2_revoke_bucket(
	const journal_t *journal,
	const unsigned long long block)
{
	return hash_64(block, journal->j_revoke->hash_shift);
}

static struct jbd2_revoke_record_s *
ifs_jbd2_find_revoke_record(
	journal_t *journal,
	unsigned long long blocknr)
{
	struct list_head *bucket;
	struct jbd2_revoke_record_s *record;

	bucket = &journal->j_revoke->hash_table[
		ifs_jbd2_revoke_bucket(journal, blocknr)];

	spin_lock(&journal->j_revoke_lock);
	list_for_each_entry(record, bucket, hash) {
		if (record->blocknr == blocknr) {
			spin_unlock(&journal->j_revoke_lock);
			return record;
		}
	}
	spin_unlock(&journal->j_revoke_lock);
	return NULL;
}

static int ifs_jbd2_insert_revoke_record(
	journal_t *journal,
	unsigned long long blocknr,
	tid_t sequence)
{
	struct jbd2_revoke_record_s *record;
	struct list_head *bucket;
	gfp_t flags = GFP_NOFS;

	if (journal_oom_retry)
		flags |= __GFP_NOFAIL;

	record = kmem_cache_alloc(
		ifs_jbd2_revoke_record_cache, flags);
	if (!record)
		return -ENOMEM;

	record->blocknr = blocknr;
	record->sequence = sequence;
	bucket = &journal->j_revoke->hash_table[
		ifs_jbd2_revoke_bucket(journal, blocknr)];

	spin_lock(&journal->j_revoke_lock);
	list_add(&record->hash, bucket);
	spin_unlock(&journal->j_revoke_lock);
	return 0;
}

static struct jbd2_revoke_table_s *
ifs_jbd2_alloc_revoke_table(int hash_size)
{
	struct jbd2_revoke_table_s *table;
	int index;

	table = kmem_cache_alloc(
		ifs_jbd2_revoke_table_cache, GFP_KERNEL);
	if (!table)
		return NULL;

	table->hash_size = hash_size;
	table->hash_shift = ilog2(hash_size);
	table->hash_table = kmalloc_array(
		hash_size, sizeof(*table->hash_table), GFP_KERNEL);
	if (!table->hash_table) {
		kmem_cache_free(
			ifs_jbd2_revoke_table_cache, table);
		return NULL;
	}

	for (index = 0; index < hash_size; ++index)
		INIT_LIST_HEAD(&table->hash_table[index]);

	return table;
}

static void ifs_jbd2_free_revoke_table(
	struct jbd2_revoke_table_s *table)
{
	int index;

	if (!table)
		return;

	for (index = 0; index < table->hash_size; ++index)
		J_ASSERT(list_empty(&table->hash_table[index]));

	kfree(table->hash_table);
	kmem_cache_free(
		ifs_jbd2_revoke_table_cache, table);
}

void jbd2_journal_destroy_revoke_record_cache(void)
{
	kmem_cache_destroy(
		ifs_jbd2_revoke_record_cache);
	ifs_jbd2_revoke_record_cache = NULL;
}

void jbd2_journal_destroy_revoke_table_cache(void)
{
	kmem_cache_destroy(
		ifs_jbd2_revoke_table_cache);
	ifs_jbd2_revoke_table_cache = NULL;
}

int __init jbd2_journal_init_revoke_record_cache(void)
{
	J_ASSERT(!ifs_jbd2_revoke_record_cache);

	ifs_jbd2_revoke_record_cache = KMEM_CACHE(
		jbd2_revoke_record_s,
		SLAB_HWCACHE_ALIGN | SLAB_TEMPORARY);
	return ifs_jbd2_revoke_record_cache ? 0 : -ENOMEM;
}

int __init jbd2_journal_init_revoke_table_cache(void)
{
	J_ASSERT(!ifs_jbd2_revoke_table_cache);

	ifs_jbd2_revoke_table_cache = KMEM_CACHE(
		jbd2_revoke_table_s, SLAB_TEMPORARY);
	return ifs_jbd2_revoke_table_cache ? 0 : -ENOMEM;
}

int jbd2_journal_init_revoke(
	journal_t *journal, int hash_size)
{
	J_ASSERT(!journal->j_revoke_table[0]);
	J_ASSERT(is_power_of_2(hash_size));

	journal->j_revoke_table[0] =
		ifs_jbd2_alloc_revoke_table(hash_size);
	if (!journal->j_revoke_table[0])
		return -ENOMEM;

	journal->j_revoke_table[1] =
		ifs_jbd2_alloc_revoke_table(hash_size);
	if (!journal->j_revoke_table[1]) {
		ifs_jbd2_free_revoke_table(
			journal->j_revoke_table[0]);
		journal->j_revoke_table[0] = NULL;
		return -ENOMEM;
	}

	journal->j_revoke = journal->j_revoke_table[1];
	spin_lock_init(&journal->j_revoke_lock);
	return 0;
}

void jbd2_journal_destroy_revoke(journal_t *journal)
{
	journal->j_revoke = NULL;
	ifs_jbd2_free_revoke_table(
		journal->j_revoke_table[0]);
	ifs_jbd2_free_revoke_table(
		journal->j_revoke_table[1]);
	journal->j_revoke_table[0] = NULL;
	journal->j_revoke_table[1] = NULL;
}

#ifdef __KERNEL__

int jbd2_journal_revoke(
	handle_t *handle,
	unsigned long long blocknr,
	struct buffer_head *bh_in)
{
	transaction_t *transaction = handle->h_transaction;
	journal_t *journal = transaction->t_journal;
	struct buffer_head *bh = bh_in;
	int error;

	might_sleep();

	if (!jbd2_journal_set_features(
		    journal, 0, 0,
		    JBD2_FEATURE_INCOMPAT_REVOKE))
		return -EINVAL;

	if (WARN_ON_ONCE(handle->h_revoke_credits <= 0))
		return -EIO;

	if (!bh)
		bh = __find_get_block_nonatomic(
			journal->j_fs_dev, blocknr,
			journal->j_blocksize);

	if (bh) {
		if (!J_EXPECT_BH(
			    bh, !buffer_revoked(bh),
			    "inconsistent data on disk")) {
			if (!bh_in)
				brelse(bh);
			return -EIO;
		}

		set_buffer_revoked(bh);
		set_buffer_revokevalid(bh);

		if (bh_in)
			jbd2_journal_forget(handle, bh_in);
		else
			__brelse(bh);
	}

	handle->h_revoke_credits--;
	error = ifs_jbd2_insert_revoke_record(
		journal, blocknr, transaction->t_tid);
	return error;
}

int jbd2_journal_cancel_revoke(
	handle_t *handle,
	struct journal_head *jh)
{
	journal_t *journal =
		handle->h_transaction->t_journal;
	struct buffer_head *bh = jh2bh(jh);
	struct jbd2_revoke_record_s *record;
	bool cancel;

	if (test_set_buffer_revokevalid(bh))
		cancel = test_clear_buffer_revoked(bh);
	else {
		clear_buffer_revoked(bh);
		cancel = true;
	}

	if (!cancel)
		return 0;

	record = ifs_jbd2_find_revoke_record(
		journal, bh->b_blocknr);
	if (!record)
		return 0;

	spin_lock(&journal->j_revoke_lock);
	list_del(&record->hash);
	spin_unlock(&journal->j_revoke_lock);
	kmem_cache_free(
		ifs_jbd2_revoke_record_cache, record);

	if (bh->b_folio &&
	    bh->b_folio->mapping &&
	    bh->b_folio->mapping->host &&
	    !sb_is_blkdev_sb(
		    bh->b_folio->mapping->host->i_sb)) {
		struct buffer_head *alias;

		alias = __find_get_block_nonatomic(
			bh->b_bdev, bh->b_blocknr, bh->b_size);
		if (alias) {
			if (alias != bh)
				clear_buffer_revoked(alias);
			__brelse(alias);
		}
	}

	return 1;
}

void jbd2_clear_buffer_revoked_flags(
	journal_t *journal)
{
	struct jbd2_revoke_table_s *table =
		journal->j_revoke;
	int index;

	for (index = 0; index < table->hash_size; ++index) {
		struct jbd2_revoke_record_s *record;

		list_for_each_entry(
			record, &table->hash_table[index], hash) {
			struct buffer_head *bh =
				__find_get_block_nonatomic(
					journal->j_fs_dev,
					record->blocknr,
					journal->j_blocksize);

			if (!bh)
				continue;
			clear_buffer_revoked(bh);
			__brelse(bh);
		}
	}
}

void jbd2_journal_switch_revoke_table(
	journal_t *journal)
{
	struct jbd2_revoke_table_s *next;
	int index;

	next = journal->j_revoke ==
		journal->j_revoke_table[0] ?
		journal->j_revoke_table[1] :
		journal->j_revoke_table[0];

	journal->j_revoke = next;
	for (index = 0; index < next->hash_size; ++index)
		INIT_LIST_HEAD(&next->hash_table[index]);
}

static void ifs_jbd2_flush_revoke_descriptor(
	journal_t *journal,
	struct buffer_head *descriptor,
	int used)
{
	jbd2_journal_revoke_header_t *header;

	if (!descriptor ||
	    is_journal_aborted(journal))
		return;

	header =
		(jbd2_journal_revoke_header_t *)
		descriptor->b_data;
	header->r_count = cpu_to_be32(used);
	jbd2_descriptor_block_csum_set(
		journal, descriptor);

	set_buffer_jwrite(descriptor);
	set_buffer_dirty(descriptor);
	write_dirty_buffer(
		descriptor, JBD2_JOURNAL_REQ_FLAGS);
}

static bool ifs_jbd2_append_revoke_record(
	transaction_t *transaction,
	struct list_head *log_bufs,
	struct buffer_head **descriptor,
	int *offset,
	const struct jbd2_revoke_record_s *record)
{
	journal_t *journal = transaction->t_journal;
	const int checksum_bytes =
		jbd2_journal_has_csum_v2or3(journal) ?
		sizeof(struct jbd2_journal_block_tail) : 0;
	const int record_bytes =
		jbd2_has_feature_64bit(journal) ? 8 : 4;

	if (is_journal_aborted(journal))
		return false;

	if (*descriptor &&
	    *offset + record_bytes >
		    journal->j_blocksize - checksum_bytes) {
		ifs_jbd2_flush_revoke_descriptor(
			journal, *descriptor, *offset);
		*descriptor = NULL;
	}

	if (!*descriptor) {
		*descriptor =
			jbd2_journal_get_descriptor_buffer(
				transaction, JBD2_REVOKE_BLOCK);
		if (!*descriptor)
			return false;

		jbd2_file_log_bh(log_bufs, *descriptor);
		*offset =
			sizeof(jbd2_journal_revoke_header_t);
	}

	if (record_bytes == 8)
		*(__be64 *)(&(*descriptor)->b_data[*offset]) =
			cpu_to_be64(record->blocknr);
	else
		*(__be32 *)(&(*descriptor)->b_data[*offset]) =
			cpu_to_be32(record->blocknr);

	*offset += record_bytes;
	return true;
}

void jbd2_journal_write_revoke_records(
	transaction_t *transaction,
	struct list_head *log_bufs)
{
	journal_t *journal = transaction->t_journal;
	struct jbd2_revoke_table_s *table;
	struct buffer_head *descriptor = NULL;
	int offset = 0;
	int bucket;

	table = journal->j_revoke ==
		journal->j_revoke_table[0] ?
		journal->j_revoke_table[1] :
		journal->j_revoke_table[0];

	for (bucket = 0; bucket < table->hash_size; ++bucket) {
		struct list_head *head =
			&table->hash_table[bucket];

		while (!list_empty(head)) {
			struct jbd2_revoke_record_s *record =
				list_first_entry(
					head,
					struct jbd2_revoke_record_s,
					hash);

			ifs_jbd2_append_revoke_record(
				transaction, log_bufs,
				&descriptor, &offset, record);

			list_del(&record->hash);
			kmem_cache_free(
				ifs_jbd2_revoke_record_cache,
				record);
		}
	}

	ifs_jbd2_flush_revoke_descriptor(
		journal, descriptor, offset);
}

#endif

int jbd2_journal_set_revoke(
	journal_t *journal,
	unsigned long long blocknr,
	tid_t sequence)
{
	struct jbd2_revoke_record_s *record;

	record = ifs_jbd2_find_revoke_record(
		journal, blocknr);
	if (record) {
		if (tid_gt(sequence, record->sequence))
			record->sequence = sequence;
		return 0;
	}

	return ifs_jbd2_insert_revoke_record(
		journal, blocknr, sequence);
}

int jbd2_journal_test_revoke(
	journal_t *journal,
	unsigned long long blocknr,
	tid_t sequence)
{
	struct jbd2_revoke_record_s *record;

	record = ifs_jbd2_find_revoke_record(
		journal, blocknr);
	if (!record)
		return 0;

	return !tid_gt(sequence, record->sequence);
}

void jbd2_journal_clear_revoke(journal_t *journal)
{
	struct jbd2_revoke_table_s *table =
		journal->j_revoke;
	int bucket;

	for (bucket = 0; bucket < table->hash_size; ++bucket) {
		struct list_head *head =
			&table->hash_table[bucket];

		while (!list_empty(head)) {
			struct jbd2_revoke_record_s *record =
				list_first_entry(
					head,
					struct jbd2_revoke_record_s,
					hash);

			list_del(&record->hash);
			kmem_cache_free(
				ifs_jbd2_revoke_record_cache,
				record);
		}
	}
}

