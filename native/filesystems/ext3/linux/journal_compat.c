/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Infiltrator Filesystem Support — EXT3 journal adapter.
 *
 * The filesystem keeps EXT3 policy here and delegates generic transaction
 * transport to the kernel JBD2 API.  This replaces the former embedded JBD
 * implementation rather than carrying a second journal engine in ext3.ko.
 */

#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/string.h>

#include "journal_internal.h"

int journal_dirty_data(handle_t *handle, struct buffer_head *bh)
{
	int result;

	if (!handle || !bh)
		return -EINVAL;
	if (is_handle_aborted(handle))
		return 0;
	if (!buffer_mapped(bh))
		return 0;
	if (!buffer_uptodate(bh))
		return -EIO;

	/*
	 * JBD2 no longer exposes the old buffer-level ordered-data API.
	 * EXT3 conservatively journals these buffers as transaction data.
	 * This preserves crash ordering and atomicity at the cost of extra
	 * journal traffic instead of silently weakening durability.
	 */
	result = jbd2_journal_get_write_access(handle, bh);
	if (result)
		return result;

	return jbd2_journal_dirty_metadata(handle, bh);
}

static int ifs_ext3_zero_journal(journal_t *journal)
{
	unsigned int logical;

	for (logical = 0; logical < journal->j_total_len; ++logical) {
		unsigned long long physical = 0;
		struct buffer_head *bh;
		int result;

		result = jbd2_journal_bmap(journal, logical, &physical);
		if (result)
			return result;

		bh = __getblk(journal->j_dev, physical, journal->j_blocksize);
		if (!bh)
			return -ENOMEM;

		lock_buffer(bh);
		memset(bh->b_data, 0, journal->j_blocksize);
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		mark_buffer_dirty(bh);
		brelse(bh);
	}

	return sync_blockdev(journal->j_dev);
}

int journal_create(journal_t *journal)
{
	journal_superblock_t *super;
	int result;

	if (!journal || !journal->j_inode)
		return -EINVAL;
	if (journal->j_total_len < JBD2_MIN_JOURNAL_BLOCKS)
		return -EINVAL;
	if (!journal->j_superblock || !journal->j_sb_buffer)
		return -EIO;

	result = ifs_ext3_zero_journal(journal);
	if (result)
		return result;

	super = journal->j_superblock;
	memset(super, 0, sizeof(*super));
	super->s_header.h_magic = cpu_to_be32(JBD2_MAGIC_NUMBER);
	super->s_header.h_blocktype = cpu_to_be32(JBD2_SUPERBLOCK_V2);
	super->s_blocksize = cpu_to_be32(journal->j_blocksize);
	super->s_maxlen = cpu_to_be32(journal->j_total_len);
	super->s_first = cpu_to_be32(1);
	super->s_sequence = cpu_to_be32(1);
	super->s_start = cpu_to_be32(0);
	super->s_feature_incompat =
		cpu_to_be32(JBD2_FEATURE_INCOMPAT_REVOKE);
	super->s_nr_users = cpu_to_be32(1);

	set_buffer_uptodate(journal->j_sb_buffer);
	mark_buffer_dirty(journal->j_sb_buffer);
	result = sync_dirty_buffer(journal->j_sb_buffer);
	if (result)
		return result;

	journal->j_flags &= ~JBD2_ABORT;
	result = jbd2_journal_load(journal);
	if (result)
		return result;

	return jbd2_journal_set_features(
		journal, 0, 0, JBD2_FEATURE_INCOMPAT_REVOKE) ? 0 : -EOPNOTSUPP;
}
