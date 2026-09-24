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
