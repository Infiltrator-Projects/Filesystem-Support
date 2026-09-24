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
