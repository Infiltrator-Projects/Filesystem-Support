/* Infiltrator Filesystem Support — EXT4 Linux storage guard.
 * Block-group accounting and protected-range validation are one host-facing
 * storage responsibility; canonical geometry remains in the EXT4 core.
 */

/*
 * Infiltrator Filesystem Support — EXT4 block-group accounting.
 *
 * The portable EXT4 core owns format geometry.  This unit binds that geometry
 * to Linux buffer heads, per-cpu counters and the multiblock allocator.
 */

#include <linux/buffer_head.h>
#include <linux/capability.h>
#include <linux/fs.h>
#include <linux/quotaops.h>
#include <linux/overflow.h>

#include "ext4.h"
#include "ext4_jbd2.h"
#include "mballoc.h"

#include <kunit/static_stub.h>
#include <trace/events/ext4.h>

struct ifs_ext4_cluster_span {
	unsigned int first;
	unsigned int last;
	bool present;
};

static unsigned int ifs_ext4_group_cluster_count(struct super_block *sb,
						  ext4_group_t group)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_fsblk_t first;
	ext4_fsblk_t total = ext4_blocks_count(sbi->s_es);
	ext4_fsblk_t blocks;

	first = ext4_group_first_block_no(sb, group);
	if (first >= total)
		return 0;

	blocks = min_t(ext4_fsblk_t, EXT4_BLOCKS_PER_GROUP(sb),
		       total - first);
	return EXT4_NUM_B2C(sbi, blocks);
}

ext4_group_t ext4_get_group_number(struct super_block *sb,
				   ext4_fsblk_t block)
{
	ext4_group_t group = 0;

	ext4_get_group_no_and_offset(sb, block, &group, NULL);
	return group;
}

void ext4_get_group_no_and_offset(struct super_block *sb,
				  ext4_fsblk_t block,
				  ext4_group_t *group_out,
				  ext4_grpblk_t *offset_out)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	const struct ext4_super_block *es = sbi->s_es;
	ifs_ext4_u32 group = 0;
	ifs_ext4_u32 offset = 0;
	IfsExt4BlockGroupStatus status;

	status = ifs_ext4_block_group_position(
		(ifs_ext4_u64)block,
		le32_to_cpu(es->s_first_data_block),
		EXT4_BLOCKS_PER_GROUP(sb),
		sbi->s_cluster_bits,
		ext4_get_groups_count(sb),
		&group, &offset);

	if (WARN_ON_ONCE(status != IFS_EXT4_BLOCK_GROUP_OK)) {
		if (group_out)
			*group_out = 0;
		if (offset_out)
			*offset_out = 0;
		return;
	}

	if (group_out)
		*group_out = (ext4_group_t)group;
	if (offset_out)
		*offset_out = (ext4_grpblk_t)offset;
}

static bool ifs_ext4_block_belongs_to_group(struct super_block *sb,
					     ext4_fsblk_t block,
					     ext4_group_t group)
{
	ext4_group_t actual;

	ext4_get_group_no_and_offset(sb, block, &actual, NULL);
	return actual == group;
}

static bool ifs_ext4_local_cluster(struct super_block *sb,
				    ext4_group_t group,
				    ext4_fsblk_t block,
				    unsigned int *cluster)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_fsblk_t first = ext4_group_first_block_no(sb, group);

	if (!ifs_ext4_block_belongs_to_group(sb, block, group))
		return false;

	*cluster = EXT4_B2C(sbi, block - first);
	return true;
}

static void ifs_ext4_span_add(struct ifs_ext4_cluster_span *spans,
			      unsigned int *count,
			      unsigned int first,
			      unsigned int last)
{
	unsigned int i;
	unsigned int insert = *count;

	for (i = 0; i < *count; i++) {
		if (last + 1 < spans[i].first) {
			insert = i;
			break;
		}
		if (first > spans[i].last + 1)
			continue;

		spans[i].first = min(spans[i].first, first);
		spans[i].last = max(spans[i].last, last);

		while (i + 1 < *count &&
		       spans[i + 1].first <= spans[i].last + 1) {
			spans[i].last = max(spans[i].last,
					    spans[i + 1].last);
			memmove(&spans[i + 1], &spans[i + 2],
				(*count - i - 2) * sizeof(*spans));
			(*count)--;
		}
		return;
	}

	if (*count >= 4)
		return;

	if (insert < *count)
		memmove(&spans[insert + 1], &spans[insert],
			(*count - insert) * sizeof(*spans));
	spans[insert].first = first;
	spans[insert].last = last;
	spans[insert].present = true;
	(*count)++;
}

static unsigned int ifs_ext4_base_meta_blocks(struct super_block *sb,
					       ext4_group_t group);

static unsigned int ifs_ext4_metadata_clusters(struct super_block *sb,
						ext4_group_t group,
						struct ext4_group_desc *desc)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ifs_ext4_cluster_span spans[4] = { };
	unsigned int group_clusters = ifs_ext4_group_cluster_count(sb, group);
	unsigned int span_count = 0;
	unsigned int base_blocks;
	unsigned int base_clusters;
	unsigned int cluster;
	unsigned int first_cluster;
	unsigned int last_cluster;
	unsigned int i;
	unsigned int total = 0;
	ext4_fsblk_t table;
	ext4_fsblk_t table_last;

	base_blocks = ifs_ext4_base_meta_blocks(sb, group);
	base_clusters = EXT4_NUM_B2C(sbi, base_blocks);
	if (base_clusters)
		ifs_ext4_span_add(spans, &span_count, 0, base_clusters - 1);

	if (ifs_ext4_local_cluster(sb, group,
				  ext4_block_bitmap(sb, desc), &cluster) &&
	    cluster < group_clusters)
		ifs_ext4_span_add(spans, &span_count, cluster, cluster);

	if (ifs_ext4_local_cluster(sb, group,
				  ext4_inode_bitmap(sb, desc), &cluster) &&
	    cluster < group_clusters)
		ifs_ext4_span_add(spans, &span_count, cluster, cluster);

	table = ext4_inode_table(sb, desc);
	if (sbi->s_itb_per_group &&
	    !check_add_overflow(table,
				(ext4_fsblk_t)sbi->s_itb_per_group - 1,
				&table_last) &&
	    ifs_ext4_local_cluster(sb, group, table, &first_cluster) &&
	    ifs_ext4_local_cluster(sb, group, table_last, &last_cluster)) {
		first_cluster = min(first_cluster, group_clusters);
		last_cluster = min(last_cluster, group_clusters - 1);
		if (first_cluster <= last_cluster)
			ifs_ext4_span_add(spans, &span_count,
					  first_cluster, last_cluster);
	}

	for (i = 0; i < span_count; i++)
		total += spans[i].last - spans[i].first + 1;

	return total;
}

static int ifs_ext4_build_uninitialised_bitmap(struct super_block *sb,
						struct buffer_head *bh,
						ext4_group_t group,
						struct ext4_group_desc *desc)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_fsblk_t table;
	unsigned int cluster;
	unsigned int bit;
	unsigned int cluster_count;
	unsigned int base_clusters;

	if (!ext4_group_desc_csum_verify(sb, group, desc)) {
		ext4_mark_group_bitmap_corrupted(
			sb, group,
			EXT4_GROUP_INFO_BBITMAP_CORRUPT |
			EXT4_GROUP_INFO_IBITMAP_CORRUPT);
		return -EFSBADCRC;
	}

	memset(bh->b_data, 0, bh->b_size);
	cluster_count = ifs_ext4_group_cluster_count(sb, group);
	base_clusters = EXT4_NUM_B2C(
		sbi, ifs_ext4_base_meta_blocks(sb, group));
	if (base_clusters > bh->b_size * 8U)
		return -EUCLEAN;

	for (bit = 0; bit < base_clusters; bit++)
		ext4_set_bit(bit, bh->b_data);

	if (ifs_ext4_local_cluster(sb, group,
				  ext4_block_bitmap(sb, desc), &cluster))
		ext4_set_bit(cluster, bh->b_data);

	if (ifs_ext4_local_cluster(sb, group,
				  ext4_inode_bitmap(sb, desc), &cluster))
		ext4_set_bit(cluster, bh->b_data);

	table = ext4_inode_table(sb, desc);
	for (bit = 0; bit < sbi->s_itb_per_group; bit++) {
		if (ifs_ext4_local_cluster(sb, group, table + bit, &cluster))
			ext4_set_bit(cluster, bh->b_data);
	}

	ext4_mark_bitmap_end(cluster_count, bh->b_size * 8U, bh->b_data);
	return 0;
}

unsigned ext4_free_clusters_after_init(struct super_block *sb,
				       ext4_group_t group,
				       struct ext4_group_desc *desc)
{
	unsigned int clusters = ifs_ext4_group_cluster_count(sb, group);
	unsigned int metadata = ifs_ext4_metadata_clusters(sb, group, desc);

	return clusters > metadata ? clusters - metadata : 0;
}

struct ext4_group_desc *ext4_get_group_desc(struct super_block *sb,
					     ext4_group_t group,
					     struct buffer_head **bh)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_group_t groups = ext4_get_groups_count(sb);
	unsigned int descriptor_index;
	unsigned int descriptor_slot;
	struct buffer_head *descriptor_bh;

	KUNIT_STATIC_STUB_REDIRECT(ext4_get_group_desc, sb, group, bh);

	if (group >= groups) {
		ext4_error(sb, "group %u outside group count %u",
			   group, groups);
		return NULL;
	}

	descriptor_index = group >> EXT4_DESC_PER_BLOCK_BITS(sb);
	descriptor_slot = group & (EXT4_DESC_PER_BLOCK(sb) - 1);
	descriptor_bh =
		sbi_array_rcu_deref(sbi, s_group_desc, descriptor_index);
	if (!descriptor_bh) {
		ext4_error(sb, "group descriptor block %u unavailable for group %u",
			   descriptor_index, group);
		return NULL;
	}

	if (bh)
		*bh = descriptor_bh;

	return (struct ext4_group_desc *)
		((u8 *)descriptor_bh->b_data +
		 descriptor_slot * EXT4_DESC_SIZE(sb));
}

struct ext4_group_info *ext4_get_group_info(struct super_block *sb,
					    ext4_group_t group)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_group_info **vector;
	unsigned int outer;
	unsigned int inner;

	if (unlikely(group >= ext4_get_groups_count(sb)))
		return NULL;

	outer = group >> EXT4_DESC_PER_BLOCK_BITS(sb);
	inner = group & (EXT4_DESC_PER_BLOCK(sb) - 1);
	vector = sbi_array_rcu_deref(sbi, s_group_info, outer);
	return vector ? vector[inner] : NULL;
}

static ext4_fsblk_t ifs_ext4_bitmap_padding_error(struct super_block *sb,
						   ext4_group_t group,
						   struct buffer_head *bh)
{
	unsigned int start = ifs_ext4_group_cluster_count(sb, group);
	unsigned int limit = bh->b_size * 8U;
	unsigned int zero;

	if (start >= limit)
		return 0;

	zero = ext4_find_next_zero_bit(bh->b_data, limit, start);
	return zero < limit ? zero : 0;
}

static ext4_fsblk_t ifs_ext4_required_cluster_missing(
	struct super_block *sb, struct ext4_group_desc *desc,
	ext4_group_t group, struct buffer_head *bh)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_fsblk_t first = ext4_group_first_block_no(sb, group);
	ext4_fsblk_t table;
	ext4_fsblk_t table_last;
	unsigned int cluster;
	unsigned int first_cluster;
	unsigned int last_cluster;
	unsigned int zero;

	if (ext4_has_feature_flex_bg(sb))
		return 0;

	if (!ifs_ext4_local_cluster(sb, group,
				   ext4_block_bitmap(sb, desc), &cluster) ||
	    cluster >= EXT4_CLUSTERS_PER_GROUP(sb) ||
	    !ext4_test_bit(cluster, bh->b_data))
		return ext4_block_bitmap(sb, desc);

	if (!ifs_ext4_local_cluster(sb, group,
				   ext4_inode_bitmap(sb, desc), &cluster) ||
	    cluster >= EXT4_CLUSTERS_PER_GROUP(sb) ||
	    !ext4_test_bit(cluster, bh->b_data))
		return ext4_inode_bitmap(sb, desc);

	table = ext4_inode_table(sb, desc);
	if (!sbi->s_itb_per_group ||
	    check_add_overflow(table,
			       (ext4_fsblk_t)sbi->s_itb_per_group - 1,
			       &table_last) ||
	    !ifs_ext4_local_cluster(sb, group, table, &first_cluster) ||
	    !ifs_ext4_local_cluster(sb, group, table_last, &last_cluster))
		return table;

	zero = ext4_find_next_zero_bit(
		bh->b_data, last_cluster + 1, first_cluster);
	if (zero <= last_cluster)
		return table;

	(void)first;
	return 0;
}

static int ifs_ext4_verify_block_bitmap(struct super_block *sb,
					 struct ext4_group_desc *desc,
					 ext4_group_t group,
					 struct buffer_head *bh)
{
	struct ext4_group_info *info;
	ext4_fsblk_t bad;

	if (EXT4_SB(sb)->s_mount_state & EXT4_FC_REPLAY)
		return 0;
	if (buffer_verified(bh))
		return 0;

	info = ext4_get_group_info(sb, group);
	if (!info || EXT4_MB_GRP_BBITMAP_CORRUPT(info))
		return -EUCLEAN;

	ext4_lock_group(sb, group);
	if (buffer_verified(bh))
		goto verified;

	if (!ext4_block_bitmap_csum_verify(sb, desc, bh) ||
	    ext4_simulate_fail(sb, EXT4_SIM_BBITMAP_CRC)) {
		ext4_unlock_group(sb, group);
		ext4_error(sb, "group %u has invalid block bitmap checksum",
			   group);
		ext4_mark_group_bitmap_corrupted(
			sb, group, EXT4_GROUP_INFO_BBITMAP_CORRUPT);
		return -EFSBADCRC;
	}

	bad = ifs_ext4_required_cluster_missing(sb, desc, group, bh);
	if (!bad)
		bad = ifs_ext4_bitmap_padding_error(sb, group, bh);
	if (bad) {
		ext4_unlock_group(sb, group);
		ext4_error(sb, "group %u has invalid block bitmap at %llu",
			   group, (unsigned long long)bad);
		ext4_mark_group_bitmap_corrupted(
			sb, group, EXT4_GROUP_INFO_BBITMAP_CORRUPT);
		return -EUCLEAN;
	}

	set_buffer_verified(bh);
verified:
	ext4_unlock_group(sb, group);
	return 0;
}

struct buffer_head *
ext4_read_block_bitmap_nowait(struct super_block *sb, ext4_group_t group,
			      bool ignore_locked)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_group_desc *desc;
	struct buffer_head *bh;
	ext4_fsblk_t block;
	int error;

	KUNIT_STATIC_STUB_REDIRECT(
		ext4_read_block_bitmap_nowait, sb, group, ignore_locked);

	desc = ext4_get_group_desc(sb, group, NULL);
	if (!desc)
		return ERR_PTR(-EUCLEAN);

	block = ext4_block_bitmap(sb, desc);
	if (block <= le32_to_cpu(sbi->s_es->s_first_data_block) ||
	    block >= ext4_blocks_count(sbi->s_es)) {
		ext4_error(sb, "group %u block bitmap points outside filesystem: %llu",
			   group, (unsigned long long)block);
		ext4_mark_group_bitmap_corrupted(
			sb, group, EXT4_GROUP_INFO_BBITMAP_CORRUPT);
		return ERR_PTR(-EUCLEAN);
	}

	bh = sb_getblk(sb, block);
	if (!bh)
		return ERR_PTR(-ENOMEM);

	if (ignore_locked && buffer_locked(bh)) {
		put_bh(bh);
		return NULL;
	}

	if (bitmap_uptodate(bh))
		goto verify;

	lock_buffer(bh);
	if (bitmap_uptodate(bh)) {
		unlock_buffer(bh);
		goto verify;
	}

	ext4_lock_group(sb, group);
	if (ext4_has_group_desc_csum(sb) &&
	    (desc->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT))) {
		if (!group) {
			error = -EUCLEAN;
			ext4_unlock_group(sb, group);
			unlock_buffer(bh);
			goto fail;
		}

		error = ifs_ext4_build_uninitialised_bitmap(
			sb, bh, group, desc);
		if (error) {
			ext4_unlock_group(sb, group);
			unlock_buffer(bh);
			goto fail;
		}

		set_bitmap_uptodate(bh);
		set_buffer_uptodate(bh);
		set_buffer_verified(bh);
		ext4_unlock_group(sb, group);
		unlock_buffer(bh);
		return bh;
	}
	ext4_unlock_group(sb, group);

	if (buffer_uptodate(bh)) {
		set_bitmap_uptodate(bh);
		unlock_buffer(bh);
		goto verify;
	}

	set_buffer_new(bh);
	trace_ext4_read_block_bitmap_load(sb, group, ignore_locked);
	ext4_read_bh_nowait(
		bh,
		REQ_META | REQ_PRIO | (ignore_locked ? REQ_RAHEAD : 0),
		ext4_end_bitmap_read,
		ext4_simulate_fail(sb, EXT4_SIM_BBITMAP_EIO));
	return bh;

verify:
	error = ifs_ext4_verify_block_bitmap(sb, desc, group, bh);
	if (!error)
		return bh;
fail:
	put_bh(bh);
	return ERR_PTR(error);
}

int ext4_wait_block_bitmap(struct super_block *sb, ext4_group_t group,
			   struct buffer_head *bh)
{
	struct ext4_group_desc *desc;

	KUNIT_STATIC_STUB_REDIRECT(ext4_wait_block_bitmap, sb, group, bh);

	if (!buffer_new(bh))
		return 0;

	desc = ext4_get_group_desc(sb, group, NULL);
	if (!desc)
		return -EUCLEAN;

	wait_on_buffer(bh);
	if (!buffer_uptodate(bh)) {
		ext4_error_err(sb, EIO,
			       "cannot read block bitmap for group %u at %llu",
			       group, (unsigned long long)bh->b_blocknr);
		ext4_mark_group_bitmap_corrupted(
			sb, group, EXT4_GROUP_INFO_BBITMAP_CORRUPT);
		return -EIO;
	}

	clear_buffer_new(bh);
	return ifs_ext4_verify_block_bitmap(sb, desc, group, bh);
}

struct buffer_head *
ext4_read_block_bitmap(struct super_block *sb, ext4_group_t group)
{
	struct buffer_head *bh;
	int error;

	bh = ext4_read_block_bitmap_nowait(sb, group, false);
	if (IS_ERR(bh))
		return bh;

	error = ext4_wait_block_bitmap(sb, group, bh);
	if (!error)
		return bh;

	put_bh(bh);
	return ERR_PTR(error);
}

static bool ifs_ext4_free_space_available(struct ext4_sb_info *sbi,
					   s64 requested,
					   unsigned int flags)
{
	struct percpu_counter *free_counter = &sbi->s_freeclusters_counter;
	struct percpu_counter *dirty_counter = &sbi->s_dirtyclusters_counter;
	s64 free_clusters = percpu_counter_read_positive(free_counter);
	s64 dirty_clusters = percpu_counter_read_positive(dirty_counter);
	s64 private_reserve = atomic64_read(&sbi->s_resv_clusters);
	s64 filesystem_reserve =
		ext4_r_blocks_count(sbi->s_es) >> sbi->s_cluster_bits;
	s64 protected = filesystem_reserve + private_reserve;

	if (free_clusters - (requested + protected + dirty_clusters) <
	    EXT4_FREECLUSTERS_WATERMARK) {
		free_clusters = percpu_counter_sum_positive(free_counter);
		dirty_clusters = percpu_counter_sum_positive(dirty_counter);
	}

	if (free_clusters >= requested + protected + dirty_clusters)
		return true;

	if (uid_eq(sbi->s_resuid, current_fsuid()) ||
	    (!gid_eq(sbi->s_resgid, GLOBAL_ROOT_GID) &&
	     in_group_p(sbi->s_resgid)) ||
	    (flags & EXT4_MB_USE_ROOT_BLOCKS) ||
	    capable(CAP_SYS_RESOURCE)) {
		if (free_clusters >=
		    requested + private_reserve + dirty_clusters)
			return true;
	}

	if (flags & EXT4_MB_USE_RESERVED)
		return free_clusters >= requested + dirty_clusters;

	return false;
}

int ext4_claim_free_clusters(struct ext4_sb_info *sbi,
			     s64 clusters, unsigned int flags)
{
	if (!ifs_ext4_free_space_available(sbi, clusters, flags))
		return -ENOSPC;

	percpu_counter_add(&sbi->s_dirtyclusters_counter, clusters);
	return 0;
}

int ext4_should_retry_alloc(struct super_block *sb, int *retries)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	if (!sbi->s_journal)
		return 0;

	(*retries)++;
	if (*retries > 3) {
		percpu_counter_inc(&sbi->s_sra_exceeded_retry_limit);
		return 0;
	}

	smp_mb();
	if (!sbi->s_mb_free_pending) {
		if (test_opt(sb, DISCARD)) {
			atomic_inc(&sbi->s_retry_alloc_pending);
			flush_work(&sbi->s_discard_work);
			atomic_dec(&sbi->s_retry_alloc_pending);
		}
		return ifs_ext4_free_space_available(sbi, 1, 0);
	}

	(void)jbd2_journal_force_commit_nested(sbi->s_journal);
	return 1;
}

ext4_fsblk_t ext4_new_meta_blocks(handle_t *handle, struct inode *inode,
				  ext4_fsblk_t goal, unsigned int flags,
				  unsigned long *count, int *error)
{
	struct ext4_allocation_request request = {
		.inode = inode,
		.goal = goal,
		.len = count ? *count : 1,
		.flags = flags,
	};
	ext4_fsblk_t block;

	block = ext4_mb_new_blocks(handle, &request, error);
	if (count)
		*count = request.len;

	if (!*error && (flags & EXT4_MB_DELALLOC_RESERVED))
		dquot_alloc_block_nofail(
			inode,
			EXT4_C2B(EXT4_SB(inode->i_sb), request.len));

	return block;
}

ext4_fsblk_t ext4_count_free_clusters(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_group_t groups = ext4_get_groups_count(sb);
	ext4_group_t group;
	ext4_fsblk_t total = 0;

	for (group = 0; group < groups; group++) {
		struct ext4_group_desc *desc;
		struct ext4_group_info *info = NULL;

		desc = ext4_get_group_desc(sb, group, NULL);
		if (!desc)
			continue;
		if (sbi->s_group_info)
			info = ext4_get_group_info(sb, group);
		if (!info || !EXT4_MB_GRP_BBITMAP_CORRUPT(info))
			total += ext4_free_group_clusters(sb, desc);
	}

	return total;
}

int ext4_bg_has_super(struct super_block *sb, ext4_group_t group)
{
	const struct ext4_super_block *es = EXT4_SB(sb)->s_es;

	return ifs_ext4_group_has_super_ex(
		ext4_has_feature_sparse_super(sb),
		ext4_has_feature_sparse_super2(sb),
		le32_to_cpu(es->s_backup_bgs[0]),
		le32_to_cpu(es->s_backup_bgs[1]),
		group);
}

static unsigned long ifs_ext4_meta_bg_descriptor_count(
	struct super_block *sb, ext4_group_t group)
{
	unsigned long descriptor_block =
		group / EXT4_DESC_PER_BLOCK(sb);
	ext4_group_t first =
		descriptor_block * EXT4_DESC_PER_BLOCK(sb);
	ext4_group_t last =
		first + EXT4_DESC_PER_BLOCK(sb) - 1;

	return group == first || group == first + 1 || group == last;
}

static unsigned long ifs_ext4_legacy_descriptor_count(
	struct super_block *sb, ext4_group_t group)
{
	if (!ext4_bg_has_super(sb, group))
		return 0;

	if (ext4_has_feature_meta_bg(sb))
		return le32_to_cpu(EXT4_SB(sb)->s_es->s_first_meta_bg);

	return EXT4_SB(sb)->s_gdb_count;
}

unsigned long ext4_bg_num_gdb(struct super_block *sb, ext4_group_t group)
{
	unsigned long first_meta =
		le32_to_cpu(EXT4_SB(sb)->s_es->s_first_meta_bg);
	unsigned long meta_group =
		group / EXT4_DESC_PER_BLOCK(sb);

	if (!ext4_has_feature_meta_bg(sb) ||
	    meta_group < first_meta)
		return ifs_ext4_legacy_descriptor_count(sb, group);

	return ifs_ext4_meta_bg_descriptor_count(sb, group);
}

static unsigned int ifs_ext4_base_meta_blocks(struct super_block *sb,
					       ext4_group_t group)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned int blocks = ext4_bg_has_super(sb, group);

	if (!ext4_has_feature_meta_bg(sb) ||
	    group < le32_to_cpu(sbi->s_es->s_first_meta_bg) *
		    sbi->s_desc_per_block) {
		if (blocks) {
			blocks +=
				ifs_ext4_legacy_descriptor_count(sb, group);
			blocks += le16_to_cpu(
				sbi->s_es->s_reserved_gdt_blocks);
		}
	} else {
		blocks += ifs_ext4_meta_bg_descriptor_count(sb, group);
	}

	return blocks;
}

unsigned int ext4_num_base_meta_blocks(struct super_block *sb,
				       ext4_group_t group)
{
	return ifs_ext4_base_meta_blocks(sb, group);
}

ext4_fsblk_t ext4_inode_to_goal_block(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	ext4_group_t group = ei->i_block_group;
	unsigned int flex = ext4_flex_bg_size(sbi);
	ext4_fsblk_t group_first;
	ext4_fsblk_t filesystem_last;
	ext4_fsblk_t group_blocks;
	ext4_grpblk_t colour;

	if (flex >= EXT4_FLEX_SIZE_DIR_ALLOC_SCHEME) {
		group &= ~(flex - 1);
		if (S_ISREG(inode->i_mode))
			group++;
	}

	group_first = ext4_group_first_block_no(inode->i_sb, group);
	if (test_opt(inode->i_sb, DELALLOC))
		return group_first;

	filesystem_last = ext4_blocks_count(sbi->s_es) - 1;
	if (group_first >= filesystem_last)
		return group_first;

	group_blocks = min_t(ext4_fsblk_t,
			     EXT4_BLOCKS_PER_GROUP(inode->i_sb),
			     filesystem_last - group_first + 1);
	colour = (task_pid_nr(current) & 15U) * (group_blocks / 16U);
	return group_first + colour;
}

unsigned int ext4_count_free(char *bitmap, unsigned int bytes)
{
	return bytes * BITS_PER_BYTE - memweight(bitmap, bytes);
}

static u32 ifs_ext4_inode_bitmap_checksum(struct super_block *sb,
					  struct buffer_head *bh)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned int bytes = EXT4_INODES_PER_GROUP(sb) >> 3;

	return ext4_chksum(sbi, sbi->s_csum_seed,
			   (u8 *)bh->b_data, bytes);
}

int ext4_inode_bitmap_csum_verify(struct super_block *sb,
				  struct ext4_group_desc *desc,
				  struct buffer_head *bh)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	u32 stored;
	u32 calculated;

	if (!ext4_has_metadata_csum(sb))
		return 1;

	stored = le16_to_cpu(desc->bg_inode_bitmap_csum_lo);
	calculated = ifs_ext4_inode_bitmap_checksum(sb, bh);
	if (sbi->s_desc_size >= EXT4_BG_INODE_BITMAP_CSUM_HI_END)
		stored |=
			(u32)le16_to_cpu(desc->bg_inode_bitmap_csum_hi) << 16;
	else
		calculated &= 0xffffU;

	return stored == calculated;
}

void ext4_inode_bitmap_csum_set(struct super_block *sb,
				struct ext4_group_desc *desc,
				struct buffer_head *bh)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	u32 checksum;

	if (!ext4_has_metadata_csum(sb))
		return;

	checksum = ifs_ext4_inode_bitmap_checksum(sb, bh);
	desc->bg_inode_bitmap_csum_lo =
		cpu_to_le16(checksum & 0xffffU);
	if (sbi->s_desc_size >= EXT4_BG_INODE_BITMAP_CSUM_HI_END)
		desc->bg_inode_bitmap_csum_hi =
			cpu_to_le16(checksum >> 16);
}

static u32 ifs_ext4_block_bitmap_checksum(struct super_block *sb,
					  struct buffer_head *bh)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned int bytes = EXT4_CLUSTERS_PER_GROUP(sb) >> 3;

	return ext4_chksum(sbi, sbi->s_csum_seed,
			   (u8 *)bh->b_data, bytes);
}

int ext4_block_bitmap_csum_verify(struct super_block *sb,
				  struct ext4_group_desc *desc,
				  struct buffer_head *bh)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	u32 stored;
	u32 calculated;

	if (!ext4_has_metadata_csum(sb))
		return 1;

	stored = le16_to_cpu(desc->bg_block_bitmap_csum_lo);
	calculated = ifs_ext4_block_bitmap_checksum(sb, bh);
	if (sbi->s_desc_size >= EXT4_BG_BLOCK_BITMAP_CSUM_HI_END)
		stored |=
			(u32)le16_to_cpu(desc->bg_block_bitmap_csum_hi) << 16;
	else
		calculated &= 0xffffU;

	return stored == calculated;
}

void ext4_block_bitmap_csum_set(struct super_block *sb,
				struct ext4_group_desc *desc,
				struct buffer_head *bh)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	u32 checksum;

	if (!ext4_has_metadata_csum(sb))
		return;

	checksum = ifs_ext4_block_bitmap_checksum(sb, bh);
	desc->bg_block_bitmap_csum_lo =
		cpu_to_le16(checksum & 0xffffU);
	if (sbi->s_desc_size >= EXT4_BG_BLOCK_BITMAP_CSUM_HI_END)
		desc->bg_block_bitmap_csum_hi =
			cpu_to_le16(checksum >> 16);
}


/* ===== protected physical ranges ===== */
/*
 * Infiltrator Filesystem Support — EXT4 protected-block index.
 *
 * This Linux adapter builds a read-mostly index of filesystem-owned physical
 * ranges.  Persistent EXT4 metadata remains authoritative; the tree exists
 * only to reject accidental data mappings into metadata or journal blocks.
 */

#include <linux/fs.h>
#include <linux/overflow.h>
#include <linux/rbtree.h>
#include <linux/rcupdate.h>
#include <linux/slab.h>

#include "ext4.h"

struct ifs_ext4_protected_range {
	struct rb_node node;
	ext4_fsblk_t first;
	ext4_fsblk_t last;
	u32 owner_ino;
};

static struct kmem_cache *ifs_ext4_range_cache;

static bool ifs_ext4_make_range(ext4_fsblk_t first, unsigned int count,
				ext4_fsblk_t *last)
{
	ext4_fsblk_t delta;

	if (!count)
		return false;

	delta = (ext4_fsblk_t)count - 1;
	if (check_add_overflow(first, delta, last))
		return false;

	return true;
}

static void ifs_ext4_free_ranges(struct ext4_system_blocks *index)
{
	struct ifs_ext4_protected_range *range;
	struct ifs_ext4_protected_range *next;

	rbtree_postorder_for_each_entry_safe(range, next, &index->root, node)
		kmem_cache_free(ifs_ext4_range_cache, range);
}

static bool ifs_ext4_can_join(const struct ifs_ext4_protected_range *left,
			      const struct ifs_ext4_protected_range *right)
{
	if (left->owner_ino != right->owner_ino ||
	    left->last == (ext4_fsblk_t)-1)
		return false;

	return left->last + 1 == right->first;
}

static int ifs_ext4_insert_range(struct ext4_system_blocks *index,
				 ext4_fsblk_t first,
				 unsigned int count,
				 u32 owner_ino)
{
	struct rb_node **link = &index->root.rb_node;
	struct rb_node *parent = NULL;
	struct ifs_ext4_protected_range *cursor = NULL;
	struct ifs_ext4_protected_range *before = NULL;
	struct ifs_ext4_protected_range *after = NULL;
	struct ifs_ext4_protected_range *added;
	ext4_fsblk_t last;

	if (!ifs_ext4_make_range(first, count, &last))
		return -EFSCORRUPTED;

	while (*link) {
		parent = *link;
		cursor = rb_entry(parent, struct ifs_ext4_protected_range, node);

		if (last < cursor->first) {
			after = cursor;
			link = &parent->rb_left;
			continue;
		}
		if (first > cursor->last) {
			before = cursor;
			link = &parent->rb_right;
			continue;
		}

		/* System ranges may never overlap, even for the same owner. */
		return -EFSCORRUPTED;
	}

	if (before && before->last + 1 == first &&
	    before->owner_ino == owner_ino) {
		before->last = last;

		if (after && ifs_ext4_can_join(before, after)) {
			before->last = after->last;
			rb_erase(&after->node, &index->root);
			kmem_cache_free(ifs_ext4_range_cache, after);
		}
		return 0;
	}

	if (after && last != (ext4_fsblk_t)-1 &&
	    last + 1 == after->first &&
	    after->owner_ino == owner_ino) {
		/*
		 * Re-keying an existing node in place would violate rb-tree ordering.
		 * Insert the replacement first, then remove the old key.
		 */
		added = kmem_cache_alloc(ifs_ext4_range_cache, GFP_KERNEL);
		if (!added)
			return -ENOMEM;

		added->first = first;
		added->last = after->last;
		added->owner_ino = owner_ino;
		rb_link_node(&added->node, parent, link);
		rb_insert_color(&added->node, &index->root);
		rb_erase(&after->node, &index->root);
		kmem_cache_free(ifs_ext4_range_cache, after);
		return 0;
	}

	added = kmem_cache_alloc(ifs_ext4_range_cache, GFP_KERNEL);
	if (!added)
		return -ENOMEM;

	added->first = first;
	added->last = last;
	added->owner_ino = owner_ino;
	rb_link_node(&added->node, parent, link);
	rb_insert_color(&added->node, &index->root);
	return 0;
}

static int ifs_ext4_index_inode_blocks(struct super_block *sb,
				       struct ext4_system_blocks *index,
				       u32 ino)
{
	struct inode *inode;
	ext4_lblk_t logical = 0;
	u64 block_count;
	int error = 0;

	if (ino < EXT4_ROOT_INO ||
	    ino > le32_to_cpu(EXT4_SB(sb)->s_es->s_inodes_count))
		return -EINVAL;

	inode = ext4_iget(sb, ino, EXT4_IGET_SPECIAL);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	block_count = DIV_ROUND_UP_ULL(i_size_read(inode), sb->s_blocksize);
	if (block_count > (u64)U32_MAX + 1ULL) {
		error = -EFBIG;
		goto out;
	}

	while ((u64)logical < block_count) {
		struct ext4_map_blocks map = {
			.m_lblk = logical,
			.m_len = (unsigned int)min_t(u64,
				block_count - logical, (u64)UINT_MAX),
		};
		int mapped;

		cond_resched();
		mapped = ext4_map_blocks(NULL, inode, &map, 0);
		if (mapped < 0) {
			error = mapped;
			break;
		}
		if (!mapped) {
			logical++;
			continue;
		}

		error = ifs_ext4_insert_range(index, map.m_pblk,
					     (unsigned int)mapped, ino);
		if (error)
			break;

		logical += (ext4_lblk_t)mapped;
	}

out:
	iput(inode);
	return error;
}

static void ifs_ext4_free_index_rcu(struct rcu_head *rcu)
{
	struct ext4_system_blocks *index =
		container_of(rcu, struct ext4_system_blocks, rcu);

	ifs_ext4_free_ranges(index);
	kfree(index);
}

int __init ext4_init_system_zone(void)
{
	ifs_ext4_range_cache = kmem_cache_create(
		"ifs_ext4_protected_range",
		sizeof(struct ifs_ext4_protected_range),
		0, SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT, NULL);

	return ifs_ext4_range_cache ? 0 : -ENOMEM;
}

void ext4_exit_system_zone(void)
{
	rcu_barrier();
	if (ifs_ext4_range_cache)
		kmem_cache_destroy(ifs_ext4_range_cache);
	ifs_ext4_range_cache = NULL;
}

int ext4_setup_system_zone(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_system_blocks *index;
	ext4_group_t group;
	ext4_group_t groups = ext4_get_groups_count(sb);
	int error = 0;

	index = kzalloc(sizeof(*index), GFP_KERNEL);
	if (!index)
		return -ENOMEM;
	index->root = RB_ROOT;

	for (group = 0; group < groups; group++) {
		struct ext4_group_desc *descriptor;
		unsigned int prefix;

		cond_resched();

		prefix = ext4_num_base_meta_blocks(sb, group);
		if (prefix) {
			error = ifs_ext4_insert_range(
				index, ext4_group_first_block_no(sb, group),
				prefix, 0);
			if (error)
				goto fail;
		}

		descriptor = ext4_get_group_desc(sb, group, NULL);
		if (!descriptor) {
			error = -EFSCORRUPTED;
			goto fail;
		}

		error = ifs_ext4_insert_range(
			index, ext4_block_bitmap(sb, descriptor), 1, 0);
		if (error)
			goto fail;

		error = ifs_ext4_insert_range(
			index, ext4_inode_bitmap(sb, descriptor), 1, 0);
		if (error)
			goto fail;

		error = ifs_ext4_insert_range(
			index, ext4_inode_table(sb, descriptor),
			sbi->s_itb_per_group, 0);
		if (error)
			goto fail;
	}

	if (ext4_has_feature_journal(sb) &&
	    le32_to_cpu(sbi->s_es->s_journal_inum)) {
		error = ifs_ext4_index_inode_blocks(
			sb, index, le32_to_cpu(sbi->s_es->s_journal_inum));
		if (error)
			goto fail;
	}

	rcu_assign_pointer(sbi->s_system_blks, index);
	return 0;

fail:
	ifs_ext4_free_ranges(index);
	kfree(index);
	return error;
}

void ext4_release_system_zone(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_system_blocks *index;

	index = rcu_dereference_protected(
		sbi->s_system_blks, lockdep_is_held(&sb->s_umount));
	RCU_INIT_POINTER(sbi->s_system_blks, NULL);

	if (index)
		call_rcu(&index->rcu, ifs_ext4_free_index_rcu);
}

int ext4_sb_block_valid(struct super_block *sb, struct inode *inode,
			ext4_fsblk_t start, unsigned int count)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_system_blocks *index;
	ext4_fsblk_t total = ext4_blocks_count(sbi->s_es);
	ext4_fsblk_t last;
	struct rb_node *node;
	int valid = 1;

	if (!ifs_ext4_make_range(start, count, &last) ||
	    start <= le32_to_cpu(sbi->s_es->s_first_data_block) ||
	    start >= total || last >= total)
		return 0;

	rcu_read_lock();
	index = rcu_dereference(sbi->s_system_blks);
	if (!index)
		goto out;

	node = index->root.rb_node;
	while (node) {
		struct ifs_ext4_protected_range *range =
			rb_entry(node, struct ifs_ext4_protected_range, node);

		if (last < range->first) {
			node = node->rb_left;
		} else if (start > range->last) {
			node = node->rb_right;
		} else {
			valid = inode && range->owner_ino == inode->i_ino;
			break;
		}
	}

out:
	rcu_read_unlock();
	return valid;
}

int ext4_inode_block_valid(struct inode *inode, ext4_fsblk_t start,
			   unsigned int count)
{
	return ext4_sb_block_valid(inode->i_sb, inode, start, count);
}

int ext4_check_blockref(const char *function, unsigned int line,
			struct inode *inode, __le32 *refs,
			unsigned int count)
{
	journal_t *journal = EXT4_SB(inode->i_sb)->s_journal;
	unsigned int index;

	if (journal && inode == journal->j_inode)
		return 0;

	for (index = 0; index < count; index++) {
		ext4_fsblk_t block = le32_to_cpu(refs[index]);

		if (!block)
			continue;
		if (ext4_inode_block_valid(inode, block, 1))
			continue;

		ext4_error_inode(inode, function, line, block,
				 "invalid block reference");
		return -EFSCORRUPTED;
	}

	return 0;
}

