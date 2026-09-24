/*
 * Filesystem Support EXT3 journal revoke engine.
 *
 * A revoke prevents an older journal image from being replayed over a block
 * whose meaning changed later.  Running and committing transactions use
 * separate hash tables so commit serialization never races transaction-side
 * insertion.  Recovery reuses the same table API while the filesystem is
 * still offline.
 */

#include <linux/bio.h>
#include <linux/fs.h>
#include <linux/hash.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/log2.h>
#include <linux/slab.h>

#include "journal.h"

struct ifs_ext3_revoke_record {
	struct list_head link;
	tid_t sequence;
	unsigned int block;
};

struct jbd_revoke_table_s {
	unsigned int bucket_count;
	unsigned int hash_shift;
	struct list_head *buckets;
};

static struct kmem_cache *ifs_ext3_revoke_record_cache;
static struct kmem_cache *ifs_ext3_revoke_table_cache;

static unsigned int ifs_ext3_revoke_bucket(
	const struct jbd_revoke_table_s *table,
	unsigned int block)
{
	return hash_32(block, table->hash_shift);
}

static struct ifs_ext3_revoke_record *
ifs_ext3_find_revoke_locked(
	struct jbd_revoke_table_s *table,
	unsigned int block)
{
	struct ifs_ext3_revoke_record *record;
	const unsigned int bucket =
		ifs_ext3_revoke_bucket(table, block);

	list_for_each_entry(
		record, &table->buckets[bucket], link) {
		if (record->block == block)
			return record;
	}
	return NULL;
}

static struct ifs_ext3_revoke_record *
ifs_ext3_find_revoke(
	journal_t *journal, unsigned int block)
{
	struct ifs_ext3_revoke_record *record;

	spin_lock(&journal->j_revoke_lock);
	record = ifs_ext3_find_revoke_locked(
		journal->j_revoke, block);
	spin_unlock(&journal->j_revoke_lock);
	return record;
}

static int ifs_ext3_add_revoke(
	journal_t *journal,
	unsigned int block,
	tid_t sequence)
{
	struct ifs_ext3_revoke_record *record;

	for (;;) {
		record = kmem_cache_alloc(
			ifs_ext3_revoke_record_cache, GFP_NOFS);
		if (record)
			break;
		if (!journal_oom_retry)
			return -ENOMEM;
		cond_resched();
	}

	record->block = block;
	record->sequence = sequence;
	INIT_LIST_HEAD(&record->link);

	spin_lock(&journal->j_revoke_lock);
	if (ifs_ext3_find_revoke_locked(
		    journal->j_revoke, block)) {
		spin_unlock(&journal->j_revoke_lock);
		kmem_cache_free(
			ifs_ext3_revoke_record_cache, record);
		return -EEXIST;
	}
	list_add(
		&record->link,
		&journal->j_revoke->buckets[
			ifs_ext3_revoke_bucket(
				journal->j_revoke, block)]);
	spin_unlock(&journal->j_revoke_lock);
	return 0;
}

static void ifs_ext3_clear_revoke_table(
	struct jbd_revoke_table_s *table)
{
	unsigned int bucket;

	if (!table)
		return;

	for (bucket = 0U;
	     bucket < table->bucket_count;
	     ++bucket) {
		struct ifs_ext3_revoke_record *record;
		struct ifs_ext3_revoke_record *next;

		list_for_each_entry_safe(
			record, next, &table->buckets[bucket], link) {
			list_del(&record->link);
			kmem_cache_free(
				ifs_ext3_revoke_record_cache, record);
		}
	}
}

void journal_destroy_revoke_caches(void)
{
	if (ifs_ext3_revoke_record_cache) {
		kmem_cache_destroy(
			ifs_ext3_revoke_record_cache);
		ifs_ext3_revoke_record_cache = NULL;
	}
	if (ifs_ext3_revoke_table_cache) {
		kmem_cache_destroy(
			ifs_ext3_revoke_table_cache);
		ifs_ext3_revoke_table_cache = NULL;
	}
}

int __init journal_init_revoke_caches(void)
{
	if (ifs_ext3_revoke_record_cache ||
	    ifs_ext3_revoke_table_cache)
		return -EBUSY;

	ifs_ext3_revoke_record_cache =
		kmem_cache_create(
			"ext3_revoke_record",
			sizeof(struct ifs_ext3_revoke_record),
			0U, SLAB_HWCACHE_ALIGN, NULL);
	if (!ifs_ext3_revoke_record_cache)
		return -ENOMEM;

	ifs_ext3_revoke_table_cache =
		kmem_cache_create(
			"ext3_revoke_table",
			sizeof(struct jbd_revoke_table_s),
			0U, 0U, NULL);
	if (!ifs_ext3_revoke_table_cache) {
		journal_destroy_revoke_caches();
		return -ENOMEM;
	}

	return 0;
}

static struct jbd_revoke_table_s *
ifs_ext3_create_revoke_table(unsigned int bucket_count)
{
	struct jbd_revoke_table_s *table;
	unsigned int bucket;

	if (bucket_count == 0U ||
	    !is_power_of_2(bucket_count))
		return NULL;

	table = kmem_cache_zalloc(
		ifs_ext3_revoke_table_cache, GFP_KERNEL);
	if (!table)
		return NULL;

	table->buckets = kmalloc_array(
		bucket_count,
		sizeof(*table->buckets),
		GFP_KERNEL);
	if (!table->buckets) {
		kmem_cache_free(
			ifs_ext3_revoke_table_cache, table);
		return NULL;
	}

	table->bucket_count = bucket_count;
	table->hash_shift = ilog2(bucket_count);
	for (bucket = 0U;
	     bucket < bucket_count;
	     ++bucket)
		INIT_LIST_HEAD(&table->buckets[bucket]);

	return table;
}

static void ifs_ext3_destroy_revoke_table(
	struct jbd_revoke_table_s *table)
{
	if (!table)
		return;

	ifs_ext3_clear_revoke_table(table);
	kfree(table->buckets);
	kmem_cache_free(
		ifs_ext3_revoke_table_cache, table);
}

int journal_init_revoke(
	journal_t *journal, int hash_size)
{
	if (!journal || hash_size <= 0 ||
	    !is_power_of_2((unsigned int)hash_size))
		return -EINVAL;
	if (journal->j_revoke_table[0] ||
	    journal->j_revoke_table[1])
		return -EBUSY;

	journal->j_revoke_table[0] =
		ifs_ext3_create_revoke_table(
			(unsigned int)hash_size);
	if (!journal->j_revoke_table[0])
		return -ENOMEM;

	journal->j_revoke_table[1] =
		ifs_ext3_create_revoke_table(
			(unsigned int)hash_size);
	if (!journal->j_revoke_table[1]) {
		ifs_ext3_destroy_revoke_table(
			journal->j_revoke_table[0]);
		journal->j_revoke_table[0] = NULL;
		return -ENOMEM;
	}

	spin_lock_init(&journal->j_revoke_lock);
	journal->j_revoke =
		journal->j_revoke_table[1];
	return 0;
}

void journal_destroy_revoke(journal_t *journal)
{
	if (!journal)
		return;

	journal->j_revoke = NULL;
	ifs_ext3_destroy_revoke_table(
		journal->j_revoke_table[0]);
	ifs_ext3_destroy_revoke_table(
		journal->j_revoke_table[1]);
	journal->j_revoke_table[0] = NULL;
	journal->j_revoke_table[1] = NULL;
}

int journal_revoke(
	handle_t *handle,
	unsigned int block,
	struct buffer_head *supplied)
{
	journal_t *journal;
	struct buffer_head *bh = supplied;
	bool borrowed = supplied != NULL;
	int error;

	if (!handle || !handle->h_transaction)
		return -EINVAL;

	journal = handle->h_transaction->t_journal;
	if (!journal_set_features(
		    journal, 0U, 0U,
		    JFS_FEATURE_INCOMPAT_REVOKE))
		return -EINVAL;

	if (!bh)
		bh = __find_get_block(
			journal->j_fs_dev,
			block,
			journal->j_blocksize);

	if (bh) {
		if (buffer_revoked(bh)) {
			if (!borrowed)
				brelse(bh);
			return -EIO;
		}

		set_buffer_revoked(bh);
		set_buffer_revokevalid(bh);
		if (borrowed)
			error = journal_forget(
				handle, supplied);
		else {
			brelse(bh);
			error = 0;
		}
		if (error)
			return error;
	}

	error = ifs_ext3_add_revoke(
		journal, block,
		handle->h_transaction->t_tid);
	if (error == -EEXIST)
		return 0;
	return error;
}

int journal_cancel_revoke(
	handle_t *handle,
	struct journal_head *jh)
{
	journal_t *journal;
	struct buffer_head *bh;
	struct ifs_ext3_revoke_record *record = NULL;
	bool lookup;

	if (!handle || !handle->h_transaction || !jh)
		return 0;

	journal = handle->h_transaction->t_journal;
	bh = jh2bh(jh);

	if (test_set_buffer_revokevalid(bh))
		lookup = test_clear_buffer_revoked(bh);
	else {
		clear_buffer_revoked(bh);
		lookup = true;
	}

	if (lookup) {
		spin_lock(&journal->j_revoke_lock);
		record = ifs_ext3_find_revoke_locked(
			journal->j_revoke,
			(unsigned int)bh->b_blocknr);
		if (record)
			list_del(&record->link);
		spin_unlock(&journal->j_revoke_lock);

		if (record)
			kmem_cache_free(
				ifs_ext3_revoke_record_cache, record);

		{
			struct buffer_head *alias =
				__find_get_block(
					bh->b_bdev,
					bh->b_blocknr,
					bh->b_size);
			if (alias) {
				if (alias != bh)
					clear_buffer_revoked(alias);
				brelse(alias);
			}
		}
	}

	return record != NULL;
}

void journal_clear_buffer_revoked_flags(
	journal_t *journal)
{
	struct jbd_revoke_table_s *table;
	unsigned int bucket;

	if (!journal || !journal->j_revoke)
		return;

	table = journal->j_revoke;
	for (bucket = 0U;
	     bucket < table->bucket_count;
	     ++bucket) {
		struct ifs_ext3_revoke_record *record;

		list_for_each_entry(
			record, &table->buckets[bucket], link) {
			struct buffer_head *bh =
				__find_get_block(
					journal->j_fs_dev,
					record->block,
					journal->j_blocksize);
			if (!bh)
				continue;
			clear_buffer_revoked(bh);
			brelse(bh);
		}
	}
}

void journal_switch_revoke_table(journal_t *journal)
{
	struct jbd_revoke_table_s *next;

	if (!journal)
		return;

	next = journal->j_revoke ==
		journal->j_revoke_table[0] ?
		journal->j_revoke_table[1] :
		journal->j_revoke_table[0];

	ifs_ext3_clear_revoke_table(next);
	journal->j_revoke = next;
}

static struct jbd_revoke_table_s *
ifs_ext3_committing_revoke_table(journal_t *journal)
{
	return journal->j_revoke ==
		journal->j_revoke_table[0] ?
		journal->j_revoke_table[1] :
		journal->j_revoke_table[0];
}

static int ifs_ext3_flush_revoke_descriptor(
	journal_t *journal,
	struct journal_head *descriptor,
	unsigned int used,
	int write_op)
{
	struct buffer_head *bh;
	journal_revoke_header_t *header;

	if (!descriptor)
		return 0;

	bh = jh2bh(descriptor);
	if (is_journal_aborted(journal)) {
		put_bh(bh);
		return -EROFS;
	}

	header =
		(journal_revoke_header_t *)bh->b_data;
	header->r_count = cpu_to_be32(used);
	set_buffer_jwrite(bh);
	set_buffer_dirty(bh);
	write_dirty_buffer(bh, write_op);
	return 0;
}

static int ifs_ext3_append_revoke(
	journal_t *journal,
	transaction_t *transaction,
	struct journal_head **descriptor,
	unsigned int *used,
	unsigned int block,
	int write_op)
{
	struct buffer_head *bh;
	journal_header_t *header;

	if (is_journal_aborted(journal))
		return -EROFS;

	if (*descriptor &&
	    *used + sizeof(__be32) >
		journal->j_blocksize) {
		int error =
			ifs_ext3_flush_revoke_descriptor(
				journal, *descriptor,
				*used, write_op);
		if (error)
			return error;
		*descriptor = NULL;
		*used = 0U;
	}

	if (!*descriptor) {
		*descriptor =
			journal_get_descriptor_buffer(journal);
		if (!*descriptor) {
			journal_abort(journal, -ENOMEM);
			return -ENOMEM;
		}

		bh = jh2bh(*descriptor);
		memset(bh->b_data, 0, journal->j_blocksize);
		header = (journal_header_t *)bh->b_data;
		header->h_magic =
			cpu_to_be32(JFS_MAGIC_NUMBER);
		header->h_blocktype =
			cpu_to_be32(JFS_REVOKE_BLOCK);
		header->h_sequence =
			cpu_to_be32(transaction->t_tid);
		journal_file_buffer(
			*descriptor,
			transaction,
			BJ_LogCtl);
		*used = sizeof(
			journal_revoke_header_t);
	}

	bh = jh2bh(*descriptor);
	*(__be32 *)(bh->b_data + *used) =
		cpu_to_be32(block);
	*used += sizeof(__be32);
	return 0;
}

void journal_write_revoke_records(
	journal_t *journal,
	transaction_t *transaction,
	int write_op)
{
	struct jbd_revoke_table_s *table;
	struct journal_head *descriptor = NULL;
	unsigned int used = 0U;
	unsigned int bucket;
	int error = 0;

	if (!journal || !transaction)
		return;

	table =
		ifs_ext3_committing_revoke_table(journal);
	for (bucket = 0U;
	     bucket < table->bucket_count && !error;
	     ++bucket) {
		struct ifs_ext3_revoke_record *record;
		struct ifs_ext3_revoke_record *next;

		list_for_each_entry_safe(
			record, next,
			&table->buckets[bucket], link) {
			error = ifs_ext3_append_revoke(
				journal, transaction,
				&descriptor, &used,
				record->block, write_op);
			list_del(&record->link);
			kmem_cache_free(
				ifs_ext3_revoke_record_cache,
				record);
			if (error)
				break;
		}
	}

	if (!error && descriptor)
		error = ifs_ext3_flush_revoke_descriptor(
			journal, descriptor,
			used, write_op);

	if (error && !is_journal_aborted(journal))
		journal_abort(journal, error);
}

int journal_set_revoke(
	journal_t *journal,
	unsigned int block,
	tid_t sequence)
{
	struct ifs_ext3_revoke_record *record;
	int error;

	if (!journal || !journal->j_revoke)
		return -EINVAL;

	spin_lock(&journal->j_revoke_lock);
	record = ifs_ext3_find_revoke_locked(
		journal->j_revoke, block);
	if (record) {
		if (tid_gt(sequence, record->sequence))
			record->sequence = sequence;
		spin_unlock(&journal->j_revoke_lock);
		return 0;
	}
	spin_unlock(&journal->j_revoke_lock);

	error = ifs_ext3_add_revoke(
		journal, block, sequence);
	return error == -EEXIST ? 0 : error;
}

int journal_test_revoke(
	journal_t *journal,
	unsigned int block,
	tid_t sequence)
{
	struct ifs_ext3_revoke_record *record;
	int revoked = 0;

	if (!journal || !journal->j_revoke)
		return 0;

	spin_lock(&journal->j_revoke_lock);
	record = ifs_ext3_find_revoke_locked(
		journal->j_revoke, block);
	if (record && !tid_gt(sequence, record->sequence))
		revoked = 1;
	spin_unlock(&journal->j_revoke_lock);
	return revoked;
}

void journal_clear_revoke(journal_t *journal)
{
	if (!journal || !journal->j_revoke)
		return;

	spin_lock(&journal->j_revoke_lock);
	ifs_ext3_clear_revoke_table(
		journal->j_revoke);
	spin_unlock(&journal->j_revoke_lock);
}
