// SPDX-License-Identifier: GPL-2.0+
/*
 * linux/fs/jbd2/journal.c
 *
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1998
 *
 * Copyright 1998 Red Hat corp --- All Rights Reserved
 *
 * Generic filesystem journal-writing code; part of the ext2fs
 * journaling system.
 *
 * This file manages journals: areas of disk reserved for logging
 * transactional updates.  This includes the kernel journaling thread
 * which is responsible for scheduling updates to the log.
 *
 * We do not actually manage the physical storage of the journal in this
 * file: that is left to a per-journal policy function, which allows us
 * to store the journal within a filesystem-specified area for ext2
 * journaling (ext2 can use a reserved inode for storing the log).
 */

/*
 * EXT4 — JBD2 journal core
 *
 * Purpose:
 *   Owns journal lifecycle, log-space accounting, commit-thread coordination and internal journal caches embedded in ext4.ko.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Head/tail arithmetic, sequence numbers and abort state must remain coherent across wraparound, I/O failure and recovery.
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

#include <linux/module.h>
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/freezer.h>
#include <linux/pagemap.h>
#include <linux/kthread.h>
#include <linux/poison.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/math64.h>
#include <linux/hash.h>
#include <linux/log2.h>
#include <linux/vmalloc.h>
#include <linux/backing-dev.h>
#include <linux/bitops.h>
#include <linux/ratelimit.h>
#include <linux/sched/mm.h>

#define CREATE_TRACE_POINTS
#include <trace/events/jbd2.h>

#include <linux/uaccess.h>
#include <asm/page.h>

#ifdef CONFIG_JBD2_DEBUG
static ushort jbd2_journal_enable_debug __read_mostly;

module_param_named(jbd2_debug, jbd2_journal_enable_debug, ushort, 0644);
MODULE_PARM_DESC(jbd2_debug, "Debugging level for jbd2");
#endif


static int jbd2_journal_create_slab(size_t slab_size);

#ifdef CONFIG_JBD2_DEBUG


/**
 * __jbd2_debug - Implements the debug operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void __jbd2_debug(int level, const char *file, const char *func,
		  unsigned int line, const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;

	if (level > jbd2_journal_enable_debug)
		return;
	va_start(args, fmt);
	vaf.fmt = fmt;
	vaf.va = &args;
	printk(KERN_DEBUG "%s: (%s, %u): %pV", file, func, line, &vaf);
	va_end(args);
}
#endif


/**
 * jbd2_superblock_csum - Implements the superblock csum operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static __be32 jbd2_superblock_csum(journal_t *j, journal_superblock_t *sb)
{
	__u32 csum;
	__be32 old_csum;

	old_csum = sb->s_checksum;
	sb->s_checksum = 0;
	csum = jbd2_chksum(j, ~0, (char *)sb, sizeof(journal_superblock_t));
	sb->s_checksum = old_csum;

	return cpu_to_be32(csum);
}


/**
 * commit_timeout - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void commit_timeout(struct timer_list *t)
{
	journal_t *journal = from_timer(journal, t, j_commit_timer);

	wake_up_process(journal->j_task);
}


/**
 * kjournald2 - Implements the kjournald2 operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int kjournald2(void *arg)
{
	journal_t *journal = arg;
	transaction_t *transaction;


	timer_setup(&journal->j_commit_timer, commit_timeout, 0);

	set_freezable();


	journal->j_task = current;
	wake_up(&journal->j_wait_done_commit);


	memalloc_nofs_save();


	write_lock(&journal->j_state_lock);

loop:
	if (journal->j_flags & JBD2_UNMOUNT)
		goto end_loop;

	jbd2_debug(1, "commit_sequence=%u, commit_request=%u\n",
		journal->j_commit_sequence, journal->j_commit_request);

	if (journal->j_commit_sequence != journal->j_commit_request) {
		jbd2_debug(1, "OK, requests differ\n");
		write_unlock(&journal->j_state_lock);
		del_timer_sync(&journal->j_commit_timer);
		jbd2_journal_commit_transaction(journal);
		write_lock(&journal->j_state_lock);
		goto loop;
	}

	wake_up(&journal->j_wait_done_commit);
	if (freezing(current)) {


		jbd2_debug(1, "Now suspending kjournald2\n");
		write_unlock(&journal->j_state_lock);
		try_to_freeze();
		write_lock(&journal->j_state_lock);
	} else {


		DEFINE_WAIT(wait);

		prepare_to_wait(&journal->j_wait_commit, &wait,
				TASK_INTERRUPTIBLE);
		transaction = journal->j_running_transaction;
		if (transaction == NULL ||
		    time_before(jiffies, transaction->t_expires)) {
			write_unlock(&journal->j_state_lock);
			schedule();
			write_lock(&journal->j_state_lock);
		}
		finish_wait(&journal->j_wait_commit, &wait);
	}

	jbd2_debug(1, "kjournald2 wakes\n");


	transaction = journal->j_running_transaction;
	if (transaction && time_after_eq(jiffies, transaction->t_expires)) {
		journal->j_commit_request = transaction->t_tid;
		jbd2_debug(1, "woke because of timeout\n");
	}
	goto loop;

end_loop:
	del_timer_sync(&journal->j_commit_timer);
	journal->j_task = NULL;
	wake_up(&journal->j_wait_done_commit);
	jbd2_debug(1, "Journal thread exiting.\n");
	write_unlock(&journal->j_state_lock);
	return 0;
}


/**
 * jbd2_journal_start_thread - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int jbd2_journal_start_thread(journal_t *journal)
{
	struct task_struct *t;

	t = kthread_run(kjournald2, journal, "jbd2/%s",
			journal->j_devname);
	if (IS_ERR(t))
		return PTR_ERR(t);

	wait_event(journal->j_wait_done_commit, journal->j_task != NULL);
	return 0;
}


/**
 * journal_kill_thread - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void journal_kill_thread(journal_t *journal)
{
	write_lock(&journal->j_state_lock);
	journal->j_flags |= JBD2_UNMOUNT;

	while (journal->j_task) {
		write_unlock(&journal->j_state_lock);
		wake_up(&journal->j_wait_commit);
		wait_event(journal->j_wait_done_commit, journal->j_task == NULL);
		write_lock(&journal->j_state_lock);
	}
	write_unlock(&journal->j_state_lock);
}


/**
 * jbd2_data_needs_escaping - Implements the data needs escaping operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline bool jbd2_data_needs_escaping(char *data)
{
	return *((__be32 *)data) == cpu_to_be32(JBD2_MAGIC_NUMBER);
}


/**
 * jbd2_data_do_escape - Implements the data do escape operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void jbd2_data_do_escape(char *data)
{
	*((unsigned int *)data) = 0;
}


/**
 * jbd2_journal_write_metadata_buffer - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_write_metadata_buffer(transaction_t *transaction,
				  struct journal_head  *jh_in,
				  struct buffer_head **bh_out,
				  sector_t blocknr)
{
	int do_escape = 0;
	struct buffer_head *new_bh;
	struct folio *new_folio;
	unsigned int new_offset;
	struct buffer_head *bh_in = jh2bh(jh_in);
	journal_t *journal = transaction->t_journal;


	J_ASSERT_BH(bh_in, buffer_jbddirty(bh_in));

	new_bh = alloc_buffer_head(GFP_NOFS|__GFP_NOFAIL);


	atomic_set(&new_bh->b_count, 1);

	spin_lock(&jh_in->b_state_lock);


	if (jh_in->b_frozen_data) {
		new_folio = virt_to_folio(jh_in->b_frozen_data);
		new_offset = offset_in_folio(new_folio, jh_in->b_frozen_data);
		do_escape = jbd2_data_needs_escaping(jh_in->b_frozen_data);
		if (do_escape)
			jbd2_data_do_escape(jh_in->b_frozen_data);
	} else {
		char *tmp;
		char *mapped_data;

		new_folio = bh_in->b_folio;
		new_offset = offset_in_folio(new_folio, bh_in->b_data);
		mapped_data = kmap_local_folio(new_folio, new_offset);


		jbd2_buffer_frozen_trigger(jh_in, mapped_data,
					   jh_in->b_triggers);
		do_escape = jbd2_data_needs_escaping(mapped_data);
		kunmap_local(mapped_data);


		if (!do_escape)
			goto escape_done;

		spin_unlock(&jh_in->b_state_lock);
		tmp = jbd2_alloc(bh_in->b_size, GFP_NOFS);
		if (!tmp) {
			brelse(new_bh);
			free_buffer_head(new_bh);
			return -ENOMEM;
		}
		spin_lock(&jh_in->b_state_lock);
		if (jh_in->b_frozen_data) {
			jbd2_free(tmp, bh_in->b_size);
			goto copy_done;
		}

		jh_in->b_frozen_data = tmp;
		memcpy_from_folio(tmp, new_folio, new_offset, bh_in->b_size);


		jh_in->b_frozen_triggers = jh_in->b_triggers;

copy_done:
		new_folio = virt_to_folio(jh_in->b_frozen_data);
		new_offset = offset_in_folio(new_folio, jh_in->b_frozen_data);
		jbd2_data_do_escape(jh_in->b_frozen_data);
	}

escape_done:
	folio_set_bh(new_bh, new_folio, new_offset);
	new_bh->b_size = bh_in->b_size;
	new_bh->b_bdev = journal->j_dev;
	new_bh->b_blocknr = blocknr;
	new_bh->b_private = bh_in;
	set_buffer_mapped(new_bh);
	set_buffer_dirty(new_bh);

	*bh_out = new_bh;


	JBUFFER_TRACE(jh_in, "file as BJ_Shadow");
	spin_lock(&journal->j_list_lock);
	__jbd2_journal_file_buffer(jh_in, transaction, BJ_Shadow);
	spin_unlock(&journal->j_list_lock);
	set_buffer_shadow(bh_in);
	spin_unlock(&jh_in->b_state_lock);

	return do_escape;
}


/**
 * __jbd2_log_start_commit - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __jbd2_log_start_commit(journal_t *journal, tid_t target)
{

	if (journal->j_commit_request == target)
		return 0;


	if (journal->j_running_transaction &&
	    journal->j_running_transaction->t_tid == target) {


		journal->j_commit_request = target;
		jbd2_debug(1, "JBD2: requesting commit %u/%u\n",
			  journal->j_commit_request,
			  journal->j_commit_sequence);
		journal->j_running_transaction->t_requested = jiffies;
		wake_up(&journal->j_wait_commit);
		return 1;
	} else if (!tid_geq(journal->j_commit_request, target))


		WARN_ONCE(1, "JBD2: bad log_start_commit: %u %u %u %u\n",
			  journal->j_commit_request,
			  journal->j_commit_sequence,
			  target, journal->j_running_transaction ?
			  journal->j_running_transaction->t_tid : 0);
	return 0;
}


/**
 * jbd2_log_start_commit - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_log_start_commit(journal_t *journal, tid_t tid)
{
	int ret;

	write_lock(&journal->j_state_lock);
	ret = __jbd2_log_start_commit(journal, tid);
	write_unlock(&journal->j_state_lock);
	return ret;
}


/**
 * __jbd2_journal_force_commit - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __jbd2_journal_force_commit(journal_t *journal)
{
	transaction_t *transaction = NULL;
	tid_t tid;
	int need_to_start = 0, ret = 0;

	read_lock(&journal->j_state_lock);
	if (journal->j_running_transaction && !current->journal_info) {
		transaction = journal->j_running_transaction;
		if (!tid_geq(journal->j_commit_request, transaction->t_tid))
			need_to_start = 1;
	} else if (journal->j_committing_transaction)
		transaction = journal->j_committing_transaction;

	if (!transaction) {

		read_unlock(&journal->j_state_lock);
		return 0;
	}
	tid = transaction->t_tid;
	read_unlock(&journal->j_state_lock);
	if (need_to_start)
		jbd2_log_start_commit(journal, tid);
	ret = jbd2_log_wait_commit(journal, tid);
	if (!ret)
		ret = 1;

	return ret;
}


/**
 * jbd2_journal_force_commit_nested - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_force_commit_nested(journal_t *journal)
{
	int ret;

	ret = __jbd2_journal_force_commit(journal);
	return ret > 0;
}


/**
 * jbd2_journal_force_commit - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_force_commit(journal_t *journal)
{
	int ret;

	J_ASSERT(!current->journal_info);
	ret = __jbd2_journal_force_commit(journal);
	if (ret > 0)
		ret = 0;
	return ret;
}


/**
 * jbd2_journal_start_commit - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_start_commit(journal_t *journal, tid_t *ptid)
{
	int ret = 0;

	write_lock(&journal->j_state_lock);
	if (journal->j_running_transaction) {
		tid_t tid = journal->j_running_transaction->t_tid;

		__jbd2_log_start_commit(journal, tid);


		if (ptid)
			*ptid = tid;
		ret = 1;
	} else if (journal->j_committing_transaction) {


		if (ptid)
			*ptid = journal->j_committing_transaction->t_tid;
		ret = 1;
	}
	write_unlock(&journal->j_state_lock);
	return ret;
}


/**
 * jbd2_trans_will_send_data_barrier - Implements the trans will send data barrier operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_trans_will_send_data_barrier(journal_t *journal, tid_t tid)
{
	int ret = 0;
	transaction_t *commit_trans;

	if (!(journal->j_flags & JBD2_BARRIER))
		return 0;
	read_lock(&journal->j_state_lock);

	if (tid_geq(journal->j_commit_sequence, tid))
		goto out;
	commit_trans = journal->j_committing_transaction;
	if (!commit_trans || commit_trans->t_tid != tid) {
		ret = 1;
		goto out;
	}


	if (journal->j_fs_dev != journal->j_dev) {
		if (!commit_trans->t_need_data_flush ||
		    commit_trans->t_state >= T_COMMIT_DFLUSH)
			goto out;
	} else {
		if (commit_trans->t_state >= T_COMMIT_JFLUSH)
			goto out;
	}
	ret = 1;
out:
	read_unlock(&journal->j_state_lock);
	return ret;
}


/**
 * jbd2_log_wait_commit - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_log_wait_commit(journal_t *journal, tid_t tid)
{
	int err = 0;

	read_lock(&journal->j_state_lock);
#ifdef CONFIG_PROVE_LOCKING


	if (tid_gt(tid, journal->j_commit_sequence) &&
	    (!journal->j_committing_transaction ||
	     journal->j_committing_transaction->t_tid != tid)) {
		read_unlock(&journal->j_state_lock);
		jbd2_might_wait_for_commit(journal);
		read_lock(&journal->j_state_lock);
	}
#endif
#ifdef CONFIG_JBD2_DEBUG
	if (!tid_geq(journal->j_commit_request, tid)) {
		printk(KERN_ERR
		       "%s: error: j_commit_request=%u, tid=%u\n",
		       __func__, journal->j_commit_request, tid);
	}
#endif
	while (tid_gt(tid, journal->j_commit_sequence)) {
		jbd2_debug(1, "JBD2: want %u, j_commit_sequence=%u\n",
				  tid, journal->j_commit_sequence);
		read_unlock(&journal->j_state_lock);
		wake_up(&journal->j_wait_commit);
		wait_event(journal->j_wait_done_commit,
				!tid_gt(tid, journal->j_commit_sequence));
		read_lock(&journal->j_state_lock);
	}
	read_unlock(&journal->j_state_lock);

	if (unlikely(is_journal_aborted(journal)))
		err = -EIO;
	return err;
}


/**
 * jbd2_fc_begin_commit - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_fc_begin_commit(journal_t *journal, tid_t tid)
{
	if (unlikely(is_journal_aborted(journal)))
		return -EIO;


	if (!journal->j_stats.ts_tid)
		return -EINVAL;

	write_lock(&journal->j_state_lock);
	if (tid_geq(journal->j_commit_sequence, tid)) {
		write_unlock(&journal->j_state_lock);
		return -EALREADY;
	}

	if (journal->j_flags & JBD2_FULL_COMMIT_ONGOING ||
	    (journal->j_flags & JBD2_FAST_COMMIT_ONGOING)) {
		DEFINE_WAIT(wait);

		prepare_to_wait(&journal->j_fc_wait, &wait,
				TASK_UNINTERRUPTIBLE);
		write_unlock(&journal->j_state_lock);
		schedule();
		finish_wait(&journal->j_fc_wait, &wait);
		return -EALREADY;
	}
	journal->j_flags |= JBD2_FAST_COMMIT_ONGOING;
	write_unlock(&journal->j_state_lock);
	jbd2_journal_lock_updates(journal);

	return 0;
}


/**
 * __jbd2_fc_end_commit - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __jbd2_fc_end_commit(journal_t *journal, tid_t tid, bool fallback)
{
	if (journal->j_fc_cleanup_callback)
		journal->j_fc_cleanup_callback(journal, 0, tid);
	jbd2_journal_unlock_updates(journal);
	write_lock(&journal->j_state_lock);
	journal->j_flags &= ~JBD2_FAST_COMMIT_ONGOING;
	if (fallback)
		journal->j_flags |= JBD2_FULL_COMMIT_ONGOING;
	write_unlock(&journal->j_state_lock);
	wake_up(&journal->j_fc_wait);
	if (fallback)
		return jbd2_complete_transaction(journal, tid);
	return 0;
}


/**
 * jbd2_fc_end_commit - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_fc_end_commit(journal_t *journal)
{
	return __jbd2_fc_end_commit(journal, 0, false);
}


/**
 * jbd2_fc_end_commit_fallback - Advances journalled state toward a durable transaction or checkpoint boundary.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_fc_end_commit_fallback(journal_t *journal)
{
	tid_t tid;

	read_lock(&journal->j_state_lock);
	tid = journal->j_running_transaction ?
		journal->j_running_transaction->t_tid : 0;
	read_unlock(&journal->j_state_lock);
	return __jbd2_fc_end_commit(journal, tid, true);
}


/**
 * jbd2_transaction_committed - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_transaction_committed(journal_t *journal, tid_t tid)
{
	return tid_geq(READ_ONCE(journal->j_commit_sequence), tid);
}


/**
 * jbd2_complete_transaction - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_complete_transaction(journal_t *journal, tid_t tid)
{
	int	need_to_wait = 1;

	read_lock(&journal->j_state_lock);
	if (journal->j_running_transaction &&
	    journal->j_running_transaction->t_tid == tid) {
		if (journal->j_commit_request != tid) {

			read_unlock(&journal->j_state_lock);
			jbd2_log_start_commit(journal, tid);
			goto wait_commit;
		}
	} else if (!(journal->j_committing_transaction &&
		     journal->j_committing_transaction->t_tid == tid))
		need_to_wait = 0;
	read_unlock(&journal->j_state_lock);
	if (!need_to_wait)
		return 0;
wait_commit:
	return jbd2_log_wait_commit(journal, tid);
}


/**
 * jbd2_journal_next_log_block - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_next_log_block(journal_t *journal, unsigned long long *retp)
{
	unsigned long blocknr;

	write_lock(&journal->j_state_lock);
	J_ASSERT(journal->j_free > 1);

	blocknr = journal->j_head;
	journal->j_head++;
	journal->j_free--;
	if (journal->j_head == journal->j_last)
		journal->j_head = journal->j_first;
	write_unlock(&journal->j_state_lock);
	return jbd2_journal_bmap(journal, blocknr, retp);
}


/**
 * jbd2_fc_get_buf - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_fc_get_buf(journal_t *journal, struct buffer_head **bh_out)
{
	unsigned long long pblock;
	unsigned long blocknr;
	int ret = 0;
	struct buffer_head *bh;
	int fc_off;

	*bh_out = NULL;

	if (journal->j_fc_off + journal->j_fc_first >= journal->j_fc_last)
		return -EINVAL;

	fc_off = journal->j_fc_off;
	blocknr = journal->j_fc_first + fc_off;
	journal->j_fc_off++;
	ret = jbd2_journal_bmap(journal, blocknr, &pblock);
	if (ret)
		return ret;

	bh = __getblk(journal->j_dev, pblock, journal->j_blocksize);
	if (!bh)
		return -ENOMEM;

	journal->j_fc_wbuf[fc_off] = bh;

	*bh_out = bh;

	return 0;
}


/**
 * jbd2_fc_wait_bufs - Implements the fc wait bufs operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_fc_wait_bufs(journal_t *journal, int num_blks)
{
	struct buffer_head *bh;
	int i, j_fc_off;

	j_fc_off = journal->j_fc_off;


	for (i = j_fc_off - 1; i >= j_fc_off - num_blks; i--) {
		bh = journal->j_fc_wbuf[i];
		wait_on_buffer(bh);


		if (unlikely(!buffer_uptodate(bh))) {
			journal->j_fc_off = i + 1;
			return -EIO;
		}
		put_bh(bh);
		journal->j_fc_wbuf[i] = NULL;
	}

	return 0;
}


/**
 * jbd2_fc_release_bufs - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void jbd2_fc_release_bufs(journal_t *journal)
{
	struct buffer_head *bh;
	int i, j_fc_off;

	j_fc_off = journal->j_fc_off;

	for (i = j_fc_off - 1; i >= 0; i--) {
		bh = journal->j_fc_wbuf[i];
		if (!bh)
			break;
		put_bh(bh);
		journal->j_fc_wbuf[i] = NULL;
	}
}


/**
 * jbd2_journal_bmap - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_bmap(journal_t *journal, unsigned long blocknr,
		 unsigned long long *retp)
{
	int err = 0;
	unsigned long long ret;
	sector_t block = blocknr;

	if (journal->j_bmap) {
		err = journal->j_bmap(journal, &block);
		if (err == 0)
			*retp = block;
	} else if (journal->j_inode) {
		ret = bmap(journal->j_inode, &block);

		if (ret || !block) {
			printk(KERN_ALERT "%s: journal block not found "
					"at offset %lu on %s\n",
			       __func__, blocknr, journal->j_devname);
			err = -EIO;
			jbd2_journal_abort(journal, err);
		} else {
			*retp = block;
		}

	} else {
		*retp = blocknr;
	}
	return err;
}


/**
 * jbd2_journal_get_descriptor_buffer - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct buffer_head *
jbd2_journal_get_descriptor_buffer(transaction_t *transaction, int type)
{
	journal_t *journal = transaction->t_journal;
	struct buffer_head *bh;
	unsigned long long blocknr;
	journal_header_t *header;
	int err;

	err = jbd2_journal_next_log_block(journal, &blocknr);

	if (err)
		return NULL;

	bh = __getblk(journal->j_dev, blocknr, journal->j_blocksize);
	if (!bh)
		return NULL;
	atomic_dec(&transaction->t_outstanding_credits);
	lock_buffer(bh);
	memset(bh->b_data, 0, journal->j_blocksize);
	header = (journal_header_t *)bh->b_data;
	header->h_magic = cpu_to_be32(JBD2_MAGIC_NUMBER);
	header->h_blocktype = cpu_to_be32(type);
	header->h_sequence = cpu_to_be32(transaction->t_tid);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	BUFFER_TRACE(bh, "return this buffer");
	return bh;
}


/**
 * jbd2_descriptor_block_csum_set - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void jbd2_descriptor_block_csum_set(journal_t *j, struct buffer_head *bh)
{
	struct jbd2_journal_block_tail *tail;
	__u32 csum;

	if (!jbd2_journal_has_csum_v2or3(j))
		return;

	tail = (struct jbd2_journal_block_tail *)(bh->b_data + j->j_blocksize -
			sizeof(struct jbd2_journal_block_tail));
	tail->t_checksum = 0;
	csum = jbd2_chksum(j, j->j_csum_seed, bh->b_data, j->j_blocksize);
	tail->t_checksum = cpu_to_be32(csum);
}


/**
 * jbd2_journal_get_log_tail - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_get_log_tail(journal_t *journal, tid_t *tid,
			      unsigned long *block)
{
	transaction_t *transaction;
	int ret;

	read_lock(&journal->j_state_lock);
	spin_lock(&journal->j_list_lock);
	transaction = journal->j_checkpoint_transactions;
	if (transaction) {
		*tid = transaction->t_tid;
		*block = transaction->t_log_start;
	} else if ((transaction = journal->j_committing_transaction) != NULL) {
		*tid = transaction->t_tid;
		*block = transaction->t_log_start;
	} else if ((transaction = journal->j_running_transaction) != NULL) {
		*tid = transaction->t_tid;
		*block = journal->j_head;
	} else {
		*tid = journal->j_transaction_sequence;
		*block = journal->j_head;
	}
	ret = tid_gt(*tid, journal->j_tail_sequence);
	spin_unlock(&journal->j_list_lock);
	read_unlock(&journal->j_state_lock);

	return ret;
}


/**
 * __jbd2_update_log_tail - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int __jbd2_update_log_tail(journal_t *journal, tid_t tid, unsigned long block)
{
	unsigned long freed;
	int ret;

	BUG_ON(!mutex_is_locked(&journal->j_checkpoint_mutex));


	ret = jbd2_journal_update_sb_log_tail(journal, tid, block, REQ_FUA);
	if (ret)
		goto out;

	write_lock(&journal->j_state_lock);
	freed = block - journal->j_tail;
	if (block < journal->j_tail)
		freed += journal->j_last - journal->j_first;

	trace_jbd2_update_log_tail(journal, tid, block, freed);
	jbd2_debug(1,
		  "Cleaning journal tail from %u to %u (offset %lu), "
		  "freeing %lu\n",
		  journal->j_tail_sequence, tid, block, freed);

	journal->j_free += freed;
	journal->j_tail_sequence = tid;
	journal->j_tail = block;
	write_unlock(&journal->j_state_lock);

out:
	return ret;
}


/**
 * jbd2_update_log_tail - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void jbd2_update_log_tail(journal_t *journal, tid_t tid, unsigned long block)
{
	mutex_lock_io(&journal->j_checkpoint_mutex);
	if (tid_gt(tid, journal->j_tail_sequence))
		__jbd2_update_log_tail(journal, tid, block);
	mutex_unlock(&journal->j_checkpoint_mutex);
}


/**
 * struct jbd2_stats_proc_session - Private EXT4 state/data structure used by jbd2 journal core.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct jbd2_stats_proc_session {
	journal_t *journal;
	struct transaction_stats_s *stats;
	int start;
	int max;
};


/**
 * jbd2_seq_info_start - Implements the seq info start operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void *jbd2_seq_info_start(struct seq_file *seq, loff_t *pos)
{
	return *pos ? NULL : SEQ_START_TOKEN;
}


/**
 * jbd2_seq_info_next - Implements the seq info next operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void *jbd2_seq_info_next(struct seq_file *seq, void *v, loff_t *pos)
{
	(*pos)++;
	return NULL;
}


/**
 * jbd2_seq_info_show - Implements the seq info show operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int jbd2_seq_info_show(struct seq_file *seq, void *v)
{
	struct jbd2_stats_proc_session *s = seq->private;

	if (v != SEQ_START_TOKEN)
		return 0;
	seq_printf(seq, "%lu transactions (%lu requested), "
		   "each up to %u blocks\n",
		   s->stats->ts_tid, s->stats->ts_requested,
		   s->journal->j_max_transaction_buffers);
	if (s->stats->ts_tid == 0)
		return 0;
	seq_printf(seq, "average: \n  %ums waiting for transaction\n",
	    jiffies_to_msecs(s->stats->run.rs_wait / s->stats->ts_tid));
	seq_printf(seq, "  %ums request delay\n",
	    (s->stats->ts_requested == 0) ? 0 :
	    jiffies_to_msecs(s->stats->run.rs_request_delay /
			     s->stats->ts_requested));
	seq_printf(seq, "  %ums running transaction\n",
	    jiffies_to_msecs(s->stats->run.rs_running / s->stats->ts_tid));
	seq_printf(seq, "  %ums transaction was being locked\n",
	    jiffies_to_msecs(s->stats->run.rs_locked / s->stats->ts_tid));
	seq_printf(seq, "  %ums flushing data (in ordered mode)\n",
	    jiffies_to_msecs(s->stats->run.rs_flushing / s->stats->ts_tid));
	seq_printf(seq, "  %ums logging transaction\n",
	    jiffies_to_msecs(s->stats->run.rs_logging / s->stats->ts_tid));
	seq_printf(seq, "  %lluus average transaction commit time\n",
		   div_u64(s->journal->j_average_commit_time, 1000));
	seq_printf(seq, "  %lu handles per transaction\n",
	    s->stats->run.rs_handle_count / s->stats->ts_tid);
	seq_printf(seq, "  %lu blocks per transaction\n",
	    s->stats->run.rs_blocks / s->stats->ts_tid);
	seq_printf(seq, "  %lu logged blocks per transaction\n",
	    s->stats->run.rs_blocks_logged / s->stats->ts_tid);
	return 0;
}


/**
 * jbd2_seq_info_stop - Implements the seq info stop operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void jbd2_seq_info_stop(struct seq_file *seq, void *v)
{
}

static const struct seq_operations jbd2_seq_info_ops = {
	.start  = jbd2_seq_info_start,
	.next   = jbd2_seq_info_next,
	.stop   = jbd2_seq_info_stop,
	.show   = jbd2_seq_info_show,
};


/**
 * jbd2_seq_info_open - Implements the seq info open operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int jbd2_seq_info_open(struct inode *inode, struct file *file)
{
	journal_t *journal = pde_data(inode);
	struct jbd2_stats_proc_session *s;
	int rc, size;

	s = kmalloc(sizeof(*s), GFP_KERNEL);
	if (s == NULL)
		return -ENOMEM;
	size = sizeof(struct transaction_stats_s);
	s->stats = kmalloc(size, GFP_KERNEL);
	if (s->stats == NULL) {
		kfree(s);
		return -ENOMEM;
	}
	spin_lock(&journal->j_history_lock);
	memcpy(s->stats, &journal->j_stats, size);
	s->journal = journal;
	spin_unlock(&journal->j_history_lock);

	rc = seq_open(file, &jbd2_seq_info_ops);
	if (rc == 0) {
		struct seq_file *m = file->private_data;
		m->private = s;
	} else {
		kfree(s->stats);
		kfree(s);
	}
	return rc;

}


/**
 * jbd2_seq_info_release - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int jbd2_seq_info_release(struct inode *inode, struct file *file)
{
	struct seq_file *seq = file->private_data;
	struct jbd2_stats_proc_session *s = seq->private;
	kfree(s->stats);
	kfree(s);
	return seq_release(inode, file);
}

static const struct proc_ops jbd2_info_proc_ops = {
	.proc_open	= jbd2_seq_info_open,
	.proc_read	= seq_read,
	.proc_lseek	= seq_lseek,
	.proc_release	= jbd2_seq_info_release,
};

static struct proc_dir_entry *proc_jbd2_stats;


/**
 * jbd2_stats_proc_init - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void jbd2_stats_proc_init(journal_t *journal)
{
	journal->j_proc_entry = proc_mkdir(journal->j_devname, proc_jbd2_stats);
	if (journal->j_proc_entry) {
		proc_create_data("info", S_IRUGO, journal->j_proc_entry,
				 &jbd2_info_proc_ops, journal);
	}
}


/**
 * jbd2_stats_proc_exit - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void jbd2_stats_proc_exit(journal_t *journal)
{
	remove_proc_entry("info", journal->j_proc_entry);
	remove_proc_entry(journal->j_devname, proc_jbd2_stats);
}


/**
 * jbd2_min_tag_size - Implements the min tag size operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int jbd2_min_tag_size(void)
{


	return sizeof(journal_block_tag_t) - 4;
}


/**
 * jbd2_journal_shrink_scan - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static unsigned long jbd2_journal_shrink_scan(struct shrinker *shrink,
					      struct shrink_control *sc)
{
	journal_t *journal = shrink->private_data;
	unsigned long nr_to_scan = sc->nr_to_scan;
	unsigned long nr_shrunk;
	unsigned long count;

	count = percpu_counter_read_positive(&journal->j_checkpoint_jh_count);
	trace_jbd2_shrink_scan_enter(journal, sc->nr_to_scan, count);

	nr_shrunk = jbd2_journal_shrink_checkpoint_list(journal, &nr_to_scan);

	count = percpu_counter_read_positive(&journal->j_checkpoint_jh_count);
	trace_jbd2_shrink_scan_exit(journal, nr_to_scan, nr_shrunk, count);

	return nr_shrunk;
}


/**
 * jbd2_journal_shrink_count - Computes derived filesystem state used for validation, accounting or policy decisions.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static unsigned long jbd2_journal_shrink_count(struct shrinker *shrink,
					       struct shrink_control *sc)
{
	journal_t *journal = shrink->private_data;
	unsigned long count;

	count = percpu_counter_read_positive(&journal->j_checkpoint_jh_count);
	trace_jbd2_shrink_count(journal, sc->nr_to_scan, count);

	return count;
}


/**
 * journal_fail_superblock - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void journal_fail_superblock(journal_t *journal)
{
	struct buffer_head *bh = journal->j_sb_buffer;
	brelse(bh);
	journal->j_sb_buffer = NULL;
}


/**
 * journal_check_superblock - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int journal_check_superblock(journal_t *journal)
{
	journal_superblock_t *sb = journal->j_superblock;
	int num_fc_blks;
	int err = -EINVAL;

	if (sb->s_header.h_magic != cpu_to_be32(JBD2_MAGIC_NUMBER) ||
	    sb->s_blocksize != cpu_to_be32(journal->j_blocksize)) {
		printk(KERN_WARNING "JBD2: no valid journal superblock found\n");
		return err;
	}

	if (be32_to_cpu(sb->s_header.h_blocktype) != JBD2_SUPERBLOCK_V1 &&
	    be32_to_cpu(sb->s_header.h_blocktype) != JBD2_SUPERBLOCK_V2) {
		printk(KERN_WARNING "JBD2: unrecognised superblock format ID\n");
		return err;
	}

	if (be32_to_cpu(sb->s_maxlen) > journal->j_total_len) {
		printk(KERN_WARNING "JBD2: journal file too short\n");
		return err;
	}

	if (be32_to_cpu(sb->s_first) == 0 ||
	    be32_to_cpu(sb->s_first) >= journal->j_total_len) {
		printk(KERN_WARNING
			"JBD2: Invalid start block of journal: %u\n",
			be32_to_cpu(sb->s_first));
		return err;
	}


	if (!jbd2_format_support_feature(journal))
		return 0;

	if ((sb->s_feature_ro_compat &
			~cpu_to_be32(JBD2_KNOWN_ROCOMPAT_FEATURES)) ||
	    (sb->s_feature_incompat &
			~cpu_to_be32(JBD2_KNOWN_INCOMPAT_FEATURES))) {
		printk(KERN_WARNING "JBD2: Unrecognised features on journal\n");
		return err;
	}

	num_fc_blks = jbd2_has_feature_fast_commit(journal) ?
				jbd2_journal_get_num_fc_blks(sb) : 0;
	if (be32_to_cpu(sb->s_maxlen) < JBD2_MIN_JOURNAL_BLOCKS ||
	    be32_to_cpu(sb->s_maxlen) - JBD2_MIN_JOURNAL_BLOCKS < num_fc_blks) {
		printk(KERN_ERR "JBD2: journal file too short %u,%d\n",
		       be32_to_cpu(sb->s_maxlen), num_fc_blks);
		return err;
	}

	if (jbd2_has_feature_csum2(journal) &&
	    jbd2_has_feature_csum3(journal)) {

		printk(KERN_ERR "JBD2: Can't enable checksumming v2 and v3 "
		       "at the same time!\n");
		return err;
	}

	if (jbd2_journal_has_csum_v2or3_feature(journal) &&
	    jbd2_has_feature_checksum(journal)) {

		printk(KERN_ERR "JBD2: Can't enable checksumming v1 and v2/3 "
		       "at the same time!\n");
		return err;
	}


	if (jbd2_journal_has_csum_v2or3_feature(journal)) {
		if (sb->s_checksum_type != JBD2_CRC32C_CHKSUM) {
			printk(KERN_ERR "JBD2: Unknown checksum type\n");
			return err;
		}

		journal->j_chksum_driver = crypto_alloc_shash("crc32c", 0, 0);
		if (IS_ERR(journal->j_chksum_driver)) {
			printk(KERN_ERR "JBD2: Cannot load crc32c driver.\n");
			err = PTR_ERR(journal->j_chksum_driver);
			journal->j_chksum_driver = NULL;
			return err;
		}

		if (sb->s_checksum != jbd2_superblock_csum(journal, sb)) {
			printk(KERN_ERR "JBD2: journal checksum error\n");
			err = -EFSBADCRC;
			return err;
		}
	}

	return 0;
}


/**
 * journal_revoke_records_per_block - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int journal_revoke_records_per_block(journal_t *journal)
{
	int record_size;
	int space = journal->j_blocksize - sizeof(jbd2_journal_revoke_header_t);

	if (jbd2_has_feature_64bit(journal))
		record_size = 8;
	else
		record_size = 4;

	if (jbd2_journal_has_csum_v2or3(journal))
		space -= sizeof(struct jbd2_journal_block_tail);
	return space / record_size;
}


/**
 * jbd2_journal_get_max_txn_bufs - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int jbd2_journal_get_max_txn_bufs(journal_t *journal)
{
	return (journal->j_total_len - journal->j_fc_wbufsize) / 3;
}


/**
 * jbd2_descriptor_blocks_per_trans - Implements the descriptor blocks per trans operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int jbd2_descriptor_blocks_per_trans(journal_t *journal)
{
	int tag_space = journal->j_blocksize - sizeof(journal_header_t);
	int tags_per_block;


	tag_space -= 16;
	if (jbd2_journal_has_csum_v2or3(journal))
		tag_space -= sizeof(struct jbd2_journal_block_tail);

	tags_per_block = (tag_space - 16) / journal_tag_bytes(journal);


	return 1 + DIV_ROUND_UP(jbd2_journal_get_max_txn_bufs(journal),
				tags_per_block);
}


/**
 * jbd2_journal_init_transaction_limits - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void jbd2_journal_init_transaction_limits(journal_t *journal)
{
	journal->j_revoke_records_per_block =
				journal_revoke_records_per_block(journal);
	journal->j_transaction_overhead_buffers =
				jbd2_descriptor_blocks_per_trans(journal);
	journal->j_max_transaction_buffers =
				jbd2_journal_get_max_txn_bufs(journal);
}


/**
 * journal_load_superblock - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int journal_load_superblock(journal_t *journal)
{
	int err;
	struct buffer_head *bh;
	journal_superblock_t *sb;

	bh = getblk_unmovable(journal->j_dev, journal->j_blk_offset,
			      journal->j_blocksize);
	if (bh)
		err = bh_read(bh, 0);
	if (!bh || err < 0) {
		pr_err("%s: Cannot read journal superblock\n", __func__);
		brelse(bh);
		return -EIO;
	}

	journal->j_sb_buffer = bh;
	sb = (journal_superblock_t *)bh->b_data;
	journal->j_superblock = sb;
	err = journal_check_superblock(journal);
	if (err) {
		journal_fail_superblock(journal);
		return err;
	}

	journal->j_tail_sequence = be32_to_cpu(sb->s_sequence);
	journal->j_tail = be32_to_cpu(sb->s_start);
	journal->j_first = be32_to_cpu(sb->s_first);
	journal->j_errno = be32_to_cpu(sb->s_errno);
	journal->j_last = be32_to_cpu(sb->s_maxlen);

	if (be32_to_cpu(sb->s_maxlen) < journal->j_total_len)
		journal->j_total_len = be32_to_cpu(sb->s_maxlen);

	if (jbd2_journal_has_csum_v2or3(journal))
		journal->j_csum_seed = jbd2_chksum(journal, ~0, sb->s_uuid,
						   sizeof(sb->s_uuid));

	jbd2_journal_init_transaction_limits(journal);

	if (jbd2_has_feature_fast_commit(journal)) {
		journal->j_fc_last = be32_to_cpu(sb->s_maxlen);
		journal->j_last = journal->j_fc_last -
				  jbd2_journal_get_num_fc_blks(sb);
		journal->j_fc_first = journal->j_last + 1;
		journal->j_fc_off = 0;
	}

	return 0;
}


/**
 * journal_init_common - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static journal_t *journal_init_common(struct block_device *bdev,
			struct block_device *fs_dev,
			unsigned long long start, int len, int blocksize)
{
	journal_t *journal;
	int err;
	int n;

	journal = kzalloc(sizeof(*journal), GFP_KERNEL);
	if (!journal)
		return ERR_PTR(-ENOMEM);

	lockdep_register_key(&journal->jbd2_trans_commit_key);
	journal->j_blocksize = blocksize;
	journal->j_dev = bdev;
	journal->j_fs_dev = fs_dev;
	journal->j_blk_offset = start;
	journal->j_total_len = len;
	jbd2_init_fs_dev_write_error(journal);

	err = journal_load_superblock(journal);
	if (err)
		goto err_cleanup;

	init_waitqueue_head(&journal->j_wait_transaction_locked);
	init_waitqueue_head(&journal->j_wait_done_commit);
	init_waitqueue_head(&journal->j_wait_commit);
	init_waitqueue_head(&journal->j_wait_updates);
	init_waitqueue_head(&journal->j_wait_reserved);
	init_waitqueue_head(&journal->j_fc_wait);
	mutex_init(&journal->j_abort_mutex);
	mutex_init(&journal->j_barrier);
	mutex_init(&journal->j_checkpoint_mutex);
	spin_lock_init(&journal->j_revoke_lock);
	spin_lock_init(&journal->j_list_lock);
	spin_lock_init(&journal->j_history_lock);
	rwlock_init(&journal->j_state_lock);

	journal->j_commit_interval = (HZ * JBD2_DEFAULT_MAX_COMMIT_AGE);
	journal->j_min_batch_time = 0;
	journal->j_max_batch_time = 15000;
	atomic_set(&journal->j_reserved_credits, 0);
	lockdep_init_map(&journal->j_trans_commit_map, "jbd2_handle",
			 &journal->jbd2_trans_commit_key, 0);


	journal->j_flags = JBD2_ABORT;


	err = jbd2_journal_init_revoke(journal, JOURNAL_REVOKE_DEFAULT_HASH);
	if (err)
		goto err_cleanup;


	err = -ENOMEM;
	n = journal->j_blocksize / jbd2_min_tag_size();
	journal->j_wbufsize = n;
	journal->j_fc_wbuf = NULL;
	journal->j_wbuf = kmalloc_array(n, sizeof(struct buffer_head *),
					GFP_KERNEL);
	if (!journal->j_wbuf)
		goto err_cleanup;

	err = percpu_counter_init(&journal->j_checkpoint_jh_count, 0,
				  GFP_KERNEL);
	if (err)
		goto err_cleanup;

	journal->j_shrink_transaction = NULL;

	journal->j_shrinker = shrinker_alloc(0, "jbd2-journal:(%u:%u)",
					     MAJOR(bdev->bd_dev),
					     MINOR(bdev->bd_dev));
	if (!journal->j_shrinker) {
		err = -ENOMEM;
		goto err_cleanup;
	}

	journal->j_shrinker->scan_objects = jbd2_journal_shrink_scan;
	journal->j_shrinker->count_objects = jbd2_journal_shrink_count;
	journal->j_shrinker->private_data = journal;

	shrinker_register(journal->j_shrinker);

	return journal;

err_cleanup:
	percpu_counter_destroy(&journal->j_checkpoint_jh_count);
	if (journal->j_chksum_driver)
		crypto_free_shash(journal->j_chksum_driver);
	kfree(journal->j_wbuf);
	jbd2_journal_destroy_revoke(journal);
	journal_fail_superblock(journal);
	lockdep_unregister_key(&journal->jbd2_trans_commit_key);
	kfree(journal);
	return ERR_PTR(err);
}


/**
 * jbd2_journal_init_dev - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
journal_t *jbd2_journal_init_dev(struct block_device *bdev,
			struct block_device *fs_dev,
			unsigned long long start, int len, int blocksize)
{
	journal_t *journal;

	journal = journal_init_common(bdev, fs_dev, start, len, blocksize);
	if (IS_ERR(journal))
		return ERR_CAST(journal);

	snprintf(journal->j_devname, sizeof(journal->j_devname),
		 "%pg", journal->j_dev);
	strreplace(journal->j_devname, '/', '!');
	jbd2_stats_proc_init(journal);

	return journal;
}


/**
 * jbd2_journal_init_inode - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
journal_t *jbd2_journal_init_inode(struct inode *inode)
{
	journal_t *journal;
	sector_t blocknr;
	int err = 0;

	blocknr = 0;
	err = bmap(inode, &blocknr);
	if (err || !blocknr) {
		pr_err("%s: Cannot locate journal superblock\n", __func__);
		return err ? ERR_PTR(err) : ERR_PTR(-EINVAL);
	}

	jbd2_debug(1, "JBD2: inode %s/%ld, size %lld, bits %d, blksize %ld\n",
		  inode->i_sb->s_id, inode->i_ino, (long long) inode->i_size,
		  inode->i_sb->s_blocksize_bits, inode->i_sb->s_blocksize);

	journal = journal_init_common(inode->i_sb->s_bdev, inode->i_sb->s_bdev,
			blocknr, inode->i_size >> inode->i_sb->s_blocksize_bits,
			inode->i_sb->s_blocksize);
	if (IS_ERR(journal))
		return ERR_CAST(journal);

	journal->j_inode = inode;
	snprintf(journal->j_devname, sizeof(journal->j_devname),
		 "%pg-%lu", journal->j_dev, journal->j_inode->i_ino);
	strreplace(journal->j_devname, '/', '!');
	jbd2_stats_proc_init(journal);

	return journal;
}


/**
 * journal_reset - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int journal_reset(journal_t *journal)
{
	journal_superblock_t *sb = journal->j_superblock;
	unsigned long long first, last;

	first = be32_to_cpu(sb->s_first);
	last = be32_to_cpu(sb->s_maxlen);
	if (first + JBD2_MIN_JOURNAL_BLOCKS > last + 1) {
		printk(KERN_ERR "JBD2: Journal too short (blocks %llu-%llu).\n",
		       first, last);
		journal_fail_superblock(journal);
		return -EINVAL;
	}

	journal->j_first = first;
	journal->j_last = last;

	if (journal->j_head != 0 && journal->j_flags & JBD2_CYCLE_RECORD) {


		if (journal->j_head < first || journal->j_head >= last) {
			printk(KERN_WARNING "JBD2: Incorrect Journal head block %lu, "
			       "disable journal_cycle_record\n",
			       journal->j_head);
			journal->j_head = journal->j_first;
		}
	} else {
		journal->j_head = journal->j_first;
	}
	journal->j_tail = journal->j_head;
	journal->j_free = journal->j_last - journal->j_first;

	journal->j_tail_sequence = journal->j_transaction_sequence;
	journal->j_commit_sequence = journal->j_transaction_sequence - 1;
	journal->j_commit_request = journal->j_commit_sequence;


	jbd2_clear_feature_fast_commit(journal);


	if (sb->s_start == 0) {
		jbd2_debug(1, "JBD2: Skipping superblock update on recovered sb "
			"(start %ld, seq %u, errno %d)\n",
			journal->j_tail, journal->j_tail_sequence,
			journal->j_errno);
		journal->j_flags |= JBD2_FLUSHED;
	} else {

		mutex_lock_io(&journal->j_checkpoint_mutex);


		jbd2_journal_update_sb_log_tail(journal,
						journal->j_tail_sequence,
						journal->j_tail, REQ_FUA);
		mutex_unlock(&journal->j_checkpoint_mutex);
	}
	return jbd2_journal_start_thread(journal);
}


/**
 * jbd2_write_superblock - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int jbd2_write_superblock(journal_t *journal, blk_opf_t write_flags)
{
	struct buffer_head *bh = journal->j_sb_buffer;
	journal_superblock_t *sb = journal->j_superblock;
	int ret = 0;


	if (!buffer_mapped(bh)) {
		unlock_buffer(bh);
		return -EIO;
	}


	write_flags |= JBD2_JOURNAL_REQ_FLAGS;
	if (!(journal->j_flags & JBD2_BARRIER))
		write_flags &= ~(REQ_FUA | REQ_PREFLUSH);

	trace_jbd2_write_superblock(journal, write_flags);

	if (buffer_write_io_error(bh)) {


		printk(KERN_ERR "JBD2: previous I/O error detected "
		       "for journal superblock update for %s.\n",
		       journal->j_devname);
		clear_buffer_write_io_error(bh);
		set_buffer_uptodate(bh);
	}
	if (jbd2_journal_has_csum_v2or3(journal))
		sb->s_checksum = jbd2_superblock_csum(journal, sb);
	get_bh(bh);
	bh->b_end_io = end_buffer_write_sync;
	submit_bh(REQ_OP_WRITE | write_flags, bh);
	wait_on_buffer(bh);
	if (buffer_write_io_error(bh)) {
		clear_buffer_write_io_error(bh);
		set_buffer_uptodate(bh);
		ret = -EIO;
	}
	if (ret) {
		printk(KERN_ERR "JBD2: I/O error when updating journal superblock for %s.\n",
				journal->j_devname);
		if (!is_journal_aborted(journal))
			jbd2_journal_abort(journal, ret);
	}

	return ret;
}


/**
 * jbd2_journal_update_sb_log_tail - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_update_sb_log_tail(journal_t *journal, tid_t tail_tid,
				    unsigned long tail_block,
				    blk_opf_t write_flags)
{
	journal_superblock_t *sb = journal->j_superblock;
	int ret;

	if (is_journal_aborted(journal))
		return -EIO;
	if (jbd2_check_fs_dev_write_error(journal)) {
		jbd2_journal_abort(journal, -EIO);
		return -EIO;
	}

	BUG_ON(!mutex_is_locked(&journal->j_checkpoint_mutex));
	jbd2_debug(1, "JBD2: updating superblock (start %lu, seq %u)\n",
		  tail_block, tail_tid);

	lock_buffer(journal->j_sb_buffer);
	sb->s_sequence = cpu_to_be32(tail_tid);
	sb->s_start    = cpu_to_be32(tail_block);

	ret = jbd2_write_superblock(journal, write_flags);
	if (ret)
		goto out;


	write_lock(&journal->j_state_lock);
	journal->j_flags &= ~JBD2_FLUSHED;
	write_unlock(&journal->j_state_lock);

out:
	return ret;
}


/**
 * jbd2_mark_journal_empty - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void jbd2_mark_journal_empty(journal_t *journal, blk_opf_t write_flags)
{
	journal_superblock_t *sb = journal->j_superblock;
	bool had_fast_commit = false;

	BUG_ON(!mutex_is_locked(&journal->j_checkpoint_mutex));
	lock_buffer(journal->j_sb_buffer);
	if (sb->s_start == 0) {
		unlock_buffer(journal->j_sb_buffer);
		return;
	}

	jbd2_debug(1, "JBD2: Marking journal as empty (seq %u)\n",
		  journal->j_tail_sequence);

	sb->s_sequence = cpu_to_be32(journal->j_tail_sequence);
	sb->s_start    = cpu_to_be32(0);
	sb->s_head     = cpu_to_be32(journal->j_head);
	if (jbd2_has_feature_fast_commit(journal)) {


		jbd2_clear_feature_fast_commit(journal);
		had_fast_commit = true;
	}

	jbd2_write_superblock(journal, write_flags);

	if (had_fast_commit)
		jbd2_set_feature_fast_commit(journal);


	write_lock(&journal->j_state_lock);
	journal->j_flags |= JBD2_FLUSHED;
	write_unlock(&journal->j_state_lock);
}


/**
 * __jbd2_journal_erase - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __jbd2_journal_erase(journal_t *journal, unsigned int flags)
{
	int err = 0;
	unsigned long block, log_offset;
	unsigned long long phys_block, block_start, block_stop;
	loff_t byte_start, byte_stop, byte_count;


	if ((flags & ~JBD2_JOURNAL_FLUSH_VALID) || !flags ||
			((flags & JBD2_JOURNAL_FLUSH_DISCARD) &&
			(flags & JBD2_JOURNAL_FLUSH_ZEROOUT)))
		return -EINVAL;

	if ((flags & JBD2_JOURNAL_FLUSH_DISCARD) &&
	    !bdev_max_discard_sectors(journal->j_dev))
		return -EOPNOTSUPP;


	log_offset = be32_to_cpu(journal->j_superblock->s_first);
	block_start =  ~0ULL;
	for (block = log_offset; block < journal->j_total_len; block++) {
		err = jbd2_journal_bmap(journal, block, &phys_block);
		if (err) {
			pr_err("JBD2: bad block at offset %lu", block);
			return err;
		}

		if (block_start == ~0ULL) {
			block_start = phys_block;
			block_stop = block_start - 1;
		}


		if (phys_block != block_stop + 1) {
			block--;
		} else {
			block_stop++;


			if (block != journal->j_total_len - 1)
				continue;
		}


		byte_start = block_start * journal->j_blocksize;
		byte_stop = block_stop * journal->j_blocksize;
		byte_count = (block_stop - block_start + 1) *
				journal->j_blocksize;

		truncate_inode_pages_range(journal->j_dev->bd_mapping,
				byte_start, byte_stop);

		if (flags & JBD2_JOURNAL_FLUSH_DISCARD) {
			err = blkdev_issue_discard(journal->j_dev,
					byte_start >> SECTOR_SHIFT,
					byte_count >> SECTOR_SHIFT,
					GFP_NOFS);
		} else if (flags & JBD2_JOURNAL_FLUSH_ZEROOUT) {
			err = blkdev_issue_zeroout(journal->j_dev,
					byte_start >> SECTOR_SHIFT,
					byte_count >> SECTOR_SHIFT,
					GFP_NOFS, 0);
		}

		if (unlikely(err != 0)) {
			pr_err("JBD2: (error %d) unable to wipe journal at physical blocks %llu - %llu",
					err, block_start, block_stop);
			return err;
		}


		block_start = ~0ULL;
	}

	return blkdev_issue_flush(journal->j_dev);
}


/**
 * jbd2_journal_update_sb_errno - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void jbd2_journal_update_sb_errno(journal_t *journal)
{
	journal_superblock_t *sb = journal->j_superblock;
	int errcode;

	lock_buffer(journal->j_sb_buffer);
	errcode = journal->j_errno;
	if (errcode == -ESHUTDOWN)
		errcode = 0;
	jbd2_debug(1, "JBD2: updating superblock error (errno %d)\n", errcode);
	sb->s_errno    = cpu_to_be32(errcode);

	jbd2_write_superblock(journal, REQ_FUA);
}


/**
 * jbd2_journal_load - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_load(journal_t *journal)
{
	int err;
	journal_superblock_t *sb = journal->j_superblock;


	err = jbd2_journal_create_slab(be32_to_cpu(sb->s_blocksize));
	if (err)
		return err;


	err = jbd2_journal_recover(journal);
	if (err) {
		pr_warn("JBD2: journal recovery failed\n");
		return err;
	}

	if (journal->j_failed_commit) {
		printk(KERN_ERR "JBD2: journal transaction %u on %s "
		       "is corrupt.\n", journal->j_failed_commit,
		       journal->j_devname);
		return -EFSCORRUPTED;
	}


	journal->j_flags &= ~JBD2_ABORT;


	err = journal_reset(journal);
	if (err) {
		pr_warn("JBD2: journal reset failed\n");
		return err;
	}

	journal->j_flags |= JBD2_LOADED;
	return 0;
}


/**
 * jbd2_journal_destroy - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_destroy(journal_t *journal)
{
	int err = 0;


	journal_kill_thread(journal);


	if (journal->j_running_transaction)
		jbd2_journal_commit_transaction(journal);


	spin_lock(&journal->j_list_lock);
	while (journal->j_checkpoint_transactions != NULL) {
		spin_unlock(&journal->j_list_lock);
		mutex_lock_io(&journal->j_checkpoint_mutex);
		err = jbd2_log_do_checkpoint(journal);
		mutex_unlock(&journal->j_checkpoint_mutex);


		if (err) {
			jbd2_journal_destroy_checkpoint(journal);
			spin_lock(&journal->j_list_lock);
			break;
		}
		spin_lock(&journal->j_list_lock);
	}

	J_ASSERT(journal->j_running_transaction == NULL);
	J_ASSERT(journal->j_committing_transaction == NULL);
	J_ASSERT(journal->j_checkpoint_transactions == NULL);
	spin_unlock(&journal->j_list_lock);


	if (!is_journal_aborted(journal) &&
	    jbd2_check_fs_dev_write_error(journal))
		jbd2_journal_abort(journal, -EIO);

	if (journal->j_sb_buffer) {
		if (!is_journal_aborted(journal)) {
			mutex_lock_io(&journal->j_checkpoint_mutex);

			write_lock(&journal->j_state_lock);
			journal->j_tail_sequence =
				++journal->j_transaction_sequence;
			write_unlock(&journal->j_state_lock);

			jbd2_mark_journal_empty(journal, REQ_PREFLUSH | REQ_FUA);
			mutex_unlock(&journal->j_checkpoint_mutex);
		} else
			err = -EIO;
		brelse(journal->j_sb_buffer);
	}

	if (journal->j_shrinker) {
		percpu_counter_destroy(&journal->j_checkpoint_jh_count);
		shrinker_free(journal->j_shrinker);
	}
	if (journal->j_proc_entry)
		jbd2_stats_proc_exit(journal);
	iput(journal->j_inode);
	if (journal->j_revoke)
		jbd2_journal_destroy_revoke(journal);
	if (journal->j_chksum_driver)
		crypto_free_shash(journal->j_chksum_driver);
	kfree(journal->j_fc_wbuf);
	kfree(journal->j_wbuf);
	lockdep_unregister_key(&journal->jbd2_trans_commit_key);
	kfree(journal);

	return err;
}


/**
 * jbd2_journal_check_used_features - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_check_used_features(journal_t *journal, unsigned long compat,
				 unsigned long ro, unsigned long incompat)
{
	journal_superblock_t *sb;

	if (!compat && !ro && !incompat)
		return 1;
	if (!jbd2_format_support_feature(journal))
		return 0;

	sb = journal->j_superblock;

	if (((be32_to_cpu(sb->s_feature_compat) & compat) == compat) &&
	    ((be32_to_cpu(sb->s_feature_ro_compat) & ro) == ro) &&
	    ((be32_to_cpu(sb->s_feature_incompat) & incompat) == incompat))
		return 1;

	return 0;
}


/**
 * jbd2_journal_check_available_features - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_check_available_features(journal_t *journal, unsigned long compat,
				      unsigned long ro, unsigned long incompat)
{
	if (!compat && !ro && !incompat)
		return 1;

	if (!jbd2_format_support_feature(journal))
		return 0;

	if ((compat   & JBD2_KNOWN_COMPAT_FEATURES) == compat &&
	    (ro       & JBD2_KNOWN_ROCOMPAT_FEATURES) == ro &&
	    (incompat & JBD2_KNOWN_INCOMPAT_FEATURES) == incompat)
		return 1;

	return 0;
}


/**
 * jbd2_journal_initialize_fast_commit - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
jbd2_journal_initialize_fast_commit(journal_t *journal)
{
	journal_superblock_t *sb = journal->j_superblock;
	unsigned long long num_fc_blks;

	num_fc_blks = jbd2_journal_get_num_fc_blks(sb);
	if (num_fc_blks > journal->j_last)
		return -EFSCORRUPTED;
	if (journal->j_last - num_fc_blks < JBD2_MIN_JOURNAL_BLOCKS)
		return -ENOSPC;


	WARN_ON(journal->j_fc_wbuf != NULL);
	journal->j_fc_wbuf = kmalloc_array(num_fc_blks,
				sizeof(struct buffer_head *), GFP_KERNEL);
	if (!journal->j_fc_wbuf)
		return -ENOMEM;

	journal->j_fc_wbufsize = num_fc_blks;
	journal->j_fc_last = journal->j_last;
	journal->j_last = journal->j_fc_last - num_fc_blks;
	journal->j_fc_first = journal->j_last + 1;
	journal->j_fc_off = 0;
	journal->j_free = journal->j_last - journal->j_first;

	return 0;
}


/**
 * jbd2_journal_set_features - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_set_features(journal_t *journal, unsigned long compat,
			  unsigned long ro, unsigned long incompat)
{
#define INCOMPAT_FEATURE_ON(f) \
		((incompat & (f)) && !(sb->s_feature_incompat & cpu_to_be32(f)))
#define COMPAT_FEATURE_ON(f) \
		((compat & (f)) && !(sb->s_feature_compat & cpu_to_be32(f)))
	journal_superblock_t *sb;

	if (jbd2_journal_check_used_features(journal, compat, ro, incompat))
		return 1;

	if (!jbd2_journal_check_available_features(journal, compat, ro, incompat))
		return 0;


	if (incompat & JBD2_FEATURE_INCOMPAT_CSUM_V2) {
		incompat &= ~JBD2_FEATURE_INCOMPAT_CSUM_V2;
		incompat |= JBD2_FEATURE_INCOMPAT_CSUM_V3;
	}


	if (incompat & JBD2_FEATURE_INCOMPAT_CSUM_V3 &&
	    compat & JBD2_FEATURE_COMPAT_CHECKSUM)
		compat &= ~JBD2_FEATURE_COMPAT_CHECKSUM;

	jbd2_debug(1, "Setting new features 0x%lx/0x%lx/0x%lx\n",
		  compat, ro, incompat);

	sb = journal->j_superblock;

	if (incompat & JBD2_FEATURE_INCOMPAT_FAST_COMMIT) {
		if (jbd2_journal_initialize_fast_commit(journal)) {
			pr_err("JBD2: Cannot enable fast commits.\n");
			return 0;
		}
	}


	if ((journal->j_chksum_driver == NULL) &&
	    INCOMPAT_FEATURE_ON(JBD2_FEATURE_INCOMPAT_CSUM_V3)) {
		journal->j_chksum_driver = crypto_alloc_shash("crc32c", 0, 0);
		if (IS_ERR(journal->j_chksum_driver)) {
			printk(KERN_ERR "JBD2: Cannot load crc32c driver.\n");
			journal->j_chksum_driver = NULL;
			return 0;
		}

		journal->j_csum_seed = jbd2_chksum(journal, ~0, sb->s_uuid,
						   sizeof(sb->s_uuid));
	}

	lock_buffer(journal->j_sb_buffer);


	if (INCOMPAT_FEATURE_ON(JBD2_FEATURE_INCOMPAT_CSUM_V3)) {
		sb->s_checksum_type = JBD2_CRC32C_CHKSUM;
		sb->s_feature_compat &=
			~cpu_to_be32(JBD2_FEATURE_COMPAT_CHECKSUM);
	}


	if (COMPAT_FEATURE_ON(JBD2_FEATURE_COMPAT_CHECKSUM))
		sb->s_feature_incompat &=
			~cpu_to_be32(JBD2_FEATURE_INCOMPAT_CSUM_V2 |
				     JBD2_FEATURE_INCOMPAT_CSUM_V3);

	sb->s_feature_compat    |= cpu_to_be32(compat);
	sb->s_feature_ro_compat |= cpu_to_be32(ro);
	sb->s_feature_incompat  |= cpu_to_be32(incompat);


	if (jbd2_journal_has_csum_v2or3(journal))
		sb->s_checksum = jbd2_superblock_csum(journal, sb);
	unlock_buffer(journal->j_sb_buffer);
	jbd2_journal_init_transaction_limits(journal);

	return 1;
#undef COMPAT_FEATURE_ON
#undef INCOMPAT_FEATURE_ON
}


/**
 * jbd2_journal_clear_features - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void jbd2_journal_clear_features(journal_t *journal, unsigned long compat,
				unsigned long ro, unsigned long incompat)
{
	journal_superblock_t *sb;

	jbd2_debug(1, "Clear features 0x%lx/0x%lx/0x%lx\n",
		  compat, ro, incompat);

	sb = journal->j_superblock;

	lock_buffer(journal->j_sb_buffer);
	sb->s_feature_compat    &= ~cpu_to_be32(compat);
	sb->s_feature_ro_compat &= ~cpu_to_be32(ro);
	sb->s_feature_incompat  &= ~cpu_to_be32(incompat);


	if (jbd2_journal_has_csum_v2or3(journal))
		sb->s_checksum = jbd2_superblock_csum(journal, sb);
	unlock_buffer(journal->j_sb_buffer);
	jbd2_journal_init_transaction_limits(journal);
}


/**
 * jbd2_journal_flush - Drives pending state toward the durability guarantee required by the calling VFS or journal interface.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_flush(journal_t *journal, unsigned int flags)
{
	int err = 0;
	transaction_t *transaction = NULL;

	write_lock(&journal->j_state_lock);


	if (journal->j_running_transaction) {
		transaction = journal->j_running_transaction;
		__jbd2_log_start_commit(journal, transaction->t_tid);
	} else if (journal->j_committing_transaction)
		transaction = journal->j_committing_transaction;


	if (transaction) {
		tid_t tid = transaction->t_tid;

		write_unlock(&journal->j_state_lock);
		jbd2_log_wait_commit(journal, tid);
	} else {
		write_unlock(&journal->j_state_lock);
	}


	spin_lock(&journal->j_list_lock);
	while (!err && journal->j_checkpoint_transactions != NULL) {
		spin_unlock(&journal->j_list_lock);
		mutex_lock_io(&journal->j_checkpoint_mutex);
		err = jbd2_log_do_checkpoint(journal);
		mutex_unlock(&journal->j_checkpoint_mutex);
		spin_lock(&journal->j_list_lock);
	}
	spin_unlock(&journal->j_list_lock);

	if (is_journal_aborted(journal))
		return -EIO;

	mutex_lock_io(&journal->j_checkpoint_mutex);
	if (!err) {
		err = jbd2_cleanup_journal_tail(journal);
		if (err < 0) {
			mutex_unlock(&journal->j_checkpoint_mutex);
			goto out;
		}
		err = 0;
	}


	jbd2_mark_journal_empty(journal, REQ_FUA);

	if (flags)
		err = __jbd2_journal_erase(journal, flags);

	mutex_unlock(&journal->j_checkpoint_mutex);
	write_lock(&journal->j_state_lock);
	J_ASSERT(!journal->j_running_transaction);
	J_ASSERT(!journal->j_committing_transaction);
	J_ASSERT(!journal->j_checkpoint_transactions);
	J_ASSERT(journal->j_head == journal->j_tail);
	J_ASSERT(journal->j_tail_sequence == journal->j_transaction_sequence);
	write_unlock(&journal->j_state_lock);
out:
	return err;
}


/**
 * jbd2_journal_wipe - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_wipe(journal_t *journal, int write)
{
	int err;

	J_ASSERT (!(journal->j_flags & JBD2_LOADED));

	if (!journal->j_tail)
		return 0;

	printk(KERN_WARNING "JBD2: %s recovery information on journal\n",
		write ? "Clearing" : "Ignoring");

	err = jbd2_journal_skip_recovery(journal);
	if (write) {

		mutex_lock_io(&journal->j_checkpoint_mutex);
		jbd2_mark_journal_empty(journal, REQ_FUA);
		mutex_unlock(&journal->j_checkpoint_mutex);
	}

	return err;
}


/**
 * jbd2_journal_abort - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void jbd2_journal_abort(journal_t *journal, int errno)
{
	transaction_t *transaction;


	mutex_lock(&journal->j_abort_mutex);


	write_lock(&journal->j_state_lock);
	if (journal->j_flags & JBD2_ABORT) {
		int old_errno = journal->j_errno;

		write_unlock(&journal->j_state_lock);
		if (old_errno != -ESHUTDOWN && errno == -ESHUTDOWN) {
			journal->j_errno = errno;
			jbd2_journal_update_sb_errno(journal);
		}
		mutex_unlock(&journal->j_abort_mutex);
		return;
	}


	pr_err("Aborting journal on device %s.\n", journal->j_devname);

	journal->j_flags |= JBD2_ABORT;
	journal->j_errno = errno;
	transaction = journal->j_running_transaction;
	if (transaction)
		__jbd2_log_start_commit(journal, transaction->t_tid);
	write_unlock(&journal->j_state_lock);


	jbd2_journal_update_sb_errno(journal);
	mutex_unlock(&journal->j_abort_mutex);
}


/**
 * jbd2_journal_errno - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_errno(journal_t *journal)
{
	int err;

	read_lock(&journal->j_state_lock);
	if (journal->j_flags & JBD2_ABORT)
		err = -EROFS;
	else
		err = journal->j_errno;
	read_unlock(&journal->j_state_lock);
	return err;
}


/**
 * jbd2_journal_clear_err - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_clear_err(journal_t *journal)
{
	int err = 0;

	write_lock(&journal->j_state_lock);
	if (journal->j_flags & JBD2_ABORT)
		err = -EROFS;
	else
		journal->j_errno = 0;
	write_unlock(&journal->j_state_lock);
	return err;
}


/**
 * jbd2_journal_ack_err - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void jbd2_journal_ack_err(journal_t *journal)
{
	write_lock(&journal->j_state_lock);
	if (journal->j_errno)
		journal->j_flags |= JBD2_ACK_ERR;
	write_unlock(&journal->j_state_lock);
}


/**
 * jbd2_journal_blocks_per_page - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int jbd2_journal_blocks_per_page(struct inode *inode)
{
	return 1 << (PAGE_SHIFT - inode->i_sb->s_blocksize_bits);
}


/**
 * journal_tag_bytes - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
size_t journal_tag_bytes(journal_t *journal)
{
	size_t sz;

	if (jbd2_has_feature_csum3(journal))
		return sizeof(journal_block_tag3_t);

	sz = sizeof(journal_block_tag_t);

	if (jbd2_has_feature_csum2(journal))
		sz += sizeof(__u16);

	if (jbd2_has_feature_64bit(journal))
		return sz;
	else
		return sz - sizeof(__u32);
}


#define JBD2_MAX_SLABS 8
static struct kmem_cache *jbd2_slab[JBD2_MAX_SLABS];

static const char *jbd2_slab_names[JBD2_MAX_SLABS] = {
	"jbd2_1k", "jbd2_2k", "jbd2_4k", "jbd2_8k",
	"jbd2_16k", "jbd2_32k", "jbd2_64k", "jbd2_128k"
};


/**
 * jbd2_journal_destroy_slabs - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void jbd2_journal_destroy_slabs(void)
{
	int i;

	for (i = 0; i < JBD2_MAX_SLABS; i++) {
		kmem_cache_destroy(jbd2_slab[i]);
		jbd2_slab[i] = NULL;
	}
}


/**
 * jbd2_journal_create_slab - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int jbd2_journal_create_slab(size_t size)
{
	static DEFINE_MUTEX(jbd2_slab_create_mutex);
	int i = order_base_2(size) - 10;
	size_t slab_size;

	if (size == PAGE_SIZE)
		return 0;

	if (i >= JBD2_MAX_SLABS)
		return -EINVAL;

	if (unlikely(i < 0))
		i = 0;
	mutex_lock(&jbd2_slab_create_mutex);
	if (jbd2_slab[i]) {
		mutex_unlock(&jbd2_slab_create_mutex);
		return 0;
	}

	slab_size = 1 << (i+10);
	jbd2_slab[i] = kmem_cache_create(jbd2_slab_names[i], slab_size,
					 slab_size, 0, NULL);
	mutex_unlock(&jbd2_slab_create_mutex);
	if (!jbd2_slab[i]) {
		printk(KERN_EMERG "JBD2: no memory for jbd2_slab cache\n");
		return -ENOMEM;
	}
	return 0;
}


/**
 * get_slab - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct kmem_cache *get_slab(size_t size)
{
	int i = order_base_2(size) - 10;

	BUG_ON(i >= JBD2_MAX_SLABS);
	if (unlikely(i < 0))
		i = 0;
	BUG_ON(jbd2_slab[i] == NULL);
	return jbd2_slab[i];
}


/**
 * jbd2_alloc - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void *jbd2_alloc(size_t size, gfp_t flags)
{
	void *ptr;

	BUG_ON(size & (size-1));

	if (size < PAGE_SIZE)
		ptr = kmem_cache_alloc(get_slab(size), flags);
	else
		ptr = (void *)__get_free_pages(flags, get_order(size));


	BUG_ON(((unsigned long) ptr) & (size-1));

	return ptr;
}


/**
 * jbd2_free - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void jbd2_free(void *ptr, size_t size)
{
	if (size < PAGE_SIZE)
		kmem_cache_free(get_slab(size), ptr);
	else
		free_pages((unsigned long)ptr, get_order(size));
};


static struct kmem_cache *jbd2_journal_head_cache;
#ifdef CONFIG_JBD2_DEBUG
static atomic_t nr_journal_heads = ATOMIC_INIT(0);
#endif


/**
 * jbd2_journal_init_journal_head_cache - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __init jbd2_journal_init_journal_head_cache(void)
{
	J_ASSERT(!jbd2_journal_head_cache);
	jbd2_journal_head_cache = kmem_cache_create("jbd2_journal_head",
				sizeof(struct journal_head),
				0,
				SLAB_TEMPORARY | SLAB_TYPESAFE_BY_RCU,
				NULL);
	if (!jbd2_journal_head_cache) {
		printk(KERN_EMERG "JBD2: no memory for journal_head cache\n");
		return -ENOMEM;
	}
	return 0;
}


/**
 * jbd2_journal_destroy_journal_head_cache - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void jbd2_journal_destroy_journal_head_cache(void)
{
	kmem_cache_destroy(jbd2_journal_head_cache);
	jbd2_journal_head_cache = NULL;
}


/**
 * journal_alloc_journal_head - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct journal_head *journal_alloc_journal_head(void)
{
	struct journal_head *ret;

#ifdef CONFIG_JBD2_DEBUG
	atomic_inc(&nr_journal_heads);
#endif
	ret = kmem_cache_zalloc(jbd2_journal_head_cache, GFP_NOFS);
	if (!ret) {
		jbd2_debug(1, "out of memory for journal_head\n");
		pr_notice_ratelimited("ENOMEM in %s, retrying.\n", __func__);
		ret = kmem_cache_zalloc(jbd2_journal_head_cache,
				GFP_NOFS | __GFP_NOFAIL);
	}
	spin_lock_init(&ret->b_state_lock);
	return ret;
}


/**
 * journal_free_journal_head - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void journal_free_journal_head(struct journal_head *jh)
{
#ifdef CONFIG_JBD2_DEBUG
	atomic_dec(&nr_journal_heads);
	memset(jh, JBD2_POISON_FREE, sizeof(*jh));
#endif
	kmem_cache_free(jbd2_journal_head_cache, jh);
}


/**
 * jbd2_journal_add_journal_head - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct journal_head *jbd2_journal_add_journal_head(struct buffer_head *bh)
{
	struct journal_head *jh;
	struct journal_head *new_jh = NULL;

repeat:
	if (!buffer_jbd(bh))
		new_jh = journal_alloc_journal_head();

	jbd_lock_bh_journal_head(bh);
	if (buffer_jbd(bh)) {
		jh = bh2jh(bh);
	} else {
		J_ASSERT_BH(bh,
			(atomic_read(&bh->b_count) > 0) ||
			(bh->b_folio && bh->b_folio->mapping));

		if (!new_jh) {
			jbd_unlock_bh_journal_head(bh);
			goto repeat;
		}

		jh = new_jh;
		new_jh = NULL;
		set_buffer_jbd(bh);
		bh->b_private = jh;
		jh->b_bh = bh;
		get_bh(bh);
		BUFFER_TRACE(bh, "added journal_head");
	}
	jh->b_jcount++;
	jbd_unlock_bh_journal_head(bh);
	if (new_jh)
		journal_free_journal_head(new_jh);
	return bh->b_private;
}


/**
 * jbd2_journal_grab_journal_head - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct journal_head *jbd2_journal_grab_journal_head(struct buffer_head *bh)
{
	struct journal_head *jh = NULL;

	jbd_lock_bh_journal_head(bh);
	if (buffer_jbd(bh)) {
		jh = bh2jh(bh);
		jh->b_jcount++;
	}
	jbd_unlock_bh_journal_head(bh);
	return jh;
}


/**
 * __journal_remove_journal_head - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void __journal_remove_journal_head(struct buffer_head *bh)
{
	struct journal_head *jh = bh2jh(bh);

	J_ASSERT_JH(jh, jh->b_transaction == NULL);
	J_ASSERT_JH(jh, jh->b_next_transaction == NULL);
	J_ASSERT_JH(jh, jh->b_cp_transaction == NULL);
	J_ASSERT_JH(jh, jh->b_jlist == BJ_None);
	J_ASSERT_BH(bh, buffer_jbd(bh));
	J_ASSERT_BH(bh, jh2bh(jh) == bh);
	BUFFER_TRACE(bh, "remove journal_head");


	bh->b_private = NULL;
	jh->b_bh = NULL;
	clear_buffer_jbd(bh);
}


/**
 * journal_release_journal_head - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void journal_release_journal_head(struct journal_head *jh, size_t b_size)
{
	if (jh->b_frozen_data) {
		printk(KERN_WARNING "%s: freeing b_frozen_data\n", __func__);
		jbd2_free(jh->b_frozen_data, b_size);
	}
	if (jh->b_committed_data) {
		printk(KERN_WARNING "%s: freeing b_committed_data\n", __func__);
		jbd2_free(jh->b_committed_data, b_size);
	}
	journal_free_journal_head(jh);
}


/**
 * jbd2_journal_put_journal_head - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void jbd2_journal_put_journal_head(struct journal_head *jh)
{
	struct buffer_head *bh = jh2bh(jh);

	jbd_lock_bh_journal_head(bh);
	J_ASSERT_JH(jh, jh->b_jcount > 0);
	--jh->b_jcount;
	if (!jh->b_jcount) {
		__journal_remove_journal_head(bh);
		jbd_unlock_bh_journal_head(bh);
		journal_release_journal_head(jh, bh->b_size);
		__brelse(bh);
	} else {
		jbd_unlock_bh_journal_head(bh);
	}
}


/**
 * jbd2_journal_init_jbd_inode - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void jbd2_journal_init_jbd_inode(struct jbd2_inode *jinode, struct inode *inode)
{
	jinode->i_transaction = NULL;
	jinode->i_next_transaction = NULL;
	jinode->i_vfs_inode = inode;
	jinode->i_flags = 0;
	jinode->i_dirty_start = 0;
	jinode->i_dirty_end = 0;
	INIT_LIST_HEAD(&jinode->i_list);
}


/**
 * jbd2_journal_release_jbd_inode - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void jbd2_journal_release_jbd_inode(journal_t *journal,
				    struct jbd2_inode *jinode)
{
	if (!journal)
		return;
restart:
	spin_lock(&journal->j_list_lock);

	if (jinode->i_flags & JI_COMMIT_RUNNING) {
		wait_queue_head_t *wq;
		DEFINE_WAIT_BIT(wait, &jinode->i_flags, __JI_COMMIT_RUNNING);
		wq = bit_waitqueue(&jinode->i_flags, __JI_COMMIT_RUNNING);
		prepare_to_wait(wq, &wait.wq_entry, TASK_UNINTERRUPTIBLE);
		spin_unlock(&journal->j_list_lock);
		schedule();
		finish_wait(wq, &wait.wq_entry);
		goto restart;
	}

	if (jinode->i_transaction) {
		list_del(&jinode->i_list);
		jinode->i_transaction = NULL;
	}
	spin_unlock(&journal->j_list_lock);
}


#ifdef CONFIG_PROC_FS

#define JBD2_STATS_PROC_NAME "fs/jbd2"


/**
 * jbd2_create_jbd_stats_proc_entry - Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void __init jbd2_create_jbd_stats_proc_entry(void)
{
	proc_jbd2_stats = proc_mkdir(JBD2_STATS_PROC_NAME, NULL);
}


/**
 * jbd2_remove_jbd_stats_proc_entry - Implements the remove jbd stats proc entry operation within the jbd2 journal core subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void __exit jbd2_remove_jbd_stats_proc_entry(void)
{
	if (proc_jbd2_stats)
		remove_proc_entry(JBD2_STATS_PROC_NAME, NULL);
}

#else

#define jbd2_create_jbd_stats_proc_entry() do {} while (0)
#define jbd2_remove_jbd_stats_proc_entry() do {} while (0)

#endif

struct kmem_cache *jbd2_handle_cache, *jbd2_inode_cache;


/**
 * jbd2_journal_init_inode_cache - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __init jbd2_journal_init_inode_cache(void)
{
	J_ASSERT(!jbd2_inode_cache);
	jbd2_inode_cache = KMEM_CACHE(jbd2_inode, 0);
	if (!jbd2_inode_cache) {
		pr_emerg("JBD2: failed to create inode cache\n");
		return -ENOMEM;
	}
	return 0;
}


/**
 * jbd2_journal_init_handle_cache - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __init jbd2_journal_init_handle_cache(void)
{
	J_ASSERT(!jbd2_handle_cache);
	jbd2_handle_cache = KMEM_CACHE(jbd2_journal_handle, SLAB_TEMPORARY);
	if (!jbd2_handle_cache) {
		printk(KERN_EMERG "JBD2: failed to create handle cache\n");
		return -ENOMEM;
	}
	return 0;
}


/**
 * jbd2_journal_destroy_inode_cache - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void jbd2_journal_destroy_inode_cache(void)
{
	kmem_cache_destroy(jbd2_inode_cache);
	jbd2_inode_cache = NULL;
}


/**
 * jbd2_journal_destroy_handle_cache - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void jbd2_journal_destroy_handle_cache(void)
{
	kmem_cache_destroy(jbd2_handle_cache);
	jbd2_handle_cache = NULL;
}


/**
 * journal_init_caches - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __init journal_init_caches(void)
{
	int ret;

	ret = jbd2_journal_init_revoke_record_cache();
	if (ret == 0)
		ret = jbd2_journal_init_revoke_table_cache();
	if (ret == 0)
		ret = jbd2_journal_init_journal_head_cache();
	if (ret == 0)
		ret = jbd2_journal_init_handle_cache();
	if (ret == 0)
		ret = jbd2_journal_init_inode_cache();
	if (ret == 0)
		ret = jbd2_journal_init_transaction_cache();
	return ret;
}


/**
 * jbd2_journal_destroy_caches - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void jbd2_journal_destroy_caches(void)
{
	jbd2_journal_destroy_revoke_record_cache();
	jbd2_journal_destroy_revoke_table_cache();
	jbd2_journal_destroy_journal_head_cache();
	jbd2_journal_destroy_handle_cache();
	jbd2_journal_destroy_inode_cache();
	jbd2_journal_destroy_transaction_cache();
	jbd2_journal_destroy_slabs();
}


/**
 * infiltratr_jbd2_init - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int __init infiltratr_jbd2_init(void)
{
	int ret;

	BUILD_BUG_ON(sizeof(struct journal_superblock_s) != 1024);

	ret = journal_init_caches();
	if (ret != 0) {
		jbd2_journal_destroy_caches();
		return ret;
	}

	jbd2_create_jbd_stats_proc_entry();
	return 0;
}


/**
 * infiltratr_jbd2_exit - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void __exit infiltratr_jbd2_exit(void)
{
#ifdef CONFIG_JBD2_DEBUG
	int n = atomic_read(&nr_journal_heads);
	if (n)
		printk(KERN_ERR "JBD2: leaked %d journal_heads!\n", n);
#endif
	jbd2_remove_jbd_stats_proc_entry();
	jbd2_journal_destroy_caches();
}
