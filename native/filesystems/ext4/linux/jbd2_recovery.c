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
