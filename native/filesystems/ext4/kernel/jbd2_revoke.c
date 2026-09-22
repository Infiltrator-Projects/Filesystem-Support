// SPDX-License-Identifier: GPL-2.0+
/*
 * linux/fs/jbd2/revoke.c
 *
 * Written by Stephen C. Tweedie <sct@redhat.com>, 2000
 *
 * Copyright 2000 Red Hat corp --- All Rights Reserved
 *
 * Journal revoke routines for the generic filesystem journaling code;
 * part of the ext2fs journaling system.
 *
 * Revoke is the mechanism used to prevent old log records for deleted
 * metadata from being replayed on top of newer data using the same
 * blocks.  The revoke mechanism is used in two separate places:
 *
 * + Commit: during commit we write the entire list of the current
 *   transaction's revoked blocks to the journal
 *
 * + Recovery: during recovery we record the transaction ID of all
 *   revoked blocks.  If there are multiple revoke records in the log
 *   for a single block, only the last one counts, and if there is a log
 *   entry for a block beyond the last revoke, then that log entry still
 *   gets replayed.
 *
 * We can get interactions between revokes and new log data within a
 * single transaction:
 *
 * Block is revoked and then journaled:
 *   The desired end result is the journaling of the new block, so we
 *   cancel the revoke before the transaction commits.
 *
 * Block is journaled and then revoked:
 *   The revoke must take precedence over the write of the block, so we
 *   need either to cancel the journal entry or to write the revoke
 *   later in the log than the log block.  In this case, we choose the
 *   latter: journaling a block cancels any revoke record for that block
 *   in the current transaction, so any revoke for that block in the
 *   transaction must have happened after the block was journaled and so
 *   the revoke must take precedence.
 *
 * Block is revoked and then written as data:
 *   The data write is allowed to succeed, but the revoke is _not_
 *   cancelled.  We still need to prevent old log records from
 *   overwriting the new data.  We don't even need to clear the revoke
 *   bit here.
 *
 * We cache revoke status of a buffer in the current transaction in b_states
 * bits.  As the name says, revokevalid flag indicates that the cached revoke
 * status of a buffer is valid and we can rely on the cached status.
 *
 * Revoke information on buffers is a tri-state value:
 *
 * RevokeValid clear:	no cached revoke status, need to look it up
 * RevokeValid set, Revoked clear:
 *			buffer has not been revoked, and cancel_revoke
 *			need do nothing.
 * RevokeValid set, Revoked set:
 *			buffer has been revoked.
 *
 * Locking rules:
 * We keep two hash tables of revoke records. One hashtable belongs to the
 * running transaction (is pointed to by journal->j_revoke), the other one
 * belongs to the committing transaction. Accesses to the second hash table
 * happen only from the kjournald and no other thread touches this table.  Also
 * journal_switch_revoke_table() which switches which hashtable belongs to the
 * running and which to the committing transaction is called only from
 * kjournald. Therefore we need no locks when accessing the hashtable belonging
 * to the committing transaction.
 *
 * All users operating on the hash table belonging to the running transaction
 * have a handle to the transaction. Therefore they are safe from kjournald
 * switching hash tables under them. For operations on the lists of entries in
 * the hash table j_revoke_lock is used.
 *
 * Finally, also replay code uses the hash tables but at this moment no one else
 * can touch them (filesystem isn't mounted yet) and hence no locking is
 * needed.
 */

/*
 * EXT4 — JBD2 revoke processing
 *
 * Purpose:
 *   Records blocks whose older logged images must not be replayed during recovery.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   A lost revoke can replay stale metadata; revoke tables therefore belong to the journal's correctness state, not merely its performance state.
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

#ifndef __KERNEL__
#include "jfs_user.h"
#else
#include <linux/time.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/init.h>
#include <linux/bio.h>
#include <linux/log2.h>
#include <linux/hash.h>
#endif

static struct kmem_cache *jbd2_revoke_record_cache;
static struct kmem_cache *jbd2_revoke_table_cache;


/**
 * struct jbd2_revoke_record_s - Private EXT4 state/data structure used by jbd2 revoke processing.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct jbd2_revoke_record_s
{
	struct list_head  hash;
	tid_t		  sequence;
	unsigned long long	  blocknr;
};


/**
 * struct jbd2_revoke_table_s - Private EXT4 state/data structure used by jbd2 revoke processing.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct jbd2_revoke_table_s
{


	int		  hash_size;
	int		  hash_shift;
	struct list_head *hash_table;
};


#ifdef __KERNEL__
static void write_one_revoke_record(transaction_t *,
				    struct list_head *,
				    struct buffer_head **, int *,
				    struct jbd2_revoke_record_s *);
static void flush_descriptor(journal_t *, struct buffer_head *, int);
#endif


/**
 * hash - Implements the hash operation within the jbd2 revoke processing subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int hash(journal_t *journal, unsigned long long block)
{
	return hash_64(block, journal->j_revoke->hash_shift);
}

/**
 * insert_revoke_hash - Implements the insert revoke hash operation within the jbd2 revoke processing subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int insert_revoke_hash(journal_t *journal, unsigned long long blocknr,
			      tid_t seq)
{
	struct list_head *hash_list;
	struct jbd2_revoke_record_s *record;
	gfp_t gfp_mask = GFP_NOFS;

	if (journal_oom_retry)
		gfp_mask |= __GFP_NOFAIL;
	record = kmem_cache_alloc(jbd2_revoke_record_cache, gfp_mask);
	if (!record)
		return -ENOMEM;

	record->sequence = seq;
	record->blocknr = blocknr;
	hash_list = &journal->j_revoke->hash_table[hash(journal, blocknr)];
	spin_lock(&journal->j_revoke_lock);
	list_add(&record->hash, hash_list);
	spin_unlock(&journal->j_revoke_lock);
	return 0;
}


/**
 * find_revoke_record - Locates filesystem state without changing the authoritative persistent representation unless the surrounding API explicitly permits it.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static struct jbd2_revoke_record_s *find_revoke_record(journal_t *journal,
						      unsigned long long blocknr)
{
	struct list_head *hash_list;
	struct jbd2_revoke_record_s *record;

	hash_list = &journal->j_revoke->hash_table[hash(journal, blocknr)];

	spin_lock(&journal->j_revoke_lock);
	record = (struct jbd2_revoke_record_s *) hash_list->next;
	while (&(record->hash) != hash_list) {
		if (record->blocknr == blocknr) {
			spin_unlock(&journal->j_revoke_lock);
			return record;
		}
		record = (struct jbd2_revoke_record_s *) record->hash.next;
	}
	spin_unlock(&journal->j_revoke_lock);
	return NULL;
}

/**
 * jbd2_journal_destroy_revoke_record_cache - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void jbd2_journal_destroy_revoke_record_cache(void)
{
	kmem_cache_destroy(jbd2_revoke_record_cache);
	jbd2_revoke_record_cache = NULL;
}

/**
 * jbd2_journal_destroy_revoke_table_cache - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void jbd2_journal_destroy_revoke_table_cache(void)
{
	kmem_cache_destroy(jbd2_revoke_table_cache);
	jbd2_revoke_table_cache = NULL;
}

/**
 * jbd2_journal_init_revoke_record_cache - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int __init jbd2_journal_init_revoke_record_cache(void)
{
	J_ASSERT(!jbd2_revoke_record_cache);
	jbd2_revoke_record_cache = KMEM_CACHE(jbd2_revoke_record_s,
					SLAB_HWCACHE_ALIGN|SLAB_TEMPORARY);

	if (!jbd2_revoke_record_cache) {
		pr_emerg("JBD2: failed to create revoke_record cache\n");
		return -ENOMEM;
	}
	return 0;
}

/**
 * jbd2_journal_init_revoke_table_cache - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int __init jbd2_journal_init_revoke_table_cache(void)
{
	J_ASSERT(!jbd2_revoke_table_cache);
	jbd2_revoke_table_cache = KMEM_CACHE(jbd2_revoke_table_s,
					     SLAB_TEMPORARY);
	if (!jbd2_revoke_table_cache) {
		pr_emerg("JBD2: failed to create revoke_table cache\n");
		return -ENOMEM;
	}
	return 0;
}

/**
 * jbd2_journal_init_revoke_table - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static struct jbd2_revoke_table_s *jbd2_journal_init_revoke_table(int hash_size)
{
	int shift = 0;
	int tmp = hash_size;
	struct jbd2_revoke_table_s *table;

	table = kmem_cache_alloc(jbd2_revoke_table_cache, GFP_KERNEL);
	if (!table)
		goto out;

	while((tmp >>= 1UL) != 0UL)
		shift++;

	table->hash_size = hash_size;
	table->hash_shift = shift;
	table->hash_table =
		kmalloc_array(hash_size, sizeof(struct list_head), GFP_KERNEL);
	if (!table->hash_table) {
		kmem_cache_free(jbd2_revoke_table_cache, table);
		table = NULL;
		goto out;
	}

	for (tmp = 0; tmp < hash_size; tmp++)
		INIT_LIST_HEAD(&table->hash_table[tmp]);

out:
	return table;
}

/**
 * jbd2_journal_destroy_revoke_table - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void jbd2_journal_destroy_revoke_table(struct jbd2_revoke_table_s *table)
{
	int i;
	struct list_head *hash_list;

	for (i = 0; i < table->hash_size; i++) {
		hash_list = &table->hash_table[i];
		J_ASSERT(list_empty(hash_list));
	}

	kfree(table->hash_table);
	kmem_cache_free(jbd2_revoke_table_cache, table);
}


/**
 * jbd2_journal_init_revoke - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int jbd2_journal_init_revoke(journal_t *journal, int hash_size)
{
	J_ASSERT(journal->j_revoke_table[0] == NULL);
	J_ASSERT(is_power_of_2(hash_size));

	journal->j_revoke_table[0] = jbd2_journal_init_revoke_table(hash_size);
	if (!journal->j_revoke_table[0])
		goto fail0;

	journal->j_revoke_table[1] = jbd2_journal_init_revoke_table(hash_size);
	if (!journal->j_revoke_table[1])
		goto fail1;

	journal->j_revoke = journal->j_revoke_table[1];

	spin_lock_init(&journal->j_revoke_lock);

	return 0;

fail1:
	jbd2_journal_destroy_revoke_table(journal->j_revoke_table[0]);
	journal->j_revoke_table[0] = NULL;
fail0:
	return -ENOMEM;
}


/**
 * jbd2_journal_destroy_revoke - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void jbd2_journal_destroy_revoke(journal_t *journal)
{
	journal->j_revoke = NULL;
	if (journal->j_revoke_table[0])
		jbd2_journal_destroy_revoke_table(journal->j_revoke_table[0]);
	if (journal->j_revoke_table[1])
		jbd2_journal_destroy_revoke_table(journal->j_revoke_table[1]);
}


#ifdef __KERNEL__


/**
 * jbd2_journal_revoke - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int jbd2_journal_revoke(handle_t *handle, unsigned long long blocknr,
		   struct buffer_head *bh_in)
{
	struct buffer_head *bh = NULL;
	journal_t *journal;
	struct block_device *bdev;
	int err;

	might_sleep();
	if (bh_in)
		BUFFER_TRACE(bh_in, "enter");

	journal = handle->h_transaction->t_journal;
	if (!jbd2_journal_set_features(journal, 0, 0, JBD2_FEATURE_INCOMPAT_REVOKE)){
		J_ASSERT (!"Cannot set revoke feature!");
		return -EINVAL;
	}

	bdev = journal->j_fs_dev;
	bh = bh_in;

	if (!bh) {
		bh = __find_get_block_nonatomic(bdev, blocknr,
						journal->j_blocksize);
		if (bh)
			BUFFER_TRACE(bh, "found on hash");
	}
#ifdef JBD2_EXPENSIVE_CHECKING
	else {
		struct buffer_head *bh2;


		bh2 = __find_get_block_nonatomic(bdev, blocknr,
						 journal->j_blocksize);
		if (bh2) {

			if (bh2 != bh && buffer_revokevalid(bh2))


				J_ASSERT_BH(bh2, buffer_revoked(bh2));
			put_bh(bh2);
		}
	}
#endif

	if (WARN_ON_ONCE(handle->h_revoke_credits <= 0)) {
		if (!bh_in)
			brelse(bh);
		return -EIO;
	}


	if (bh) {
		if (!J_EXPECT_BH(bh, !buffer_revoked(bh),
				 "inconsistent data on disk")) {
			if (!bh_in)
				brelse(bh);
			return -EIO;
		}
		set_buffer_revoked(bh);
		set_buffer_revokevalid(bh);
		if (bh_in) {
			BUFFER_TRACE(bh_in, "call jbd2_journal_forget");
			jbd2_journal_forget(handle, bh_in);
		} else {
			BUFFER_TRACE(bh, "call brelse");
			__brelse(bh);
		}
	}
	handle->h_revoke_credits--;

	jbd2_debug(2, "insert revoke for block %llu, bh_in=%p\n",blocknr, bh_in);
	err = insert_revoke_hash(journal, blocknr,
				handle->h_transaction->t_tid);
	BUFFER_TRACE(bh_in, "exit");
	return err;
}


/**
 * jbd2_journal_cancel_revoke - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int jbd2_journal_cancel_revoke(handle_t *handle, struct journal_head *jh)
{
	struct jbd2_revoke_record_s *record;
	journal_t *journal = handle->h_transaction->t_journal;
	int need_cancel;
	int did_revoke = 0;
	struct buffer_head *bh = jh2bh(jh);
	struct address_space *bh_mapping = bh->b_folio->mapping;

	jbd2_debug(4, "journal_head %p, cancelling revoke\n", jh);


	if (test_set_buffer_revokevalid(bh)) {
		need_cancel = test_clear_buffer_revoked(bh);
	} else {
		need_cancel = 1;
		clear_buffer_revoked(bh);
	}

	if (need_cancel) {
		record = find_revoke_record(journal, bh->b_blocknr);
		if (record) {
			jbd2_debug(4, "cancelled existing revoke on "
				  "blocknr %llu\n", (unsigned long long)bh->b_blocknr);
			spin_lock(&journal->j_revoke_lock);
			list_del(&record->hash);
			spin_unlock(&journal->j_revoke_lock);
			kmem_cache_free(jbd2_revoke_record_cache, record);
			did_revoke = 1;
		}
	}

#ifdef JBD2_EXPENSIVE_CHECKING

	record = find_revoke_record(journal, bh->b_blocknr);
	J_ASSERT_JH(jh, record == NULL);
#endif


	if (need_cancel && !sb_is_blkdev_sb(bh_mapping->host->i_sb)) {
		struct buffer_head *bh2;

		bh2 = __find_get_block_nonatomic(bh->b_bdev, bh->b_blocknr,
						 bh->b_size);
		if (bh2) {
			WARN_ON_ONCE(bh2 == bh);
			clear_buffer_revoked(bh2);
			__brelse(bh2);
		}
	}
	return did_revoke;
}


/**
 * jbd2_clear_buffer_revoked_flags - Implements the clear buffer revoked flags operation within the jbd2 revoke processing subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void jbd2_clear_buffer_revoked_flags(journal_t *journal)
{
	struct jbd2_revoke_table_s *revoke = journal->j_revoke;
	int i = 0;

	for (i = 0; i < revoke->hash_size; i++) {
		struct list_head *hash_list;
		struct list_head *list_entry;
		hash_list = &revoke->hash_table[i];

		list_for_each(list_entry, hash_list) {
			struct jbd2_revoke_record_s *record;
			struct buffer_head *bh;
			record = (struct jbd2_revoke_record_s *)list_entry;
			bh = __find_get_block_nonatomic(journal->j_fs_dev,
							record->blocknr,
							journal->j_blocksize);
			if (bh) {
				clear_buffer_revoked(bh);
				__brelse(bh);
			}
		}
	}
}


/**
 * jbd2_journal_switch_revoke_table - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void jbd2_journal_switch_revoke_table(journal_t *journal)
{
	int i;

	if (journal->j_revoke == journal->j_revoke_table[0])
		journal->j_revoke = journal->j_revoke_table[1];
	else
		journal->j_revoke = journal->j_revoke_table[0];

	for (i = 0; i < journal->j_revoke->hash_size; i++)
		INIT_LIST_HEAD(&journal->j_revoke->hash_table[i]);
}


/**
 * jbd2_journal_write_revoke_records - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void jbd2_journal_write_revoke_records(transaction_t *transaction,
				       struct list_head *log_bufs)
{
	journal_t *journal = transaction->t_journal;
	struct buffer_head *descriptor;
	struct jbd2_revoke_record_s *record;
	struct jbd2_revoke_table_s *revoke;
	struct list_head *hash_list;
	int i, offset, count;

	descriptor = NULL;
	offset = 0;
	count = 0;


	revoke = journal->j_revoke == journal->j_revoke_table[0] ?
		journal->j_revoke_table[1] : journal->j_revoke_table[0];

	for (i = 0; i < revoke->hash_size; i++) {
		hash_list = &revoke->hash_table[i];

		while (!list_empty(hash_list)) {
			record = (struct jbd2_revoke_record_s *)
				hash_list->next;
			write_one_revoke_record(transaction, log_bufs,
						&descriptor, &offset, record);
			count++;
			list_del(&record->hash);
			kmem_cache_free(jbd2_revoke_record_cache, record);
		}
	}
	if (descriptor)
		flush_descriptor(journal, descriptor, offset);
	jbd2_debug(1, "Wrote %d revoke records\n", count);
}


/**
 * write_one_revoke_record - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void write_one_revoke_record(transaction_t *transaction,
				    struct list_head *log_bufs,
				    struct buffer_head **descriptorp,
				    int *offsetp,
				    struct jbd2_revoke_record_s *record)
{
	journal_t *journal = transaction->t_journal;
	int csum_size = 0;
	struct buffer_head *descriptor;
	int sz, offset;


	if (is_journal_aborted(journal))
		return;

	descriptor = *descriptorp;
	offset = *offsetp;


	if (jbd2_journal_has_csum_v2or3(journal))
		csum_size = sizeof(struct jbd2_journal_block_tail);

	if (jbd2_has_feature_64bit(journal))
		sz = 8;
	else
		sz = 4;


	if (descriptor) {
		if (offset + sz > journal->j_blocksize - csum_size) {
			flush_descriptor(journal, descriptor, offset);
			descriptor = NULL;
		}
	}

	if (!descriptor) {
		descriptor = jbd2_journal_get_descriptor_buffer(transaction,
							JBD2_REVOKE_BLOCK);
		if (!descriptor)
			return;


		BUFFER_TRACE(descriptor, "file in log_bufs");
		jbd2_file_log_bh(log_bufs, descriptor);

		offset = sizeof(jbd2_journal_revoke_header_t);
		*descriptorp = descriptor;
	}

	if (jbd2_has_feature_64bit(journal))
		* ((__be64 *)(&descriptor->b_data[offset])) =
			cpu_to_be64(record->blocknr);
	else
		* ((__be32 *)(&descriptor->b_data[offset])) =
			cpu_to_be32(record->blocknr);
	offset += sz;

	*offsetp = offset;
}


/**
 * flush_descriptor - Drives pending state toward the durability guarantee required by the calling VFS or journal interface.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void flush_descriptor(journal_t *journal,
			     struct buffer_head *descriptor,
			     int offset)
{
	jbd2_journal_revoke_header_t *header;

	if (is_journal_aborted(journal))
		return;

	header = (jbd2_journal_revoke_header_t *)descriptor->b_data;
	header->r_count = cpu_to_be32(offset);
	jbd2_descriptor_block_csum_set(journal, descriptor);

	set_buffer_jwrite(descriptor);
	BUFFER_TRACE(descriptor, "write");
	set_buffer_dirty(descriptor);
	write_dirty_buffer(descriptor, JBD2_JOURNAL_REQ_FLAGS);
}
#endif


/**
 * jbd2_journal_set_revoke - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int jbd2_journal_set_revoke(journal_t *journal,
		       unsigned long long blocknr,
		       tid_t sequence)
{
	struct jbd2_revoke_record_s *record;

	record = find_revoke_record(journal, blocknr);
	if (record) {


		if (tid_gt(sequence, record->sequence))
			record->sequence = sequence;
		return 0;
	}
	return insert_revoke_hash(journal, blocknr, sequence);
}


/**
 * jbd2_journal_test_revoke - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int jbd2_journal_test_revoke(journal_t *journal,
			unsigned long long blocknr,
			tid_t sequence)
{
	struct jbd2_revoke_record_s *record;

	record = find_revoke_record(journal, blocknr);
	if (!record)
		return 0;
	if (tid_gt(sequence, record->sequence))
		return 0;
	return 1;
}


/**
 * jbd2_journal_clear_revoke - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void jbd2_journal_clear_revoke(journal_t *journal)
{
	int i;
	struct list_head *hash_list;
	struct jbd2_revoke_record_s *record;
	struct jbd2_revoke_table_s *revoke;

	revoke = journal->j_revoke;

	for (i = 0; i < revoke->hash_size; i++) {
		hash_list = &revoke->hash_table[i];
		while (!list_empty(hash_list)) {
			record = (struct jbd2_revoke_record_s*) hash_list->next;
			list_del(&record->hash);
			kmem_cache_free(jbd2_revoke_record_cache, record);
		}
	}
}
