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
