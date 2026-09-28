/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Infiltrator Filesystem Support — EXT4 journal tracepoints.
 *
 * Diagnostic tracing is deliberately separated from journal correctness.
 * These events expose transaction identity, latency and checkpoint progress
 * without participating in any state transition or persistence decision.
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM infiltratr_ext4_jbd2

#if !defined(INFILTRATOR_EXT4_JOURNAL_TRACE_H) || defined(TRACE_HEADER_MULTI_READ)
#define INFILTRATOR_EXT4_JOURNAL_TRACE_H

#include <linux/jbd2.h>
#include <linux/jiffies.h>
#include <linux/kdev_t.h>
#include <linux/tracepoint.h>

struct transaction_chp_stats_s;
struct transaction_run_stats_s;

#define IFS_EXT4_TRACE_JOURNAL_DEV(journal) \
	((journal) && (journal)->j_fs_dev ? (journal)->j_fs_dev->bd_dev : 0)

TRACE_EVENT(jbd2_checkpoint,
	TP_PROTO(journal_t *journal, int result),
	TP_ARGS(journal, result),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(int, result)
	),
	TP_fast_assign(
		__entry->device = IFS_EXT4_TRACE_JOURNAL_DEV(journal);
		__entry->result = result;
	),
	TP_printk("device=%u:%u result=%d",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->result)
);

DECLARE_EVENT_CLASS(ifs_ext4_commit_phase,
	TP_PROTO(journal_t *journal, transaction_t *transaction),
	TP_ARGS(journal, transaction),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(tid_t, transaction_id)
		__field(bool, synchronous)
	),
	TP_fast_assign(
		__entry->device = IFS_EXT4_TRACE_JOURNAL_DEV(journal);
		__entry->transaction_id = transaction ? transaction->t_tid : 0;
		__entry->synchronous =
			transaction ? transaction->t_synchronous_commit : false;
	),
	TP_printk("device=%u:%u transaction=%u synchronous=%u",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->transaction_id, __entry->synchronous)
);

DEFINE_EVENT(ifs_ext4_commit_phase, jbd2_start_commit,
	TP_PROTO(journal_t *journal, transaction_t *transaction),
	TP_ARGS(journal, transaction)
);

DEFINE_EVENT(ifs_ext4_commit_phase, jbd2_commit_locking,
	TP_PROTO(journal_t *journal, transaction_t *transaction),
	TP_ARGS(journal, transaction)
);

DEFINE_EVENT(ifs_ext4_commit_phase, jbd2_commit_flushing,
	TP_PROTO(journal_t *journal, transaction_t *transaction),
	TP_ARGS(journal, transaction)
);

DEFINE_EVENT(ifs_ext4_commit_phase, jbd2_commit_logging,
	TP_PROTO(journal_t *journal, transaction_t *transaction),
	TP_ARGS(journal, transaction)
);

DEFINE_EVENT(ifs_ext4_commit_phase, jbd2_drop_transaction,
	TP_PROTO(journal_t *journal, transaction_t *transaction),
	TP_ARGS(journal, transaction)
);

TRACE_EVENT(jbd2_end_commit,
	TP_PROTO(journal_t *journal, transaction_t *transaction),
	TP_ARGS(journal, transaction),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(tid_t, transaction_id)
		__field(tid_t, next_tail_id)
		__field(bool, synchronous)
	),
	TP_fast_assign(
		__entry->device = IFS_EXT4_TRACE_JOURNAL_DEV(journal);
		__entry->transaction_id = transaction ? transaction->t_tid : 0;
		__entry->next_tail_id = journal ? journal->j_tail_sequence : 0;
		__entry->synchronous =
			transaction ? transaction->t_synchronous_commit : false;
	),
	TP_printk("device=%u:%u transaction=%u next_tail=%u synchronous=%u",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->transaction_id, __entry->next_tail_id,
		  __entry->synchronous)
);

TRACE_EVENT(jbd2_submit_inode_data,
	TP_PROTO(struct inode *inode),
	TP_ARGS(inode),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(ino_t, inode_number)
	),
	TP_fast_assign(
		__entry->device = inode ? inode->i_sb->s_dev : 0;
		__entry->inode_number = inode ? inode->i_ino : 0;
	),
	TP_printk("device=%u:%u inode=%lu",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  (unsigned long)__entry->inode_number)
);

DECLARE_EVENT_CLASS(ifs_ext4_handle_begin,
	TP_PROTO(dev_t dev, tid_t tid, unsigned int type,
		 unsigned int line_no, int requested_blocks),
	TP_ARGS(dev, tid, type, line_no, requested_blocks),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(tid_t, transaction_id)
		__field(unsigned int, handle_type)
		__field(unsigned int, source_line)
		__field(int, requested_blocks)
	),
	TP_fast_assign(
		__entry->device = dev;
		__entry->transaction_id = tid;
		__entry->handle_type = type;
		__entry->source_line = line_no;
		__entry->requested_blocks = requested_blocks;
	),
	TP_printk("device=%u:%u transaction=%u type=%u line=%u requested=%d",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->transaction_id, __entry->handle_type,
		  __entry->source_line, __entry->requested_blocks)
);

DEFINE_EVENT(ifs_ext4_handle_begin, jbd2_handle_start,
	TP_PROTO(dev_t dev, tid_t tid, unsigned int type,
		 unsigned int line_no, int requested_blocks),
	TP_ARGS(dev, tid, type, line_no, requested_blocks)
);

DEFINE_EVENT(ifs_ext4_handle_begin, jbd2_handle_restart,
	TP_PROTO(dev_t dev, tid_t tid, unsigned int type,
		 unsigned int line_no, int requested_blocks),
	TP_ARGS(dev, tid, type, line_no, requested_blocks)
);

TRACE_EVENT(jbd2_handle_extend,
	TP_PROTO(dev_t dev, tid_t tid, unsigned int type,
		 unsigned int line_no, int buffer_credits,
		 int requested_blocks),
	TP_ARGS(dev, tid, type, line_no, buffer_credits, requested_blocks),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(tid_t, transaction_id)
		__field(unsigned int, handle_type)
		__field(unsigned int, source_line)
		__field(int, credits_before)
		__field(int, requested_blocks)
	),
	TP_fast_assign(
		__entry->device = dev;
		__entry->transaction_id = tid;
		__entry->handle_type = type;
		__entry->source_line = line_no;
		__entry->credits_before = buffer_credits;
		__entry->requested_blocks = requested_blocks;
	),
	TP_printk("device=%u:%u transaction=%u type=%u line=%u credits=%d requested=%d",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->transaction_id, __entry->handle_type,
		  __entry->source_line, __entry->credits_before,
		  __entry->requested_blocks)
);

TRACE_EVENT(jbd2_handle_stats,
	TP_PROTO(dev_t dev, tid_t tid, unsigned int type,
		 unsigned int line_no, int interval, int sync,
		 int requested_blocks, int dirtied_blocks),
	TP_ARGS(dev, tid, type, line_no, interval, sync,
		requested_blocks, dirtied_blocks),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(tid_t, transaction_id)
		__field(unsigned int, handle_type)
		__field(unsigned int, source_line)
		__field(int, interval)
		__field(int, synchronous)
		__field(int, requested_blocks)
		__field(int, dirtied_blocks)
	),
	TP_fast_assign(
		__entry->device = dev;
		__entry->transaction_id = tid;
		__entry->handle_type = type;
		__entry->source_line = line_no;
		__entry->interval = interval;
		__entry->synchronous = sync;
		__entry->requested_blocks = requested_blocks;
		__entry->dirtied_blocks = dirtied_blocks;
	),
	TP_printk("device=%u:%u transaction=%u type=%u line=%u interval=%d synchronous=%d requested=%d dirtied=%d",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->transaction_id, __entry->handle_type,
		  __entry->source_line, __entry->interval,
		  __entry->synchronous, __entry->requested_blocks,
		  __entry->dirtied_blocks)
);

TRACE_EVENT(jbd2_run_stats,
	TP_PROTO(dev_t dev, tid_t tid, struct transaction_run_stats_s *stats),
	TP_ARGS(dev, tid, stats),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(tid_t, transaction_id)
		__field(unsigned long, wait_ms)
		__field(unsigned long, request_delay_ms)
		__field(unsigned long, running_ms)
		__field(unsigned long, locked_ms)
		__field(unsigned long, flushing_ms)
		__field(unsigned long, logging_ms)
		__field(__u32, handles)
		__field(__u32, blocks)
		__field(__u32, logged_blocks)
	),
	TP_fast_assign(
		__entry->device = dev;
		__entry->transaction_id = tid;
		__entry->wait_ms = jiffies_to_msecs(stats->rs_wait);
		__entry->request_delay_ms =
			jiffies_to_msecs(stats->rs_request_delay);
		__entry->running_ms = jiffies_to_msecs(stats->rs_running);
		__entry->locked_ms = jiffies_to_msecs(stats->rs_locked);
		__entry->flushing_ms = jiffies_to_msecs(stats->rs_flushing);
		__entry->logging_ms = jiffies_to_msecs(stats->rs_logging);
		__entry->handles = stats->rs_handle_count;
		__entry->blocks = stats->rs_blocks;
		__entry->logged_blocks = stats->rs_blocks_logged;
	),
	TP_printk("device=%u:%u transaction=%u wait_ms=%lu request_delay_ms=%lu running_ms=%lu locked_ms=%lu flushing_ms=%lu logging_ms=%lu handles=%u blocks=%u logged=%u",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->transaction_id, __entry->wait_ms,
		  __entry->request_delay_ms, __entry->running_ms,
		  __entry->locked_ms, __entry->flushing_ms,
		  __entry->logging_ms, __entry->handles,
		  __entry->blocks, __entry->logged_blocks)
);

TRACE_EVENT(jbd2_checkpoint_stats,
	TP_PROTO(dev_t dev, tid_t tid, struct transaction_chp_stats_s *stats),
	TP_ARGS(dev, tid, stats),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(tid_t, transaction_id)
		__field(unsigned long, elapsed_ms)
		__field(__u32, forced_closes)
		__field(__u32, written_buffers)
		__field(__u32, dropped_buffers)
	),
	TP_fast_assign(
		__entry->device = dev;
		__entry->transaction_id = tid;
		__entry->elapsed_ms = jiffies_to_msecs(stats->cs_chp_time);
		__entry->forced_closes = stats->cs_forced_to_close;
		__entry->written_buffers = stats->cs_written;
		__entry->dropped_buffers = stats->cs_dropped;
	),
	TP_printk("device=%u:%u transaction=%u elapsed_ms=%lu forced=%u written=%u dropped=%u",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->transaction_id, __entry->elapsed_ms,
		  __entry->forced_closes, __entry->written_buffers,
		  __entry->dropped_buffers)
);

TRACE_EVENT(jbd2_update_log_tail,
	TP_PROTO(journal_t *journal, tid_t first_tid,
		 unsigned long block_nr, unsigned long freed),
	TP_ARGS(journal, first_tid, block_nr, freed),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(tid_t, old_tail)
		__field(tid_t, new_tail)
		__field(unsigned long, block_number)
		__field(unsigned long, freed_blocks)
	),
	TP_fast_assign(
		__entry->device = IFS_EXT4_TRACE_JOURNAL_DEV(journal);
		__entry->old_tail = journal ? journal->j_tail_sequence : 0;
		__entry->new_tail = first_tid;
		__entry->block_number = block_nr;
		__entry->freed_blocks = freed;
	),
	TP_printk("device=%u:%u tail=%u->%u block=%lu freed=%lu",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->old_tail, __entry->new_tail,
		  __entry->block_number, __entry->freed_blocks)
);

TRACE_EVENT(jbd2_write_superblock,
	TP_PROTO(journal_t *journal, blk_opf_t write_flags),
	TP_ARGS(journal, write_flags),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(blk_opf_t, operation_flags)
	),
	TP_fast_assign(
		__entry->device = IFS_EXT4_TRACE_JOURNAL_DEV(journal);
		__entry->operation_flags = write_flags;
	),
	TP_printk("device=%u:%u operation_flags=0x%x",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  (__force u32)__entry->operation_flags)
);

TRACE_EVENT(jbd2_lock_buffer_stall,
	TP_PROTO(dev_t dev, unsigned long stall_ms),
	TP_ARGS(dev, stall_ms),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(unsigned long, stall_ms)
	),
	TP_fast_assign(
		__entry->device = dev;
		__entry->stall_ms = stall_ms;
	),
	TP_printk("device=%u:%u stall_ms=%lu",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->stall_ms)
);

DECLARE_EVENT_CLASS(ifs_ext4_checkpoint_pressure,
	TP_PROTO(journal_t *journal, unsigned long nr_to_scan,
		 unsigned long count),
	TP_ARGS(journal, nr_to_scan, count),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(unsigned long, requested_scan)
		__field(unsigned long, checkpoint_count)
	),
	TP_fast_assign(
		__entry->device = IFS_EXT4_TRACE_JOURNAL_DEV(journal);
		__entry->requested_scan = nr_to_scan;
		__entry->checkpoint_count = count;
	),
	TP_printk("device=%u:%u scan=%lu checkpoints=%lu",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->requested_scan, __entry->checkpoint_count)
);

DEFINE_EVENT(ifs_ext4_checkpoint_pressure, jbd2_shrink_count,
	TP_PROTO(journal_t *journal, unsigned long nr_to_scan,
		 unsigned long count),
	TP_ARGS(journal, nr_to_scan, count)
);

DEFINE_EVENT(ifs_ext4_checkpoint_pressure, jbd2_shrink_scan_enter,
	TP_PROTO(journal_t *journal, unsigned long nr_to_scan,
		 unsigned long count),
	TP_ARGS(journal, nr_to_scan, count)
);

TRACE_EVENT(jbd2_shrink_scan_exit,
	TP_PROTO(journal_t *journal, unsigned long nr_to_scan,
		 unsigned long nr_shrunk, unsigned long count),
	TP_ARGS(journal, nr_to_scan, nr_shrunk, count),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(unsigned long, requested_scan)
		__field(unsigned long, released)
		__field(unsigned long, checkpoint_count)
	),
	TP_fast_assign(
		__entry->device = IFS_EXT4_TRACE_JOURNAL_DEV(journal);
		__entry->requested_scan = nr_to_scan;
		__entry->released = nr_shrunk;
		__entry->checkpoint_count = count;
	),
	TP_printk("device=%u:%u scan=%lu released=%lu checkpoints=%lu",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->requested_scan, __entry->released,
		  __entry->checkpoint_count)
);

TRACE_EVENT(jbd2_shrink_checkpoint_list,
	TP_PROTO(journal_t *journal, tid_t first_tid, tid_t tid,
		 tid_t last_tid, unsigned long nr_freed, tid_t next_tid),
	TP_ARGS(journal, first_tid, tid, last_tid, nr_freed, next_tid),
	TP_STRUCT__entry(
		__field(dev_t, device)
		__field(tid_t, first_transaction)
		__field(tid_t, current_transaction)
		__field(tid_t, last_transaction)
		__field(tid_t, next_transaction)
		__field(unsigned long, released)
	),
	TP_fast_assign(
		__entry->device = IFS_EXT4_TRACE_JOURNAL_DEV(journal);
		__entry->first_transaction = first_tid;
		__entry->current_transaction = tid;
		__entry->last_transaction = last_tid;
		__entry->next_transaction = next_tid;
		__entry->released = nr_freed;
	),
	TP_printk("device=%u:%u range=%u:%u:%u next=%u released=%lu",
		  MAJOR(__entry->device), MINOR(__entry->device),
		  __entry->first_transaction, __entry->current_transaction,
		  __entry->last_transaction, __entry->next_transaction,
		  __entry->released)
);

#endif /* INFILTRATOR_EXT4_JOURNAL_TRACE_H */

#include <trace/define_trace.h>
