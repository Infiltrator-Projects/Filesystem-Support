/* Infiltrator Filesystem Support — EXT4 Linux volume administration.
 * Physical mapping queries, multiple-mount protection and sysfs exposure are
 * grouped as host administration around the canonical EXT4 engine.
 */

#include "ext4.h"
#include "fsmap.h"
#include "mballoc.h"

#include <linux/fsmap.h>
#include <linux/list_sort.h>
#include <linux/sort.h>
#include <trace/events/ext4.h>

struct ext4_fsmap_query {
	struct ext4_fsmap_head *head;
	ext4_fsmap_format_t format;
	void *format_arg;
	ext4_fsblk_t next_block;
	u32 device;
	ext4_group_t group;
	struct ext4_fsmap low;
	struct ext4_fsmap high;
	struct ext4_fsmap pending_free;
	struct list_head metadata;
	bool final_record;
};

struct ext4_fsmap_device {
	int (*query)(struct super_block *, struct ext4_fsmap *,
		     struct ext4_fsmap_query *);
	u32 device;
};

void ext4_fsmap_from_internal(struct super_block *sb, struct fsmap *dst,
			      struct ext4_fsmap *src)
{
	dst->fmr_device = src->fmr_device;
	dst->fmr_flags = src->fmr_flags;
	dst->fmr_physical = src->fmr_physical << sb->s_blocksize_bits;
	dst->fmr_owner = src->fmr_owner;
	dst->fmr_offset = 0;
	dst->fmr_length = src->fmr_length << sb->s_blocksize_bits;
	memset(dst->fmr_reserved, 0, sizeof(dst->fmr_reserved));
}

void ext4_fsmap_to_internal(struct super_block *sb, struct ext4_fsmap *dst,
			    struct fsmap *src)
{
	dst->fmr_device = src->fmr_device;
	dst->fmr_flags = src->fmr_flags;
	dst->fmr_physical = src->fmr_physical >> sb->s_blocksize_bits;
	dst->fmr_owner = src->fmr_owner;
	dst->fmr_length = src->fmr_length >> sb->s_blocksize_bits;
}

static int ext4_fsmap_device_compare(const void *a, const void *b)
{
	const struct ext4_fsmap_device *left = a;
	const struct ext4_fsmap_device *right = b;

	if (left->device < right->device)
		return -1;
	if (left->device > right->device)
		return 1;
	return 0;
}

static ext4_fsblk_t ext4_fsmap_end(const struct ext4_fsmap *record)
{
	return record->fmr_physical + record->fmr_length;
}

static bool ext4_fsmap_before_low(const struct ext4_fsmap_query *query,
				  const struct ext4_fsmap *record)
{
	return ext4_fsmap_end(record) <= query->low.fmr_physical;
}

static int ext4_fsmap_emit(struct super_block *sb,
			    struct ext4_fsmap_query *query,
			    struct ext4_fsmap *record)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_fsmap out;
	ext4_group_t group;
	ext4_grpblk_t cluster;
	ext4_fsblk_t end;
	int err;

	if (fatal_signal_pending(current))
		return -EINTR;

	if (ext4_fsmap_before_low(query, record)) {
		end = ext4_fsmap_end(record);
		if (query->next_block < end)
			query->next_block = end;
		return EXT4_QUERY_RANGE_CONTINUE;
	}

	if (query->head->fmh_count == 0) {
		if (query->head->fmh_entries == UINT_MAX)
			return EXT4_QUERY_RANGE_ABORT;
		if (record->fmr_physical > query->next_block)
			query->head->fmh_entries++;
		if (!query->final_record)
			query->head->fmh_entries++;

		end = ext4_fsmap_end(record);
		if (query->next_block < end)
			query->next_block = end;
		return EXT4_QUERY_RANGE_CONTINUE;
	}

	if (record->fmr_physical > query->next_block) {
		if (query->head->fmh_entries >= query->head->fmh_count)
			return EXT4_QUERY_RANGE_ABORT;

		ext4_get_group_no_and_offset(
			sb, query->next_block, &group, &cluster);
		trace_ext4_fsmap_mapping(
			sb, query->device, group, EXT4_C2B(sbi, cluster),
			record->fmr_physical - query->next_block,
			EXT4_FMR_OWN_UNKNOWN);

		memset(&out, 0, sizeof(out));
		out.fmr_device = query->device;
		out.fmr_physical = query->next_block;
		out.fmr_owner = EXT4_FMR_OWN_UNKNOWN;
		out.fmr_length = record->fmr_physical - query->next_block;
		out.fmr_flags = FMR_OF_SPECIAL_OWNER;
		err = query->format(&out, query->format_arg);
		if (err)
			return err;
		query->head->fmh_entries++;
	}

	if (!query->final_record) {
		if (query->head->fmh_entries >= query->head->fmh_count)
			return EXT4_QUERY_RANGE_ABORT;

		ext4_get_group_no_and_offset(
			sb, record->fmr_physical, &group, &cluster);
		trace_ext4_fsmap_mapping(
			sb, query->device, group, EXT4_C2B(sbi, cluster),
			record->fmr_length, record->fmr_owner);

		out = *record;
		out.fmr_device = query->device;
		out.fmr_flags = FMR_OF_SPECIAL_OWNER;
		err = query->format(&out, query->format_arg);
		if (err)
			return err;
		query->head->fmh_entries++;
	}

	end = ext4_fsmap_end(record);
	if (query->next_block < end)
		query->next_block = end;
	return EXT4_QUERY_RANGE_CONTINUE;
}

static int ext4_fsmap_metadata_compare(void *priv,
				       const struct list_head *a,
				       const struct list_head *b)
{
	const struct ext4_fsmap *left =
		container_of(a, struct ext4_fsmap, fmr_list);
	const struct ext4_fsmap *right =
		container_of(b, struct ext4_fsmap, fmr_list);

	if (left->fmr_physical < right->fmr_physical)
		return -1;
	if (left->fmr_physical > right->fmr_physical)
		return 1;
	return 0;
}

static void ext4_fsmap_free_metadata(struct list_head *head)
{
	struct ext4_fsmap *record;
	struct ext4_fsmap *next;

	list_for_each_entry_safe(record, next, head, fmr_list) {
		list_del(&record->fmr_list);
		kfree(record);
	}
}

static int ext4_fsmap_add_metadata(struct list_head *head,
				   ext4_fsblk_t block,
				   ext4_fsblk_t length,
				   u64 owner)
{
	struct ext4_fsmap *record;

	if (!length)
		return 0;

	record = kzalloc(sizeof(*record), GFP_NOFS);
	if (!record)
		return -ENOMEM;

	record->fmr_physical = block;
	record->fmr_length = length;
	record->fmr_owner = owner;
	list_add_tail(&record->fmr_list, head);
	return 0;
}

static int ext4_fsmap_add_group_prefix(struct super_block *sb,
				       ext4_group_t group,
				       struct list_head *head)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_fsblk_t block = ext4_group_first_block_no(sb, group);
	const unsigned long meta_group =
		group / EXT4_DESC_PER_BLOCK(sb);
	const unsigned long first_meta_group =
		le32_to_cpu(sbi->s_es->s_first_meta_bg);
	unsigned long gdt_blocks;
	unsigned long reserved;
	int err;

	if (ext4_bg_has_super(sb, group)) {
		err = ext4_fsmap_add_metadata(
			head, block, 1, EXT4_FMR_OWN_FS);
		if (err)
			return err;
		block++;
	}

	gdt_blocks = ext4_bg_num_gdb(sb, group);
	err = ext4_fsmap_add_metadata(
		head, block, gdt_blocks, EXT4_FMR_OWN_GDT);
	if (err)
		return err;
	block += gdt_blocks;

	if (ext4_has_feature_meta_bg(sb) &&
	    meta_group >= first_meta_group)
		return 0;

	reserved = le16_to_cpu(sbi->s_es->s_reserved_gdt_blocks);
	return ext4_fsmap_add_metadata(
		head, block, reserved, EXT4_FMR_OWN_RESV_GDT);
}

static void ext4_fsmap_merge_metadata(struct list_head *head)
{
	struct ext4_fsmap *record;
	struct ext4_fsmap *next;
	struct ext4_fsmap *previous = NULL;

	list_for_each_entry_safe(record, next, head, fmr_list) {
		if (previous &&
		    previous->fmr_owner == record->fmr_owner &&
		    ext4_fsmap_end(previous) == record->fmr_physical) {
			previous->fmr_length += record->fmr_length;
			list_del(&record->fmr_list);
			kfree(record);
			continue;
		}
		previous = record;
	}
}

static int ext4_fsmap_build_metadata(struct super_block *sb,
				     struct list_head *head)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_group_t group;
	int err;

	INIT_LIST_HEAD(head);
	for (group = 0; group < sbi->s_groups_count; ++group) {
		struct ext4_group_desc *desc =
			ext4_get_group_desc(sb, group, NULL);

		if (!desc) {
			err = -EFSCORRUPTED;
			goto fail;
		}

		err = ext4_fsmap_add_group_prefix(sb, group, head);
		if (err)
			goto fail;

		err = ext4_fsmap_add_metadata(
			head, ext4_block_bitmap(sb, desc), 1,
			EXT4_FMR_OWN_BLKBM);
		if (err)
			goto fail;

		err = ext4_fsmap_add_metadata(
			head, ext4_inode_bitmap(sb, desc), 1,
			EXT4_FMR_OWN_INOBM);
		if (err)
			goto fail;

		err = ext4_fsmap_add_metadata(
			head, ext4_inode_table(sb, desc),
			sbi->s_itb_per_group, EXT4_FMR_OWN_INODES);
		if (err)
			goto fail;
	}

	list_sort(NULL, head, ext4_fsmap_metadata_compare);
	ext4_fsmap_merge_metadata(head);
	return 0;

fail:
	ext4_fsmap_free_metadata(head);
	return err;
}

static int ext4_fsmap_flush_metadata_before(
	struct super_block *sb, struct ext4_fsmap_query *query,
	ext4_fsblk_t block)
{
	struct ext4_fsmap *record;
	struct ext4_fsmap *next;
	int err;

	list_for_each_entry_safe(record, next, &query->metadata, fmr_list) {
		if (ext4_fsmap_end(record) <= query->next_block) {
			list_del(&record->fmr_list);
			kfree(record);
			continue;
		}
		if (record->fmr_physical >= block)
			break;

		err = ext4_fsmap_emit(sb, query, record);
		if (err)
			return err;
		list_del(&record->fmr_list);
		kfree(record);
	}

	return 0;
}

static int ext4_fsmap_metadata_query(struct super_block *sb,
				     ext4_group_t group,
				     ext4_grpblk_t start,
				     ext4_grpblk_t length,
				     void *private)
{
	struct ext4_fsmap_query *query = private;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	const ext4_fsblk_t first =
		ext4_group_first_block_no(sb, group) +
		EXT4_C2B(sbi, start);
	const ext4_fsblk_t end =
		first + EXT4_C2B(sbi, length);
	struct ext4_fsmap *record;
	struct ext4_fsmap *next;
	int err;

	list_for_each_entry_safe(record, next, &query->metadata, fmr_list) {
		if (ext4_fsmap_end(record) <= query->next_block) {
			list_del(&record->fmr_list);
			kfree(record);
			continue;
		}

		if (record->fmr_physical > end ||
		    ext4_fsmap_end(record) <= first)
			continue;

		if (query->pending_free.fmr_owner) {
			err = ext4_fsmap_emit(
				sb, query, &query->pending_free);
			if (err)
				return err;
			query->pending_free.fmr_owner = 0;
		}

		err = ext4_fsmap_emit(sb, query, record);
		if (err)
			return err;
		list_del(&record->fmr_list);
		kfree(record);
	}

	if (query->next_block < first)
		query->next_block = first;
	return 0;
}

static int ext4_fsmap_free_query(struct super_block *sb,
				 ext4_group_t group,
				 ext4_grpblk_t start,
				 ext4_grpblk_t length,
				 void *private)
{
	struct ext4_fsmap_query *query = private;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	const ext4_fsblk_t first =
		ext4_group_first_block_no(sb, group) +
		EXT4_C2B(sbi, start);
	const ext4_fsblk_t blocks = EXT4_C2B(sbi, length);
	struct ext4_fsmap record = {
		.fmr_physical = first,
		.fmr_length = blocks,
		.fmr_owner = EXT4_FMR_OWN_FREE,
	};
	int err;

	if (query->pending_free.fmr_owner &&
	    ext4_fsmap_end(&query->pending_free) == first) {
		query->pending_free.fmr_length += blocks;
		return 0;
	}

	if (query->pending_free.fmr_owner) {
		err = ext4_fsmap_emit(sb, query, &query->pending_free);
		if (err)
			return err;
		query->pending_free.fmr_owner = 0;
	}

	err = ext4_fsmap_flush_metadata_before(sb, query, first);
	if (err)
		return err;

	if (ext4_fsmap_end(&record) ==
	    ext4_group_first_block_no(sb, group + 1)) {
		query->pending_free = record;
		return 0;
	}

	return ext4_fsmap_emit(sb, query, &record);
}

static int ext4_fsmap_query_log(struct super_block *sb,
				struct ext4_fsmap *keys,
				struct ext4_fsmap_query *query)
{
	journal_t *journal = EXT4_SB(sb)->s_journal;
	struct ext4_fsmap record;

	query->low = keys[0];
	query->low.fmr_length = 0;
	memset(&query->high, 0xff, sizeof(query->high));

	if (keys[0].fmr_physical > 0)
		return 0;

	memset(&record, 0, sizeof(record));
	record.fmr_physical = journal->j_blk_offset;
	record.fmr_length = journal->j_total_len;
	record.fmr_owner = EXT4_FMR_OWN_LOG;
	return ext4_fsmap_emit(sb, query, &record);
}

static int ext4_fsmap_query_data(struct super_block *sb,
				 struct ext4_fsmap *keys,
				 struct ext4_fsmap_query *query)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	const ext4_fsblk_t first_data =
		le32_to_cpu(sbi->s_es->s_first_data_block);
	const ext4_fsblk_t block_count =
		ext4_blocks_count(sbi->s_es);
	ext4_group_t first_group;
	ext4_group_t last_group;
	ext4_grpblk_t first_cluster;
	ext4_grpblk_t last_cluster;
	ext4_fsblk_t end_block;
	int err;

	if (keys[0].fmr_physical >= block_count)
		return 0;
	if (keys[0].fmr_physical < first_data)
		keys[0].fmr_physical = first_data;
	if (keys[1].fmr_physical >= block_count)
		keys[1].fmr_physical = block_count - 1;
	if (keys[1].fmr_physical < keys[0].fmr_physical)
		return 0;

	ext4_get_group_no_and_offset(
		sb, keys[0].fmr_physical,
		&first_group, &first_cluster);
	ext4_get_group_no_and_offset(
		sb, keys[1].fmr_physical,
		&last_group, &last_cluster);

	query->low = keys[0];
	query->low.fmr_physical = EXT4_C2B(sbi, first_cluster);
	query->low.fmr_length = 0;
	memset(&query->high, 0xff, sizeof(query->high));

	err = ext4_fsmap_build_metadata(sb, &query->metadata);
	if (err)
		return err;

	for (query->group = first_group;
	     query->group <= last_group;
	     ++query->group) {
		if (query->group == last_group) {
			query->high = keys[1];
			query->high.fmr_physical =
				EXT4_C2B(sbi, last_cluster);
			query->high.fmr_length = 0;
		}

		err = ext4_mballoc_query_range(
			sb, query->group,
			EXT4_B2C(sbi, query->low.fmr_physical),
			EXT4_B2C(sbi, query->high.fmr_physical),
			ext4_fsmap_metadata_query,
			ext4_fsmap_free_query, query);
		if (err)
			goto out;

		if (query->group == first_group)
			memset(&query->low, 0, sizeof(query->low));
	}

	if (query->pending_free.fmr_owner) {
		err = ext4_fsmap_emit(
			sb, query, &query->pending_free);
		if (err)
			goto out;
	}

	end_block = keys[1].fmr_physical + 1;
	{
		struct ext4_fsmap sentinel = {
			.fmr_physical = end_block,
			.fmr_owner = EXT4_FMR_OWN_FREE,
		};
		query->final_record = true;
		err = ext4_fsmap_emit(sb, query, &sentinel);
	}

out:
	ext4_fsmap_free_metadata(&query->metadata);
	return err;
}

static bool ext4_fsmap_device_valid(struct super_block *sb,
				    const struct ext4_fsmap *key)
{
	if (key->fmr_device == 0 ||
	    key->fmr_device == UINT_MAX ||
	    key->fmr_device == new_encode_dev(sb->s_bdev->bd_dev))
		return true;

	return EXT4_SB(sb)->s_journal_bdev_file &&
	       key->fmr_device == new_encode_dev(
		       file_bdev(EXT4_SB(sb)->s_journal_bdev_file)->bd_dev);
}

static bool ext4_fsmap_keys_ordered(const struct ext4_fsmap *low,
				    const struct ext4_fsmap *high)
{
	if (low->fmr_device != high->fmr_device)
		return low->fmr_device < high->fmr_device;
	if (low->fmr_physical != high->fmr_physical)
		return low->fmr_physical < high->fmr_physical;
	return low->fmr_owner < high->fmr_owner;
}

#define EXT4_FSMAP_DEVICE_COUNT 2

int ext4_getfsmap(struct super_block *sb, struct ext4_fsmap_head *head,
		  ext4_fsmap_format_t formatter, void *arg)
{
	struct ext4_fsmap keys[2];
	struct ext4_fsmap_device devices[EXT4_FSMAP_DEVICE_COUNT] = { 0 };
	struct ext4_fsmap_query query = {
		.head = head,
		.format = formatter,
		.format_arg = arg,
	};
	int i;
	int err = 0;

	if (head->fmh_iflags & ~FMH_IF_VALID)
		return -EINVAL;
	if (!ext4_fsmap_device_valid(sb, &head->fmh_keys[0]) ||
	    !ext4_fsmap_device_valid(sb, &head->fmh_keys[1]))
		return -EINVAL;

	head->fmh_entries = 0;
	devices[0].device = new_encode_dev(sb->s_bdev->bd_dev);
	devices[0].query = ext4_fsmap_query_data;

	if (EXT4_SB(sb)->s_journal_bdev_file) {
		devices[1].device = new_encode_dev(
			file_bdev(EXT4_SB(sb)->s_journal_bdev_file)->bd_dev);
		devices[1].query = ext4_fsmap_query_log;
	}

	sort(devices, EXT4_FSMAP_DEVICE_COUNT,
	     sizeof(devices[0]), ext4_fsmap_device_compare, NULL);

	keys[0] = head->fmh_keys[0];
	keys[0].fmr_physical += keys[0].fmr_length;
	keys[0].fmr_owner = 0;
	keys[0].fmr_length = 0;
	memset(&keys[1], 0xff, sizeof(keys[1]));

	if (!ext4_fsmap_keys_ordered(keys, &head->fmh_keys[1]))
		return -EINVAL;

	query.next_block =
		head->fmh_keys[0].fmr_physical +
		head->fmh_keys[0].fmr_length;

	for (i = 0; i < EXT4_FSMAP_DEVICE_COUNT; ++i) {
		if (!devices[i].query)
			continue;
		if (head->fmh_keys[0].fmr_device > devices[i].device)
			continue;
		if (head->fmh_keys[1].fmr_device < devices[i].device)
			break;

		if (devices[i].device == head->fmh_keys[1].fmr_device)
			keys[1] = head->fmh_keys[1];
		if (devices[i].device > head->fmh_keys[0].fmr_device)
			memset(&keys[0], 0, sizeof(keys[0]));

		query.device = devices[i].device;
		query.final_record = false;
		query.group = (ext4_group_t)-1;
		err = devices[i].query(sb, keys, &query);
		if (err)
			break;
		query.next_block = 0;
	}

	head->fmh_oflags = FMH_OF_DEV_T;
	return err;
}


/* ===== multiple-mount protection ===== */
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/random.h>
#include <linux/utsname.h>

#include "ext4.h"

static __le32 ext4_mmp_checksum(struct super_block *sb,
				const struct mmp_struct *mmp)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	const int length = offsetof(struct mmp_struct, mmp_checksum);

	return cpu_to_le32(
		ext4_chksum(sbi, sbi->s_csum_seed, (const char *)mmp, length));
}

static bool ext4_mmp_checksum_valid(struct super_block *sb,
				    const struct mmp_struct *mmp)
{
	return !ext4_has_metadata_csum(sb) ||
	       mmp->mmp_checksum == ext4_mmp_checksum(sb, mmp);
}

static void ext4_mmp_update_checksum(struct super_block *sb,
				     struct mmp_struct *mmp)
{
	if (ext4_has_metadata_csum(sb))
		mmp->mmp_checksum = ext4_mmp_checksum(sb, mmp);
}

static int ext4_mmp_write_unfrozen(struct super_block *sb,
				    struct buffer_head *bh)
{
	struct mmp_struct *mmp = (struct mmp_struct *)bh->b_data;

	ext4_mmp_update_checksum(sb, mmp);
	lock_buffer(bh);
	bh->b_end_io = end_buffer_write_sync;
	get_bh(bh);
	submit_bh(REQ_OP_WRITE | REQ_SYNC | REQ_META | REQ_PRIO, bh);
	wait_on_buffer(bh);
	return buffer_uptodate(bh) ? 0 : -EIO;
}

static int ext4_mmp_write(struct super_block *sb, struct buffer_head *bh)
{
	int err;

	sb_start_write(sb);
	err = ext4_mmp_write_unfrozen(sb, bh);
	sb_end_write(sb);
	return err;
}

static int ext4_mmp_read(struct super_block *sb, struct buffer_head **bh,
			 ext4_fsblk_t block)
{
	struct mmp_struct *mmp;
	int err;

	if (*bh)
		clear_buffer_uptodate(*bh);
	else {
		*bh = sb_getblk(sb, block);
		if (!*bh)
			return -ENOMEM;
	}

	lock_buffer(*bh);
	err = ext4_read_bh(*bh, REQ_META | REQ_PRIO, NULL, false);
	if (err)
		goto fail;

	mmp = (struct mmp_struct *)(*bh)->b_data;
	if (le32_to_cpu(mmp->mmp_magic) != EXT4_MMP_MAGIC) {
		err = -EFSCORRUPTED;
		goto fail;
	}
	if (!ext4_mmp_checksum_valid(sb, mmp)) {
		err = -EFSBADCRC;
		goto fail;
	}

	return 0;

fail:
	brelse(*bh);
	*bh = NULL;
	ext4_warning(sb, "error %d while reading MMP block %llu", err, block);
	return err;
}

void __dump_mmp_msg(struct super_block *sb, struct mmp_struct *mmp,
		    const char *function, unsigned int line,
		    const char *msg)
{
	__ext4_warning(sb, function, line, "%s", msg);
	__ext4_warning(
		sb, function, line,
		"MMP last update: time=%llu node=%.*s device=%.*s",
		(unsigned long long)le64_to_cpu(mmp->mmp_time),
		(int)sizeof(mmp->mmp_nodename), mmp->mmp_nodename,
		(int)sizeof(mmp->mmp_bdevname), mmp->mmp_bdevname);
}

static unsigned int ext4_mmp_random_sequence(void)
{
	return get_random_u32_below(EXT4_MMP_SEQ_MAX + 1U);
}

static unsigned int ext4_mmp_check_interval(
	unsigned int configured, unsigned long elapsed_jiffies)
{
	unsigned int measured = EXT4_MMP_CHECK_MULT *
				(unsigned int)(elapsed_jiffies / HZ);

	if (measured < EXT4_MMP_MIN_CHECK_INTERVAL)
		measured = EXT4_MMP_MIN_CHECK_INTERVAL;
	if (measured > EXT4_MMP_MAX_CHECK_INTERVAL)
		measured = EXT4_MMP_MAX_CHECK_INTERVAL;

	return max(configured, measured);
}

static int ext4_mmp_thread(void *data)
{
	struct super_block *sb = data;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_super_block *es = sbi->s_es;
	struct buffer_head *bh = sbi->s_mmp_bh;
	struct mmp_struct *mmp = (struct mmp_struct *)bh->b_data;
	const ext4_fsblk_t block = le64_to_cpu(es->s_mmp_block);
	const unsigned int update_interval =
		max_t(unsigned int,
		      le16_to_cpu(es->s_mmp_update_interval),
		      EXT4_MMP_MIN_CHECK_INTERVAL);
	unsigned int check_interval =
		max(EXT4_MMP_CHECK_MULT * update_interval,
		    EXT4_MMP_MIN_CHECK_INTERVAL);
	unsigned int sequence = 0;
	unsigned long failed_writes = 0;
	int err = 0;

	mmp->mmp_time = cpu_to_le64(ktime_get_real_seconds());
	mmp->mmp_check_interval = cpu_to_le16(check_interval);
	memcpy(mmp->mmp_nodename, init_utsname()->nodename,
	       sizeof(mmp->mmp_nodename));

	while (!kthread_should_stop() && !ext4_forced_shutdown(sb)) {
		unsigned long started;
		unsigned long elapsed;

		if (!ext4_has_feature_mmp(sb)) {
			ext4_warning(sb, "MMP feature disabled while heartbeat active");
			break;
		}

		if (++sequence > EXT4_MMP_SEQ_MAX)
			sequence = 1;

		mmp->mmp_seq = cpu_to_le32(sequence);
		mmp->mmp_time = cpu_to_le64(ktime_get_real_seconds());
		started = jiffies;

		err = ext4_mmp_write(sb, bh);
		if (err && (failed_writes++ % 60UL) == 0)
			ext4_error_err(sb, -err, "error writing MMP heartbeat");

		elapsed = jiffies - started;
		if (elapsed < update_interval * HZ)
			schedule_timeout_interruptible(
				update_interval * HZ - elapsed);

		elapsed = jiffies - started;
		if (elapsed > check_interval * HZ) {
			struct buffer_head *verify_bh = NULL;
			struct mmp_struct *verify;

			err = ext4_mmp_read(sb, &verify_bh, block);
			if (err)
				break;

			verify = (struct mmp_struct *)verify_bh->b_data;
			if (mmp->mmp_seq != verify->mmp_seq ||
			    memcmp(mmp->mmp_nodename,
				   verify->mmp_nodename,
				   sizeof(mmp->mmp_nodename)) != 0) {
				dump_mmp_msg(
					sb, verify,
					"MMP heartbeat changed by another writer");
				put_bh(verify_bh);
				err = -EBUSY;
				break;
			}
			put_bh(verify_bh);
		}

		check_interval =
			ext4_mmp_check_interval(update_interval, elapsed);
		mmp->mmp_check_interval = cpu_to_le16(check_interval);
	}

	if (!ext4_forced_shutdown(sb)) {
		mmp->mmp_seq = cpu_to_le32(EXT4_MMP_SEQ_CLEAN);
		mmp->mmp_time = cpu_to_le64(ktime_get_real_seconds());
		if (!err)
			err = ext4_mmp_write(sb, bh);
	}

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop())
			schedule();
	}
	__set_current_state(TASK_RUNNING);
	return err;
}

void ext4_stop_mmpd(struct ext4_sb_info *sbi)
{
	if (!sbi->s_mmp_tsk)
		return;

	kthread_stop(sbi->s_mmp_tsk);
	brelse(sbi->s_mmp_bh);
	sbi->s_mmp_bh = NULL;
	sbi->s_mmp_tsk = NULL;
}

int ext4_multi_mount_protect(struct super_block *sb, ext4_fsblk_t mmp_block)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_super_block *es = sbi->s_es;
	struct buffer_head *bh = NULL;
	struct mmp_struct *mmp;
	unsigned int check_interval;
	unsigned int wait_seconds;
	u32 observed_sequence;
	u32 claimed_sequence;
	int err;

	if (mmp_block < le32_to_cpu(es->s_first_data_block) ||
	    mmp_block >= ext4_blocks_count(es))
		return -EINVAL;

	err = ext4_mmp_read(sb, &bh, mmp_block);
	if (err)
		return err;

	mmp = (struct mmp_struct *)bh->b_data;
	check_interval = max_t(
		unsigned int,
		le16_to_cpu(es->s_mmp_update_interval),
		EXT4_MMP_MIN_CHECK_INTERVAL);
	check_interval = max_t(
		unsigned int,
		check_interval,
		le16_to_cpu(mmp->mmp_check_interval));

	observed_sequence = le32_to_cpu(mmp->mmp_seq);
	if (observed_sequence == EXT4_MMP_SEQ_FSCK) {
		dump_mmp_msg(sb, mmp, "filesystem check is active");
		err = -EBUSY;
		goto fail;
	}

	wait_seconds = min(check_interval * 2U + 1U, check_interval + 60U);

	if (observed_sequence != EXT4_MMP_SEQ_CLEAN) {
		if (schedule_timeout_interruptible(HZ * wait_seconds) != 0) {
			err = -ETIMEDOUT;
			goto fail;
		}

		err = ext4_mmp_read(sb, &bh, mmp_block);
		if (err)
			goto fail;

		mmp = (struct mmp_struct *)bh->b_data;
		if (observed_sequence != le32_to_cpu(mmp->mmp_seq)) {
			dump_mmp_msg(sb, mmp, "filesystem is active elsewhere");
			err = -EBUSY;
			goto fail;
		}
	}

	claimed_sequence = ext4_mmp_random_sequence();
	mmp->mmp_seq = cpu_to_le32(claimed_sequence);
	err = ext4_mmp_write_unfrozen(sb, bh);
	if (err)
		goto fail;

	if (schedule_timeout_interruptible(HZ * wait_seconds) != 0) {
		err = -ETIMEDOUT;
		goto fail;
	}

	err = ext4_mmp_read(sb, &bh, mmp_block);
	if (err)
		goto fail;

	mmp = (struct mmp_struct *)bh->b_data;
	if (claimed_sequence != le32_to_cpu(mmp->mmp_seq)) {
		dump_mmp_msg(sb, mmp, "MMP claim was overwritten");
		err = -EBUSY;
		goto fail;
	}

	sbi->s_mmp_bh = bh;
	BUILD_BUG_ON(sizeof(mmp->mmp_bdevname) < BDEVNAME_SIZE);
	snprintf(mmp->mmp_bdevname, sizeof(mmp->mmp_bdevname),
		 "%pg", bh->b_bdev);

	sbi->s_mmp_tsk = kthread_run(
		ext4_mmp_thread, sb, "kmmpd-%.*s",
		(int)sizeof(mmp->mmp_bdevname), mmp->mmp_bdevname);
	if (IS_ERR(sbi->s_mmp_tsk)) {
		err = PTR_ERR(sbi->s_mmp_tsk);
		sbi->s_mmp_tsk = NULL;
		sbi->s_mmp_bh = NULL;
		goto fail;
	}

	return 0;

fail:
	brelse(bh);
	return err;
}


/* ===== sysfs control/reporting ===== */
#include <linux/fs.h>
#include <linux/part_stat.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/time.h>

#include "ext4.h"
#include "ext4_jbd2.h"

enum ext4_attr_kind {
	EXT4_ATTR_NOOP = 0,
	EXT4_ATTR_DIRTY_BLOCKS,
	EXT4_ATTR_SESSION_WRITES,
	EXT4_ATTR_LIFETIME_WRITES,
	EXT4_ATTR_RESERVED_CLUSTERS,
	EXT4_ATTR_SRA_RETRY_LIMIT,
	EXT4_ATTR_INODE_READAHEAD,
	EXT4_ATTR_TRIGGER_ERROR,
	EXT4_ATTR_FIRST_ERROR_TIME,
	EXT4_ATTR_LAST_ERROR_TIME,
	EXT4_ATTR_CLUSTERS_IN_GROUP,
	EXT4_ATTR_MB_ORDER,
	EXT4_ATTR_FEATURE,
	EXT4_ATTR_INT,
	EXT4_ATTR_UINT,
	EXT4_ATTR_ULONG,
	EXT4_ATTR_U64,
	EXT4_ATTR_U8,
	EXT4_ATTR_STRING,
	EXT4_ATTR_ATOMIC,
	EXT4_ATTR_JOURNAL_TASK,
};

enum ext4_attr_source {
	EXT4_ATTR_EXPLICIT = 0,
	EXT4_ATTR_SBI,
	EXT4_ATTR_DISK_SUPER,
};

struct ext4_attr {
	struct attribute attr;
	u16 kind;
	u16 source;
	u16 size;
	union {
		int offset;
		void *ptr;
	} target;
};

static const char ext4_proc_path[] = "fs/ext4";
static struct proc_dir_entry *ext4_proc_root;
static struct kobject *ext4_root;
static struct kobject *ext4_features;

static void *ext4_attr_pointer(struct ext4_attr *attr,
			       struct ext4_sb_info *sbi)
{
	switch (attr->source) {
	case EXT4_ATTR_EXPLICIT:
		return attr->target.ptr;
	case EXT4_ATTR_SBI:
		return (char *)sbi + attr->target.offset;
	case EXT4_ATTR_DISK_SUPER:
		return (char *)sbi->s_es + attr->target.offset;
	default:
		return NULL;
	}
}

static ssize_t ext4_show_session_writes(struct ext4_sb_info *sbi, char *buf)
{
	struct super_block *sb = sbi->s_buddy_cache->i_sb;
	const unsigned long sectors =
		part_stat_read(sb->s_bdev, sectors[STAT_WRITE]);

	return sysfs_emit(
		buf, "%lu\n",
		(sectors - sbi->s_sectors_written_start) >> 1);
}

static ssize_t ext4_show_lifetime_writes(struct ext4_sb_info *sbi, char *buf)
{
	struct super_block *sb = sbi->s_buddy_cache->i_sb;
	const u64 session_kbytes =
		(part_stat_read(sb->s_bdev, sectors[STAT_WRITE]) -
		 sbi->s_sectors_written_start) >> 1;

	return sysfs_emit(
		buf, "%llu\n",
		(unsigned long long)(sbi->s_kbytes_written + session_kbytes));
}

static ssize_t ext4_store_inode_readahead(struct ext4_sb_info *sbi,
					  const char *buf, size_t count)
{
	unsigned long value;
	int err = kstrtoul(skip_spaces(buf), 0, &value);

	if (err)
		return err;
	if (value && (!is_power_of_2(value) || value > 0x40000000UL))
		return -EINVAL;

	sbi->s_inode_readahead_blks = value;
	return count;
}

static ssize_t ext4_store_reserved_clusters(struct ext4_sb_info *sbi,
					     const char *buf,
					     size_t count)
{
	unsigned long long value;
	const ext4_fsblk_t total_clusters =
		ext4_blocks_count(sbi->s_es) >> sbi->s_cluster_bits;
	int err = kstrtoull(skip_spaces(buf), 0, &value);

	if (err)
		return err;
	if (value >= total_clusters)
		return -EINVAL;

	atomic64_set(&sbi->s_resv_clusters, value);
	return count;
}

static ssize_t ext4_store_test_error(struct ext4_sb_info *sbi,
				     const char *buf, size_t count)
{
	size_t length = count;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (length && buf[length - 1] == '\n')
		length--;
	if (length)
		ext4_error(sbi->s_sb, "%.*s", (int)length, buf);
	return count;
}

static ssize_t ext4_show_journal_task(struct ext4_sb_info *sbi, char *buf)
{
	if (!sbi->s_journal)
		return sysfs_emit(buf, "<none>\n");
	return sysfs_emit(buf, "%d\n", task_pid_vnr(sbi->s_journal->j_task));
}

#define EXT4_ATTRIBUTE(attr_name, attr_mode, kind_value) \
static struct ext4_attr ext4_attr_##attr_name = { \
	.attr = { .name = __stringify(attr_name), .mode = attr_mode }, \
	.kind = EXT4_ATTR_##kind_value, \
}

#define EXT4_ATTRIBUTE_OFFSET(attr_name, attr_mode, kind_value, source_value, type_name, member_name) \
static struct ext4_attr ext4_attr_##attr_name = { \
	.attr = { .name = __stringify(attr_name), .mode = attr_mode }, \
	.kind = EXT4_ATTR_##kind_value, \
	.source = EXT4_ATTR_##source_value, \
	.target.offset = offsetof(struct type_name, member_name), \
}

#define EXT4_ATTRIBUTE_STRING(attr_name, attr_mode, width_value, source_value, type_name, member_name) \
static struct ext4_attr ext4_attr_##attr_name = { \
	.attr = { .name = __stringify(attr_name), .mode = attr_mode }, \
	.kind = EXT4_ATTR_STRING, \
	.source = EXT4_ATTR_##source_value, \
	.size = width_value, \
	.target.offset = offsetof(struct type_name, member_name), \
}

#define EXT4_ATTRIBUTE_PTR(attr_name, attr_mode, kind_value, pointer_value) \
static struct ext4_attr ext4_attr_##attr_name = { \
	.attr = { .name = __stringify(attr_name), .mode = attr_mode }, \
	.kind = EXT4_ATTR_##kind_value, \
	.source = EXT4_ATTR_EXPLICIT, \
	.target.ptr = pointer_value, \
}

#define EXT4_ATTR_ENTRY(attr_name) (&ext4_attr_##attr_name.attr)

EXT4_ATTRIBUTE(delayed_allocation_blocks, 0444, DIRTY_BLOCKS);
EXT4_ATTRIBUTE(session_write_kbytes, 0444, SESSION_WRITES);
EXT4_ATTRIBUTE(lifetime_write_kbytes, 0444, LIFETIME_WRITES);
EXT4_ATTRIBUTE(reserved_clusters, 0644, RESERVED_CLUSTERS);
EXT4_ATTRIBUTE(sra_exceeded_retry_limit, 0444, SRA_RETRY_LIMIT);

EXT4_ATTRIBUTE_OFFSET(inode_readahead_blks, 0644, INODE_READAHEAD,
		      SBI, ext4_sb_info, s_inode_readahead_blks);
EXT4_ATTRIBUTE_OFFSET(mb_group_prealloc, 0644, CLUSTERS_IN_GROUP,
		      SBI, ext4_sb_info, s_mb_group_prealloc);
EXT4_ATTRIBUTE_OFFSET(mb_best_avail_max_trim_order, 0644, MB_ORDER,
		      SBI, ext4_sb_info, s_mb_best_avail_max_trim_order);

#define EXT4_RW_SBI_UINT(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0644, UINT, SBI, ext4_sb_info, member)
#define EXT4_RW_SBI_INT(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0644, INT, SBI, ext4_sb_info, member)
#define EXT4_RW_SBI_ULONG(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0644, ULONG, SBI, ext4_sb_info, member)
#define EXT4_RO_SBI_ATOMIC(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0444, ATOMIC, SBI, ext4_sb_info, member)
#define EXT4_RO_DISK_UINT(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0444, UINT, DISK_SUPER, ext4_super_block, member)
#define EXT4_RO_DISK_U8(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0444, U8, DISK_SUPER, ext4_super_block, member)
#define EXT4_RO_DISK_U64(name, member) \
	EXT4_ATTRIBUTE_OFFSET(name, 0444, U64, DISK_SUPER, ext4_super_block, member)
#define EXT4_RO_DISK_STRING(name, member, width) \
	EXT4_ATTRIBUTE_STRING(name, 0444, width, DISK_SUPER, ext4_super_block, member)

EXT4_RW_SBI_UINT(inode_goal, s_inode_goal);
EXT4_RW_SBI_UINT(mb_stats, s_mb_stats);
EXT4_RW_SBI_UINT(mb_max_to_scan, s_mb_max_to_scan);
EXT4_RW_SBI_UINT(mb_min_to_scan, s_mb_min_to_scan);
EXT4_RW_SBI_UINT(mb_order2_req, s_mb_order2_reqs);
EXT4_RW_SBI_UINT(mb_stream_req, s_mb_stream_request);
EXT4_RW_SBI_UINT(mb_max_linear_groups, s_mb_max_linear_groups);
EXT4_RW_SBI_UINT(extent_max_zeroout_kb, s_extent_max_zeroout_kb);
EXT4_ATTRIBUTE(trigger_fs_error, 0200, TRIGGER_ERROR);
EXT4_RW_SBI_INT(err_ratelimit_interval_ms, s_err_ratelimit_state.interval);
EXT4_RW_SBI_INT(err_ratelimit_burst, s_err_ratelimit_state.burst);
EXT4_RW_SBI_INT(warning_ratelimit_interval_ms, s_warning_ratelimit_state.interval);
EXT4_RW_SBI_INT(warning_ratelimit_burst, s_warning_ratelimit_state.burst);
EXT4_RW_SBI_INT(msg_ratelimit_interval_ms, s_msg_ratelimit_state.interval);
EXT4_RW_SBI_INT(msg_ratelimit_burst, s_msg_ratelimit_state.burst);
#ifdef CONFIG_EXT4_DEBUG
EXT4_RW_SBI_ULONG(simulate_fail, s_simulate_fail);
#endif
EXT4_RO_SBI_ATOMIC(warning_count, s_warning_count);
EXT4_RO_SBI_ATOMIC(msg_count, s_msg_count);
EXT4_RO_DISK_UINT(errors_count, s_error_count);
EXT4_RO_DISK_U8(first_error_errcode, s_first_error_errcode);
EXT4_RO_DISK_U8(last_error_errcode, s_last_error_errcode);
EXT4_RO_DISK_UINT(first_error_ino, s_first_error_ino);
EXT4_RO_DISK_UINT(last_error_ino, s_last_error_ino);
EXT4_RO_DISK_U64(first_error_block, s_first_error_block);
EXT4_RO_DISK_U64(last_error_block, s_last_error_block);
EXT4_RO_DISK_UINT(first_error_line, s_first_error_line);
EXT4_RO_DISK_UINT(last_error_line, s_last_error_line);
EXT4_RO_DISK_STRING(first_error_func, s_first_error_func, 32);
EXT4_RO_DISK_STRING(last_error_func, s_last_error_func, 32);
EXT4_ATTRIBUTE(first_error_time, 0444, FIRST_ERROR_TIME);
EXT4_ATTRIBUTE(last_error_time, 0444, LAST_ERROR_TIME);
EXT4_ATTRIBUTE(journal_task, 0444, JOURNAL_TASK);
EXT4_RW_SBI_UINT(mb_prefetch, s_mb_prefetch);
EXT4_RW_SBI_UINT(mb_prefetch_limit, s_mb_prefetch_limit);
EXT4_RW_SBI_ULONG(last_trim_minblks, s_last_trim_minblks);

static unsigned int ext4_legacy_writeback_bump = 128;
EXT4_ATTRIBUTE_PTR(max_writeback_mb_bump, 0444, UINT,
		   &ext4_legacy_writeback_bump);

static struct attribute *ext4_attrs[] = {
	EXT4_ATTR_ENTRY(delayed_allocation_blocks),
	EXT4_ATTR_ENTRY(session_write_kbytes),
	EXT4_ATTR_ENTRY(lifetime_write_kbytes),
	EXT4_ATTR_ENTRY(reserved_clusters),
	EXT4_ATTR_ENTRY(sra_exceeded_retry_limit),
	EXT4_ATTR_ENTRY(inode_readahead_blks),
	EXT4_ATTR_ENTRY(inode_goal),
	EXT4_ATTR_ENTRY(mb_stats),
	EXT4_ATTR_ENTRY(mb_max_to_scan),
	EXT4_ATTR_ENTRY(mb_min_to_scan),
	EXT4_ATTR_ENTRY(mb_order2_req),
	EXT4_ATTR_ENTRY(mb_stream_req),
	EXT4_ATTR_ENTRY(mb_group_prealloc),
	EXT4_ATTR_ENTRY(mb_max_linear_groups),
	EXT4_ATTR_ENTRY(max_writeback_mb_bump),
	EXT4_ATTR_ENTRY(extent_max_zeroout_kb),
	EXT4_ATTR_ENTRY(trigger_fs_error),
	EXT4_ATTR_ENTRY(err_ratelimit_interval_ms),
	EXT4_ATTR_ENTRY(err_ratelimit_burst),
	EXT4_ATTR_ENTRY(warning_ratelimit_interval_ms),
	EXT4_ATTR_ENTRY(warning_ratelimit_burst),
	EXT4_ATTR_ENTRY(msg_ratelimit_interval_ms),
	EXT4_ATTR_ENTRY(msg_ratelimit_burst),
	EXT4_ATTR_ENTRY(mb_best_avail_max_trim_order),
	EXT4_ATTR_ENTRY(errors_count),
	EXT4_ATTR_ENTRY(warning_count),
	EXT4_ATTR_ENTRY(msg_count),
	EXT4_ATTR_ENTRY(first_error_ino),
	EXT4_ATTR_ENTRY(last_error_ino),
	EXT4_ATTR_ENTRY(first_error_block),
	EXT4_ATTR_ENTRY(last_error_block),
	EXT4_ATTR_ENTRY(first_error_line),
	EXT4_ATTR_ENTRY(last_error_line),
	EXT4_ATTR_ENTRY(first_error_func),
	EXT4_ATTR_ENTRY(last_error_func),
	EXT4_ATTR_ENTRY(first_error_errcode),
	EXT4_ATTR_ENTRY(last_error_errcode),
	EXT4_ATTR_ENTRY(first_error_time),
	EXT4_ATTR_ENTRY(last_error_time),
	EXT4_ATTR_ENTRY(journal_task),
#ifdef CONFIG_EXT4_DEBUG
	EXT4_ATTR_ENTRY(simulate_fail),
#endif
	EXT4_ATTR_ENTRY(mb_prefetch),
	EXT4_ATTR_ENTRY(mb_prefetch_limit),
	EXT4_ATTR_ENTRY(last_trim_minblks),
	NULL,
};

ATTRIBUTE_GROUPS(ext4);

#define EXT4_FEATURE_ATTRIBUTE(name) EXT4_ATTRIBUTE(name, 0444, FEATURE)

EXT4_FEATURE_ATTRIBUTE(lazy_itable_init);
EXT4_FEATURE_ATTRIBUTE(batched_discard);
EXT4_FEATURE_ATTRIBUTE(meta_bg_resize);
#ifdef CONFIG_FS_ENCRYPTION
EXT4_FEATURE_ATTRIBUTE(encryption);
EXT4_FEATURE_ATTRIBUTE(test_dummy_encryption_v2);
#endif
#if IS_ENABLED(CONFIG_UNICODE)
EXT4_FEATURE_ATTRIBUTE(casefold);
#endif
#ifdef CONFIG_FS_VERITY
EXT4_FEATURE_ATTRIBUTE(verity);
#endif
EXT4_FEATURE_ATTRIBUTE(metadata_csum_seed);
EXT4_FEATURE_ATTRIBUTE(fast_commit);
#if IS_ENABLED(CONFIG_UNICODE) && defined(CONFIG_FS_ENCRYPTION)
EXT4_FEATURE_ATTRIBUTE(encrypted_casefold);
#endif

static struct attribute *ext4_feat_attrs[] = {
	EXT4_ATTR_ENTRY(lazy_itable_init),
	EXT4_ATTR_ENTRY(batched_discard),
	EXT4_ATTR_ENTRY(meta_bg_resize),
#ifdef CONFIG_FS_ENCRYPTION
	EXT4_ATTR_ENTRY(encryption),
	EXT4_ATTR_ENTRY(test_dummy_encryption_v2),
#endif
#if IS_ENABLED(CONFIG_UNICODE)
	EXT4_ATTR_ENTRY(casefold),
#endif
#ifdef CONFIG_FS_VERITY
	EXT4_ATTR_ENTRY(verity),
#endif
	EXT4_ATTR_ENTRY(metadata_csum_seed),
	EXT4_ATTR_ENTRY(fast_commit),
#if IS_ENABLED(CONFIG_UNICODE) && defined(CONFIG_FS_ENCRYPTION)
	EXT4_ATTR_ENTRY(encrypted_casefold),
#endif
	NULL,
};

ATTRIBUTE_GROUPS(ext4_feat);

static ssize_t ext4_show_timestamp(char *buf, __le32 low, __u8 high)
{
	const time64_t value = ((time64_t)high << 32) + le32_to_cpu(low);
	return sysfs_emit(buf, "%lld\n", value);
}

static ssize_t ext4_show_generic(struct ext4_attr *attr,
				 struct ext4_sb_info *sbi,
				 char *buf)
{
	void *ptr = ext4_attr_pointer(attr, sbi);

	if (!ptr)
		return 0;

	switch (attr->kind) {
	case EXT4_ATTR_INODE_READAHEAD:
	case EXT4_ATTR_CLUSTERS_IN_GROUP:
	case EXT4_ATTR_MB_ORDER:
	case EXT4_ATTR_UINT:
		if (attr->source == EXT4_ATTR_DISK_SUPER)
			return sysfs_emit(buf, "%u\n", le32_to_cpup(ptr));
		return sysfs_emit(buf, "%u\n", *(unsigned int *)ptr);
	case EXT4_ATTR_INT:
		return sysfs_emit(buf, "%d\n", *(int *)ptr);
	case EXT4_ATTR_ULONG:
		return sysfs_emit(buf, "%lu\n", *(unsigned long *)ptr);
	case EXT4_ATTR_U8:
		return sysfs_emit(buf, "%u\n", *(unsigned char *)ptr);
	case EXT4_ATTR_U64:
		if (attr->source == EXT4_ATTR_DISK_SUPER)
			return sysfs_emit(buf, "%llu\n", le64_to_cpup(ptr));
		return sysfs_emit(buf, "%llu\n",
				  *(unsigned long long *)ptr);
	case EXT4_ATTR_STRING:
		return sysfs_emit(buf, "%.*s\n", attr->size, (char *)ptr);
	case EXT4_ATTR_ATOMIC:
		return sysfs_emit(buf, "%d\n", atomic_read((atomic_t *)ptr));
	default:
		return 0;
	}
}

static ssize_t ext4_attr_show(struct kobject *kobj,
			      struct attribute *attribute,
			      char *buf)
{
	struct ext4_sb_info *sbi =
		container_of(kobj, struct ext4_sb_info, s_kobj);
	struct ext4_attr *attr =
		container_of(attribute, struct ext4_attr, attr);

	switch (attr->kind) {
	case EXT4_ATTR_DIRTY_BLOCKS:
		return sysfs_emit(
			buf, "%llu\n",
			(s64)EXT4_C2B(
				sbi,
				percpu_counter_sum(
					&sbi->s_dirtyclusters_counter)));
	case EXT4_ATTR_SESSION_WRITES:
		return ext4_show_session_writes(sbi, buf);
	case EXT4_ATTR_LIFETIME_WRITES:
		return ext4_show_lifetime_writes(sbi, buf);
	case EXT4_ATTR_RESERVED_CLUSTERS:
		return sysfs_emit(
			buf, "%llu\n",
			(unsigned long long)
				atomic64_read(&sbi->s_resv_clusters));
	case EXT4_ATTR_SRA_RETRY_LIMIT:
		return sysfs_emit(
			buf, "%llu\n",
			(unsigned long long)
				percpu_counter_sum(
					&sbi->s_sra_exceeded_retry_limit));
	case EXT4_ATTR_FEATURE:
		return sysfs_emit(buf, "supported\n");
	case EXT4_ATTR_FIRST_ERROR_TIME:
		return ext4_show_timestamp(
			buf, sbi->s_es->s_first_error_time,
			sbi->s_es->s_first_error_time_hi);
	case EXT4_ATTR_LAST_ERROR_TIME:
		return ext4_show_timestamp(
			buf, sbi->s_es->s_last_error_time,
			sbi->s_es->s_last_error_time_hi);
	case EXT4_ATTR_JOURNAL_TASK:
		return ext4_show_journal_task(sbi, buf);
	default:
		return ext4_show_generic(attr, sbi, buf);
	}
}

static ssize_t ext4_store_generic(struct ext4_attr *attr,
				  struct ext4_sb_info *sbi,
				  const char *buf,
				  size_t count)
{
	void *ptr = ext4_attr_pointer(attr, sbi);
	unsigned int uvalue;
	unsigned long ulvalue;
	int ivalue;
	int err;

	if (!ptr)
		return -EINVAL;

	switch (attr->kind) {
	case EXT4_ATTR_INT:
		err = kstrtoint(skip_spaces(buf), 0, &ivalue);
		if (err)
			return err;
		if (ivalue < 0)
			return -EINVAL;
		*(int *)ptr = ivalue;
		return count;
	case EXT4_ATTR_UINT:
		err = kstrtouint(skip_spaces(buf), 0, &uvalue);
		if (err)
			return err;
		if (attr->source == EXT4_ATTR_DISK_SUPER)
			*(__le32 *)ptr = cpu_to_le32(uvalue);
		else
			*(unsigned int *)ptr = uvalue;
		return count;
	case EXT4_ATTR_MB_ORDER:
		err = kstrtouint(skip_spaces(buf), 0, &uvalue);
		if (err)
			return err;
		if (uvalue > 64)
			return -EINVAL;
		*(unsigned int *)ptr = uvalue;
		return count;
	case EXT4_ATTR_CLUSTERS_IN_GROUP:
		err = kstrtouint(skip_spaces(buf), 0, &uvalue);
		if (err)
			return err;
		if (uvalue > sbi->s_clusters_per_group)
			return -EINVAL;
		*(unsigned int *)ptr = uvalue;
		return count;
	case EXT4_ATTR_ULONG:
		err = kstrtoul(skip_spaces(buf), 0, &ulvalue);
		if (err)
			return err;
		*(unsigned long *)ptr = ulvalue;
		return count;
	default:
		return -EOPNOTSUPP;
	}
}

static ssize_t ext4_attr_store(struct kobject *kobj,
			       struct attribute *attribute,
			       const char *buf,
			       size_t count)
{
	struct ext4_sb_info *sbi =
		container_of(kobj, struct ext4_sb_info, s_kobj);
	struct ext4_attr *attr =
		container_of(attribute, struct ext4_attr, attr);

	switch (attr->kind) {
	case EXT4_ATTR_RESERVED_CLUSTERS:
		return ext4_store_reserved_clusters(sbi, buf, count);
	case EXT4_ATTR_INODE_READAHEAD:
		return ext4_store_inode_readahead(sbi, buf, count);
	case EXT4_ATTR_TRIGGER_ERROR:
		return ext4_store_test_error(sbi, buf, count);
	default:
		return ext4_store_generic(attr, sbi, buf, count);
	}
}

static void ext4_sb_kobj_release(struct kobject *kobj)
{
	struct ext4_sb_info *sbi =
		container_of(kobj, struct ext4_sb_info, s_kobj);
	complete(&sbi->s_kobj_unregister);
}

static void ext4_feature_kobj_release(struct kobject *kobj)
{
	kfree(kobj);
}

static const struct sysfs_ops ext4_sysfs_ops = {
	.show = ext4_attr_show,
	.store = ext4_attr_store,
};

static const struct kobj_type ext4_sb_ktype = {
	.default_groups = ext4_groups,
	.sysfs_ops = &ext4_sysfs_ops,
	.release = ext4_sb_kobj_release,
};

static const struct kobj_type ext4_feature_ktype = {
	.default_groups = ext4_feat_groups,
	.sysfs_ops = &ext4_sysfs_ops,
	.release = ext4_feature_kobj_release,
};

void ext4_notify_error_sysfs(struct ext4_sb_info *sbi)
{
	mutex_lock(&sbi->s_error_notify_mutex);
	if (sbi->s_kobj.state_in_sysfs)
		sysfs_notify(&sbi->s_kobj, NULL, "errors_count");
	mutex_unlock(&sbi->s_error_notify_mutex);
}

int ext4_register_sysfs(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int err;

	init_completion(&sbi->s_kobj_unregister);
	mutex_lock(&sbi->s_error_notify_mutex);
	err = kobject_init_and_add(
		&sbi->s_kobj, &ext4_sb_ktype, ext4_root, "%s", sb->s_id);
	mutex_unlock(&sbi->s_error_notify_mutex);
	if (err) {
		kobject_put(&sbi->s_kobj);
		wait_for_completion(&sbi->s_kobj_unregister);
		return err;
	}

	if (ext4_proc_root)
		sbi->s_proc = proc_mkdir(sb->s_id, ext4_proc_root);
	if (!sbi->s_proc)
		return 0;

	proc_create_single_data(
		"options", S_IRUGO, sbi->s_proc,
		ext4_seq_options_show, sb);
	proc_create_single_data(
		"es_shrinker_info", S_IRUGO, sbi->s_proc,
		ext4_seq_es_shrinker_info_show, sb);
	proc_create_single_data(
		"fc_info", 0444, sbi->s_proc,
		ext4_fc_info_show, sb);
	proc_create_seq_data(
		"mb_groups", S_IRUGO, sbi->s_proc,
		&ext4_mb_seq_groups_ops, sb);
	proc_create_single_data(
		"mb_stats", 0444, sbi->s_proc,
		ext4_seq_mb_stats_show, sb);
	proc_create_seq_data(
		"mb_structs_summary", 0444, sbi->s_proc,
		&ext4_mb_seq_structs_summary_ops, sb);
	return 0;
}

void ext4_unregister_sysfs(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	if (sbi->s_proc)
		remove_proc_subtree(sb->s_id, ext4_proc_root);

	mutex_lock(&sbi->s_error_notify_mutex);
	kobject_del(&sbi->s_kobj);
	mutex_unlock(&sbi->s_error_notify_mutex);
}

int __init ext4_init_sysfs(void)
{
	int err;

	ext4_root = kobject_create_and_add("ext4", fs_kobj);
	if (!ext4_root)
		return -ENOMEM;

	ext4_features = kzalloc(sizeof(*ext4_features), GFP_KERNEL);
	if (!ext4_features) {
		err = -ENOMEM;
		goto fail_root;
	}

	err = kobject_init_and_add(
		ext4_features, &ext4_feature_ktype,
		ext4_root, "features");
	if (err)
		goto fail_features;

	ext4_proc_root = proc_mkdir(ext4_proc_path, NULL);
	return 0;

fail_features:
	kobject_put(ext4_features);
	ext4_features = NULL;
fail_root:
	kobject_put(ext4_root);
	ext4_root = NULL;
	return err;
}

void ext4_exit_sysfs(void)
{
	kobject_put(ext4_features);
	ext4_features = NULL;
	kobject_put(ext4_root);
	ext4_root = NULL;
	remove_proc_entry(ext4_proc_path, NULL);
	ext4_proc_root = NULL;
}

