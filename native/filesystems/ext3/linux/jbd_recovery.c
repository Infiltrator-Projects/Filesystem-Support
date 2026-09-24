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
