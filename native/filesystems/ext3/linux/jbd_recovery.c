/*
 * linux/fs/jbd/recovery.c
 *
 * Written by Stephen C. Tweedie <sct@redhat.com>, 1999
 *
 * Copyright 1999-2000 Red Hat Software --- All Rights Reserved
 *
 * This file is part of the Linux kernel and is made available under
 * the terms of the GNU General Public License, version 2, or at your
 * option, any later version, incorporated herein by reference.
 *
 * Journal recovery routines for the generic filesystem journaling code;
 * part of the ext2fs journaling system.
 */

/*
 * EXT3 — Journal recovery
 *
 * Purpose:
 *   Scans and replays a journal after an unclean shutdown, respecting transaction sequence and revoke information.
 *
 * Filesystem model:
 *   This file belongs to a standalone EXT3 VFS implementation with its historical JBD engine embedded in ext3.ko.
 *
 * Correctness focus:
 *   Recovery consumes untrusted persistent log records; every length, sequence and block reference must be validated before replay.
 *
 * Project rules:
 *   - EXT3 requires its journal semantics; it is not an EXT4 compatibility registration.
 *   - Preserve the journal, recovery, ordered/writeback/journal data modes and EXT3 on-disk limits.
 *   - JBD and the metadata cache are private implementation code, not separately deployed modules.
 *
 * Commentary policy:
 *   Comments explain invariants, ownership, persistence ordering and
 *   non-obvious design intent. They deliberately avoid restating C syntax.
 */

#ifndef __KERNEL__
#include "jfs_user.h"
#else
#include <linux/time.h>
#include <linux/fs.h>
#include "journal.h"
#include <linux/errno.h>
#include <linux/blkdev.h>
#endif


/**
 * struct recovery_info - Private EXT3 state/data structure used by journal recovery.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct recovery_info
{
	tid_t		start_transaction;
	tid_t		end_transaction;

	int		nr_replays;
	int		nr_revokes;
	int		nr_revoke_hits;
};


/**
 * enum passtype - Private EXT3 state/value set used by journal recovery.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
enum passtype {PASS_SCAN, PASS_REVOKE, PASS_REPLAY};
static int do_one_pass(journal_t *journal,
				struct recovery_info *info, enum passtype pass);
static int scan_revoke_records(journal_t *, struct buffer_head *,
				tid_t, struct recovery_info *);

#ifdef __KERNEL__


/**
 * journal_brelse_array - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void journal_brelse_array(struct buffer_head *b[], int n)
{
	while (--n >= 0)
		brelse (b[n]);
}


#define MAXBUF 8


/**
 * do_readahead - Implements the do readahead operation within the journal recovery subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int do_readahead(journal_t *journal, unsigned int start)
{
	int err;
	unsigned int max, next;
	unsigned int blocknr;
	struct buffer_head *bh;

	max = start + (128 * 1024 / journal->j_blocksize);
	if (max > journal->j_maxlen)
		max = journal->j_maxlen;

	for (next = start; next < max; next++) {
		err = journal_bmap(journal, next, &blocknr);
		if (err) {
			printk(KERN_ERR "JBD: bad block at offset %u\n", next);
			return err;
		}

		bh = __getblk(journal->j_dev, blocknr, journal->j_blocksize);
		if (!bh)
			return -ENOMEM;

		bh_readahead(bh, REQ_RAHEAD);
		brelse(bh);
	}

	return 0;
}

#endif


/**
 * jread - Implements the jread operation within the journal recovery subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int jread(struct buffer_head **bhp, journal_t *journal,
		 unsigned int offset)
{
	int err;
	unsigned int blocknr;
	struct buffer_head *bh;

	*bhp = NULL;

	if (offset >= journal->j_maxlen) {
		printk(KERN_ERR "JBD: corrupted journal superblock\n");
		return -EIO;
	}

	err = journal_bmap(journal, offset, &blocknr);

	if (err) {
		printk (KERN_ERR "JBD: bad block at offset %u\n",
			offset);
		return err;
	}

	bh = __getblk(journal->j_dev, blocknr, journal->j_blocksize);
	if (!bh)
		return -ENOMEM;

	if (!buffer_uptodate(bh)) {


		if (!buffer_req(bh))
			do_readahead(journal, offset);
		wait_on_buffer(bh);
	}

	if (!buffer_uptodate(bh)) {
		printk (KERN_ERR "JBD: Failed to read block at offset %u\n",
			offset);
		brelse(bh);
		return -EIO;
	}

	*bhp = bh;
	return 0;
}


/**
 * count_tags - Computes derived filesystem state used for validation, accounting or policy decisions.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int count_tags(struct buffer_head *bh, int size)
{
	char *			tagp;
	journal_block_tag_t *	tag;
	int			nr = 0;

	tagp = &bh->b_data[sizeof(journal_header_t)];

	while ((tagp - bh->b_data + sizeof(journal_block_tag_t)) <= size) {
		tag = (journal_block_tag_t *) tagp;

		nr++;
		tagp += sizeof(journal_block_tag_t);
		if (!(tag->t_flags & cpu_to_be32(JFS_FLAG_SAME_UUID)))
			tagp += 16;

		if (tag->t_flags & cpu_to_be32(JFS_FLAG_LAST_TAG))
			break;
	}

	return nr;
}


#define wrap(journal, var)						\
do {									\
	if (var >= (journal)->j_last)					\
		var -= ((journal)->j_last - (journal)->j_first);	\
} while (0)


/**
 * journal_recover - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int journal_recover(journal_t *journal)
{
	int			err, err2;
	journal_superblock_t *	sb;

	struct recovery_info	info;

	memset(&info, 0, sizeof(info));
	sb = journal->j_superblock;


	if (!sb->s_start) {
		jbd_debug(1, "No recovery required, last transaction %d\n",
			  be32_to_cpu(sb->s_sequence));
		journal->j_transaction_sequence = be32_to_cpu(sb->s_sequence) + 1;
		return 0;
	}

	err = do_one_pass(journal, &info, PASS_SCAN);
	if (!err)
		err = do_one_pass(journal, &info, PASS_REVOKE);
	if (!err)
		err = do_one_pass(journal, &info, PASS_REPLAY);

	jbd_debug(1, "JBD: recovery, exit status %d, "
		  "recovered transactions %u to %u\n",
		  err, info.start_transaction, info.end_transaction);
	jbd_debug(1, "JBD: Replayed %d and revoked %d/%d blocks\n",
		  info.nr_replays, info.nr_revoke_hits, info.nr_revokes);


	journal->j_transaction_sequence = ++info.end_transaction;

	journal_clear_revoke(journal);
	err2 = sync_blockdev(journal->j_fs_dev);
	if (!err)
		err = err2;

	if (journal->j_flags & JFS_BARRIER) {
		err2 = blkdev_issue_flush(journal->j_fs_dev);
		if (!err)
			err = err2;
	}

	return err;
}


/**
 * journal_skip_recovery - Participates in crash recovery and reconstruction of durable filesystem state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int journal_skip_recovery(journal_t *journal)
{
	int			err;
	struct recovery_info	info;

	memset (&info, 0, sizeof(info));

	err = do_one_pass(journal, &info, PASS_SCAN);

	if (err) {
		printk(KERN_ERR "JBD: error %d scanning journal\n", err);
		++journal->j_transaction_sequence;
	} else {
#ifdef CONFIG_JBD_DEBUG
		int dropped = info.end_transaction -
			      be32_to_cpu(journal->j_superblock->s_sequence);
		jbd_debug(1,
			  "JBD: ignoring %d transaction%s from the journal.\n",
			  dropped, (dropped == 1) ? "" : "s");
#endif
		journal->j_transaction_sequence = ++info.end_transaction;
	}

	journal->j_tail = 0;
	return err;
}


/**
 * do_one_pass - Implements the do one pass operation within the journal recovery subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int do_one_pass(journal_t *journal,
			struct recovery_info *info, enum passtype pass)
{
	unsigned int		first_commit_ID, next_commit_ID;
	unsigned int		next_log_block;
	int			err, success = 0;
	journal_superblock_t *	sb;
	journal_header_t *	tmp;
	struct buffer_head *	bh;
	unsigned int		sequence;
	int			blocktype;


	sb = journal->j_superblock;
	next_commit_ID = be32_to_cpu(sb->s_sequence);
	next_log_block = be32_to_cpu(sb->s_start);

	first_commit_ID = next_commit_ID;
	if (pass == PASS_SCAN)
		info->start_transaction = first_commit_ID;

	jbd_debug(1, "Starting recovery pass %d\n", pass);


	while (1) {
		int			flags;
		char *			tagp;
		journal_block_tag_t *	tag;
		struct buffer_head *	obh;
		struct buffer_head *	nbh;

		cond_resched();


		if (pass != PASS_SCAN)
			if (tid_geq(next_commit_ID, info->end_transaction))
				break;

		jbd_debug(2, "Scanning for sequence ID %u at %u/%u\n",
			  next_commit_ID, next_log_block, journal->j_last);


		jbd_debug(3, "JBD: checking block %u\n", next_log_block);
		err = jread(&bh, journal, next_log_block);
		if (err)
			goto failed;

		next_log_block++;
		wrap(journal, next_log_block);


		tmp = (journal_header_t *)bh->b_data;

		if (tmp->h_magic != cpu_to_be32(JFS_MAGIC_NUMBER)) {
			brelse(bh);
			break;
		}

		blocktype = be32_to_cpu(tmp->h_blocktype);
		sequence = be32_to_cpu(tmp->h_sequence);
		jbd_debug(3, "Found magic %d, sequence %d\n",
			  blocktype, sequence);

		if (sequence != next_commit_ID) {
			brelse(bh);
			break;
		}


		switch(blocktype) {
		case JFS_DESCRIPTOR_BLOCK:


			if (pass != PASS_REPLAY) {
				next_log_block +=
					count_tags(bh, journal->j_blocksize);
				wrap(journal, next_log_block);
				brelse(bh);
				continue;
			}


			tagp = &bh->b_data[sizeof(journal_header_t)];
			while ((tagp - bh->b_data +sizeof(journal_block_tag_t))
			       <= journal->j_blocksize) {
				unsigned int io_block;

				tag = (journal_block_tag_t *) tagp;
				flags = be32_to_cpu(tag->t_flags);

				io_block = next_log_block++;
				wrap(journal, next_log_block);
				err = jread(&obh, journal, io_block);
				if (err) {


					success = err;
					printk (KERN_ERR
						"JBD: IO error %d recovering "
						"block %u in log\n",
						err, io_block);
				} else {
					unsigned int blocknr;

					J_ASSERT(obh != NULL);
					blocknr = be32_to_cpu(tag->t_blocknr);


					if (journal_test_revoke
					    (journal, blocknr,
					     next_commit_ID)) {
						brelse(obh);
						++info->nr_revoke_hits;
						goto skip_write;
					}


					nbh = __getblk(journal->j_fs_dev,
							blocknr,
							journal->j_blocksize);
					if (nbh == NULL) {
						printk(KERN_ERR
						       "JBD: Out of memory "
						       "during recovery.\n");
						err = -ENOMEM;
						brelse(bh);
						brelse(obh);
						goto failed;
					}

					lock_buffer(nbh);
					memcpy(nbh->b_data, obh->b_data,
							journal->j_blocksize);
					if (flags & JFS_FLAG_ESCAPE) {
						*((__be32 *)nbh->b_data) =
						cpu_to_be32(JFS_MAGIC_NUMBER);
					}

					BUFFER_TRACE(nbh, "marking dirty");
					set_buffer_uptodate(nbh);
					mark_buffer_dirty(nbh);
					BUFFER_TRACE(nbh, "marking uptodate");
					++info->nr_replays;

					unlock_buffer(nbh);
					brelse(obh);
					brelse(nbh);
				}

			skip_write:
				tagp += sizeof(journal_block_tag_t);
				if (!(flags & JFS_FLAG_SAME_UUID))
					tagp += 16;

				if (flags & JFS_FLAG_LAST_TAG)
					break;
			}

			brelse(bh);
			continue;

		case JFS_COMMIT_BLOCK:


			brelse(bh);
			next_commit_ID++;
			continue;

		case JFS_REVOKE_BLOCK:


			if (pass != PASS_REVOKE) {
				brelse(bh);
				continue;
			}

			err = scan_revoke_records(journal, bh,
						  next_commit_ID, info);
			brelse(bh);
			if (err)
				goto failed;
			continue;

		default:
			jbd_debug(3, "Unrecognised magic %d, end of scan.\n",
				  blocktype);
			brelse(bh);
			goto done;
		}
	}

 done:


	if (pass == PASS_SCAN)
		info->end_transaction = next_commit_ID;
	else {


		if (info->end_transaction != next_commit_ID) {
			printk (KERN_ERR "JBD: recovery pass %d ended at "
				"transaction %u, expected %u\n",
				pass, next_commit_ID, info->end_transaction);
			if (!success)
				success = -EIO;
		}
	}

	return success;

 failed:
	return err;
}


/**
 * scan_revoke_records - Implements the scan revoke records operation within the journal recovery subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int scan_revoke_records(journal_t *journal, struct buffer_head *bh,
			       tid_t sequence, struct recovery_info *info)
{
	journal_revoke_header_t *header;
	int offset, max;

	header = (journal_revoke_header_t *) bh->b_data;
	offset = sizeof(journal_revoke_header_t);
	max = be32_to_cpu(header->r_count);

	while (offset < max) {
		unsigned int blocknr;
		int err;

		blocknr = be32_to_cpu(* ((__be32 *) (bh->b_data+offset)));
		offset += 4;
		err = journal_set_revoke(journal, blocknr, sequence);
		if (err)
			return err;
		++info->nr_revokes;
	}
	return 0;
}
