// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2003-2006, Cluster File Systems, Inc, info@clusterfs.com
 * Written by Alex Tomas <alex@clusterfs.com>
 */

/*
 * EXT4 — Multiblock allocator
 *
 * Purpose:
 *   Implements EXT4's extent-oriented free-space search, preallocation, locality grouping and buddy-based allocation policy.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Buddy/cache structures are derived acceleration state; persistent bitmaps and allocation counters remain authoritative and must agree at transaction boundaries.
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

#include "ext4_jbd2.h"
#include "mballoc.h"
#include <linux/log2.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/nospec.h>
#include <linux/backing-dev.h>
#include <linux/freezer.h>
#include <trace/events/ext4.h>
#include <kunit/static_stub.h>


static struct kmem_cache *ext4_pspace_cachep;
static struct kmem_cache *ext4_ac_cachep;
static struct kmem_cache *ext4_free_data_cachep;


#define NR_GRPINFO_CACHES 8
static struct kmem_cache *ext4_groupinfo_caches[NR_GRPINFO_CACHES];

static const char * const ext4_groupinfo_slab_names[NR_GRPINFO_CACHES] = {
	"ext4_groupinfo_1k", "ext4_groupinfo_2k", "ext4_groupinfo_4k",
	"ext4_groupinfo_8k", "ext4_groupinfo_16k", "ext4_groupinfo_32k",
	"ext4_groupinfo_64k", "ext4_groupinfo_128k"
};

static void ext4_mb_generate_from_pa(struct super_block *sb, void *bitmap,
					ext4_group_t group);
static void ext4_mb_new_preallocation(struct ext4_allocation_context *ac);

static int ext4_mb_scan_group(struct ext4_allocation_context *ac,
			      ext4_group_t group);

static int ext4_try_to_trim_range(struct super_block *sb,
		struct ext4_buddy *e4b, ext4_grpblk_t start,
		ext4_grpblk_t max, ext4_grpblk_t minblocks);


static DEFINE_PER_CPU(u64, discard_pa_seq);
/**
 * ext4_get_discard_pa_seq_sum - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline u64 ext4_get_discard_pa_seq_sum(void)
{
	int __cpu;
	u64 __seq = 0;

	for_each_possible_cpu(__cpu)
		__seq += per_cpu(discard_pa_seq, __cpu);
	return __seq;
}

/**
 * mb_correct_addr_and_bit - Implements the mb correct addr and bit operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void *mb_correct_addr_and_bit(int *bit, void *addr)
{
#if BITS_PER_LONG == 64
	*bit += ((unsigned long) addr & 7UL) << 3;
	addr = (void *) ((unsigned long) addr & ~7UL);
#elif BITS_PER_LONG == 32
	*bit += ((unsigned long) addr & 3UL) << 3;
	addr = (void *) ((unsigned long) addr & ~3UL);
#else
#error "how many bits you are?!"
#endif
	return addr;
}

/**
 * mb_test_bit - Implements the mb test bit operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int mb_test_bit(int bit, void *addr)
{


	addr = mb_correct_addr_and_bit(&bit, addr);
	return ext4_test_bit(bit, addr);
}

/**
 * mb_set_bit - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void mb_set_bit(int bit, void *addr)
{
	addr = mb_correct_addr_and_bit(&bit, addr);
	ext4_set_bit(bit, addr);
}

/**
 * mb_clear_bit - Implements the mb clear bit operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void mb_clear_bit(int bit, void *addr)
{
	addr = mb_correct_addr_and_bit(&bit, addr);
	ext4_clear_bit(bit, addr);
}

/**
 * mb_test_and_clear_bit - Implements the mb test and clear bit operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int mb_test_and_clear_bit(int bit, void *addr)
{
	addr = mb_correct_addr_and_bit(&bit, addr);
	return ext4_test_and_clear_bit(bit, addr);
}

/**
 * mb_find_next_zero_bit - Locates filesystem state without changing the authoritative persistent representation unless the surrounding API explicitly permits it.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int mb_find_next_zero_bit(void *addr, int max, int start)
{
	int fix = 0, ret, tmpmax;
	addr = mb_correct_addr_and_bit(&fix, addr);
	tmpmax = max + fix;
	start += fix;

	ret = ext4_find_next_zero_bit(addr, tmpmax, start) - fix;
	if (ret > max)
		return max;
	return ret;
}

/**
 * mb_find_next_bit - Locates filesystem state without changing the authoritative persistent representation unless the surrounding API explicitly permits it.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int mb_find_next_bit(void *addr, int max, int start)
{
	int fix = 0, ret, tmpmax;
	addr = mb_correct_addr_and_bit(&fix, addr);
	tmpmax = max + fix;
	start += fix;

	ret = ext4_find_next_bit(addr, tmpmax, start) - fix;
	if (ret > max)
		return max;
	return ret;
}

/**
 * mb_find_buddy - Locates filesystem state without changing the authoritative persistent representation unless the surrounding API explicitly permits it.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void *mb_find_buddy(struct ext4_buddy *e4b, int order, int *max)
{
	char *bb;

	BUG_ON(e4b->bd_bitmap == e4b->bd_buddy);
	BUG_ON(max == NULL);

	if (order > e4b->bd_blkbits + 1) {
		*max = 0;
		return NULL;
	}


	if (order == 0) {
		*max = 1 << (e4b->bd_blkbits + 3);
		return e4b->bd_bitmap;
	}

	bb = e4b->bd_buddy + EXT4_SB(e4b->bd_sb)->s_mb_offsets[order];
	*max = EXT4_SB(e4b->bd_sb)->s_mb_maxs[order];

	return bb;
}

#ifdef DOUBLE_CHECK
/**
 * mb_free_blocks_double - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void mb_free_blocks_double(struct inode *inode, struct ext4_buddy *e4b,
			   int first, int count)
{
	int i;
	struct super_block *sb = e4b->bd_sb;

	if (unlikely(e4b->bd_info->bb_bitmap == NULL))
		return;
	assert_spin_locked(ext4_group_lock_ptr(sb, e4b->bd_group));
	for (i = 0; i < count; i++) {
		if (!mb_test_bit(first + i, e4b->bd_info->bb_bitmap)) {
			ext4_fsblk_t blocknr;

			blocknr = ext4_group_first_block_no(sb, e4b->bd_group);
			blocknr += EXT4_C2B(EXT4_SB(sb), first + i);
			ext4_mark_group_bitmap_corrupted(sb, e4b->bd_group,
					EXT4_GROUP_INFO_BBITMAP_CORRUPT);
			ext4_grp_locked_error(sb, e4b->bd_group,
					      inode ? inode->i_ino : 0,
					      blocknr,
					      "freeing block already freed "
					      "(bit %u)",
					      first + i);
		}
		mb_clear_bit(first + i, e4b->bd_info->bb_bitmap);
	}
}

/**
 * mb_mark_used_double - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void mb_mark_used_double(struct ext4_buddy *e4b, int first, int count)
{
	int i;

	if (unlikely(e4b->bd_info->bb_bitmap == NULL))
		return;
	assert_spin_locked(ext4_group_lock_ptr(e4b->bd_sb, e4b->bd_group));
	for (i = 0; i < count; i++) {
		BUG_ON(mb_test_bit(first + i, e4b->bd_info->bb_bitmap));
		mb_set_bit(first + i, e4b->bd_info->bb_bitmap);
	}
}

/**
 * mb_cmp_bitmaps - Implements the mb cmp bitmaps operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void mb_cmp_bitmaps(struct ext4_buddy *e4b, void *bitmap)
{
	if (unlikely(e4b->bd_info->bb_bitmap == NULL))
		return;
	if (memcmp(e4b->bd_info->bb_bitmap, bitmap, e4b->bd_sb->s_blocksize)) {
		unsigned char *b1, *b2;
		int i;
		b1 = (unsigned char *) e4b->bd_info->bb_bitmap;
		b2 = (unsigned char *) bitmap;
		for (i = 0; i < e4b->bd_sb->s_blocksize; i++) {
			if (b1[i] != b2[i]) {
				ext4_msg(e4b->bd_sb, KERN_ERR,
					 "corruption in group %u "
					 "at byte %u(%u): %x in copy != %x "
					 "on disk/prealloc",
					 e4b->bd_group, i, i * 8, b1[i], b2[i]);
				BUG();
			}
		}
	}
}

/**
 * mb_group_bb_bitmap_alloc - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void mb_group_bb_bitmap_alloc(struct super_block *sb,
			struct ext4_group_info *grp, ext4_group_t group)
{
	struct buffer_head *bh;

	grp->bb_bitmap = kmalloc(sb->s_blocksize, GFP_NOFS);
	if (!grp->bb_bitmap)
		return;

	bh = ext4_read_block_bitmap(sb, group);
	if (IS_ERR_OR_NULL(bh)) {
		kfree(grp->bb_bitmap);
		grp->bb_bitmap = NULL;
		return;
	}

	memcpy(grp->bb_bitmap, bh->b_data, sb->s_blocksize);
	put_bh(bh);
}

/**
 * mb_group_bb_bitmap_free - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void mb_group_bb_bitmap_free(struct ext4_group_info *grp)
{
	kfree(grp->bb_bitmap);
}

#else
/**
 * mb_free_blocks_double - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void mb_free_blocks_double(struct inode *inode,
				struct ext4_buddy *e4b, int first, int count)
{
	return;
}
/**
 * mb_mark_used_double - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void mb_mark_used_double(struct ext4_buddy *e4b,
						int first, int count)
{
	return;
}
/**
 * mb_cmp_bitmaps - Implements the mb cmp bitmaps operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void mb_cmp_bitmaps(struct ext4_buddy *e4b, void *bitmap)
{
	return;
}

/**
 * mb_group_bb_bitmap_alloc - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void mb_group_bb_bitmap_alloc(struct super_block *sb,
			struct ext4_group_info *grp, ext4_group_t group)
{
	return;
}

/**
 * mb_group_bb_bitmap_free - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void mb_group_bb_bitmap_free(struct ext4_group_info *grp)
{
	return;
}
#endif

#ifdef AGGRESSIVE_CHECK

#define MB_CHECK_ASSERT(assert)						\
/**
 * MB_CHECK_ASSERT - Implements the MB CHECK ASSERT operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
do {									\
	if (!(assert)) {						\
		printk(KERN_EMERG					\
			"Assertion failure in %s() at %s:%d: \"%s\"\n",	\
			function, file, line, # assert);		\
		BUG();							\
	}								\
}/**
 * __mb_check_buddy - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)


static void __mb_check_buddy(struct ext4_buddy *e4b, char *file,
				const char *function, int line)
{
	struct super_block *sb = e4b->bd_sb;
	int order = e4b->bd_blkbits + 1;
	int max;
	int max2;
	int i;
	int j;
	int k;
	int count;
	struct ext4_group_info *grp;
	int fragments = 0;
	int fstart;
	struct list_head *cur;
	void *buddy;
	void *buddy2;

	if (e4b->bd_info->bb_check_counter++ % 10)
		return;

	while (order > 1) {
		buddy = mb_find_buddy(e4b, order, &max);
		MB_CHECK_ASSERT(buddy);
		buddy2 = mb_find_buddy(e4b, order - 1, &max2);
		MB_CHECK_ASSERT(buddy2);
		MB_CHECK_ASSERT(buddy != buddy2);
		MB_CHECK_ASSERT(max * 2 == max2);

		count = 0;
		for (i = 0; i < max; i++) {

			if (mb_test_bit(i, buddy)) {

				if (!mb_test_bit(i << 1, buddy2)) {
					MB_CHECK_ASSERT(
						mb_test_bit((i<<1)+1, buddy2));
				}
				continue;
			}

			count++;
		}
		MB_CHECK_ASSERT(e4b->bd_info->bb_counters[order] == count);
		order--;
	}

	fstart = -1;
	buddy = mb_find_buddy(e4b, 0, &max);
	for (i = 0; i < max; i++) {
		if (!mb_test_bit(i, buddy)) {
			MB_CHECK_ASSERT(i >= e4b->bd_info->bb_first_free);
			if (fstart == -1) {
				fragments++;
				fstart = i;
			}
		} else {
			fstart = -1;
		}
		if (!(i & 1)) {
			int in_use, zero_bit_count = 0;

			in_use = mb_test_bit(i, buddy) || mb_test_bit(i + 1, buddy);
			for (j = 1; j < e4b->bd_blkbits + 2; j++) {
				buddy2 = mb_find_buddy(e4b, j, &max2);
				k = i >> j;
				MB_CHECK_ASSERT(k < max2);
				if (!mb_test_bit(k, buddy2))
					zero_bit_count++;
			}
			MB_CHECK_ASSERT(zero_bit_count == !in_use);
		}
	}
	MB_CHECK_ASSERT(!EXT4_MB_GRP_NEED_INIT(e4b->bd_info));
	MB_CHECK_ASSERT(e4b->bd_info->bb_fragments == fragments);

	grp = ext4_get_group_info(sb, e4b->bd_group);
	if (!grp)
		return;
	list_for_each(cur, &grp->bb_prealloc_list) {
		ext4_group_t groupnr;
		struct ext4_prealloc_space *pa;
		pa = list_entry(cur, struct ext4_prealloc_space, pa_group_list);
		if (!pa->pa_len)
			continue;
		ext4_get_group_no_and_offset(sb, pa->pa_pstart, &groupnr, &k);
		MB_CHECK_ASSERT(groupnr == e4b->bd_group);
		for (i = 0; i < pa->pa_len; i++)
			MB_CHECK_ASSERT(mb_test_bit(k + i, buddy));
	}
}
#undef MB_CHECK_ASSERT
#define mb_check_buddy(e4b) __mb_check_buddy(e4b,	\
/**
 * ext4_mb_mark_free_simple - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
					__FILE__, __func__, __LINE__)
#else
#define mb_check_buddy(e4b)
#endif


static void ext4_mb_mark_free_simple(struct super_block *sb,
				void *buddy, ext4_grpblk_t first, ext4_grpblk_t len,
					struct ext4_group_info *grp)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_grpblk_t min;
	ext4_grpblk_t max;
	ext4_grpblk_t chunk;
	unsigned int border;

	BUG_ON(len > EXT4_CLUSTERS_PER_GROUP(sb));

	border = 2 << sb->s_blocksize_bits;

	while (len > 0) {

		max = ffs(first | border) - 1;


		min = fls(len) - 1;

		if (max < min)
			min = max;
		chunk = 1 << min;


		grp->bb_counters[min]++;
		if (min > 0)
			mb_clear_bit(first >> min,
				     buddy + sbi->s_mb_offsets[min]);

		len -= chunk;
		first += chunk;
	}
}

/**
 * mb_avg_fragment_size_order - Implements the mb avg fragment size order operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int mb_avg_fragment_size_order(struct super_block *sb, ext4_grpblk_t len)
{
	int order;


	order = fls(len) - 2;
	if (order < 0)
		return 0;
	if (order == MB_NUM_ORDERS(sb))
		order--;
	if (WARN_ON_ONCE(order > MB_NUM_ORDERS(sb)))
		order = MB_NUM_ORDERS(sb) - 1;
	return order;
}


/**
 * mb_update_avg_fragment_size - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void
mb_update_avg_fragment_size(struct super_block *sb, struct ext4_group_info *grp)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int new, old;

	if (!test_opt2(sb, MB_OPTIMIZE_SCAN))
		return;

	old = grp->bb_avg_fragment_size_order;
	new = grp->bb_fragments == 0 ? -1 :
	      mb_avg_fragment_size_order(sb, grp->bb_free / grp->bb_fragments);
	if (new == old)
		return;

	if (old >= 0)
		xa_erase(&sbi->s_mb_avg_fragment_size[old], grp->bb_group);

	grp->bb_avg_fragment_size_order = new;
	if (new >= 0) {


		int err = xa_insert(&sbi->s_mb_avg_fragment_size[new],
				    grp->bb_group, grp, GFP_ATOMIC);
		if (err)
			mb_debug(sb, "insert group: %u to s_mb_avg_fragment_size[%d] failed, err %d",
				 grp->bb_group, new, err);
	}
}

/**
 * ext4_get_allocation_groups_count - Computes derived filesystem state used for validation, accounting or policy decisions.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static ext4_group_t ext4_get_allocation_groups_count(
				struct ext4_allocation_context *ac)
{
	ext4_group_t ngroups = ext4_get_groups_count(ac->ac_sb);


	if (!(ext4_test_inode_flag(ac->ac_inode, EXT4_INODE_EXTENTS)))
		ngroups = EXT4_SB(ac->ac_sb)->s_blockfile_groups;


	smp_rmb();

	return ngroups;
}

/**
 * ext4_mb_scan_groups_xa_range - Implements the mb scan groups xa range operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_scan_groups_xa_range(struct ext4_allocation_context *ac,
					struct xarray *xa,
					ext4_group_t start, ext4_group_t end)
{
	struct super_block *sb = ac->ac_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	enum criteria cr = ac->ac_criteria;
	ext4_group_t ngroups = ext4_get_allocation_groups_count(ac);
	unsigned long group = start;
	struct ext4_group_info *grp;

	if (WARN_ON_ONCE(end > ngroups || start >= end))
		return 0;

	xa_for_each_range(xa, group, grp, start, end - 1) {
		int err;

		if (sbi->s_mb_stats)
			atomic64_inc(&sbi->s_bal_cX_groups_considered[cr]);

		err = ext4_mb_scan_group(ac, grp->bb_group);
		if (err || ac->ac_status != AC_STATUS_CONTINUE)
			return err;

		cond_resched();
	}

	return 0;
}


/**
 * ext4_mb_scan_groups_largest_free_order_range - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int
ext4_mb_scan_groups_largest_free_order_range(struct ext4_allocation_context *ac,
					     int order, ext4_group_t start,
					     ext4_group_t end)
{
	struct xarray *xa = &EXT4_SB(ac->ac_sb)->s_mb_largest_free_orders[order];

	if (xa_empty(xa))
		return 0;

	return ext4_mb_scan_groups_xa_range(ac, xa, start, end);
}


/**
 * ext4_mb_scan_groups_p2_aligned - Implements the mb scan groups p2 aligned operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_scan_groups_p2_aligned(struct ext4_allocation_context *ac,
					  ext4_group_t group)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	int i;
	int ret = 0;
	ext4_group_t start, end;

	start = group;
	end = ext4_get_allocation_groups_count(ac);
wrap_around:
	for (i = ac->ac_2order; i < MB_NUM_ORDERS(ac->ac_sb); i++) {
		ret = ext4_mb_scan_groups_largest_free_order_range(ac, i,
								   start, end);
		if (ret || ac->ac_status != AC_STATUS_CONTINUE)
			return ret;
	}
	if (start) {
		end = start;
		start = 0;
		goto wrap_around;
	}

	if (sbi->s_mb_stats)
		atomic64_inc(&sbi->s_bal_cX_failed[ac->ac_criteria]);


	ac->ac_criteria = CR_GOAL_LEN_FAST;
	return ret;
}


/**
 * ext4_mb_scan_groups_avg_frag_order_range - Implements the mb scan groups avg frag order range operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int
ext4_mb_scan_groups_avg_frag_order_range(struct ext4_allocation_context *ac,
					 int order, ext4_group_t start,
					 ext4_group_t end)
{
	struct xarray *xa = &EXT4_SB(ac->ac_sb)->s_mb_avg_fragment_size[order];

	if (xa_empty(xa))
		return 0;

	return ext4_mb_scan_groups_xa_range(ac, xa, start, end);
}


/**
 * ext4_mb_scan_groups_goal_fast - Implements the mb scan groups goal fast operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_scan_groups_goal_fast(struct ext4_allocation_context *ac,
					 ext4_group_t group)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	int i, ret = 0;
	ext4_group_t start, end;

	start = group;
	end = ext4_get_allocation_groups_count(ac);
wrap_around:
	i = mb_avg_fragment_size_order(ac->ac_sb, ac->ac_g_ex.fe_len);
	for (; i < MB_NUM_ORDERS(ac->ac_sb); i++) {
		ret = ext4_mb_scan_groups_avg_frag_order_range(ac, i,
							       start, end);
		if (ret || ac->ac_status != AC_STATUS_CONTINUE)
			return ret;
	}
	if (start) {
		end = start;
		start = 0;
		goto wrap_around;
	}

	if (sbi->s_mb_stats)
		atomic64_inc(&sbi->s_bal_cX_failed[ac->ac_criteria]);


	if (ac->ac_flags & EXT4_MB_HINT_DATA)
		ac->ac_criteria = CR_BEST_AVAIL_LEN;
	else
		ac->ac_criteria = CR_GOAL_LEN_SLOW;

	return ret;
}


/**
 * ext4_mb_scan_groups_best_avail - Implements the mb scan groups best avail operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_scan_groups_best_avail(struct ext4_allocation_context *ac,
					  ext4_group_t group)
{
	int ret = 0;
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	int i, order, min_order;
	unsigned long num_stripe_clusters = 0;
	ext4_group_t start, end;


	order = fls(ac->ac_g_ex.fe_len) - 1;
	if (WARN_ON_ONCE(order - 1 > MB_NUM_ORDERS(ac->ac_sb)))
		order = MB_NUM_ORDERS(ac->ac_sb);
	min_order = order - sbi->s_mb_best_avail_max_trim_order;
	if (min_order < 0)
		min_order = 0;

	if (sbi->s_stripe > 0) {


		num_stripe_clusters = EXT4_NUM_B2C(sbi, sbi->s_stripe);
		if (1 << min_order < num_stripe_clusters)


			min_order = fls(num_stripe_clusters) - 1;
	}

	if (1 << min_order < ac->ac_o_ex.fe_len)
		min_order = fls(ac->ac_o_ex.fe_len);

	start = group;
	end = ext4_get_allocation_groups_count(ac);
wrap_around:
	for (i = order; i >= min_order; i--) {
		int frag_order;


		ac->ac_g_ex.fe_len = 1 << i;

		if (num_stripe_clusters > 0) {


			ac->ac_g_ex.fe_len = roundup(ac->ac_g_ex.fe_len,
						     num_stripe_clusters);
		}

		frag_order = mb_avg_fragment_size_order(ac->ac_sb,
							ac->ac_g_ex.fe_len);

		ret = ext4_mb_scan_groups_avg_frag_order_range(ac, frag_order,
							       start, end);
		if (ret || ac->ac_status != AC_STATUS_CONTINUE)
			return ret;
	}
	if (start) {
		end = start;
		start = 0;
		goto wrap_around;
	}


	ac->ac_g_ex.fe_len = ac->ac_orig_goal_len;
	if (sbi->s_mb_stats)
		atomic64_inc(&sbi->s_bal_cX_failed[ac->ac_criteria]);
	ac->ac_criteria = CR_GOAL_LEN_SLOW;

	return ret;
}

/**
 * should_optimize_scan - Implements the should optimize scan operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int should_optimize_scan(struct ext4_allocation_context *ac)
{
	if (unlikely(!test_opt2(ac->ac_sb, MB_OPTIMIZE_SCAN)))
		return 0;
	if (ac->ac_criteria >= CR_GOAL_LEN_SLOW)
		return 0;
	return 1;
}


/**
 * next_linear_group - Implements the next linear group operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void next_linear_group(ext4_group_t *group, ext4_group_t ngroups)
{


	*group =  *group + 1 >= ngroups ? 0 : *group + 1;
}

/**
 * ext4_mb_scan_groups_linear - Implements the mb scan groups linear operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_scan_groups_linear(struct ext4_allocation_context *ac,
		ext4_group_t ngroups, ext4_group_t *start, ext4_group_t count)
{
	int ret, i;
	enum criteria cr = ac->ac_criteria;
	struct super_block *sb = ac->ac_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_group_t group = *start;

	for (i = 0; i < count; i++, next_linear_group(&group, ngroups)) {
		ret = ext4_mb_scan_group(ac, group);
		if (ret || ac->ac_status != AC_STATUS_CONTINUE)
			return ret;
		cond_resched();
	}

	*start = group;
	if (count == ngroups)
		ac->ac_criteria++;


	if (sbi->s_mb_stats && i == ngroups)
		atomic64_inc(&sbi->s_bal_cX_failed[cr]);

	return 0;
}

/**
 * ext4_mb_scan_groups - Implements the mb scan groups operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_scan_groups(struct ext4_allocation_context *ac)
{
	int ret = 0;
	ext4_group_t start;
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	ext4_group_t ngroups = ext4_get_allocation_groups_count(ac);


	start = ac->ac_g_ex.fe_group;
	if (start >= ngroups)
		start = 0;
	ac->ac_prefetch_grp = start;
	ac->ac_prefetch_nr = 0;

	if (!should_optimize_scan(ac))
		return ext4_mb_scan_groups_linear(ac, ngroups, &start, ngroups);


	if (sbi->s_mb_max_linear_groups)
		ret = ext4_mb_scan_groups_linear(ac, ngroups, &start,
						 sbi->s_mb_max_linear_groups);
	if (ret || ac->ac_status != AC_STATUS_CONTINUE)
		return ret;

	switch (ac->ac_criteria) {
	case CR_POWER2_ALIGNED:
		return ext4_mb_scan_groups_p2_aligned(ac, start);
	case CR_GOAL_LEN_FAST:
		return ext4_mb_scan_groups_goal_fast(ac, start);
	case CR_BEST_AVAIL_LEN:
		return ext4_mb_scan_groups_best_avail(ac, start);
	default:


		WARN_ON(1);
	}

	return 0;
}


/**
 * mb_set_largest_free_order - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void
mb_set_largest_free_order(struct super_block *sb, struct ext4_group_info *grp)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int new, old = grp->bb_largest_free_order;

	for (new = MB_NUM_ORDERS(sb) - 1; new >= 0; new--)
		if (grp->bb_counters[new] > 0)
			break;


	if (new == old)
		return;

	if (old >= 0) {
		struct xarray *xa = &sbi->s_mb_largest_free_orders[old];

		if (!xa_empty(xa) && xa_load(xa, grp->bb_group))
			xa_erase(xa, grp->bb_group);
	}

	grp->bb_largest_free_order = new;
	if (test_opt2(sb, MB_OPTIMIZE_SCAN) && new >= 0 && grp->bb_free) {


		int err = xa_insert(&sbi->s_mb_largest_free_orders[new],
				    grp->bb_group, grp, GFP_ATOMIC);
		if (err)
			mb_debug(sb, "insert group: %u to s_mb_largest_free_orders[%d] failed, err %d",
				 grp->bb_group, new, err);
	}
}

/**
 * ext4_mb_generate_buddy - Implements the mb generate buddy operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack
void ext4_mb_generate_buddy(struct super_block *sb,
			    void *buddy, void *bitmap, ext4_group_t group,
			    struct ext4_group_info *grp)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_grpblk_t max = EXT4_CLUSTERS_PER_GROUP(sb);
	ext4_grpblk_t i = 0;
	ext4_grpblk_t first;
	ext4_grpblk_t len;
	unsigned free = 0;
	unsigned fragments = 0;
	unsigned long long period = get_cycles();


	i = mb_find_next_zero_bit(bitmap, max, 0);
	grp->bb_first_free = i;
	while (i < max) {
		fragments++;
		first = i;
		i = mb_find_next_bit(bitmap, max, i);
		len = i - first;
		free += len;
		if (len > 1)
			ext4_mb_mark_free_simple(sb, buddy, first, len, grp);
		else
			grp->bb_counters[0]++;
		if (i < max)
			i = mb_find_next_zero_bit(bitmap, max, i);
	}
	grp->bb_fragments = fragments;

	if (free != grp->bb_free) {
		ext4_grp_locked_error(sb, group, 0, 0,
				      "block bitmap and bg descriptor "
				      "inconsistent: %u vs %u free clusters",
				      free, grp->bb_free);


		grp->bb_free = free;
		ext4_mark_group_bitmap_corrupted(sb, group,
					EXT4_GROUP_INFO_BBITMAP_CORRUPT);
	}
	mb_set_largest_free_order(sb, grp);
	mb_update_avg_fragment_size(sb, grp);

	clear_bit(EXT4_GROUP_INFO_NEED_INIT_BIT, &(grp->bb_state));

	period = get_cycles() - period;
	atomic_inc(&sbi->s_mb_buddies_generated);
	atomic64_add(period, &sbi->s_mb_generation_time);
}

/**
 * mb_regenerate_buddy - Implements the mb regenerate buddy operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void mb_regenerate_buddy(struct ext4_buddy *e4b)
{
	int count;
	int order = 1;
	void *buddy;

	while ((buddy = mb_find_buddy(e4b, order++, &count)))
		mb_set_bits(buddy, 0, count);

	e4b->bd_info->bb_fragments = 0;
	memset(e4b->bd_info->bb_counters, 0,
		sizeof(*e4b->bd_info->bb_counters) *
		(e4b->bd_sb->s_blocksize_bits + 2));

	ext4_mb_generate_buddy(e4b->bd_sb, e4b->bd_buddy,
		e4b->bd_bitmap, e4b->bd_group, e4b->bd_info);
}


/**
 * ext4_mb_init_cache - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_init_cache(struct folio *folio, char *incore, gfp_t gfp)
{
	ext4_group_t ngroups;
	unsigned int blocksize;
	int blocks_per_page;
	int groups_per_page;
	int err = 0;
	int i;
	ext4_group_t first_group, group;
	int first_block;
	struct super_block *sb;
	struct buffer_head *bhs;
	struct buffer_head **bh = NULL;
	struct inode *inode;
	char *data;
	char *bitmap;
	struct ext4_group_info *grinfo;

	inode = folio->mapping->host;
	sb = inode->i_sb;
	ngroups = ext4_get_groups_count(sb);
	blocksize = i_blocksize(inode);
	blocks_per_page = PAGE_SIZE / blocksize;

	mb_debug(sb, "init folio %lu\n", folio->index);

	groups_per_page = blocks_per_page >> 1;
	if (groups_per_page == 0)
		groups_per_page = 1;


	if (groups_per_page > 1) {
		i = sizeof(struct buffer_head *) * groups_per_page;
		bh = kzalloc(i, gfp);
		if (bh == NULL)
			return -ENOMEM;
	} else
		bh = &bhs;

	first_group = folio->index * blocks_per_page / 2;


	for (i = 0, group = first_group; i < groups_per_page; i++, group++) {
		if (group >= ngroups)
			break;

		grinfo = ext4_get_group_info(sb, group);
		if (!grinfo)
			continue;


		if (folio_test_uptodate(folio) &&
				!EXT4_MB_GRP_NEED_INIT(grinfo)) {
			bh[i] = NULL;
			continue;
		}
		bh[i] = ext4_read_block_bitmap_nowait(sb, group, false);
		if (IS_ERR(bh[i])) {
			err = PTR_ERR(bh[i]);
			bh[i] = NULL;
			goto out;
		}
		mb_debug(sb, "read bitmap for group %u\n", group);
	}


	for (i = 0, group = first_group; i < groups_per_page; i++, group++) {
		int err2;

		if (!bh[i])
			continue;
		err2 = ext4_wait_block_bitmap(sb, group, bh[i]);
		if (!err)
			err = err2;
	}

	first_block = folio->index * blocks_per_page;
	for (i = 0; i < blocks_per_page; i++) {
		group = (first_block + i) >> 1;
		if (group >= ngroups)
			break;

		if (!bh[group - first_group])

			continue;

		if (!buffer_verified(bh[group - first_group]))

			continue;
		err = 0;


		data = folio_address(folio) + (i * blocksize);
		bitmap = bh[group - first_group]->b_data;


		grinfo = ext4_get_group_info(sb, group);
		if (!grinfo) {
			err = -EFSCORRUPTED;
		        goto out;
		}
		if ((first_block + i) & 1) {

			BUG_ON(incore == NULL);
			mb_debug(sb, "put buddy for group %u in folio %lu/%x\n",
				group, folio->index, i * blocksize);
			trace_ext4_mb_buddy_bitmap_load(sb, group);
			grinfo->bb_fragments = 0;
			memset(grinfo->bb_counters, 0,
			       sizeof(*grinfo->bb_counters) *
			       (MB_NUM_ORDERS(sb)));


			ext4_lock_group(sb, group);

			memset(data, 0xff, blocksize);
			ext4_mb_generate_buddy(sb, data, incore, group, grinfo);
			ext4_unlock_group(sb, group);
			incore = NULL;
		} else {

			BUG_ON(incore != NULL);
			mb_debug(sb, "put bitmap for group %u in folio %lu/%x\n",
				group, folio->index, i * blocksize);
			trace_ext4_mb_bitmap_load(sb, group);


			ext4_lock_group(sb, group);
			memcpy(data, bitmap, blocksize);


			ext4_mb_generate_from_pa(sb, data, group);
			WARN_ON_ONCE(!RB_EMPTY_ROOT(&grinfo->bb_free_root));
			ext4_unlock_group(sb, group);


			incore = data;
		}
	}
	folio_mark_uptodate(folio);

out:
	if (bh) {
		for (i = 0; i < groups_per_page; i++)
			brelse(bh[i]);
		if (bh != &bhs)
			kfree(bh);
	}
	return err;
}


/**
 * ext4_mb_get_buddy_page_lock - Implements the mb get buddy page lock operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_get_buddy_page_lock(struct super_block *sb,
		ext4_group_t group, struct ext4_buddy *e4b, gfp_t gfp)
{
	struct inode *inode = EXT4_SB(sb)->s_buddy_cache;
	int block, pnum, poff;
	int blocks_per_page;
	struct folio *folio;

	e4b->bd_buddy_folio = NULL;
	e4b->bd_bitmap_folio = NULL;

	blocks_per_page = PAGE_SIZE / sb->s_blocksize;


	block = group * 2;
	pnum = block / blocks_per_page;
	poff = block % blocks_per_page;
	folio = __filemap_get_folio(inode->i_mapping, pnum,
			FGP_LOCK | FGP_ACCESSED | FGP_CREAT, gfp);
	if (IS_ERR(folio))
		return PTR_ERR(folio);
	BUG_ON(folio->mapping != inode->i_mapping);
	e4b->bd_bitmap_folio = folio;
	e4b->bd_bitmap = folio_address(folio) + (poff * sb->s_blocksize);

	if (blocks_per_page >= 2) {

		return 0;
	}


	folio = __filemap_get_folio(inode->i_mapping, block + 1,
			FGP_LOCK | FGP_ACCESSED | FGP_CREAT, gfp);
	if (IS_ERR(folio))
		return PTR_ERR(folio);
	BUG_ON(folio->mapping != inode->i_mapping);
	e4b->bd_buddy_folio = folio;
	return 0;
}

/**
 * ext4_mb_put_buddy_page_lock - Implements the mb put buddy page lock operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_put_buddy_page_lock(struct ext4_buddy *e4b)
{
	if (e4b->bd_bitmap_folio) {
		folio_unlock(e4b->bd_bitmap_folio);
		folio_put(e4b->bd_bitmap_folio);
	}
	if (e4b->bd_buddy_folio) {
		folio_unlock(e4b->bd_buddy_folio);
		folio_put(e4b->bd_buddy_folio);
	}
}


/**
 * ext4_mb_init_group - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack
int ext4_mb_init_group(struct super_block *sb, ext4_group_t group, gfp_t gfp)
{

	struct ext4_group_info *this_grp;
	struct ext4_buddy e4b;
	struct folio *folio;
	int ret = 0;

	might_sleep();
	mb_debug(sb, "init group %u\n", group);
	this_grp = ext4_get_group_info(sb, group);
	if (!this_grp)
		return -EFSCORRUPTED;


	ret = ext4_mb_get_buddy_page_lock(sb, group, &e4b, gfp);
	if (ret || !EXT4_MB_GRP_NEED_INIT(this_grp)) {


		goto err;
	}

	folio = e4b.bd_bitmap_folio;
	ret = ext4_mb_init_cache(folio, NULL, gfp);
	if (ret)
		goto err;
	if (!folio_test_uptodate(folio)) {
		ret = -EIO;
		goto err;
	}

	if (e4b.bd_buddy_folio == NULL) {


		ret = 0;
		goto err;
	}

	folio = e4b.bd_buddy_folio;
	ret = ext4_mb_init_cache(folio, e4b.bd_bitmap, gfp);
	if (ret)
		goto err;
	if (!folio_test_uptodate(folio)) {
		ret = -EIO;
		goto err;
	}
err:
	ext4_mb_put_buddy_page_lock(&e4b);
	return ret;
}


/**
 * ext4_mb_load_buddy_gfp - Reads or materialises filesystem state for validation or higher-level processing.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack int
ext4_mb_load_buddy_gfp(struct super_block *sb, ext4_group_t group,
		       struct ext4_buddy *e4b, gfp_t gfp)
{
	int blocks_per_page;
	int block;
	int pnum;
	int poff;
	struct folio *folio;
	int ret;
	struct ext4_group_info *grp;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct inode *inode = sbi->s_buddy_cache;

	might_sleep();
	mb_debug(sb, "load group %u\n", group);

	blocks_per_page = PAGE_SIZE / sb->s_blocksize;
	grp = ext4_get_group_info(sb, group);
	if (!grp)
		return -EFSCORRUPTED;

	e4b->bd_blkbits = sb->s_blocksize_bits;
	e4b->bd_info = grp;
	e4b->bd_sb = sb;
	e4b->bd_group = group;
	e4b->bd_buddy_folio = NULL;
	e4b->bd_bitmap_folio = NULL;

	if (unlikely(EXT4_MB_GRP_NEED_INIT(grp))) {


		ret = ext4_mb_init_group(sb, group, gfp);
		if (ret)
			return ret;
	}


	block = group * 2;
	pnum = block / blocks_per_page;
	poff = block % blocks_per_page;


	folio = __filemap_get_folio(inode->i_mapping, pnum, FGP_ACCESSED, 0);
	if (IS_ERR(folio) || !folio_test_uptodate(folio) || folio_test_locked(folio)) {


		if (!IS_ERR(folio))
			folio_put(folio);
		folio = __filemap_get_folio(inode->i_mapping, pnum,
				FGP_LOCK | FGP_ACCESSED | FGP_CREAT, gfp);
		if (!IS_ERR(folio)) {
			if (WARN_RATELIMIT(folio->mapping != inode->i_mapping,
	"ext4: bitmap's mapping != inode->i_mapping\n")) {

				folio_unlock(folio);
				ret = -EINVAL;
				goto err;
			}
			if (!folio_test_uptodate(folio)) {
				ret = ext4_mb_init_cache(folio, NULL, gfp);
				if (ret) {
					folio_unlock(folio);
					goto err;
				}
				mb_cmp_bitmaps(e4b, folio_address(folio) +
					       (poff * sb->s_blocksize));
			}
			folio_unlock(folio);
		}
	}
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto err;
	}
	if (!folio_test_uptodate(folio)) {
		ret = -EIO;
		goto err;
	}


	e4b->bd_bitmap_folio = folio;
	e4b->bd_bitmap = folio_address(folio) + (poff * sb->s_blocksize);

	block++;
	pnum = block / blocks_per_page;
	poff = block % blocks_per_page;

	folio = __filemap_get_folio(inode->i_mapping, pnum, FGP_ACCESSED, 0);
	if (IS_ERR(folio) || !folio_test_uptodate(folio) || folio_test_locked(folio)) {
		if (!IS_ERR(folio))
			folio_put(folio);
		folio = __filemap_get_folio(inode->i_mapping, pnum,
				FGP_LOCK | FGP_ACCESSED | FGP_CREAT, gfp);
		if (!IS_ERR(folio)) {
			if (WARN_RATELIMIT(folio->mapping != inode->i_mapping,
	"ext4: buddy bitmap's mapping != inode->i_mapping\n")) {

				folio_unlock(folio);
				ret = -EINVAL;
				goto err;
			}
			if (!folio_test_uptodate(folio)) {
				ret = ext4_mb_init_cache(folio, e4b->bd_bitmap,
							 gfp);
				if (ret) {
					folio_unlock(folio);
					goto err;
				}
			}
			folio_unlock(folio);
		}
	}
	if (IS_ERR(folio)) {
		ret = PTR_ERR(folio);
		goto err;
	}
	if (!folio_test_uptodate(folio)) {
		ret = -EIO;
		goto err;
	}


	e4b->bd_buddy_folio = folio;
	e4b->bd_buddy = folio_address(folio) + (poff * sb->s_blocksize);

	return 0;

err:
	if (!IS_ERR_OR_NULL(folio))
		folio_put(folio);
	if (e4b->bd_bitmap_folio)
		folio_put(e4b->bd_bitmap_folio);

	e4b->bd_buddy = NULL;
	e4b->bd_bitmap = NULL;
	return ret;
}

/**
 * ext4_mb_load_buddy - Reads or materialises filesystem state for validation or higher-level processing.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_load_buddy(struct super_block *sb, ext4_group_t group,
			      struct ext4_buddy *e4b)
{
	return ext4_mb_load_buddy_gfp(sb, group, e4b, GFP_NOFS);
}

/**
 * ext4_mb_unload_buddy - Implements the mb unload buddy operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_unload_buddy(struct ext4_buddy *e4b)
{
	if (e4b->bd_bitmap_folio)
		folio_put(e4b->bd_bitmap_folio);
	if (e4b->bd_buddy_folio)
		folio_put(e4b->bd_buddy_folio);
}


/**
 * mb_find_order_for_block - Locates filesystem state without changing the authoritative persistent representation unless the surrounding API explicitly permits it.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int mb_find_order_for_block(struct ext4_buddy *e4b, int block)
{
	int order = 1, max;
	void *bb;

	BUG_ON(e4b->bd_bitmap == e4b->bd_buddy);
	BUG_ON(block >= (1 << (e4b->bd_blkbits + 3)));

	while (order <= e4b->bd_blkbits + 1) {
		bb = mb_find_buddy(e4b, order, &max);
		if (!mb_test_bit(block >> order, bb)) {

			return order;
		}
		order++;
	}
	return 0;
}

/**
 * mb_clear_bits - Implements the mb clear bits operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void mb_clear_bits(void *bm, int cur, int len)
{
	__u32 *addr;

	len = cur + len;
	while (cur < len) {
		if ((cur & 31) == 0 && (len - cur) >= 32) {

			addr = bm + (cur >> 3);
			*addr = 0;
			cur += 32;
			continue;
		}
		mb_clear_bit(cur, bm);
		cur++;
	}
}


/**
 * mb_test_and_clear_bits - Implements the mb test and clear bits operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int mb_test_and_clear_bits(void *bm, int cur, int len)
{
	__u32 *addr;
	int zero_bit = -1;

	len = cur + len;
	while (cur < len) {
		if ((cur & 31) == 0 && (len - cur) >= 32) {

			addr = bm + (cur >> 3);
			if (*addr != (__u32)(-1) && zero_bit == -1)
				zero_bit = cur + mb_find_next_zero_bit(addr, 32, 0);
			*addr = 0;
			cur += 32;
			continue;
		}
		if (!mb_test_and_clear_bit(cur, bm) && zero_bit == -1)
			zero_bit = cur;
		cur++;
	}

	return zero_bit;
}

/**
 * mb_set_bits - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void mb_set_bits(void *bm, int cur, int len)
{
	__u32 *addr;

	len = cur + len;
	while (cur < len) {
		if ((cur & 31) == 0 && (len - cur) >= 32) {

			addr = bm + (cur >> 3);
			*addr = 0xffffffff;
			cur += 32;
			continue;
		}
		mb_set_bit(cur, bm);
		cur++;
	}
}

/**
 * mb_buddy_adjust_border - Implements the mb buddy adjust border operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int mb_buddy_adjust_border(int* bit, void* bitmap, int side)
{
	if (mb_test_bit(*bit + side, bitmap)) {
		mb_clear_bit(*bit, bitmap);
		(*bit) -= side;
		return 1;
	}
	else {
		(*bit) += side;
		mb_set_bit(*bit, bitmap);
		return -1;
	}
}

/**
 * mb_buddy_mark_free - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void mb_buddy_mark_free(struct ext4_buddy *e4b, int first, int last)
{
	int max;
	int order = 1;
	void *buddy = mb_find_buddy(e4b, order, &max);

	while (buddy) {
		void *buddy2;


		if (first & 1)
			e4b->bd_info->bb_counters[order] += mb_buddy_adjust_border(&first, buddy, -1);
		if (!(last & 1))
			e4b->bd_info->bb_counters[order] += mb_buddy_adjust_border(&last, buddy, 1);
		if (first > last)
			break;
		order++;

		buddy2 = mb_find_buddy(e4b, order, &max);
		if (!buddy2) {
			mb_clear_bits(buddy, first, last - first + 1);
			e4b->bd_info->bb_counters[order - 1] += last - first + 1;
			break;
		}
		first >>= 1;
		last >>= 1;
		buddy = buddy2;
	}
}

/**
 * mb_free_blocks - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void mb_free_blocks(struct inode *inode, struct ext4_buddy *e4b,
			   int first, int count)
{
	int left_is_free = 0;
	int right_is_free = 0;
	int block;
	int last = first + count - 1;
	struct super_block *sb = e4b->bd_sb;

	if (WARN_ON(count == 0))
		return;
	BUG_ON(last >= (sb->s_blocksize << 3));
	assert_spin_locked(ext4_group_lock_ptr(sb, e4b->bd_group));

	if (unlikely(EXT4_MB_GRP_BBITMAP_CORRUPT(e4b->bd_info)))
		return;

	mb_check_buddy(e4b);
	mb_free_blocks_double(inode, e4b, first, count);


	if (first != 0)
		left_is_free = !mb_test_bit(first - 1, e4b->bd_bitmap);
	block = mb_test_and_clear_bits(e4b->bd_bitmap, first, count);
	if (last + 1 < EXT4_SB(sb)->s_mb_maxs[0])
		right_is_free = !mb_test_bit(last + 1, e4b->bd_bitmap);

	if (unlikely(block != -1)) {
		struct ext4_sb_info *sbi = EXT4_SB(sb);
		ext4_fsblk_t blocknr;


		if (sbi->s_mount_state & EXT4_FC_REPLAY) {
			mb_regenerate_buddy(e4b);
			goto check;
		}

		blocknr = ext4_group_first_block_no(sb, e4b->bd_group);
		blocknr += EXT4_C2B(sbi, block);
		ext4_mark_group_bitmap_corrupted(sb, e4b->bd_group,
				EXT4_GROUP_INFO_BBITMAP_CORRUPT);
		ext4_grp_locked_error(sb, e4b->bd_group,
				      inode ? inode->i_ino : 0, blocknr,
				      "freeing already freed block (bit %u); block bitmap corrupt.",
				      block);
		return;
	}

	this_cpu_inc(discard_pa_seq);
	e4b->bd_info->bb_free += count;
	if (first < e4b->bd_info->bb_first_free)
		e4b->bd_info->bb_first_free = first;


	if (left_is_free && right_is_free)
		e4b->bd_info->bb_fragments--;
	else if (!left_is_free && !right_is_free)
		e4b->bd_info->bb_fragments++;


	if (first & 1) {
		first += !left_is_free;
		e4b->bd_info->bb_counters[0] += left_is_free ? -1 : 1;
	}
	if (!(last & 1)) {
		last -= !right_is_free;
		e4b->bd_info->bb_counters[0] += right_is_free ? -1 : 1;
	}

	if (first <= last)
		mb_buddy_mark_free(e4b, first >> 1, last >> 1);

	mb_set_largest_free_order(sb, e4b->bd_info);
	mb_update_avg_fragment_size(sb, e4b->bd_info);
check:
	mb_check_buddy(e4b);
}

/**
 * mb_find_extent - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int mb_find_extent(struct ext4_buddy *e4b, int block,
				int needed, struct ext4_free_extent *ex)
{
	int max, order, next;
	void *buddy;

	assert_spin_locked(ext4_group_lock_ptr(e4b->bd_sb, e4b->bd_group));
	BUG_ON(ex == NULL);

	buddy = mb_find_buddy(e4b, 0, &max);
	BUG_ON(buddy == NULL);
	BUG_ON(block >= max);
	if (mb_test_bit(block, buddy)) {
		ex->fe_len = 0;
		ex->fe_start = 0;
		ex->fe_group = 0;
		return 0;
	}


	order = mb_find_order_for_block(e4b, block);

	ex->fe_len = (1 << order) - (block & ((1 << order) - 1));
	ex->fe_start = block;
	ex->fe_group = e4b->bd_group;

	block = block >> order;

	while (needed > ex->fe_len &&
	       mb_find_buddy(e4b, order, &max)) {

		if (block + 1 >= max)
			break;

		next = (block + 1) * (1 << order);
		if (mb_test_bit(next, e4b->bd_bitmap))
			break;

		order = mb_find_order_for_block(e4b, next);

		block = next >> order;
		ex->fe_len += 1 << order;
	}

	if (ex->fe_start + ex->fe_len > EXT4_CLUSTERS_PER_GROUP(e4b->bd_sb)) {

		WARN_ON(1);
		ext4_grp_locked_error(e4b->bd_sb, e4b->bd_group, 0, 0,
			"corruption or bug in mb_find_extent "
			"block=%d, order=%d needed=%d ex=%u/%d/%d@%u",
			block, order, needed, ex->fe_group, ex->fe_start,
			ex->fe_len, ex->fe_logical);
		ex->fe_len = 0;
		ex->fe_start = 0;
		ex->fe_group = 0;
	}
	return ex->fe_len;
}

/**
 * mb_mark_used - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int mb_mark_used(struct ext4_buddy *e4b, struct ext4_free_extent *ex)
{
	int ord;
	int mlen = 0;
	int max = 0;
	int start = ex->fe_start;
	int len = ex->fe_len;
	unsigned ret = 0;
	int len0 = len;
	void *buddy;
	int ord_start, ord_end;

	BUG_ON(start + len > (e4b->bd_sb->s_blocksize << 3));
	BUG_ON(e4b->bd_group != ex->fe_group);
	assert_spin_locked(ext4_group_lock_ptr(e4b->bd_sb, e4b->bd_group));
	mb_check_buddy(e4b);
	mb_mark_used_double(e4b, start, len);

	this_cpu_inc(discard_pa_seq);
	e4b->bd_info->bb_free -= len;
	if (e4b->bd_info->bb_first_free == start)
		e4b->bd_info->bb_first_free += len;


	if (start != 0)
		mlen = !mb_test_bit(start - 1, e4b->bd_bitmap);
	if (start + len < EXT4_SB(e4b->bd_sb)->s_mb_maxs[0])
		max = !mb_test_bit(start + len, e4b->bd_bitmap);
	if (mlen && max)
		e4b->bd_info->bb_fragments++;
	else if (!mlen && !max)
		e4b->bd_info->bb_fragments--;


	while (len) {
		ord = mb_find_order_for_block(e4b, start);

		if (((start >> ord) << ord) == start && len >= (1 << ord)) {

			mlen = 1 << ord;
			buddy = mb_find_buddy(e4b, ord, &max);
			BUG_ON((start >> ord) >= max);
			mb_set_bit(start >> ord, buddy);
			e4b->bd_info->bb_counters[ord]--;
			start += mlen;
			len -= mlen;
			BUG_ON(len < 0);
			continue;
		}


		if (ret == 0)
			ret = len | (ord << 16);

		BUG_ON(ord <= 0);
		buddy = mb_find_buddy(e4b, ord, &max);
		mb_set_bit(start >> ord, buddy);
		e4b->bd_info->bb_counters[ord]--;

		ord_start = (start >> ord) << ord;
		ord_end = ord_start + (1 << ord);

		if (start > ord_start)
			ext4_mb_mark_free_simple(e4b->bd_sb, e4b->bd_buddy,
						 ord_start, start - ord_start,
						 e4b->bd_info);


		if (start + len < ord_end) {
			ext4_mb_mark_free_simple(e4b->bd_sb, e4b->bd_buddy,
						 start + len,
						 ord_end - (start + len),
						 e4b->bd_info);
			break;
		}
		len = start + len - ord_end;
		start = ord_end;
	}
	mb_set_largest_free_order(e4b->bd_sb, e4b->bd_info);

	mb_update_avg_fragment_size(e4b->bd_sb, e4b->bd_info);
	mb_set_bits(e4b->bd_bitmap, ex->fe_start, len0);
	mb_check_buddy(e4b);

	return ret;
}


/**
 * ext4_mb_use_best_found - Implements the mb use best found operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_use_best_found(struct ext4_allocation_context *ac,
					struct ext4_buddy *e4b)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	int ret;

	BUG_ON(ac->ac_b_ex.fe_group != e4b->bd_group);
	BUG_ON(ac->ac_status == AC_STATUS_FOUND);

	ac->ac_b_ex.fe_len = min(ac->ac_b_ex.fe_len, ac->ac_g_ex.fe_len);
	ac->ac_b_ex.fe_logical = ac->ac_g_ex.fe_logical;
	ret = mb_mark_used(e4b, &ac->ac_b_ex);


	ac->ac_f_ex = ac->ac_b_ex;

	ac->ac_status = AC_STATUS_FOUND;
	ac->ac_tail = ret & 0xffff;
	ac->ac_buddy = ret >> 16;


	ac->ac_bitmap_folio = e4b->bd_bitmap_folio;
	folio_get(ac->ac_bitmap_folio);
	ac->ac_buddy_folio = e4b->bd_buddy_folio;
	folio_get(ac->ac_buddy_folio);

	if (ac->ac_flags & EXT4_MB_STREAM_ALLOC) {
		spin_lock(&sbi->s_md_lock);
		sbi->s_mb_last_group = ac->ac_f_ex.fe_group;
		sbi->s_mb_last_start = ac->ac_f_ex.fe_start;
		spin_unlock(&sbi->s_md_lock);
	}


	if (ac->ac_o_ex.fe_len < ac->ac_b_ex.fe_len)
		ext4_mb_new_preallocation(ac);

}

/**
 * ext4_mb_check_limits - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_check_limits(struct ext4_allocation_context *ac,
					struct ext4_buddy *e4b,
					int finish_group)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_free_extent *bex = &ac->ac_b_ex;
	struct ext4_free_extent *gex = &ac->ac_g_ex;

	if (ac->ac_status == AC_STATUS_FOUND)
		return;


	if (ac->ac_found > sbi->s_mb_max_to_scan &&
			!(ac->ac_flags & EXT4_MB_HINT_FIRST)) {
		ac->ac_status = AC_STATUS_BREAK;
		return;
	}


	if (bex->fe_len < gex->fe_len)
		return;

	if (finish_group || ac->ac_found > sbi->s_mb_min_to_scan)
		ext4_mb_use_best_found(ac, e4b);
}


/**
 * ext4_mb_measure_extent - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_measure_extent(struct ext4_allocation_context *ac,
					struct ext4_free_extent *ex,
					struct ext4_buddy *e4b)
{
	struct ext4_free_extent *bex = &ac->ac_b_ex;
	struct ext4_free_extent *gex = &ac->ac_g_ex;

	BUG_ON(ex->fe_len <= 0);
	BUG_ON(ex->fe_len > EXT4_CLUSTERS_PER_GROUP(ac->ac_sb));
	BUG_ON(ex->fe_start >= EXT4_CLUSTERS_PER_GROUP(ac->ac_sb));
	BUG_ON(ac->ac_status != AC_STATUS_CONTINUE);

	ac->ac_found++;
	ac->ac_cX_found[ac->ac_criteria]++;


	if (unlikely(ac->ac_flags & EXT4_MB_HINT_FIRST)) {
		*bex = *ex;
		ext4_mb_use_best_found(ac, e4b);
		return;
	}


	if (ex->fe_len == gex->fe_len) {
		*bex = *ex;
		ext4_mb_use_best_found(ac, e4b);
		return;
	}


	if (bex->fe_len == 0) {
		*bex = *ex;
		return;
	}


	if (bex->fe_len < gex->fe_len) {


		if (ex->fe_len > bex->fe_len)
			*bex = *ex;
	} else if (ex->fe_len > gex->fe_len) {


		if (ex->fe_len < bex->fe_len)
			*bex = *ex;
	}

	ext4_mb_check_limits(ac, e4b, 0);
}

/**
 * ext4_mb_try_best_found - Implements the mb try best found operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack
void ext4_mb_try_best_found(struct ext4_allocation_context *ac,
					struct ext4_buddy *e4b)
{
	struct ext4_free_extent ex = ac->ac_b_ex;
	ext4_group_t group = ex.fe_group;
	int max;
	int err;

	BUG_ON(ex.fe_len <= 0);
	err = ext4_mb_load_buddy(ac->ac_sb, group, e4b);
	if (err)
		return;

	ext4_lock_group(ac->ac_sb, group);
	if (unlikely(EXT4_MB_GRP_BBITMAP_CORRUPT(e4b->bd_info)))
		goto out;

	max = mb_find_extent(e4b, ex.fe_start, ex.fe_len, &ex);

	if (max > 0) {
		ac->ac_b_ex = ex;
		ext4_mb_use_best_found(ac, e4b);
	}

out:
	ext4_unlock_group(ac->ac_sb, group);
	ext4_mb_unload_buddy(e4b);
}

/**
 * ext4_mb_find_by_goal - Locates filesystem state without changing the authoritative persistent representation unless the surrounding API explicitly permits it.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack
int ext4_mb_find_by_goal(struct ext4_allocation_context *ac,
				struct ext4_buddy *e4b)
{
	ext4_group_t group = ac->ac_g_ex.fe_group;
	int max;
	int err;
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_group_info *grp = ext4_get_group_info(ac->ac_sb, group);
	struct ext4_free_extent ex;

	if (!grp)
		return -EFSCORRUPTED;
	if (!(ac->ac_flags & (EXT4_MB_HINT_TRY_GOAL | EXT4_MB_HINT_GOAL_ONLY)))
		return 0;
	if (grp->bb_free == 0)
		return 0;

	err = ext4_mb_load_buddy(ac->ac_sb, group, e4b);
	if (err) {
		if (EXT4_MB_GRP_BBITMAP_CORRUPT(e4b->bd_info) &&
		    !(ac->ac_flags & EXT4_MB_HINT_GOAL_ONLY))
			return 0;
		return err;
	}

	ext4_lock_group(ac->ac_sb, group);
	if (unlikely(EXT4_MB_GRP_BBITMAP_CORRUPT(e4b->bd_info)))
		goto out;

	max = mb_find_extent(e4b, ac->ac_g_ex.fe_start,
			     ac->ac_g_ex.fe_len, &ex);
	ex.fe_logical = 0xDEADFA11;

	if (max >= ac->ac_g_ex.fe_len &&
	    ac->ac_g_ex.fe_len == EXT4_NUM_B2C(sbi, sbi->s_stripe)) {
		ext4_fsblk_t start;

		start = ext4_grp_offs_to_block(ac->ac_sb, &ex);

		if (do_div(start, sbi->s_stripe) == 0) {
			ac->ac_found++;
			ac->ac_b_ex = ex;
			ext4_mb_use_best_found(ac, e4b);
		}
	} else if (max >= ac->ac_g_ex.fe_len) {
		BUG_ON(ex.fe_len <= 0);
		BUG_ON(ex.fe_group != ac->ac_g_ex.fe_group);
		BUG_ON(ex.fe_start != ac->ac_g_ex.fe_start);
		ac->ac_found++;
		ac->ac_b_ex = ex;
		ext4_mb_use_best_found(ac, e4b);
	} else if (max > 0 && (ac->ac_flags & EXT4_MB_HINT_MERGE)) {


		BUG_ON(ex.fe_len <= 0);
		BUG_ON(ex.fe_group != ac->ac_g_ex.fe_group);
		BUG_ON(ex.fe_start != ac->ac_g_ex.fe_start);
		ac->ac_found++;
		ac->ac_b_ex = ex;
		ext4_mb_use_best_found(ac, e4b);
	}
out:
	ext4_unlock_group(ac->ac_sb, group);
	ext4_mb_unload_buddy(e4b);

	return 0;
}


/**
 * ext4_mb_simple_scan_group - Implements the mb simple scan group operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack
void ext4_mb_simple_scan_group(struct ext4_allocation_context *ac,
					struct ext4_buddy *e4b)
{
	struct super_block *sb = ac->ac_sb;
	struct ext4_group_info *grp = e4b->bd_info;
	void *buddy;
	int i;
	int k;
	int max;

	BUG_ON(ac->ac_2order <= 0);
	for (i = ac->ac_2order; i < MB_NUM_ORDERS(sb); i++) {
		if (grp->bb_counters[i] == 0)
			continue;

		buddy = mb_find_buddy(e4b, i, &max);
		if (WARN_RATELIMIT(buddy == NULL,
			 "ext4: mb_simple_scan_group: mb_find_buddy failed, (%d)\n", i))
			continue;

		k = mb_find_next_zero_bit(buddy, max, 0);
		if (k >= max) {
			ext4_mark_group_bitmap_corrupted(ac->ac_sb,
					e4b->bd_group,
					EXT4_GROUP_INFO_BBITMAP_CORRUPT);
			ext4_grp_locked_error(ac->ac_sb, e4b->bd_group, 0, 0,
				"%d free clusters of order %d. But found 0",
				grp->bb_counters[i], i);
			break;
		}
		ac->ac_found++;
		ac->ac_cX_found[ac->ac_criteria]++;

		ac->ac_b_ex.fe_len = 1 << i;
		ac->ac_b_ex.fe_start = k << i;
		ac->ac_b_ex.fe_group = e4b->bd_group;

		ext4_mb_use_best_found(ac, e4b);

		BUG_ON(ac->ac_f_ex.fe_len != ac->ac_g_ex.fe_len);

		if (EXT4_SB(sb)->s_mb_stats)
			atomic_inc(&EXT4_SB(sb)->s_bal_2orders);

		break;
	}
}


/**
 * ext4_mb_complex_scan_group - Implements the mb complex scan group operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack
void ext4_mb_complex_scan_group(struct ext4_allocation_context *ac,
					struct ext4_buddy *e4b)
{
	struct super_block *sb = ac->ac_sb;
	void *bitmap = e4b->bd_bitmap;
	struct ext4_free_extent ex;
	int i, j, freelen;
	int free;

	free = e4b->bd_info->bb_free;
	if (WARN_ON(free <= 0))
		return;

	i = e4b->bd_info->bb_first_free;

	while (free && ac->ac_status == AC_STATUS_CONTINUE) {
		i = mb_find_next_zero_bit(bitmap,
						EXT4_CLUSTERS_PER_GROUP(sb), i);
		if (i >= EXT4_CLUSTERS_PER_GROUP(sb)) {


			ext4_mark_group_bitmap_corrupted(sb, e4b->bd_group,
					EXT4_GROUP_INFO_BBITMAP_CORRUPT);
			ext4_grp_locked_error(sb, e4b->bd_group, 0, 0,
					"%d free clusters as per "
					"group info. But bitmap says 0",
					free);
			break;
		}

		if (!ext4_mb_cr_expensive(ac->ac_criteria)) {


			j = mb_find_next_bit(bitmap,
						EXT4_CLUSTERS_PER_GROUP(sb), i);
			freelen = j - i;

			if (freelen < ac->ac_g_ex.fe_len) {
				i = j;
				free -= freelen;
				continue;
			}
		}

		mb_find_extent(e4b, i, ac->ac_g_ex.fe_len, &ex);
		if (WARN_ON(ex.fe_len <= 0))
			break;
		if (free < ex.fe_len) {
			ext4_mark_group_bitmap_corrupted(sb, e4b->bd_group,
					EXT4_GROUP_INFO_BBITMAP_CORRUPT);
			ext4_grp_locked_error(sb, e4b->bd_group, 0, 0,
					"%d free clusters as per "
					"group info. But got %d blocks",
					free, ex.fe_len);


			break;
		}
		ex.fe_logical = 0xDEADC0DE;
		ext4_mb_measure_extent(ac, &ex, e4b);

		i += ex.fe_len;
		free -= ex.fe_len;
	}

	ext4_mb_check_limits(ac, e4b, 1);
}


/**
 * ext4_mb_scan_aligned - Implements the mb scan aligned operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack
void ext4_mb_scan_aligned(struct ext4_allocation_context *ac,
				 struct ext4_buddy *e4b)
{
	struct super_block *sb = ac->ac_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	void *bitmap = e4b->bd_bitmap;
	struct ext4_free_extent ex;
	ext4_fsblk_t first_group_block;
	ext4_fsblk_t a;
	ext4_grpblk_t i, stripe;
	int max;

	BUG_ON(sbi->s_stripe == 0);


	first_group_block = ext4_group_first_block_no(sb, e4b->bd_group);

	a = first_group_block + sbi->s_stripe - 1;
	do_div(a, sbi->s_stripe);
	i = (a * sbi->s_stripe) - first_group_block;

	stripe = EXT4_NUM_B2C(sbi, sbi->s_stripe);
	i = EXT4_B2C(sbi, i);
	while (i < EXT4_CLUSTERS_PER_GROUP(sb)) {
		if (!mb_test_bit(i, bitmap)) {
			max = mb_find_extent(e4b, i, stripe, &ex);
			if (max >= stripe) {
				ac->ac_found++;
				ac->ac_cX_found[ac->ac_criteria]++;
				ex.fe_logical = 0xDEADF00D;
				ac->ac_b_ex = ex;
				ext4_mb_use_best_found(ac, e4b);
				break;
			}
		}
		i += stripe;
	}
}

/**
 * __ext4_mb_scan_group - Implements the mb scan group operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void __ext4_mb_scan_group(struct ext4_allocation_context *ac)
{
	bool is_stripe_aligned;
	struct ext4_sb_info *sbi;
	enum criteria cr = ac->ac_criteria;

	ac->ac_groups_scanned++;
	if (cr == CR_POWER2_ALIGNED)
		return ext4_mb_simple_scan_group(ac, ac->ac_e4b);

	sbi = EXT4_SB(ac->ac_sb);
	is_stripe_aligned = false;
	if ((sbi->s_stripe >= sbi->s_cluster_ratio) &&
	    !(ac->ac_g_ex.fe_len % EXT4_NUM_B2C(sbi, sbi->s_stripe)))
		is_stripe_aligned = true;

	if ((cr == CR_GOAL_LEN_FAST || cr == CR_BEST_AVAIL_LEN) &&
	    is_stripe_aligned)
		ext4_mb_scan_aligned(ac, ac->ac_e4b);

	if (ac->ac_status == AC_STATUS_CONTINUE)
		ext4_mb_complex_scan_group(ac, ac->ac_e4b);
}


/**
 * ext4_mb_good_group - Implements the mb good group operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static bool ext4_mb_good_group(struct ext4_allocation_context *ac,
				ext4_group_t group, enum criteria cr)
{
	ext4_grpblk_t free, fragments;
	int flex_size = ext4_flex_bg_size(EXT4_SB(ac->ac_sb));
	struct ext4_group_info *grp = ext4_get_group_info(ac->ac_sb, group);

	BUG_ON(cr < CR_POWER2_ALIGNED || cr >= EXT4_MB_NUM_CRS);

	if (unlikely(!grp || EXT4_MB_GRP_BBITMAP_CORRUPT(grp)))
		return false;

	free = grp->bb_free;
	if (free == 0)
		return false;

	fragments = grp->bb_fragments;
	if (fragments == 0)
		return false;

	switch (cr) {
	case CR_POWER2_ALIGNED:
		BUG_ON(ac->ac_2order == 0);


		if ((ac->ac_flags & EXT4_MB_HINT_DATA) &&
		    (flex_size >= EXT4_FLEX_SIZE_DIR_ALLOC_SCHEME) &&
		    ((group % flex_size) == 0))
			return false;

		if (free < ac->ac_g_ex.fe_len)
			return false;

		if (ac->ac_2order >= MB_NUM_ORDERS(ac->ac_sb))
			return true;

		if (grp->bb_largest_free_order < ac->ac_2order)
			return false;

		return true;
	case CR_GOAL_LEN_FAST:
	case CR_BEST_AVAIL_LEN:
		if ((free / fragments) >= ac->ac_g_ex.fe_len)
			return true;
		break;
	case CR_GOAL_LEN_SLOW:
		if (free >= ac->ac_g_ex.fe_len)
			return true;
		break;
	case CR_ANY_FREE:
		return true;
	default:
		BUG();
	}

	return false;
}


/**
 * ext4_mb_good_group_nolock - Implements the mb good group nolock operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_good_group_nolock(struct ext4_allocation_context *ac,
				     ext4_group_t group, enum criteria cr)
{
	struct ext4_group_info *grp = ext4_get_group_info(ac->ac_sb, group);
	struct super_block *sb = ac->ac_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	bool should_lock = ac->ac_flags & EXT4_MB_STRICT_CHECK;
	ext4_grpblk_t free;
	int ret = 0;

	if (!grp)
		return -EFSCORRUPTED;
	if (sbi->s_mb_stats)
		atomic64_inc(&sbi->s_bal_cX_groups_considered[ac->ac_criteria]);
	if (should_lock) {
		ext4_lock_group(sb, group);
		__release(ext4_group_lock_ptr(sb, group));
	}
	free = grp->bb_free;
	if (free == 0)
		goto out;


	if (cr < CR_ANY_FREE && free < ac->ac_g_ex.fe_len)
		goto out;
	if (unlikely(EXT4_MB_GRP_BBITMAP_CORRUPT(grp)))
		goto out;
	if (should_lock) {
		__acquire(ext4_group_lock_ptr(sb, group));
		ext4_unlock_group(sb, group);
	}


	if (unlikely(EXT4_MB_GRP_NEED_INIT(grp))) {
		struct ext4_group_desc *gdp =
			ext4_get_group_desc(sb, group, NULL);
		int ret;


		if (!ext4_mb_cr_expensive(cr) &&
		    (!sbi->s_log_groups_per_flex ||
		     ((group & ((1 << sbi->s_log_groups_per_flex) - 1)) != 0)) &&
		    !(ext4_has_group_desc_csum(sb) &&
		      (gdp->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT))))
			return 0;
		ret = ext4_mb_init_group(sb, group, GFP_NOFS);
		if (ret)
			return ret;
	}

	if (should_lock) {
		ext4_lock_group(sb, group);
		__release(ext4_group_lock_ptr(sb, group));
	}
	ret = ext4_mb_good_group(ac, group, cr);
out:
	if (should_lock) {
		__acquire(ext4_group_lock_ptr(sb, group));
		ext4_unlock_group(sb, group);
	}
	return ret;
}


/**
 * ext4_mb_prefetch - Implements the mb prefetch operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
ext4_group_t ext4_mb_prefetch(struct super_block *sb, ext4_group_t group,
			      unsigned int nr, int *cnt)
{
	ext4_group_t ngroups = ext4_get_groups_count(sb);
	struct buffer_head *bh;
	struct blk_plug plug;

	blk_start_plug(&plug);
	while (nr-- > 0) {
		struct ext4_group_desc *gdp = ext4_get_group_desc(sb, group,
								  NULL);
		struct ext4_group_info *grp = ext4_get_group_info(sb, group);


		if (gdp && grp && !EXT4_MB_GRP_TEST_AND_SET_READ(grp) &&
		    EXT4_MB_GRP_NEED_INIT(grp) &&
		    ext4_free_group_clusters(sb, gdp) > 0 ) {
			bh = ext4_read_block_bitmap_nowait(sb, group, true);
			if (bh && !IS_ERR(bh)) {
				if (!buffer_uptodate(bh) && cnt)
					(*cnt)++;
				brelse(bh);
			}
		}
		if (++group >= ngroups)
			group = 0;
	}
	blk_finish_plug(&plug);
	return group;
}


/**
 * ext4_mb_might_prefetch - Implements the mb might prefetch operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_might_prefetch(struct ext4_allocation_context *ac,
				   ext4_group_t group)
{
	struct ext4_sb_info *sbi;

	if (ac->ac_prefetch_grp != group)
		return;

	sbi = EXT4_SB(ac->ac_sb);
	if (ext4_mb_cr_expensive(ac->ac_criteria) ||
	    ac->ac_prefetch_ios < sbi->s_mb_prefetch_limit) {
		unsigned int nr = sbi->s_mb_prefetch;

		if (ext4_has_feature_flex_bg(ac->ac_sb)) {
			nr = 1 << sbi->s_log_groups_per_flex;
			nr -= group & (nr - 1);
			nr = umin(nr, sbi->s_mb_prefetch);
		}

		ac->ac_prefetch_nr = nr;
		ac->ac_prefetch_grp = ext4_mb_prefetch(ac->ac_sb, group, nr,
						       &ac->ac_prefetch_ios);
	}
}


/**
 * ext4_mb_prefetch_fini - Implements the mb prefetch fini operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void ext4_mb_prefetch_fini(struct super_block *sb, ext4_group_t group,
			   unsigned int nr)
{
	struct ext4_group_desc *gdp;
	struct ext4_group_info *grp;

	while (nr-- > 0) {
		if (!group)
			group = ext4_get_groups_count(sb);
		group--;
		gdp = ext4_get_group_desc(sb, group, NULL);
		grp = ext4_get_group_info(sb, group);

		if (grp && gdp && EXT4_MB_GRP_NEED_INIT(grp) &&
		    ext4_free_group_clusters(sb, gdp) > 0) {
			if (ext4_mb_init_group(sb, group, GFP_NOFS))
				break;
		}
	}
}

/**
 * ext4_mb_scan_group - Implements the mb scan group operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_scan_group(struct ext4_allocation_context *ac,
			      ext4_group_t group)
{
	int ret;
	struct super_block *sb = ac->ac_sb;
	enum criteria cr = ac->ac_criteria;

	ext4_mb_might_prefetch(ac, group);


	if (cr < CR_ANY_FREE && spin_is_locked(ext4_group_lock_ptr(sb, group)))
		return 0;


	ret = ext4_mb_good_group_nolock(ac, group, cr);
	if (ret <= 0) {
		if (!ac->ac_first_err)
			ac->ac_first_err = ret;
		return 0;
	}

	ret = ext4_mb_load_buddy(sb, group, ac->ac_e4b);
	if (ret)
		return ret;


	if (cr >= CR_ANY_FREE)
		ext4_lock_group(sb, group);
	else if (!ext4_try_lock_group(sb, group))
		goto out_unload;


	if (unlikely(!ext4_mb_good_group(ac, group, cr)))
		goto out_unlock;

	__ext4_mb_scan_group(ac);

out_unlock:
	ext4_unlock_group(sb, group);
out_unload:
	ext4_mb_unload_buddy(ac->ac_e4b);
	return ret;
}

/**
 * ext4_mb_regular_allocator - Implements the mb regular allocator operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack int
ext4_mb_regular_allocator(struct ext4_allocation_context *ac)
{
	ext4_group_t i;
	int err = 0;
	struct super_block *sb = ac->ac_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_buddy e4b;

	BUG_ON(ac->ac_status == AC_STATUS_FOUND);


	err = ext4_mb_find_by_goal(ac, &e4b);
	if (err || ac->ac_status == AC_STATUS_FOUND)
		goto out;

	if (unlikely(ac->ac_flags & EXT4_MB_HINT_GOAL_ONLY))
		goto out;


	i = fls(ac->ac_g_ex.fe_len);
	ac->ac_2order = 0;


	if (i >= sbi->s_mb_order2_reqs && i <= MB_NUM_ORDERS(sb)) {
		if (is_power_of_2(ac->ac_g_ex.fe_len))
			ac->ac_2order = array_index_nospec(i - 1,
							   MB_NUM_ORDERS(sb));
	}


	if (ac->ac_flags & EXT4_MB_STREAM_ALLOC) {

		spin_lock(&sbi->s_md_lock);
		ac->ac_g_ex.fe_group = sbi->s_mb_last_group;
		ac->ac_g_ex.fe_start = sbi->s_mb_last_start;
		spin_unlock(&sbi->s_md_lock);
	}


	ac->ac_criteria = CR_GOAL_LEN_FAST;
	if (ac->ac_2order)
		ac->ac_criteria = CR_POWER2_ALIGNED;

	ac->ac_e4b = &e4b;
	ac->ac_prefetch_ios = 0;
	ac->ac_first_err = 0;
repeat:
	while (ac->ac_criteria < EXT4_MB_NUM_CRS) {
		err = ext4_mb_scan_groups(ac);
		if (err)
			goto out;

		if (ac->ac_status != AC_STATUS_CONTINUE)
			break;
	}

	if (ac->ac_b_ex.fe_len > 0 && ac->ac_status != AC_STATUS_FOUND &&
	    !(ac->ac_flags & EXT4_MB_HINT_FIRST)) {


		ext4_mb_try_best_found(ac, &e4b);
		if (ac->ac_status != AC_STATUS_FOUND) {
			int lost;


			lost = atomic_inc_return(&sbi->s_mb_lost_chunks);
			mb_debug(sb, "lost chunk, group: %u, start: %d, len: %d, lost: %d\n",
				 ac->ac_b_ex.fe_group, ac->ac_b_ex.fe_start,
				 ac->ac_b_ex.fe_len, lost);

			ac->ac_b_ex.fe_group = 0;
			ac->ac_b_ex.fe_start = 0;
			ac->ac_b_ex.fe_len = 0;
			ac->ac_status = AC_STATUS_CONTINUE;
			ac->ac_flags |= EXT4_MB_HINT_FIRST;
			ac->ac_criteria = CR_ANY_FREE;
			goto repeat;
		}
	}

	if (sbi->s_mb_stats && ac->ac_status == AC_STATUS_FOUND)
		atomic64_inc(&sbi->s_bal_cX_hits[ac->ac_criteria]);
out:
	if (!err && ac->ac_status != AC_STATUS_FOUND && ac->ac_first_err)
		err = ac->ac_first_err;

	mb_debug(sb, "Best len %d, origin len %d, ac_status %u, ac_flags 0x%x, cr %d ret %d\n",
		 ac->ac_b_ex.fe_len, ac->ac_o_ex.fe_len, ac->ac_status,
		 ac->ac_flags, ac->ac_criteria, err);

	if (ac->ac_prefetch_nr)
		ext4_mb_prefetch_fini(sb, ac->ac_prefetch_grp, ac->ac_prefetch_nr);

	return err;
}

/**
 * ext4_mb_seq_groups_start - Implements the mb seq groups start operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void *ext4_mb_seq_groups_start(struct seq_file *seq, loff_t *pos)
{
	struct super_block *sb = pde_data(file_inode(seq->file));
	ext4_group_t group;

	if (*pos < 0 || *pos >= ext4_get_groups_count(sb))
		return NULL;
	group = *pos + 1;
	return (void *) ((unsigned long) group);
}

/**
 * ext4_mb_seq_groups_next - Implements the mb seq groups next operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void *ext4_mb_seq_groups_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct super_block *sb = pde_data(file_inode(seq->file));
	ext4_group_t group;

	++*pos;
	if (*pos < 0 || *pos >= ext4_get_groups_count(sb))
		return NULL;
	group = *pos + 1;
	return (void *) ((unsigned long) group);
}

/**
 * ext4_mb_seq_groups_show - Implements the mb seq groups show operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_seq_groups_show(struct seq_file *seq, void *v)
{
	struct super_block *sb = pde_data(file_inode(seq->file));
	ext4_group_t group = (ext4_group_t) ((unsigned long) v);
	int i, err;
	char nbuf[16];
	struct ext4_buddy e4b;
	struct ext4_group_info *grinfo;
	unsigned char blocksize_bits = min_t(unsigned char,
					     sb->s_blocksize_bits,
					     EXT4_MAX_BLOCK_LOG_SIZE);
	struct sg {
		struct ext4_group_info info;
		ext4_grpblk_t counters[EXT4_MAX_BLOCK_LOG_SIZE + 2];
	} sg;

	group--;
	if (group == 0)
		seq_puts(seq, "#group: free  frags first ["
			      " 2^0   2^1   2^2   2^3   2^4   2^5   2^6  "
			      " 2^7   2^8   2^9   2^10  2^11  2^12  2^13  ]\n");

	i = (blocksize_bits + 2) * sizeof(sg.info.bb_counters[0]) +
		sizeof(struct ext4_group_info);

	grinfo = ext4_get_group_info(sb, group);
	if (!grinfo)
		return 0;

	if (unlikely(EXT4_MB_GRP_NEED_INIT(grinfo))) {
		err = ext4_mb_load_buddy(sb, group, &e4b);
		if (err) {
			seq_printf(seq, "#%-5u: %s\n", group, ext4_decode_error(NULL, err, nbuf));
			return 0;
		}
		ext4_mb_unload_buddy(&e4b);
	}


	memcpy(&sg, grinfo, i);
	seq_printf(seq, "#%-5u: %-5u %-5u %-5u [", group, sg.info.bb_free,
			sg.info.bb_fragments, sg.info.bb_first_free);
	for (i = 0; i <= 13; i++)
		seq_printf(seq, " %-5u", i <= blocksize_bits + 1 ?
				sg.info.bb_counters[i] : 0);
	seq_puts(seq, " ]");
	if (EXT4_MB_GRP_BBITMAP_CORRUPT(&sg.info))
		seq_puts(seq, " Block bitmap corrupted!");
	seq_putc(seq, '\n');
	return 0;
}

/**
 * ext4_mb_seq_groups_stop - Implements the mb seq groups stop operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_seq_groups_stop(struct seq_file *seq, void *v)
{
}

const struct seq_operations ext4_mb_seq_groups_ops = {
	.start  = ext4_mb_seq_groups_start,
	.next   = ext4_mb_seq_groups_next,
	.stop   = ext4_mb_seq_groups_stop,
	.show   = ext4_mb_seq_groups_show,
};

/**
 * ext4_seq_mb_stats_show - Implements the seq mb stats show operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int ext4_seq_mb_stats_show(struct seq_file *seq, void *offset)
{
	struct super_block *sb = seq->private;
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	seq_puts(seq, "mballoc:\n");
	if (!sbi->s_mb_stats) {
		seq_puts(seq, "\tmb stats collection turned off.\n");
		seq_puts(
			seq,
			"\tTo enable, please write \"1\" to sysfs file mb_stats.\n");
		return 0;
	}
	seq_printf(seq, "\treqs: %u\n", atomic_read(&sbi->s_bal_reqs));
	seq_printf(seq, "\tsuccess: %u\n", atomic_read(&sbi->s_bal_success));

	seq_printf(seq, "\tgroups_scanned: %u\n",
		   atomic_read(&sbi->s_bal_groups_scanned));


	seq_puts(seq, "\tcr_p2_aligned_stats:\n");
	seq_printf(seq, "\t\thits: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_hits[CR_POWER2_ALIGNED]));
	seq_printf(
		seq, "\t\tgroups_considered: %llu\n",
		atomic64_read(
			&sbi->s_bal_cX_groups_considered[CR_POWER2_ALIGNED]));
	seq_printf(seq, "\t\textents_scanned: %u\n",
		   atomic_read(&sbi->s_bal_cX_ex_scanned[CR_POWER2_ALIGNED]));
	seq_printf(seq, "\t\tuseless_loops: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_failed[CR_POWER2_ALIGNED]));


	seq_puts(seq, "\tcr_goal_fast_stats:\n");
	seq_printf(seq, "\t\thits: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_hits[CR_GOAL_LEN_FAST]));
	seq_printf(seq, "\t\tgroups_considered: %llu\n",
		   atomic64_read(
			   &sbi->s_bal_cX_groups_considered[CR_GOAL_LEN_FAST]));
	seq_printf(seq, "\t\textents_scanned: %u\n",
		   atomic_read(&sbi->s_bal_cX_ex_scanned[CR_GOAL_LEN_FAST]));
	seq_printf(seq, "\t\tuseless_loops: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_failed[CR_GOAL_LEN_FAST]));


	seq_puts(seq, "\tcr_best_avail_stats:\n");
	seq_printf(seq, "\t\thits: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_hits[CR_BEST_AVAIL_LEN]));
	seq_printf(
		seq, "\t\tgroups_considered: %llu\n",
		atomic64_read(
			&sbi->s_bal_cX_groups_considered[CR_BEST_AVAIL_LEN]));
	seq_printf(seq, "\t\textents_scanned: %u\n",
		   atomic_read(&sbi->s_bal_cX_ex_scanned[CR_BEST_AVAIL_LEN]));
	seq_printf(seq, "\t\tuseless_loops: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_failed[CR_BEST_AVAIL_LEN]));


	seq_puts(seq, "\tcr_goal_slow_stats:\n");
	seq_printf(seq, "\t\thits: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_hits[CR_GOAL_LEN_SLOW]));
	seq_printf(seq, "\t\tgroups_considered: %llu\n",
		   atomic64_read(
			   &sbi->s_bal_cX_groups_considered[CR_GOAL_LEN_SLOW]));
	seq_printf(seq, "\t\textents_scanned: %u\n",
		   atomic_read(&sbi->s_bal_cX_ex_scanned[CR_GOAL_LEN_SLOW]));
	seq_printf(seq, "\t\tuseless_loops: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_failed[CR_GOAL_LEN_SLOW]));


	seq_puts(seq, "\tcr_any_free_stats:\n");
	seq_printf(seq, "\t\thits: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_hits[CR_ANY_FREE]));
	seq_printf(
		seq, "\t\tgroups_considered: %llu\n",
		atomic64_read(&sbi->s_bal_cX_groups_considered[CR_ANY_FREE]));
	seq_printf(seq, "\t\textents_scanned: %u\n",
		   atomic_read(&sbi->s_bal_cX_ex_scanned[CR_ANY_FREE]));
	seq_printf(seq, "\t\tuseless_loops: %llu\n",
		   atomic64_read(&sbi->s_bal_cX_failed[CR_ANY_FREE]));


	seq_printf(seq, "\textents_scanned: %u\n",
		   atomic_read(&sbi->s_bal_ex_scanned));
	seq_printf(seq, "\t\tgoal_hits: %u\n", atomic_read(&sbi->s_bal_goals));
	seq_printf(seq, "\t\tlen_goal_hits: %u\n",
		   atomic_read(&sbi->s_bal_len_goals));
	seq_printf(seq, "\t\t2^n_hits: %u\n", atomic_read(&sbi->s_bal_2orders));
	seq_printf(seq, "\t\tbreaks: %u\n", atomic_read(&sbi->s_bal_breaks));
	seq_printf(seq, "\t\tlost: %u\n", atomic_read(&sbi->s_mb_lost_chunks));
	seq_printf(seq, "\tbuddies_generated: %u/%u\n",
		   atomic_read(&sbi->s_mb_buddies_generated),
		   ext4_get_groups_count(sb));
	seq_printf(seq, "\tbuddies_time_used: %llu\n",
		   atomic64_read(&sbi->s_mb_generation_time));
	seq_printf(seq, "\tpreallocated: %u\n",
		   atomic_read(&sbi->s_mb_preallocated));
	seq_printf(seq, "\tdiscarded: %u\n", atomic_read(&sbi->s_mb_discarded));
	return 0;
}

/**
 * ext4_mb_seq_structs_summary_start - Implements the mb seq structs summary start operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void *ext4_mb_seq_structs_summary_start(struct seq_file *seq, loff_t *pos)
{
	struct super_block *sb = pde_data(file_inode(seq->file));
	unsigned long position;

	if (*pos < 0 || *pos >= 2*MB_NUM_ORDERS(sb))
		return NULL;
	position = *pos + 1;
	return (void *) ((unsigned long) position);
}

/**
 * ext4_mb_seq_structs_summary_next - Implements the mb seq structs summary next operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void *ext4_mb_seq_structs_summary_next(struct seq_file *seq, void *v, loff_t *pos)
{
	struct super_block *sb = pde_data(file_inode(seq->file));
	unsigned long position;

	++*pos;
	if (*pos < 0 || *pos >= 2*MB_NUM_ORDERS(sb))
		return NULL;
	position = *pos + 1;
	return (void *) ((unsigned long) position);
}

/**
 * ext4_mb_seq_structs_summary_show - Implements the mb seq structs summary show operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_seq_structs_summary_show(struct seq_file *seq, void *v)
{
	struct super_block *sb = pde_data(file_inode(seq->file));
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned long position = ((unsigned long) v);
	struct ext4_group_info *grp;
	unsigned int count;
	unsigned long idx;

	position--;
	if (position >= MB_NUM_ORDERS(sb)) {
		position -= MB_NUM_ORDERS(sb);
		if (position == 0)
			seq_puts(seq, "avg_fragment_size_lists:\n");

		count = 0;
		xa_for_each(&sbi->s_mb_avg_fragment_size[position], idx, grp)
			count++;
		seq_printf(seq, "\tlist_order_%u_groups: %u\n",
					(unsigned int)position, count);
		return 0;
	}

	if (position == 0) {
		seq_printf(seq, "optimize_scan: %d\n",
			   test_opt2(sb, MB_OPTIMIZE_SCAN) ? 1 : 0);
		seq_puts(seq, "max_free_order_lists:\n");
	}
	count = 0;
	xa_for_each(&sbi->s_mb_largest_free_orders[position], idx, grp)
		count++;
	seq_printf(seq, "\tlist_order_%u_groups: %u\n",
		   (unsigned int)position, count);

	return 0;
}

/**
 * ext4_mb_seq_structs_summary_stop - Implements the mb seq structs summary stop operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_seq_structs_summary_stop(struct seq_file *seq, void *v)
{
}

const struct seq_operations ext4_mb_seq_structs_summary_ops = {
	.start  = ext4_mb_seq_structs_summary_start,
	.next   = ext4_mb_seq_structs_summary_next,
	.stop   = ext4_mb_seq_structs_summary_stop,
	.show   = ext4_mb_seq_structs_summary_show,
};

/**
 * get_groupinfo_cache - Implements the get groupinfo cache operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static struct kmem_cache *get_groupinfo_cache(int blocksize_bits)
{
	int cache_index = blocksize_bits - EXT4_MIN_BLOCK_LOG_SIZE;
	struct kmem_cache *cachep = ext4_groupinfo_caches[cache_index];

	BUG_ON(!cachep);
	return cachep;
}


/**
 * ext4_mb_alloc_groupinfo - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int ext4_mb_alloc_groupinfo(struct super_block *sb, ext4_group_t ngroups)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned size;
	struct ext4_group_info ***old_groupinfo, ***new_groupinfo;

	size = (ngroups + EXT4_DESC_PER_BLOCK(sb) - 1) >>
		EXT4_DESC_PER_BLOCK_BITS(sb);
	if (size <= sbi->s_group_info_size)
		return 0;

	size = roundup_pow_of_two(sizeof(*sbi->s_group_info) * size);
	new_groupinfo = kvzalloc(size, GFP_KERNEL);
	if (!new_groupinfo) {
		ext4_msg(sb, KERN_ERR, "can't allocate buddy meta group");
		return -ENOMEM;
	}
	rcu_read_lock();
	old_groupinfo = rcu_dereference(sbi->s_group_info);
	if (old_groupinfo)
		memcpy(new_groupinfo, old_groupinfo,
		       sbi->s_group_info_size * sizeof(*sbi->s_group_info));
	rcu_read_unlock();
	rcu_assign_pointer(sbi->s_group_info, new_groupinfo);
	sbi->s_group_info_size = size / sizeof(*sbi->s_group_info);
	if (old_groupinfo)
		ext4_kvfree_array_rcu(old_groupinfo);
	ext4_debug("allocated s_groupinfo array for %d meta_bg's\n",
		   sbi->s_group_info_size);
	return 0;
}


/**
 * ext4_mb_add_groupinfo - Implements the mb add groupinfo operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int ext4_mb_add_groupinfo(struct super_block *sb, ext4_group_t group,
			  struct ext4_group_desc *desc)
{
	int i;
	int metalen = 0;
	int idx = group >> EXT4_DESC_PER_BLOCK_BITS(sb);
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_group_info **meta_group_info;
	struct kmem_cache *cachep = get_groupinfo_cache(sb->s_blocksize_bits);


	if (group % EXT4_DESC_PER_BLOCK(sb) == 0) {
		metalen = sizeof(*meta_group_info) <<
			EXT4_DESC_PER_BLOCK_BITS(sb);
		meta_group_info = kmalloc(metalen, GFP_NOFS);
		if (meta_group_info == NULL) {
			ext4_msg(sb, KERN_ERR, "can't allocate mem "
				 "for a buddy group");
			return -ENOMEM;
		}
		rcu_read_lock();
		rcu_dereference(sbi->s_group_info)[idx] = meta_group_info;
		rcu_read_unlock();
	}

	meta_group_info = sbi_array_rcu_deref(sbi, s_group_info, idx);
	i = group & (EXT4_DESC_PER_BLOCK(sb) - 1);

	meta_group_info[i] = kmem_cache_zalloc(cachep, GFP_NOFS);
	if (meta_group_info[i] == NULL) {
		ext4_msg(sb, KERN_ERR, "can't allocate buddy mem");
		goto exit_group_info;
	}
	set_bit(EXT4_GROUP_INFO_NEED_INIT_BIT,
		&(meta_group_info[i]->bb_state));


	if (ext4_has_group_desc_csum(sb) &&
	    (desc->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT))) {
		meta_group_info[i]->bb_free =
			ext4_free_clusters_after_init(sb, group, desc);
	} else {
		meta_group_info[i]->bb_free =
			ext4_free_group_clusters(sb, desc);
	}

	INIT_LIST_HEAD(&meta_group_info[i]->bb_prealloc_list);
	init_rwsem(&meta_group_info[i]->alloc_sem);
	meta_group_info[i]->bb_free_root = RB_ROOT;
	meta_group_info[i]->bb_largest_free_order = -1;
	meta_group_info[i]->bb_avg_fragment_size_order = -1;
	meta_group_info[i]->bb_group = group;

	mb_group_bb_bitmap_alloc(sb, meta_group_info[i], group);
	return 0;

exit_group_info:

	if (group % EXT4_DESC_PER_BLOCK(sb) == 0) {
		struct ext4_group_info ***group_info;

		rcu_read_lock();
		group_info = rcu_dereference(sbi->s_group_info);
		kfree(group_info[idx]);
		group_info[idx] = NULL;
		rcu_read_unlock();
	}
	return -ENOMEM;
}

/**
 * ext4_mb_init_backend - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_init_backend(struct super_block *sb)
{
	ext4_group_t ngroups = ext4_get_groups_count(sb);
	ext4_group_t i;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int err;
	struct ext4_group_desc *desc;
	struct ext4_group_info ***group_info;
	struct kmem_cache *cachep;

	err = ext4_mb_alloc_groupinfo(sb, ngroups);
	if (err)
		return err;

	sbi->s_buddy_cache = new_inode(sb);
	if (sbi->s_buddy_cache == NULL) {
		ext4_msg(sb, KERN_ERR, "can't get new inode");
		goto err_freesgi;
	}


	sbi->s_buddy_cache->i_ino = EXT4_BAD_INO;
	EXT4_I(sbi->s_buddy_cache)->i_disksize = 0;
	for (i = 0; i < ngroups; i++) {
		cond_resched();
		desc = ext4_get_group_desc(sb, i, NULL);
		if (desc == NULL) {
			ext4_msg(sb, KERN_ERR, "can't read descriptor %u", i);
			goto err_freebuddy;
		}
		if (ext4_mb_add_groupinfo(sb, i, desc) != 0)
			goto err_freebuddy;
	}

	if (ext4_has_feature_flex_bg(sb)) {


		if (sbi->s_es->s_log_groups_per_flex >= 32) {
			ext4_msg(sb, KERN_ERR, "too many log groups per flexible block group");
			goto err_freebuddy;
		}
		sbi->s_mb_prefetch = min_t(uint, 1 << sbi->s_es->s_log_groups_per_flex,
			BLK_MAX_SEGMENT_SIZE >> (sb->s_blocksize_bits - 9));
		sbi->s_mb_prefetch *= 8;
	} else {
		sbi->s_mb_prefetch = 32;
	}
	if (sbi->s_mb_prefetch > ext4_get_groups_count(sb))
		sbi->s_mb_prefetch = ext4_get_groups_count(sb);


	sbi->s_mb_prefetch_limit = sbi->s_mb_prefetch * 4;
	if (sbi->s_mb_prefetch_limit > ext4_get_groups_count(sb))
		sbi->s_mb_prefetch_limit = ext4_get_groups_count(sb);

	return 0;

err_freebuddy:
	cachep = get_groupinfo_cache(sb->s_blocksize_bits);
	while (i-- > 0) {
		struct ext4_group_info *grp = ext4_get_group_info(sb, i);

		if (grp)
			kmem_cache_free(cachep, grp);
	}
	i = sbi->s_group_info_size;
	rcu_read_lock();
	group_info = rcu_dereference(sbi->s_group_info);
	while (i-- > 0)
		kfree(group_info[i]);
	rcu_read_unlock();
	iput(sbi->s_buddy_cache);
err_freesgi:
	kvfree(rcu_access_pointer(sbi->s_group_info));
	return -ENOMEM;
}

/**
 * ext4_groupinfo_destroy_slabs - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_groupinfo_destroy_slabs(void)
{
	int i;

	for (i = 0; i < NR_GRPINFO_CACHES; i++) {
		kmem_cache_destroy(ext4_groupinfo_caches[i]);
		ext4_groupinfo_caches[i] = NULL;
	}
}

/**
 * ext4_groupinfo_create_slab - Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_groupinfo_create_slab(size_t size)
{
	static DEFINE_MUTEX(ext4_grpinfo_slab_create_mutex);
	int slab_size;
	int blocksize_bits = order_base_2(size);
	int cache_index = blocksize_bits - EXT4_MIN_BLOCK_LOG_SIZE;
	struct kmem_cache *cachep;

	if (cache_index >= NR_GRPINFO_CACHES)
		return -EINVAL;

	if (unlikely(cache_index < 0))
		cache_index = 0;

	mutex_lock(&ext4_grpinfo_slab_create_mutex);
	if (ext4_groupinfo_caches[cache_index]) {
		mutex_unlock(&ext4_grpinfo_slab_create_mutex);
		return 0;
	}

	slab_size = offsetof(struct ext4_group_info,
				bb_counters[blocksize_bits + 2]);

	cachep = kmem_cache_create(ext4_groupinfo_slab_names[cache_index],
					slab_size, 0, SLAB_RECLAIM_ACCOUNT,
					NULL);

	ext4_groupinfo_caches[cache_index] = cachep;

	mutex_unlock(&ext4_grpinfo_slab_create_mutex);
	if (!cachep) {
		printk(KERN_EMERG
		       "EXT4-fs: no memory for groupinfo slab cache\n");
		return -ENOMEM;
	}

	return 0;
}

/**
 * ext4_discard_work - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_discard_work(struct work_struct *work)
{
	struct ext4_sb_info *sbi = container_of(work,
			struct ext4_sb_info, s_discard_work);
	struct super_block *sb = sbi->s_sb;
	struct ext4_free_data *fd, *nfd;
	struct ext4_buddy e4b;
	LIST_HEAD(discard_list);
	ext4_group_t grp, load_grp;
	int err = 0;

	spin_lock(&sbi->s_md_lock);
	list_splice_init(&sbi->s_discard_list, &discard_list);
	spin_unlock(&sbi->s_md_lock);

	load_grp = UINT_MAX;
	list_for_each_entry_safe(fd, nfd, &discard_list, efd_list) {


		if ((sb->s_flags & SB_ACTIVE) && !err &&
		    !atomic_read(&sbi->s_retry_alloc_pending)) {
			grp = fd->efd_group;
			if (grp != load_grp) {
				if (load_grp != UINT_MAX)
					ext4_mb_unload_buddy(&e4b);

				err = ext4_mb_load_buddy(sb, grp, &e4b);
				if (err) {
					kmem_cache_free(ext4_free_data_cachep, fd);
					load_grp = UINT_MAX;
					continue;
				} else {
					load_grp = grp;
				}
			}

			ext4_lock_group(sb, grp);
			ext4_try_to_trim_range(sb, &e4b, fd->efd_start_cluster,
						fd->efd_start_cluster + fd->efd_count - 1, 1);
			ext4_unlock_group(sb, grp);
		}
		kmem_cache_free(ext4_free_data_cachep, fd);
	}

	if (load_grp != UINT_MAX)
		ext4_mb_unload_buddy(&e4b);
}

/**
 * ext4_mb_avg_fragment_size_destroy - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_mb_avg_fragment_size_destroy(struct ext4_sb_info *sbi)
{
	if (!sbi->s_mb_avg_fragment_size)
		return;

	for (int i = 0; i < MB_NUM_ORDERS(sbi->s_sb); i++)
		xa_destroy(&sbi->s_mb_avg_fragment_size[i]);

	kfree(sbi->s_mb_avg_fragment_size);
	sbi->s_mb_avg_fragment_size = NULL;
}

/**
 * ext4_mb_largest_free_orders_destroy - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_mb_largest_free_orders_destroy(struct ext4_sb_info *sbi)
{
	if (!sbi->s_mb_largest_free_orders)
		return;

	for (int i = 0; i < MB_NUM_ORDERS(sbi->s_sb); i++)
		xa_destroy(&sbi->s_mb_largest_free_orders[i]);

	kfree(sbi->s_mb_largest_free_orders);
	sbi->s_mb_largest_free_orders = NULL;
}

/**
 * ext4_mb_init - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int ext4_mb_init(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned i, j;
	unsigned offset, offset_incr;
	unsigned max;
	int ret;

	i = MB_NUM_ORDERS(sb) * sizeof(*sbi->s_mb_offsets);

	sbi->s_mb_offsets = kmalloc(i, GFP_KERNEL);
	if (sbi->s_mb_offsets == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	i = MB_NUM_ORDERS(sb) * sizeof(*sbi->s_mb_maxs);
	sbi->s_mb_maxs = kmalloc(i, GFP_KERNEL);
	if (sbi->s_mb_maxs == NULL) {
		ret = -ENOMEM;
		goto out;
	}

	ret = ext4_groupinfo_create_slab(sb->s_blocksize);
	if (ret < 0)
		goto out;


	sbi->s_mb_maxs[0] = sb->s_blocksize << 3;
	sbi->s_mb_offsets[0] = 0;

	i = 1;
	offset = 0;
	offset_incr = 1 << (sb->s_blocksize_bits - 1);
	max = sb->s_blocksize << 2;
	do {
		sbi->s_mb_offsets[i] = offset;
		sbi->s_mb_maxs[i] = max;
		offset += offset_incr;
		offset_incr = offset_incr >> 1;
		max = max >> 1;
		i++;
	} while (i < MB_NUM_ORDERS(sb));

	sbi->s_mb_avg_fragment_size =
		kmalloc_array(MB_NUM_ORDERS(sb), sizeof(struct xarray),
			GFP_KERNEL);
	if (!sbi->s_mb_avg_fragment_size) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < MB_NUM_ORDERS(sb); i++)
		xa_init(&sbi->s_mb_avg_fragment_size[i]);

	sbi->s_mb_largest_free_orders =
		kmalloc_array(MB_NUM_ORDERS(sb), sizeof(struct xarray),
			GFP_KERNEL);
	if (!sbi->s_mb_largest_free_orders) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < MB_NUM_ORDERS(sb); i++)
		xa_init(&sbi->s_mb_largest_free_orders[i]);

	spin_lock_init(&sbi->s_md_lock);
	sbi->s_mb_free_pending = 0;
	INIT_LIST_HEAD(&sbi->s_freed_data_list[0]);
	INIT_LIST_HEAD(&sbi->s_freed_data_list[1]);
	INIT_LIST_HEAD(&sbi->s_discard_list);
	INIT_WORK(&sbi->s_discard_work, ext4_discard_work);
	atomic_set(&sbi->s_retry_alloc_pending, 0);

	sbi->s_mb_max_to_scan = MB_DEFAULT_MAX_TO_SCAN;
	sbi->s_mb_min_to_scan = MB_DEFAULT_MIN_TO_SCAN;
	sbi->s_mb_stats = MB_DEFAULT_STATS;
	sbi->s_mb_stream_request = MB_DEFAULT_STREAM_THRESHOLD;
	sbi->s_mb_order2_reqs = MB_DEFAULT_ORDER2_REQS;
	sbi->s_mb_best_avail_max_trim_order = MB_DEFAULT_BEST_AVAIL_TRIM_ORDER;


	sbi->s_mb_group_prealloc = max(MB_DEFAULT_GROUP_PREALLOC >>
				       sbi->s_cluster_bits, 32);


	if (sbi->s_stripe > 1) {
		sbi->s_mb_group_prealloc = roundup(
			sbi->s_mb_group_prealloc, EXT4_NUM_B2C(sbi, sbi->s_stripe));
	}

	sbi->s_locality_groups = alloc_percpu(struct ext4_locality_group);
	if (sbi->s_locality_groups == NULL) {
		ret = -ENOMEM;
		goto out;
	}
	for_each_possible_cpu(i) {
		struct ext4_locality_group *lg;
		lg = per_cpu_ptr(sbi->s_locality_groups, i);
		mutex_init(&lg->lg_mutex);
		for (j = 0; j < PREALLOC_TB_SIZE; j++)
			INIT_LIST_HEAD(&lg->lg_prealloc_list[j]);
		spin_lock_init(&lg->lg_prealloc_lock);
	}

	if (bdev_nonrot(sb->s_bdev))
		sbi->s_mb_max_linear_groups = 0;
	else
		sbi->s_mb_max_linear_groups = MB_DEFAULT_LINEAR_LIMIT;

	ret = ext4_mb_init_backend(sb);
	if (ret != 0)
		goto out_free_locality_groups;

	return 0;

out_free_locality_groups:
	free_percpu(sbi->s_locality_groups);
	sbi->s_locality_groups = NULL;
out:
	ext4_mb_avg_fragment_size_destroy(sbi);
	ext4_mb_largest_free_orders_destroy(sbi);
	kfree(sbi->s_mb_offsets);
	sbi->s_mb_offsets = NULL;
	kfree(sbi->s_mb_maxs);
	sbi->s_mb_maxs = NULL;
	return ret;
}


/**
 * ext4_mb_cleanup_pa - Implements the mb cleanup pa operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_cleanup_pa(struct ext4_group_info *grp)
{
	struct ext4_prealloc_space *pa;
	struct list_head *cur, *tmp;
	int count = 0;

	list_for_each_safe(cur, tmp, &grp->bb_prealloc_list) {
		pa = list_entry(cur, struct ext4_prealloc_space, pa_group_list);
		list_del(&pa->pa_group_list);
		count++;
		kmem_cache_free(ext4_pspace_cachep, pa);
	}
	return count;
}

/**
 * ext4_mb_release - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void ext4_mb_release(struct super_block *sb)
{
	ext4_group_t ngroups = ext4_get_groups_count(sb);
	ext4_group_t i;
	int num_meta_group_infos;
	struct ext4_group_info *grinfo, ***group_info;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct kmem_cache *cachep = get_groupinfo_cache(sb->s_blocksize_bits);
	int count;


	flush_work(&sbi->s_discard_work);
	WARN_ON_ONCE(!list_empty(&sbi->s_discard_list));

	group_info = rcu_access_pointer(sbi->s_group_info);
	if (group_info) {
		for (i = 0; i < ngroups; i++) {
			cond_resched();
			grinfo = ext4_get_group_info(sb, i);
			if (!grinfo)
				continue;
			mb_group_bb_bitmap_free(grinfo);
			ext4_lock_group(sb, i);
			count = ext4_mb_cleanup_pa(grinfo);
			if (count)
				mb_debug(sb, "mballoc: %d PAs left\n",
					 count);
			ext4_unlock_group(sb, i);
			kmem_cache_free(cachep, grinfo);
		}
		num_meta_group_infos = (ngroups +
				EXT4_DESC_PER_BLOCK(sb) - 1) >>
			EXT4_DESC_PER_BLOCK_BITS(sb);
		for (i = 0; i < num_meta_group_infos; i++)
			kfree(group_info[i]);
		kvfree(group_info);
	}
	ext4_mb_avg_fragment_size_destroy(sbi);
	ext4_mb_largest_free_orders_destroy(sbi);
	kfree(sbi->s_mb_offsets);
	kfree(sbi->s_mb_maxs);
	iput(sbi->s_buddy_cache);
	if (sbi->s_mb_stats) {
		ext4_msg(sb, KERN_INFO,
		       "mballoc: %u blocks %u reqs (%u success)",
				atomic_read(&sbi->s_bal_allocated),
				atomic_read(&sbi->s_bal_reqs),
				atomic_read(&sbi->s_bal_success));
		ext4_msg(sb, KERN_INFO,
		      "mballoc: %u extents scanned, %u groups scanned, %u goal hits, "
				"%u 2^N hits, %u breaks, %u lost",
				atomic_read(&sbi->s_bal_ex_scanned),
				atomic_read(&sbi->s_bal_groups_scanned),
				atomic_read(&sbi->s_bal_goals),
				atomic_read(&sbi->s_bal_2orders),
				atomic_read(&sbi->s_bal_breaks),
				atomic_read(&sbi->s_mb_lost_chunks));
		ext4_msg(sb, KERN_INFO,
		       "mballoc: %u generated and it took %llu",
				atomic_read(&sbi->s_mb_buddies_generated),
				atomic64_read(&sbi->s_mb_generation_time));
		ext4_msg(sb, KERN_INFO,
		       "mballoc: %u preallocated, %u discarded",
				atomic_read(&sbi->s_mb_preallocated),
				atomic_read(&sbi->s_mb_discarded));
	}

	free_percpu(sbi->s_locality_groups);
}

/**
 * ext4_issue_discard - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_issue_discard(struct super_block *sb,
		ext4_group_t block_group, ext4_grpblk_t cluster, int count)
{
	ext4_fsblk_t discard_block;

	discard_block = (EXT4_C2B(EXT4_SB(sb), cluster) +
			 ext4_group_first_block_no(sb, block_group));
	count = EXT4_C2B(EXT4_SB(sb), count);
	trace_ext4_discard_blocks(sb,
			(unsigned long long) discard_block, count);

	return sb_issue_discard(sb, discard_block, count, GFP_NOFS, 0);
}

/**
 * ext4_free_data_in_buddy - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_free_data_in_buddy(struct super_block *sb,
				    struct ext4_free_data *entry)
{
	struct ext4_buddy e4b;
	struct ext4_group_info *db;
	int err, count = 0;

	mb_debug(sb, "gonna free %u blocks in group %u (0x%p):",
		 entry->efd_count, entry->efd_group, entry);

	err = ext4_mb_load_buddy(sb, entry->efd_group, &e4b);

	BUG_ON(err != 0);

	spin_lock(&EXT4_SB(sb)->s_md_lock);
	EXT4_SB(sb)->s_mb_free_pending -= entry->efd_count;
	spin_unlock(&EXT4_SB(sb)->s_md_lock);

	db = e4b.bd_info;

	count += entry->efd_count;
	ext4_lock_group(sb, entry->efd_group);

	rb_erase(&entry->efd_node, &(db->bb_free_root));
	mb_free_blocks(NULL, &e4b, entry->efd_start_cluster, entry->efd_count);


	EXT4_MB_GRP_CLEAR_TRIMMED(db);

	if (!db->bb_free_root.rb_node) {


		folio_put(e4b.bd_buddy_folio);
		folio_put(e4b.bd_bitmap_folio);
	}
	ext4_unlock_group(sb, entry->efd_group);
	ext4_mb_unload_buddy(&e4b);

	mb_debug(sb, "freed %d blocks in 1 structures\n", count);
}


/**
 * ext4_process_freed_data - Implements the process freed data operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void ext4_process_freed_data(struct super_block *sb, tid_t commit_tid)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_free_data *entry, *tmp;
	LIST_HEAD(freed_data_list);
	struct list_head *s_freed_head = &sbi->s_freed_data_list[commit_tid & 1];
	bool wake;

	list_replace_init(s_freed_head, &freed_data_list);

	list_for_each_entry(entry, &freed_data_list, efd_list)
		ext4_free_data_in_buddy(sb, entry);

	if (test_opt(sb, DISCARD)) {
		spin_lock(&sbi->s_md_lock);
		wake = list_empty(&sbi->s_discard_list);
		list_splice_tail(&freed_data_list, &sbi->s_discard_list);
		spin_unlock(&sbi->s_md_lock);
		if (wake)
			queue_work(system_unbound_wq, &sbi->s_discard_work);
	} else {
		list_for_each_entry_safe(entry, tmp, &freed_data_list, efd_list)
			kmem_cache_free(ext4_free_data_cachep, entry);
	}
}

/**
 * ext4_init_mballoc - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int __init ext4_init_mballoc(void)
{
	ext4_pspace_cachep = KMEM_CACHE(ext4_prealloc_space,
					SLAB_RECLAIM_ACCOUNT);
	if (ext4_pspace_cachep == NULL)
		goto out;

	ext4_ac_cachep = KMEM_CACHE(ext4_allocation_context,
				    SLAB_RECLAIM_ACCOUNT);
	if (ext4_ac_cachep == NULL)
		goto out_pa_free;

	ext4_free_data_cachep = KMEM_CACHE(ext4_free_data,
					   SLAB_RECLAIM_ACCOUNT);
	if (ext4_free_data_cachep == NULL)
		goto out_ac_free;

	return 0;

out_ac_free:
	kmem_cache_destroy(ext4_ac_cachep);
out_pa_free:
	kmem_cache_destroy(ext4_pspace_cachep);
out:
	return -ENOMEM;
}

/**
 * ext4_exit_mballoc - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void ext4_exit_mballoc(void)
{


	rcu_barrier();
	kmem_cache_destroy(ext4_pspace_cachep);
	kmem_cache_destroy(ext4_ac_cachep);
	kmem_cache_destroy(ext4_free_data_cachep);
	ext4_groupinfo_destroy_slabs();
}

#define EXT4_MB_BITMAP_MARKED_CHECK 0x0001
#define EXT4_MB_SYNC_UPDATE 0x0002
/**
 * ext4_mb_mark_context - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int
ext4_mb_mark_context(handle_t *handle, struct super_block *sb, bool state,
		     ext4_group_t group, ext4_grpblk_t blkoff,
		     ext4_grpblk_t len, int flags, ext4_grpblk_t *ret_changed)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct buffer_head *bitmap_bh = NULL;
	struct ext4_group_desc *gdp;
	struct buffer_head *gdp_bh;
	int err;
	unsigned int i, already, changed = len;

	KUNIT_STATIC_STUB_REDIRECT(ext4_mb_mark_context,
				   handle, sb, state, group, blkoff, len,
				   flags, ret_changed);

	if (ret_changed)
		*ret_changed = 0;
	bitmap_bh = ext4_read_block_bitmap(sb, group);
	if (IS_ERR(bitmap_bh))
		return PTR_ERR(bitmap_bh);

	if (handle) {
		BUFFER_TRACE(bitmap_bh, "getting write access");
		err = ext4_journal_get_write_access(handle, sb, bitmap_bh,
						    EXT4_JTR_NONE);
		if (err)
			goto out_err;
	}

	err = -EIO;
	gdp = ext4_get_group_desc(sb, group, &gdp_bh);
	if (!gdp)
		goto out_err;

	if (handle) {
		BUFFER_TRACE(gdp_bh, "get_write_access");
		err = ext4_journal_get_write_access(handle, sb, gdp_bh,
						    EXT4_JTR_NONE);
		if (err)
			goto out_err;
	}

	ext4_lock_group(sb, group);
	if (ext4_has_group_desc_csum(sb) &&
	    (gdp->bg_flags & cpu_to_le16(EXT4_BG_BLOCK_UNINIT))) {
		gdp->bg_flags &= cpu_to_le16(~EXT4_BG_BLOCK_UNINIT);
		ext4_free_group_clusters_set(sb, gdp,
			ext4_free_clusters_after_init(sb, group, gdp));
	}

	if (flags & EXT4_MB_BITMAP_MARKED_CHECK) {
		already = 0;
		for (i = 0; i < len; i++)
			if (mb_test_bit(blkoff + i, bitmap_bh->b_data) ==
					state)
				already++;
		changed = len - already;
	}

	if (state) {
		mb_set_bits(bitmap_bh->b_data, blkoff, len);
		ext4_free_group_clusters_set(sb, gdp,
			ext4_free_group_clusters(sb, gdp) - changed);
	} else {
		mb_clear_bits(bitmap_bh->b_data, blkoff, len);
		ext4_free_group_clusters_set(sb, gdp,
			ext4_free_group_clusters(sb, gdp) + changed);
	}

	ext4_block_bitmap_csum_set(sb, gdp, bitmap_bh);
	ext4_group_desc_csum_set(sb, group, gdp);
	ext4_unlock_group(sb, group);
	if (ret_changed)
		*ret_changed = changed;

	if (sbi->s_log_groups_per_flex) {
		ext4_group_t flex_group = ext4_flex_group(sbi, group);
		struct flex_groups *fg = sbi_array_rcu_deref(sbi,
					   s_flex_groups, flex_group);

		if (state)
			atomic64_sub(changed, &fg->free_clusters);
		else
			atomic64_add(changed, &fg->free_clusters);
	}

	err = ext4_handle_dirty_metadata(handle, NULL, bitmap_bh);
	if (err)
		goto out_err;
	err = ext4_handle_dirty_metadata(handle, NULL, gdp_bh);
	if (err)
		goto out_err;

	if (flags & EXT4_MB_SYNC_UPDATE) {
		sync_dirty_buffer(bitmap_bh);
		sync_dirty_buffer(gdp_bh);
	}

out_err:
	brelse(bitmap_bh);
	return err;
}


/**
 * ext4_mb_mark_diskspace_used - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack int
ext4_mb_mark_diskspace_used(struct ext4_allocation_context *ac, handle_t *handle)
{
	struct ext4_group_desc *gdp;
	struct ext4_sb_info *sbi;
	struct super_block *sb;
	ext4_fsblk_t block;
	int err, len;
	int flags = 0;
	ext4_grpblk_t changed;

	BUG_ON(ac->ac_status != AC_STATUS_FOUND);
	BUG_ON(ac->ac_b_ex.fe_len <= 0);

	sb = ac->ac_sb;
	sbi = EXT4_SB(sb);

	gdp = ext4_get_group_desc(sb, ac->ac_b_ex.fe_group, NULL);
	if (!gdp)
		return -EIO;
	ext4_debug("using block group %u(%d)\n", ac->ac_b_ex.fe_group,
			ext4_free_group_clusters(sb, gdp));

	block = ext4_grp_offs_to_block(sb, &ac->ac_b_ex);
	len = EXT4_C2B(sbi, ac->ac_b_ex.fe_len);
	if (!ext4_inode_block_valid(ac->ac_inode, block, len)) {
		ext4_error(sb, "Allocating blocks %llu-%llu which overlap "
			   "fs metadata", block, block+len);


		err = ext4_mb_mark_context(handle, sb, true,
					   ac->ac_b_ex.fe_group,
					   ac->ac_b_ex.fe_start,
					   ac->ac_b_ex.fe_len,
					   0, NULL);
		if (!err)
			err = -EFSCORRUPTED;
		return err;
	}

#ifdef AGGRESSIVE_CHECK
	flags |= EXT4_MB_BITMAP_MARKED_CHECK;
#endif
	err = ext4_mb_mark_context(handle, sb, true, ac->ac_b_ex.fe_group,
				   ac->ac_b_ex.fe_start, ac->ac_b_ex.fe_len,
				   flags, &changed);

	if (err && changed == 0)
		return err;

#ifdef AGGRESSIVE_CHECK
	BUG_ON(changed != ac->ac_b_ex.fe_len);
#endif
	percpu_counter_sub(&sbi->s_freeclusters_counter, ac->ac_b_ex.fe_len);

	return err;
}


/**
 * ext4_mb_mark_bb - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void ext4_mb_mark_bb(struct super_block *sb, ext4_fsblk_t block,
		     int len, bool state)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_group_t group;
	ext4_grpblk_t blkoff;
	int err = 0;
	unsigned int clen, thisgrp_len;

	while (len > 0) {
		ext4_get_group_no_and_offset(sb, block, &group, &blkoff);


		thisgrp_len = min_t(unsigned int, (unsigned int)len,
			EXT4_BLOCKS_PER_GROUP(sb) - EXT4_C2B(sbi, blkoff));
		clen = EXT4_NUM_B2C(sbi, thisgrp_len);

		if (!ext4_sb_block_valid(sb, NULL, block, thisgrp_len)) {
			ext4_error(sb, "Marking blocks in system zone - "
				   "Block = %llu, len = %u",
				   block, thisgrp_len);
			break;
		}

		err = ext4_mb_mark_context(NULL, sb, state,
					   group, blkoff, clen,
					   EXT4_MB_BITMAP_MARKED_CHECK |
					   EXT4_MB_SYNC_UPDATE,
					   NULL);
		if (err)
			break;

		block += thisgrp_len;
		len -= thisgrp_len;
		BUG_ON(len < 0);
	}
}


/**
 * ext4_mb_normalize_group_request - Implements the mb normalize group request operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_normalize_group_request(struct ext4_allocation_context *ac)
{
	struct super_block *sb = ac->ac_sb;
	struct ext4_locality_group *lg = ac->ac_lg;

	BUG_ON(lg == NULL);
	ac->ac_g_ex.fe_len = EXT4_SB(sb)->s_mb_group_prealloc;
	mb_debug(sb, "goal %u blocks for locality group\n", ac->ac_g_ex.fe_len);
}


/**
 * ext4_mb_pa_rb_next_iter - Implements the mb pa rb next iter operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline struct rb_node*
ext4_mb_pa_rb_next_iter(ext4_lblk_t new_start, ext4_lblk_t cur_start, struct rb_node *node)
{
	if (new_start < cur_start)
		return node->rb_left;
	else
		return node->rb_right;
}

/**
 * ext4_mb_pa_assert_overlap - Implements the mb pa assert overlap operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void
ext4_mb_pa_assert_overlap(struct ext4_allocation_context *ac,
			  ext4_lblk_t start, loff_t end)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_inode_info *ei = EXT4_I(ac->ac_inode);
	struct ext4_prealloc_space *tmp_pa;
	ext4_lblk_t tmp_pa_start;
	loff_t tmp_pa_end;
	struct rb_node *iter;

	read_lock(&ei->i_prealloc_lock);
	for (iter = ei->i_prealloc_node.rb_node; iter;
	     iter = ext4_mb_pa_rb_next_iter(start, tmp_pa_start, iter)) {
		tmp_pa = rb_entry(iter, struct ext4_prealloc_space,
				  pa_node.inode_node);
		tmp_pa_start = tmp_pa->pa_lstart;
		tmp_pa_end = pa_logical_end(sbi, tmp_pa);

		spin_lock(&tmp_pa->pa_lock);
		if (tmp_pa->pa_deleted == 0)
			BUG_ON(!(start >= tmp_pa_end || end <= tmp_pa_start));
		spin_unlock(&tmp_pa->pa_lock);
	}
	read_unlock(&ei->i_prealloc_lock);
}


/**
 * ext4_mb_pa_adjust_overlap - Implements the mb pa adjust overlap operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void
ext4_mb_pa_adjust_overlap(struct ext4_allocation_context *ac,
			  ext4_lblk_t *start, loff_t *end)
{
	struct ext4_inode_info *ei = EXT4_I(ac->ac_inode);
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_prealloc_space *tmp_pa = NULL, *left_pa = NULL, *right_pa = NULL;
	struct rb_node *iter;
	ext4_lblk_t new_start, tmp_pa_start, right_pa_start = -1;
	loff_t new_end, tmp_pa_end, left_pa_end = -1;

	new_start = *start;
	new_end = *end;


	read_lock(&ei->i_prealloc_lock);


	for (iter = ei->i_prealloc_node.rb_node; iter;
	     iter = ext4_mb_pa_rb_next_iter(ac->ac_o_ex.fe_logical,
					    tmp_pa_start, iter)) {
		tmp_pa = rb_entry(iter, struct ext4_prealloc_space,
				  pa_node.inode_node);
		tmp_pa_start = tmp_pa->pa_lstart;
		tmp_pa_end = pa_logical_end(sbi, tmp_pa);


		spin_lock(&tmp_pa->pa_lock);
		if (tmp_pa->pa_deleted == 0)
			BUG_ON(!(ac->ac_o_ex.fe_logical >= tmp_pa_end ||
				 ac->ac_o_ex.fe_logical < tmp_pa_start));
		spin_unlock(&tmp_pa->pa_lock);
	}


	if (tmp_pa) {
		if (tmp_pa->pa_lstart < ac->ac_o_ex.fe_logical) {
			struct rb_node *tmp;

			left_pa = tmp_pa;
			tmp = rb_next(&left_pa->pa_node.inode_node);
			if (tmp) {
				right_pa = rb_entry(tmp,
						    struct ext4_prealloc_space,
						    pa_node.inode_node);
			}
		} else {
			struct rb_node *tmp;

			right_pa = tmp_pa;
			tmp = rb_prev(&right_pa->pa_node.inode_node);
			if (tmp) {
				left_pa = rb_entry(tmp,
						   struct ext4_prealloc_space,
						   pa_node.inode_node);
			}
		}
	}


	if (left_pa) {
		for (iter = &left_pa->pa_node.inode_node;;
		     iter = rb_prev(iter)) {
			if (!iter) {
				left_pa = NULL;
				break;
			}

			tmp_pa = rb_entry(iter, struct ext4_prealloc_space,
					  pa_node.inode_node);
			left_pa = tmp_pa;
			spin_lock(&tmp_pa->pa_lock);
			if (tmp_pa->pa_deleted == 0) {
				spin_unlock(&tmp_pa->pa_lock);
				break;
			}
			spin_unlock(&tmp_pa->pa_lock);
		}
	}

	if (right_pa) {
		for (iter = &right_pa->pa_node.inode_node;;
		     iter = rb_next(iter)) {
			if (!iter) {
				right_pa = NULL;
				break;
			}

			tmp_pa = rb_entry(iter, struct ext4_prealloc_space,
					  pa_node.inode_node);
			right_pa = tmp_pa;
			spin_lock(&tmp_pa->pa_lock);
			if (tmp_pa->pa_deleted == 0) {
				spin_unlock(&tmp_pa->pa_lock);
				break;
			}
			spin_unlock(&tmp_pa->pa_lock);
		}
	}

	if (left_pa) {
		left_pa_end = pa_logical_end(sbi, left_pa);
		BUG_ON(left_pa_end > ac->ac_o_ex.fe_logical);
	}

	if (right_pa) {
		right_pa_start = right_pa->pa_lstart;
		BUG_ON(right_pa_start <= ac->ac_o_ex.fe_logical);
	}


	if (left_pa) {
		if (left_pa_end > new_start)
			new_start = left_pa_end;
	}

	if (right_pa) {
		if (right_pa_start < new_end)
			new_end = right_pa_start;
	}
	read_unlock(&ei->i_prealloc_lock);


	ext4_mb_pa_assert_overlap(ac, new_start, new_end);

	*start = new_start;
	*end = new_end;
}


/**
 * ext4_mb_normalize_request - Implements the mb normalize request operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack void
ext4_mb_normalize_request(struct ext4_allocation_context *ac,
				struct ext4_allocation_request *ar)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_super_block *es = sbi->s_es;
	int bsbits, max;
	loff_t size, start_off, end;
	loff_t orig_size __maybe_unused;
	ext4_lblk_t start;


	if (!(ac->ac_flags & EXT4_MB_HINT_DATA))
		return;


	if (unlikely(ac->ac_flags & EXT4_MB_HINT_GOAL_ONLY))
		return;


	if (ac->ac_flags & EXT4_MB_HINT_NOPREALLOC)
		return;

	if (ac->ac_flags & EXT4_MB_HINT_GROUP_ALLOC) {
		ext4_mb_normalize_group_request(ac);
		return ;
	}

	bsbits = ac->ac_sb->s_blocksize_bits;


	size = extent_logical_end(sbi, &ac->ac_o_ex);
	size = size << bsbits;
	if (size < i_size_read(ac->ac_inode))
		size = i_size_read(ac->ac_inode);
	orig_size = size;


	max = 2 << bsbits;

#define NRL_CHECK_SIZE(req, size, max, chunk_size)	\
		(req <= (size) || max <= (chunk_size))


	start_off = 0;
	if (size <= 16 * 1024) {
		size = 16 * 1024;
	} else if (size <= 32 * 1024) {
		size = 32 * 1024;
	} else if (size <= 64 * 1024) {
		size = 64 * 1024;
	} else if (size <= 128 * 1024) {
		size = 128 * 1024;
	} else if (size <= 256 * 1024) {
		size = 256 * 1024;
	} else if (size <= 512 * 1024) {
		size = 512 * 1024;
	} else if (size <= 1024 * 1024) {
		size = 1024 * 1024;
	} else if (NRL_CHECK_SIZE(size, 4 * 1024 * 1024, max, 2 * 1024)) {
		start_off = ((loff_t)ac->ac_o_ex.fe_logical >>
						(21 - bsbits)) << 21;
		size = 2 * 1024 * 1024;
	} else if (NRL_CHECK_SIZE(size, 8 * 1024 * 1024, max, 4 * 1024)) {
		start_off = ((loff_t)ac->ac_o_ex.fe_logical >>
							(22 - bsbits)) << 22;
		size = 4 * 1024 * 1024;
	} else if (NRL_CHECK_SIZE(EXT4_C2B(sbi, ac->ac_o_ex.fe_len),
					(8<<20)>>bsbits, max, 8 * 1024)) {
		start_off = ((loff_t)ac->ac_o_ex.fe_logical >>
							(23 - bsbits)) << 23;
		size = 8 * 1024 * 1024;
	} else {
		start_off = (loff_t) ac->ac_o_ex.fe_logical << bsbits;
		size	  = (loff_t) EXT4_C2B(sbi,
					      ac->ac_o_ex.fe_len) << bsbits;
	}
	size = size >> bsbits;
	start = start_off >> bsbits;


	start = max(start, rounddown(ac->ac_o_ex.fe_logical,
			(ext4_lblk_t)EXT4_BLOCKS_PER_GROUP(ac->ac_sb)));


	if (start + size > EXT_MAX_BLOCKS)
		size = EXT_MAX_BLOCKS - start;


	if (ar->pleft && start <= ar->lleft) {
		size -= ar->lleft + 1 - start;
		start = ar->lleft + 1;
	}
	if (ar->pright && start + size - 1 >= ar->lright)
		size -= start + size - ar->lright;


	if (size > EXT4_BLOCKS_PER_GROUP(ac->ac_sb))
		size = EXT4_BLOCKS_PER_GROUP(ac->ac_sb);

	end = start + size;

	ext4_mb_pa_adjust_overlap(ac, &start, &end);

	size = end - start;


	if (start + size <= ac->ac_o_ex.fe_logical ||
			start > ac->ac_o_ex.fe_logical) {
		ext4_msg(ac->ac_sb, KERN_ERR,
			 "start %lu, size %lu, fe_logical %lu",
			 (unsigned long) start, (unsigned long) size,
			 (unsigned long) ac->ac_o_ex.fe_logical);
		BUG();
	}
	BUG_ON(size <= 0 || size > EXT4_BLOCKS_PER_GROUP(ac->ac_sb));


	ac->ac_g_ex.fe_logical = start;
	ac->ac_g_ex.fe_len = EXT4_NUM_B2C(sbi, size);
	ac->ac_orig_goal_len = ac->ac_g_ex.fe_len;


	if (ar->pright && (ar->lright == (start + size)) &&
	    ar->pright >= size &&
	    ar->pright - size >= le32_to_cpu(es->s_first_data_block)) {

		ext4_get_group_no_and_offset(ac->ac_sb, ar->pright - size,
						&ac->ac_g_ex.fe_group,
						&ac->ac_g_ex.fe_start);
		ac->ac_flags |= EXT4_MB_HINT_TRY_GOAL;
	}
	if (ar->pleft && (ar->lleft + 1 == start) &&
	    ar->pleft + 1 < ext4_blocks_count(es)) {

		ext4_get_group_no_and_offset(ac->ac_sb, ar->pleft + 1,
						&ac->ac_g_ex.fe_group,
						&ac->ac_g_ex.fe_start);
		ac->ac_flags |= EXT4_MB_HINT_TRY_GOAL;
	}

	mb_debug(ac->ac_sb, "goal: %lld(was %lld) blocks at %u\n", size,
		 orig_size, start);
}

/**
 * ext4_mb_collect_stats - Implements the mb collect stats operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_collect_stats(struct ext4_allocation_context *ac)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);

	if (sbi->s_mb_stats && ac->ac_g_ex.fe_len >= 1) {
		atomic_inc(&sbi->s_bal_reqs);
		atomic_add(ac->ac_b_ex.fe_len, &sbi->s_bal_allocated);
		if (ac->ac_b_ex.fe_len >= ac->ac_o_ex.fe_len)
			atomic_inc(&sbi->s_bal_success);

		atomic_add(ac->ac_found, &sbi->s_bal_ex_scanned);
		for (int i=0; i<EXT4_MB_NUM_CRS; i++) {
			atomic_add(ac->ac_cX_found[i], &sbi->s_bal_cX_ex_scanned[i]);
		}

		atomic_add(ac->ac_groups_scanned, &sbi->s_bal_groups_scanned);
		if (ac->ac_g_ex.fe_start == ac->ac_b_ex.fe_start &&
				ac->ac_g_ex.fe_group == ac->ac_b_ex.fe_group)
			atomic_inc(&sbi->s_bal_goals);

		if (ac->ac_f_ex.fe_len == ac->ac_orig_goal_len)
			atomic_inc(&sbi->s_bal_len_goals);

		if (ac->ac_found > sbi->s_mb_max_to_scan)
			atomic_inc(&sbi->s_bal_breaks);
	}

	if (ac->ac_op == EXT4_MB_HISTORY_ALLOC)
		trace_ext4_mballoc_alloc(ac);
	else
		trace_ext4_mballoc_prealloc(ac);
}


/**
 * ext4_discard_allocated_blocks - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_discard_allocated_blocks(struct ext4_allocation_context *ac)
{
	struct ext4_prealloc_space *pa = ac->ac_pa;
	struct ext4_buddy e4b;
	int err;

	if (pa == NULL) {
		if (ac->ac_f_ex.fe_len == 0)
			return;
		err = ext4_mb_load_buddy(ac->ac_sb, ac->ac_f_ex.fe_group, &e4b);
		if (WARN_RATELIMIT(err,
				   "ext4: mb_load_buddy failed (%d)", err))


			return;
		ext4_lock_group(ac->ac_sb, ac->ac_f_ex.fe_group);
		mb_free_blocks(ac->ac_inode, &e4b, ac->ac_f_ex.fe_start,
			       ac->ac_f_ex.fe_len);
		ext4_unlock_group(ac->ac_sb, ac->ac_f_ex.fe_group);
		ext4_mb_unload_buddy(&e4b);
		return;
	}
	if (pa->pa_type == MB_INODE_PA) {
		spin_lock(&pa->pa_lock);
		pa->pa_free += ac->ac_b_ex.fe_len;
		spin_unlock(&pa->pa_lock);
	}
}


/**
 * ext4_mb_use_inode_pa - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_use_inode_pa(struct ext4_allocation_context *ac,
				struct ext4_prealloc_space *pa)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	ext4_fsblk_t start;
	ext4_fsblk_t end;
	int len;


	start = pa->pa_pstart + (ac->ac_o_ex.fe_logical - pa->pa_lstart);
	end = min(pa->pa_pstart + EXT4_C2B(sbi, pa->pa_len),
		  start + EXT4_C2B(sbi, ac->ac_o_ex.fe_len));
	len = EXT4_NUM_B2C(sbi, end - start);
	ext4_get_group_no_and_offset(ac->ac_sb, start, &ac->ac_b_ex.fe_group,
					&ac->ac_b_ex.fe_start);
	ac->ac_b_ex.fe_len = len;
	ac->ac_status = AC_STATUS_FOUND;
	ac->ac_pa = pa;

	BUG_ON(start < pa->pa_pstart);
	BUG_ON(end > pa->pa_pstart + EXT4_C2B(sbi, pa->pa_len));
	BUG_ON(pa->pa_free < len);
	BUG_ON(ac->ac_b_ex.fe_len <= 0);
	pa->pa_free -= len;

	mb_debug(ac->ac_sb, "use %llu/%d from inode pa %p\n", start, len, pa);
}


/**
 * ext4_mb_use_group_pa - Implements the mb use group pa operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_use_group_pa(struct ext4_allocation_context *ac,
				struct ext4_prealloc_space *pa)
{
	unsigned int len = ac->ac_o_ex.fe_len;

	ext4_get_group_no_and_offset(ac->ac_sb, pa->pa_pstart,
					&ac->ac_b_ex.fe_group,
					&ac->ac_b_ex.fe_start);
	ac->ac_b_ex.fe_len = len;
	ac->ac_status = AC_STATUS_FOUND;
	ac->ac_pa = pa;


	mb_debug(ac->ac_sb, "use %u/%u from group pa %p\n",
		 pa->pa_lstart, len, pa);
}


/**
 * ext4_mb_check_group_pa - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static struct ext4_prealloc_space *
ext4_mb_check_group_pa(ext4_fsblk_t goal_block,
			struct ext4_prealloc_space *pa,
			struct ext4_prealloc_space *cpa)
{
	ext4_fsblk_t cur_distance, new_distance;

	if (cpa == NULL) {
		atomic_inc(&pa->pa_count);
		return pa;
	}
	cur_distance = abs(goal_block - cpa->pa_pstart);
	new_distance = abs(goal_block - pa->pa_pstart);

	if (cur_distance <= new_distance)
		return cpa;


	atomic_dec(&cpa->pa_count);
	atomic_inc(&pa->pa_count);
	return pa;
}


/**
 * ext4_mb_pa_goal_check - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static bool
ext4_mb_pa_goal_check(struct ext4_allocation_context *ac,
		      struct ext4_prealloc_space *pa)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	ext4_fsblk_t start;

	if (likely(!(ac->ac_flags & EXT4_MB_HINT_GOAL_ONLY)))
		return true;


	start = pa->pa_pstart +
		(ac->ac_g_ex.fe_logical - pa->pa_lstart);
	if (ext4_grp_offs_to_block(ac->ac_sb, &ac->ac_g_ex) != start)
		return false;

	if (ac->ac_g_ex.fe_len > pa->pa_len -
	    EXT4_B2C(sbi, ac->ac_g_ex.fe_logical - pa->pa_lstart))
		return false;

	return true;
}


/**
 * ext4_mb_use_preallocated - Implements the mb use preallocated operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack bool
ext4_mb_use_preallocated(struct ext4_allocation_context *ac)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	int order, i;
	struct ext4_inode_info *ei = EXT4_I(ac->ac_inode);
	struct ext4_locality_group *lg;
	struct ext4_prealloc_space *tmp_pa = NULL, *cpa = NULL;
	struct rb_node *iter;
	ext4_fsblk_t goal_block;


	if (!(ac->ac_flags & EXT4_MB_HINT_DATA))
		return false;


	read_lock(&ei->i_prealloc_lock);

	if (RB_EMPTY_ROOT(&ei->i_prealloc_node)) {
		goto try_group_pa;
	}


	for (iter = ei->i_prealloc_node.rb_node; iter;
	     iter = ext4_mb_pa_rb_next_iter(ac->ac_o_ex.fe_logical,
					    tmp_pa->pa_lstart, iter)) {
		tmp_pa = rb_entry(iter, struct ext4_prealloc_space,
				  pa_node.inode_node);
	}


	if (tmp_pa->pa_lstart > ac->ac_o_ex.fe_logical) {
		struct rb_node *tmp;
		tmp = rb_prev(&tmp_pa->pa_node.inode_node);

		if (tmp) {
			tmp_pa = rb_entry(tmp, struct ext4_prealloc_space,
					    pa_node.inode_node);
		} else {


			goto try_group_pa;
		}
	}

	BUG_ON(!(tmp_pa && tmp_pa->pa_lstart <= ac->ac_o_ex.fe_logical));


	for (iter = &tmp_pa->pa_node.inode_node;; iter = rb_prev(iter)) {
		if (!iter) {


			goto try_group_pa;
		}
		tmp_pa = rb_entry(iter, struct ext4_prealloc_space,
				  pa_node.inode_node);
		spin_lock(&tmp_pa->pa_lock);
		if (tmp_pa->pa_deleted == 0) {


			break;
		} else {
			spin_unlock(&tmp_pa->pa_lock);
		}
	}

	BUG_ON(!(tmp_pa && tmp_pa->pa_lstart <= ac->ac_o_ex.fe_logical));
	BUG_ON(tmp_pa->pa_deleted == 1);


	if (ac->ac_o_ex.fe_logical >= pa_logical_end(sbi, tmp_pa)) {
		spin_unlock(&tmp_pa->pa_lock);
		goto try_group_pa;
	}


	if (!(ext4_test_inode_flag(ac->ac_inode, EXT4_INODE_EXTENTS)) &&
	    (tmp_pa->pa_pstart + EXT4_C2B(sbi, tmp_pa->pa_len) >
	     EXT4_MAX_BLOCK_FILE_PHYS)) {


		spin_unlock(&tmp_pa->pa_lock);
		goto try_group_pa;
	}

	if (tmp_pa->pa_free && likely(ext4_mb_pa_goal_check(ac, tmp_pa))) {
		atomic_inc(&tmp_pa->pa_count);
		ext4_mb_use_inode_pa(ac, tmp_pa);
		spin_unlock(&tmp_pa->pa_lock);
		read_unlock(&ei->i_prealloc_lock);
		return true;
	} else {


		WARN_ON_ONCE(tmp_pa->pa_free == 0);
	}
	spin_unlock(&tmp_pa->pa_lock);
try_group_pa:
	read_unlock(&ei->i_prealloc_lock);


	if (!(ac->ac_flags & EXT4_MB_HINT_GROUP_ALLOC))
		return false;


	lg = ac->ac_lg;
	if (lg == NULL)
		return false;
	order  = fls(ac->ac_o_ex.fe_len) - 1;
	if (order > PREALLOC_TB_SIZE - 1)

		order = PREALLOC_TB_SIZE - 1;

	goal_block = ext4_grp_offs_to_block(ac->ac_sb, &ac->ac_g_ex);


	for (i = order; i < PREALLOC_TB_SIZE; i++) {
		rcu_read_lock();
		list_for_each_entry_rcu(tmp_pa, &lg->lg_prealloc_list[i],
					pa_node.lg_list) {
			spin_lock(&tmp_pa->pa_lock);
			if (tmp_pa->pa_deleted == 0 &&
					tmp_pa->pa_free >= ac->ac_o_ex.fe_len) {

				cpa = ext4_mb_check_group_pa(goal_block,
								tmp_pa, cpa);
			}
			spin_unlock(&tmp_pa->pa_lock);
		}
		rcu_read_unlock();
	}
	if (cpa) {
		ext4_mb_use_group_pa(ac, cpa);
		return true;
	}
	return false;
}


/**
 * ext4_mb_generate_from_pa - Implements the mb generate from pa operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack
void ext4_mb_generate_from_pa(struct super_block *sb, void *bitmap,
					ext4_group_t group)
{
	struct ext4_group_info *grp = ext4_get_group_info(sb, group);
	struct ext4_prealloc_space *pa;
	struct list_head *cur;
	ext4_group_t groupnr;
	ext4_grpblk_t start;
	int preallocated = 0;
	int len;

	if (!grp)
		return;


	list_for_each(cur, &grp->bb_prealloc_list) {
		pa = list_entry(cur, struct ext4_prealloc_space, pa_group_list);
		spin_lock(&pa->pa_lock);
		ext4_get_group_no_and_offset(sb, pa->pa_pstart,
					     &groupnr, &start);
		len = pa->pa_len;
		spin_unlock(&pa->pa_lock);
		if (unlikely(len == 0))
			continue;
		BUG_ON(groupnr != group);
		mb_set_bits(bitmap, start, len);
		preallocated += len;
	}
	mb_debug(sb, "preallocated %d for group %u\n", preallocated, group);
}

/**
 * ext4_mb_mark_pa_deleted - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_mark_pa_deleted(struct super_block *sb,
				    struct ext4_prealloc_space *pa)
{
	struct ext4_inode_info *ei;

	if (pa->pa_deleted) {
		ext4_warning(sb, "deleted pa, type:%d, pblk:%llu, lblk:%u, len:%d\n",
			     pa->pa_type, pa->pa_pstart, pa->pa_lstart,
			     pa->pa_len);
		return;
	}

	pa->pa_deleted = 1;

	if (pa->pa_type == MB_INODE_PA) {
		ei = EXT4_I(pa->pa_inode);
		atomic_dec(&ei->i_prealloc_active);
	}
}

/**
 * ext4_mb_pa_free - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_mb_pa_free(struct ext4_prealloc_space *pa)
{
	BUG_ON(!pa);
	BUG_ON(atomic_read(&pa->pa_count));
	BUG_ON(pa->pa_deleted == 0);
	kmem_cache_free(ext4_pspace_cachep, pa);
}

/**
 * ext4_mb_pa_callback - Implements the mb pa callback operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_pa_callback(struct rcu_head *head)
{
	struct ext4_prealloc_space *pa;

	pa = container_of(head, struct ext4_prealloc_space, u.pa_rcu);
	ext4_mb_pa_free(pa);
}


/**
 * ext4_mb_put_pa - Implements the mb put pa operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_put_pa(struct ext4_allocation_context *ac,
			struct super_block *sb, struct ext4_prealloc_space *pa)
{
	ext4_group_t grp;
	ext4_fsblk_t grp_blk;
	struct ext4_inode_info *ei = EXT4_I(ac->ac_inode);


	spin_lock(&pa->pa_lock);
	if (!atomic_dec_and_test(&pa->pa_count) || pa->pa_free != 0) {
		spin_unlock(&pa->pa_lock);
		return;
	}

	if (pa->pa_deleted == 1) {
		spin_unlock(&pa->pa_lock);
		return;
	}

	ext4_mb_mark_pa_deleted(sb, pa);
	spin_unlock(&pa->pa_lock);

	grp_blk = pa->pa_pstart;


	if (pa->pa_type == MB_GROUP_PA)
		grp_blk--;

	grp = ext4_get_group_number(sb, grp_blk);


	ext4_lock_group(sb, grp);
	list_del(&pa->pa_group_list);
	ext4_unlock_group(sb, grp);

	if (pa->pa_type == MB_INODE_PA) {
		write_lock(pa->pa_node_lock.inode_lock);
		rb_erase(&pa->pa_node.inode_node, &ei->i_prealloc_node);
		write_unlock(pa->pa_node_lock.inode_lock);
		ext4_mb_pa_free(pa);
	} else {
		spin_lock(pa->pa_node_lock.lg_lock);
		list_del_rcu(&pa->pa_node.lg_list);
		spin_unlock(pa->pa_node_lock.lg_lock);
		call_rcu(&(pa)->u.pa_rcu, ext4_mb_pa_callback);
	}
}

/**
 * ext4_mb_pa_rb_insert - Implements the mb pa rb insert operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_pa_rb_insert(struct rb_root *root, struct rb_node *new)
{
	struct rb_node **iter = &root->rb_node, *parent = NULL;
	struct ext4_prealloc_space *iter_pa, *new_pa;
	ext4_lblk_t iter_start, new_start;

	while (*iter) {
		iter_pa = rb_entry(*iter, struct ext4_prealloc_space,
				   pa_node.inode_node);
		new_pa = rb_entry(new, struct ext4_prealloc_space,
				   pa_node.inode_node);
		iter_start = iter_pa->pa_lstart;
		new_start = new_pa->pa_lstart;

		parent = *iter;
		if (new_start < iter_start)
			iter = &((*iter)->rb_left);
		else
			iter = &((*iter)->rb_right);
	}

	rb_link_node(new, parent, iter);
	rb_insert_color(new, root);
}


/**
 * ext4_mb_new_inode_pa - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack void
ext4_mb_new_inode_pa(struct ext4_allocation_context *ac)
{
	struct super_block *sb = ac->ac_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_prealloc_space *pa;
	struct ext4_group_info *grp;
	struct ext4_inode_info *ei;


	BUG_ON(ac->ac_o_ex.fe_len >= ac->ac_b_ex.fe_len);
	BUG_ON(ac->ac_status != AC_STATUS_FOUND);
	BUG_ON(!S_ISREG(ac->ac_inode->i_mode));
	BUG_ON(ac->ac_pa == NULL);

	pa = ac->ac_pa;

	if (ac->ac_b_ex.fe_len < ac->ac_orig_goal_len) {
		struct ext4_free_extent ex = {
			.fe_logical = ac->ac_g_ex.fe_logical,
			.fe_len = ac->ac_orig_goal_len,
		};
		loff_t orig_goal_end = extent_logical_end(sbi, &ex);
		loff_t o_ex_end = extent_logical_end(sbi, &ac->ac_o_ex);


		BUG_ON(ac->ac_g_ex.fe_logical > ac->ac_o_ex.fe_logical);
		BUG_ON(ac->ac_g_ex.fe_len < ac->ac_o_ex.fe_len);


		ex.fe_len = ac->ac_b_ex.fe_len;

		ex.fe_logical = orig_goal_end - EXT4_C2B(sbi, ex.fe_len);
		if (ac->ac_o_ex.fe_logical >= ex.fe_logical)
			goto adjust_bex;

		ex.fe_logical = ac->ac_g_ex.fe_logical;
		if (o_ex_end <= extent_logical_end(sbi, &ex))
			goto adjust_bex;

		ex.fe_logical = ac->ac_o_ex.fe_logical;
adjust_bex:
		ac->ac_b_ex.fe_logical = ex.fe_logical;

		BUG_ON(ac->ac_o_ex.fe_logical < ac->ac_b_ex.fe_logical);
		BUG_ON(extent_logical_end(sbi, &ex) > orig_goal_end);
	}

	pa->pa_lstart = ac->ac_b_ex.fe_logical;
	pa->pa_pstart = ext4_grp_offs_to_block(sb, &ac->ac_b_ex);
	pa->pa_len = ac->ac_b_ex.fe_len;
	pa->pa_free = pa->pa_len;
	spin_lock_init(&pa->pa_lock);
	INIT_LIST_HEAD(&pa->pa_group_list);
	pa->pa_deleted = 0;
	pa->pa_type = MB_INODE_PA;

	mb_debug(sb, "new inode pa %p: %llu/%d for %u\n", pa, pa->pa_pstart,
		 pa->pa_len, pa->pa_lstart);
	trace_ext4_mb_new_inode_pa(ac, pa);

	atomic_add(pa->pa_free, &sbi->s_mb_preallocated);
	ext4_mb_use_inode_pa(ac, pa);

	ei = EXT4_I(ac->ac_inode);
	grp = ext4_get_group_info(sb, ac->ac_b_ex.fe_group);
	if (!grp)
		return;

	pa->pa_node_lock.inode_lock = &ei->i_prealloc_lock;
	pa->pa_inode = ac->ac_inode;

	list_add(&pa->pa_group_list, &grp->bb_prealloc_list);

	write_lock(pa->pa_node_lock.inode_lock);
	ext4_mb_pa_rb_insert(&ei->i_prealloc_node, &pa->pa_node.inode_node);
	write_unlock(pa->pa_node_lock.inode_lock);
	atomic_inc(&ei->i_prealloc_active);
}


/**
 * ext4_mb_new_group_pa - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack void
ext4_mb_new_group_pa(struct ext4_allocation_context *ac)
{
	struct super_block *sb = ac->ac_sb;
	struct ext4_locality_group *lg;
	struct ext4_prealloc_space *pa;
	struct ext4_group_info *grp;


	BUG_ON(ac->ac_o_ex.fe_len >= ac->ac_b_ex.fe_len);
	BUG_ON(ac->ac_status != AC_STATUS_FOUND);
	BUG_ON(!S_ISREG(ac->ac_inode->i_mode));
	BUG_ON(ac->ac_pa == NULL);

	pa = ac->ac_pa;

	pa->pa_pstart = ext4_grp_offs_to_block(sb, &ac->ac_b_ex);
	pa->pa_lstart = pa->pa_pstart;
	pa->pa_len = ac->ac_b_ex.fe_len;
	pa->pa_free = pa->pa_len;
	spin_lock_init(&pa->pa_lock);
	INIT_LIST_HEAD(&pa->pa_node.lg_list);
	INIT_LIST_HEAD(&pa->pa_group_list);
	pa->pa_deleted = 0;
	pa->pa_type = MB_GROUP_PA;

	mb_debug(sb, "new group pa %p: %llu/%d for %u\n", pa, pa->pa_pstart,
		 pa->pa_len, pa->pa_lstart);
	trace_ext4_mb_new_group_pa(ac, pa);

	ext4_mb_use_group_pa(ac, pa);
	atomic_add(pa->pa_free, &EXT4_SB(sb)->s_mb_preallocated);

	grp = ext4_get_group_info(sb, ac->ac_b_ex.fe_group);
	if (!grp)
		return;
	lg = ac->ac_lg;
	BUG_ON(lg == NULL);

	pa->pa_node_lock.lg_lock = &lg->lg_prealloc_lock;
	pa->pa_inode = NULL;

	list_add(&pa->pa_group_list, &grp->bb_prealloc_list);


}

/**
 * ext4_mb_new_preallocation - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_new_preallocation(struct ext4_allocation_context *ac)
{
	if (ac->ac_flags & EXT4_MB_HINT_GROUP_ALLOC)
		ext4_mb_new_group_pa(ac);
	else
		ext4_mb_new_inode_pa(ac);
}


/**
 * ext4_mb_release_inode_pa - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack void
ext4_mb_release_inode_pa(struct ext4_buddy *e4b, struct buffer_head *bitmap_bh,
			struct ext4_prealloc_space *pa)
{
	struct super_block *sb = e4b->bd_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	unsigned int end;
	unsigned int next;
	ext4_group_t group;
	ext4_grpblk_t bit;
	unsigned long long grp_blk_start;
	int free = 0;

	BUG_ON(pa->pa_deleted == 0);
	ext4_get_group_no_and_offset(sb, pa->pa_pstart, &group, &bit);
	grp_blk_start = pa->pa_pstart - EXT4_C2B(sbi, bit);
	BUG_ON(group != e4b->bd_group && pa->pa_len != 0);
	end = bit + pa->pa_len;

	while (bit < end) {
		bit = mb_find_next_zero_bit(bitmap_bh->b_data, end, bit);
		if (bit >= end)
			break;
		next = mb_find_next_bit(bitmap_bh->b_data, end, bit);
		mb_debug(sb, "free preallocated %u/%u in group %u\n",
			 (unsigned) ext4_group_first_block_no(sb, group) + bit,
			 (unsigned) next - bit, (unsigned) group);
		free += next - bit;

		trace_ext4_mballoc_discard(sb, NULL, group, bit, next - bit);
		trace_ext4_mb_release_inode_pa(pa, (grp_blk_start +
						    EXT4_C2B(sbi, bit)),
					       next - bit);
		mb_free_blocks(pa->pa_inode, e4b, bit, next - bit);
		bit = next + 1;
	}
	if (free != pa->pa_free) {
		ext4_msg(e4b->bd_sb, KERN_CRIT,
			 "pa %p: logic %lu, phys. %lu, len %d",
			 pa, (unsigned long) pa->pa_lstart,
			 (unsigned long) pa->pa_pstart,
			 pa->pa_len);
		ext4_grp_locked_error(sb, group, 0, 0, "free %u, pa_free %u",
					free, pa->pa_free);


	}
	atomic_add(free, &sbi->s_mb_discarded);
}

/**
 * ext4_mb_release_group_pa - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack void
ext4_mb_release_group_pa(struct ext4_buddy *e4b,
				struct ext4_prealloc_space *pa)
{
	struct super_block *sb = e4b->bd_sb;
	ext4_group_t group;
	ext4_grpblk_t bit;

	trace_ext4_mb_release_group_pa(sb, pa);
	BUG_ON(pa->pa_deleted == 0);
	ext4_get_group_no_and_offset(sb, pa->pa_pstart, &group, &bit);
	if (unlikely(group != e4b->bd_group && pa->pa_len != 0)) {
		ext4_warning(sb, "bad group: expected %u, group %u, pa_start %llu",
			     e4b->bd_group, group, pa->pa_pstart);
		return;
	}
	mb_free_blocks(pa->pa_inode, e4b, bit, pa->pa_len);
	atomic_add(pa->pa_len, &EXT4_SB(sb)->s_mb_discarded);
	trace_ext4_mballoc_discard(sb, NULL, group, bit, pa->pa_len);
}


/**
 * ext4_mb_discard_group_preallocations - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack int
ext4_mb_discard_group_preallocations(struct super_block *sb,
				     ext4_group_t group, int *busy)
{
	struct ext4_group_info *grp = ext4_get_group_info(sb, group);
	struct buffer_head *bitmap_bh = NULL;
	struct ext4_prealloc_space *pa, *tmp;
	LIST_HEAD(list);
	struct ext4_buddy e4b;
	struct ext4_inode_info *ei;
	int err;
	int free = 0;

	if (!grp)
		return 0;
	mb_debug(sb, "discard preallocation for group %u\n", group);
	if (list_empty(&grp->bb_prealloc_list))
		goto out_dbg;

	bitmap_bh = ext4_read_block_bitmap(sb, group);
	if (IS_ERR(bitmap_bh)) {
		err = PTR_ERR(bitmap_bh);
		ext4_error_err(sb, -err,
			       "Error %d reading block bitmap for %u",
			       err, group);
		goto out_dbg;
	}

	err = ext4_mb_load_buddy(sb, group, &e4b);
	if (err) {
		ext4_warning(sb, "Error %d loading buddy information for %u",
			     err, group);
		put_bh(bitmap_bh);
		goto out_dbg;
	}

	ext4_lock_group(sb, group);
	list_for_each_entry_safe(pa, tmp,
				&grp->bb_prealloc_list, pa_group_list) {
		spin_lock(&pa->pa_lock);
		if (atomic_read(&pa->pa_count)) {
			spin_unlock(&pa->pa_lock);
			*busy = 1;
			continue;
		}
		if (pa->pa_deleted) {
			spin_unlock(&pa->pa_lock);
			continue;
		}


		ext4_mb_mark_pa_deleted(sb, pa);

		if (!free)
			this_cpu_inc(discard_pa_seq);


		free += pa->pa_free;

		spin_unlock(&pa->pa_lock);

		list_del(&pa->pa_group_list);
		list_add(&pa->u.pa_tmp_list, &list);
	}


	list_for_each_entry_safe(pa, tmp, &list, u.pa_tmp_list) {


		if (pa->pa_type == MB_GROUP_PA) {
			spin_lock(pa->pa_node_lock.lg_lock);
			list_del_rcu(&pa->pa_node.lg_list);
			spin_unlock(pa->pa_node_lock.lg_lock);
		} else {
			write_lock(pa->pa_node_lock.inode_lock);
			ei = EXT4_I(pa->pa_inode);
			rb_erase(&pa->pa_node.inode_node, &ei->i_prealloc_node);
			write_unlock(pa->pa_node_lock.inode_lock);
		}

		list_del(&pa->u.pa_tmp_list);

		if (pa->pa_type == MB_GROUP_PA) {
			ext4_mb_release_group_pa(&e4b, pa);
			call_rcu(&(pa)->u.pa_rcu, ext4_mb_pa_callback);
		} else {
			ext4_mb_release_inode_pa(&e4b, bitmap_bh, pa);
			ext4_mb_pa_free(pa);
		}
	}

	ext4_unlock_group(sb, group);
	ext4_mb_unload_buddy(&e4b);
	put_bh(bitmap_bh);
out_dbg:
	mb_debug(sb, "discarded (%d) blocks preallocated for group %u bb_free (%d)\n",
		 free, group, grp->bb_free);
	return free;
}


/**
 * ext4_discard_preallocations - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void ext4_discard_preallocations(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bitmap_bh = NULL;
	struct ext4_prealloc_space *pa, *tmp;
	ext4_group_t group = 0;
	LIST_HEAD(list);
	struct ext4_buddy e4b;
	struct rb_node *iter;
	int err;

	if (!S_ISREG(inode->i_mode))
		return;

	if (EXT4_SB(sb)->s_mount_state & EXT4_FC_REPLAY)
		return;

	mb_debug(sb, "discard preallocation for inode %lu\n",
		 inode->i_ino);
	trace_ext4_discard_preallocations(inode,
			atomic_read(&ei->i_prealloc_active));

repeat:

	write_lock(&ei->i_prealloc_lock);
	for (iter = rb_first(&ei->i_prealloc_node); iter;
	     iter = rb_next(iter)) {
		pa = rb_entry(iter, struct ext4_prealloc_space,
			      pa_node.inode_node);
		BUG_ON(pa->pa_node_lock.inode_lock != &ei->i_prealloc_lock);

		spin_lock(&pa->pa_lock);
		if (atomic_read(&pa->pa_count)) {


			spin_unlock(&pa->pa_lock);
			write_unlock(&ei->i_prealloc_lock);
			ext4_msg(sb, KERN_ERR,
				 "uh-oh! used pa while discarding");
			WARN_ON(1);
			schedule_timeout_uninterruptible(HZ);
			goto repeat;

		}
		if (pa->pa_deleted == 0) {
			ext4_mb_mark_pa_deleted(sb, pa);
			spin_unlock(&pa->pa_lock);
			rb_erase(&pa->pa_node.inode_node, &ei->i_prealloc_node);
			list_add(&pa->u.pa_tmp_list, &list);
			continue;
		}


		spin_unlock(&pa->pa_lock);
		write_unlock(&ei->i_prealloc_lock);


		schedule_timeout_uninterruptible(HZ);
		goto repeat;
	}
	write_unlock(&ei->i_prealloc_lock);

	list_for_each_entry_safe(pa, tmp, &list, u.pa_tmp_list) {
		BUG_ON(pa->pa_type != MB_INODE_PA);
		group = ext4_get_group_number(sb, pa->pa_pstart);

		err = ext4_mb_load_buddy_gfp(sb, group, &e4b,
					     GFP_NOFS|__GFP_NOFAIL);
		if (err) {
			ext4_error_err(sb, -err, "Error %d loading buddy information for %u",
				       err, group);
			continue;
		}

		bitmap_bh = ext4_read_block_bitmap(sb, group);
		if (IS_ERR(bitmap_bh)) {
			err = PTR_ERR(bitmap_bh);
			ext4_error_err(sb, -err, "Error %d reading block bitmap for %u",
				       err, group);
			ext4_mb_unload_buddy(&e4b);
			continue;
		}

		ext4_lock_group(sb, group);
		list_del(&pa->pa_group_list);
		ext4_mb_release_inode_pa(&e4b, bitmap_bh, pa);
		ext4_unlock_group(sb, group);

		ext4_mb_unload_buddy(&e4b);
		put_bh(bitmap_bh);

		list_del(&pa->u.pa_tmp_list);
		ext4_mb_pa_free(pa);
	}
}

/**
 * ext4_mb_pa_alloc - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_pa_alloc(struct ext4_allocation_context *ac)
{
	struct ext4_prealloc_space *pa;

	BUG_ON(ext4_pspace_cachep == NULL);
	pa = kmem_cache_zalloc(ext4_pspace_cachep, GFP_NOFS);
	if (!pa)
		return -ENOMEM;
	atomic_set(&pa->pa_count, 1);
	ac->ac_pa = pa;
	return 0;
}

/**
 * ext4_mb_pa_put_free - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_pa_put_free(struct ext4_allocation_context *ac)
{
	struct ext4_prealloc_space *pa = ac->ac_pa;

	BUG_ON(!pa);
	ac->ac_pa = NULL;
	WARN_ON(!atomic_dec_and_test(&pa->pa_count));


	pa->pa_deleted = 1;
	ext4_mb_pa_free(pa);
}

#ifdef CONFIG_EXT4_DEBUG
/**
 * ext4_mb_show_pa - Implements the mb show pa operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_mb_show_pa(struct super_block *sb)
{
	ext4_group_t i, ngroups;

	if (ext4_forced_shutdown(sb))
		return;

	ngroups = ext4_get_groups_count(sb);
	mb_debug(sb, "groups: ");
	for (i = 0; i < ngroups; i++) {
		struct ext4_group_info *grp = ext4_get_group_info(sb, i);
		struct ext4_prealloc_space *pa;
		ext4_grpblk_t start;
		struct list_head *cur;

		if (!grp)
			continue;
		ext4_lock_group(sb, i);
		list_for_each(cur, &grp->bb_prealloc_list) {
			pa = list_entry(cur, struct ext4_prealloc_space,
					pa_group_list);
			spin_lock(&pa->pa_lock);
			ext4_get_group_no_and_offset(sb, pa->pa_pstart,
						     NULL, &start);
			spin_unlock(&pa->pa_lock);
			mb_debug(sb, "PA:%u:%d:%d\n", i, start,
				 pa->pa_len);
		}
		ext4_unlock_group(sb, i);
		mb_debug(sb, "%u: %d/%d\n", i, grp->bb_free,
			 grp->bb_fragments);
	}
}

/**
 * ext4_mb_show_ac - Implements the mb show ac operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_show_ac(struct ext4_allocation_context *ac)
{
	struct super_block *sb = ac->ac_sb;

	if (ext4_forced_shutdown(sb))
		return;

	mb_debug(sb, "Can't allocate:"
			" Allocation context details:");
	mb_debug(sb, "status %u flags 0x%x",
			ac->ac_status, ac->ac_flags);
	mb_debug(sb, "orig %lu/%lu/%lu@%lu, "
			"goal %lu/%lu/%lu@%lu, "
			"best %lu/%lu/%lu@%lu cr %d",
			(unsigned long)ac->ac_o_ex.fe_group,
			(unsigned long)ac->ac_o_ex.fe_start,
			(unsigned long)ac->ac_o_ex.fe_len,
			(unsigned long)ac->ac_o_ex.fe_logical,
			(unsigned long)ac->ac_g_ex.fe_group,
			(unsigned long)ac->ac_g_ex.fe_start,
			(unsigned long)ac->ac_g_ex.fe_len,
			(unsigned long)ac->ac_g_ex.fe_logical,
			(unsigned long)ac->ac_b_ex.fe_group,
			(unsigned long)ac->ac_b_ex.fe_start,
			(unsigned long)ac->ac_b_ex.fe_len,
			(unsigned long)ac->ac_b_ex.fe_logical,
			(int)ac->ac_criteria);
	mb_debug(sb, "%u found", ac->ac_found);
	mb_debug(sb, "used pa: %s, ", ac->ac_pa ? "yes" : "no");
	if (ac->ac_pa)
		mb_debug(sb, "pa_type %s\n", ac->ac_pa->pa_type == MB_GROUP_PA ?
			 "group pa" : "inode pa");
	ext4_mb_show_pa(sb);
}
#else
/**
 * ext4_mb_show_pa - Implements the mb show pa operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_mb_show_pa(struct super_block *sb)
{
}
/**
 * ext4_mb_show_ac - Implements the mb show ac operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_mb_show_ac(struct ext4_allocation_context *ac)
{
	ext4_mb_show_pa(ac->ac_sb);
}
#endif


/**
 * ext4_mb_group_or_file - Implements the mb group or file operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_group_or_file(struct ext4_allocation_context *ac)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	int bsbits = ac->ac_sb->s_blocksize_bits;
	loff_t size, isize;
	bool inode_pa_eligible, group_pa_eligible;

	if (!(ac->ac_flags & EXT4_MB_HINT_DATA))
		return;

	if (unlikely(ac->ac_flags & EXT4_MB_HINT_GOAL_ONLY))
		return;

	group_pa_eligible = sbi->s_mb_group_prealloc > 0;
	inode_pa_eligible = true;
	size = extent_logical_end(sbi, &ac->ac_o_ex);
	isize = (i_size_read(ac->ac_inode) + ac->ac_sb->s_blocksize - 1)
		>> bsbits;


	if ((size == isize) && !ext4_fs_is_busy(sbi) &&
	    !inode_is_open_for_write(ac->ac_inode))
		inode_pa_eligible = false;

	size = max(size, isize);

	if (size > sbi->s_mb_stream_request)
		group_pa_eligible = false;

	if (!group_pa_eligible) {
		if (inode_pa_eligible)
			ac->ac_flags |= EXT4_MB_STREAM_ALLOC;
		else
			ac->ac_flags |= EXT4_MB_HINT_NOPREALLOC;
		return;
	}

	BUG_ON(ac->ac_lg != NULL);


	ac->ac_lg = raw_cpu_ptr(sbi->s_locality_groups);


	ac->ac_flags |= EXT4_MB_HINT_GROUP_ALLOC;


	mutex_lock(&ac->ac_lg->lg_mutex);
}

/**
 * ext4_mb_initialize_context - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack void
ext4_mb_initialize_context(struct ext4_allocation_context *ac,
				struct ext4_allocation_request *ar)
{
	struct super_block *sb = ar->inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_super_block *es = sbi->s_es;
	ext4_group_t group;
	unsigned int len;
	ext4_fsblk_t goal;
	ext4_grpblk_t block;


	len = ar->len;


	if (len >= EXT4_CLUSTERS_PER_GROUP(sb))
		len = EXT4_CLUSTERS_PER_GROUP(sb);


	goal = ar->goal;
	if (goal < le32_to_cpu(es->s_first_data_block) ||
			goal >= ext4_blocks_count(es))
		goal = le32_to_cpu(es->s_first_data_block);
	ext4_get_group_no_and_offset(sb, goal, &group, &block);


	ac->ac_b_ex.fe_logical = EXT4_LBLK_CMASK(sbi, ar->logical);
	ac->ac_status = AC_STATUS_CONTINUE;
	ac->ac_sb = sb;
	ac->ac_inode = ar->inode;
	ac->ac_o_ex.fe_logical = ac->ac_b_ex.fe_logical;
	ac->ac_o_ex.fe_group = group;
	ac->ac_o_ex.fe_start = block;
	ac->ac_o_ex.fe_len = len;
	ac->ac_g_ex = ac->ac_o_ex;
	ac->ac_orig_goal_len = ac->ac_g_ex.fe_len;
	ac->ac_flags = ar->flags;


	ext4_mb_group_or_file(ac);

	mb_debug(sb, "init ac: %u blocks @ %u, goal %u, flags 0x%x, 2^%d, "
			"left: %u/%u, right %u/%u to %swritable\n",
			(unsigned) ar->len, (unsigned) ar->logical,
			(unsigned) ar->goal, ac->ac_flags, ac->ac_2order,
			(unsigned) ar->lleft, (unsigned) ar->pleft,
			(unsigned) ar->lright, (unsigned) ar->pright,
			inode_is_open_for_write(ar->inode) ? "" : "non-");
}

/**
 * ext4_mb_discard_lg_preallocations - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack void
ext4_mb_discard_lg_preallocations(struct super_block *sb,
					struct ext4_locality_group *lg,
					int order, int total_entries)
{
	ext4_group_t group = 0;
	struct ext4_buddy e4b;
	LIST_HEAD(discard_list);
	struct ext4_prealloc_space *pa, *tmp;

	mb_debug(sb, "discard locality group preallocation\n");

	spin_lock(&lg->lg_prealloc_lock);
	list_for_each_entry_rcu(pa, &lg->lg_prealloc_list[order],
				pa_node.lg_list,
				lockdep_is_held(&lg->lg_prealloc_lock)) {
		spin_lock(&pa->pa_lock);
		if (atomic_read(&pa->pa_count)) {


			spin_unlock(&pa->pa_lock);
			continue;
		}
		if (pa->pa_deleted) {
			spin_unlock(&pa->pa_lock);
			continue;
		}

		BUG_ON(pa->pa_type != MB_GROUP_PA);


		ext4_mb_mark_pa_deleted(sb, pa);
		spin_unlock(&pa->pa_lock);

		list_del_rcu(&pa->pa_node.lg_list);
		list_add(&pa->u.pa_tmp_list, &discard_list);

		total_entries--;
		if (total_entries <= 5) {


			break;
		}
	}
	spin_unlock(&lg->lg_prealloc_lock);

	list_for_each_entry_safe(pa, tmp, &discard_list, u.pa_tmp_list) {
		int err;

		group = ext4_get_group_number(sb, pa->pa_pstart);
		err = ext4_mb_load_buddy_gfp(sb, group, &e4b,
					     GFP_NOFS|__GFP_NOFAIL);
		if (err) {
			ext4_error_err(sb, -err, "Error %d loading buddy information for %u",
				       err, group);
			continue;
		}
		ext4_lock_group(sb, group);
		list_del(&pa->pa_group_list);
		ext4_mb_release_group_pa(&e4b, pa);
		ext4_unlock_group(sb, group);

		ext4_mb_unload_buddy(&e4b);
		list_del(&pa->u.pa_tmp_list);
		call_rcu(&(pa)->u.pa_rcu, ext4_mb_pa_callback);
	}
}


/**
 * ext4_mb_add_n_trim - Implements the mb add n trim operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_add_n_trim(struct ext4_allocation_context *ac)
{
	int order, added = 0, lg_prealloc_count = 1;
	struct super_block *sb = ac->ac_sb;
	struct ext4_locality_group *lg = ac->ac_lg;
	struct ext4_prealloc_space *tmp_pa, *pa = ac->ac_pa;

	order = fls(pa->pa_free) - 1;
	if (order > PREALLOC_TB_SIZE - 1)

		order = PREALLOC_TB_SIZE - 1;

	spin_lock(&lg->lg_prealloc_lock);
	list_for_each_entry_rcu(tmp_pa, &lg->lg_prealloc_list[order],
				pa_node.lg_list,
				lockdep_is_held(&lg->lg_prealloc_lock)) {
		spin_lock(&tmp_pa->pa_lock);
		if (tmp_pa->pa_deleted) {
			spin_unlock(&tmp_pa->pa_lock);
			continue;
		}
		if (!added && pa->pa_free < tmp_pa->pa_free) {

			list_add_tail_rcu(&pa->pa_node.lg_list,
						&tmp_pa->pa_node.lg_list);
			added = 1;


		}
		spin_unlock(&tmp_pa->pa_lock);
		lg_prealloc_count++;
	}
	if (!added)
		list_add_tail_rcu(&pa->pa_node.lg_list,
					&lg->lg_prealloc_list[order]);
	spin_unlock(&lg->lg_prealloc_lock);


	if (lg_prealloc_count > 8)
		ext4_mb_discard_lg_preallocations(sb, lg,
						  order, lg_prealloc_count);
}


/**
 * ext4_mb_release_context - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_release_context(struct ext4_allocation_context *ac)
{
	struct ext4_sb_info *sbi = EXT4_SB(ac->ac_sb);
	struct ext4_prealloc_space *pa = ac->ac_pa;
	if (pa) {
		if (pa->pa_type == MB_GROUP_PA) {

			spin_lock(&pa->pa_lock);
			pa->pa_pstart += EXT4_C2B(sbi, ac->ac_b_ex.fe_len);
			pa->pa_lstart += EXT4_C2B(sbi, ac->ac_b_ex.fe_len);
			pa->pa_free -= ac->ac_b_ex.fe_len;
			pa->pa_len -= ac->ac_b_ex.fe_len;
			spin_unlock(&pa->pa_lock);


			if (likely(pa->pa_free)) {
				spin_lock(pa->pa_node_lock.lg_lock);
				list_del_rcu(&pa->pa_node.lg_list);
				spin_unlock(pa->pa_node_lock.lg_lock);
				ext4_mb_add_n_trim(ac);
			}
		}

		ext4_mb_put_pa(ac, ac->ac_sb, pa);
	}
	if (ac->ac_bitmap_folio)
		folio_put(ac->ac_bitmap_folio);
	if (ac->ac_buddy_folio)
		folio_put(ac->ac_buddy_folio);
	if (ac->ac_flags & EXT4_MB_HINT_GROUP_ALLOC)
		mutex_unlock(&ac->ac_lg->lg_mutex);
	ext4_mb_collect_stats(ac);
}

/**
 * ext4_mb_discard_preallocations - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_mb_discard_preallocations(struct super_block *sb, int needed)
{
	ext4_group_t i, ngroups = ext4_get_groups_count(sb);
	int ret;
	int freed = 0, busy = 0;
	int retry = 0;

	trace_ext4_mb_discard_preallocations(sb, needed);

	if (needed == 0)
		needed = EXT4_CLUSTERS_PER_GROUP(sb) + 1;
 repeat:
	for (i = 0; i < ngroups && needed > 0; i++) {
		ret = ext4_mb_discard_group_preallocations(sb, i, &busy);
		freed += ret;
		needed -= ret;
		cond_resched();
	}

	if (needed > 0 && busy && ++retry < 3) {
		busy = 0;
		goto repeat;
	}

	return freed;
}

/**
 * ext4_mb_discard_preallocations_should_retry - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static bool ext4_mb_discard_preallocations_should_retry(struct super_block *sb,
			struct ext4_allocation_context *ac, u64 *seq)
{
	int freed;
	u64 seq_retry = 0;
	bool ret = false;

	freed = ext4_mb_discard_preallocations(sb, ac->ac_o_ex.fe_len);
	if (freed) {
		ret = true;
		goto out_dbg;
	}
	seq_retry = ext4_get_discard_pa_seq_sum();
	if (!(ac->ac_flags & EXT4_MB_STRICT_CHECK) || seq_retry != *seq) {
		ac->ac_flags |= EXT4_MB_STRICT_CHECK;
		*seq = seq_retry;
		ret = true;
	}

out_dbg:
	mb_debug(sb, "freed %d, retry ? %s\n", freed, ret ? "yes" : "no");
	return ret;
}


/**
 * ext4_mb_new_blocks_simple - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static ext4_fsblk_t
ext4_mb_new_blocks_simple(struct ext4_allocation_request *ar, int *errp)
{
	struct buffer_head *bitmap_bh;
	struct super_block *sb = ar->inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	ext4_group_t group, nr;
	ext4_grpblk_t blkoff;
	ext4_grpblk_t max = EXT4_CLUSTERS_PER_GROUP(sb);
	ext4_grpblk_t i = 0;
	ext4_fsblk_t goal, block;
	struct ext4_super_block *es = sbi->s_es;

	goal = ar->goal;
	if (goal < le32_to_cpu(es->s_first_data_block) ||
			goal >= ext4_blocks_count(es))
		goal = le32_to_cpu(es->s_first_data_block);

	ar->len = 0;
	ext4_get_group_no_and_offset(sb, goal, &group, &blkoff);
	for (nr = ext4_get_groups_count(sb); nr > 0; nr--) {
		bitmap_bh = ext4_read_block_bitmap(sb, group);
		if (IS_ERR(bitmap_bh)) {
			*errp = PTR_ERR(bitmap_bh);
			pr_warn("Failed to read block bitmap\n");
			return 0;
		}

		while (1) {
			i = mb_find_next_zero_bit(bitmap_bh->b_data, max,
						blkoff);
			if (i >= max)
				break;
			if (ext4_fc_replay_check_excluded(sb,
				ext4_group_first_block_no(sb, group) +
				EXT4_C2B(sbi, i))) {
				blkoff = i + 1;
			} else
				break;
		}
		brelse(bitmap_bh);
		if (i < max)
			break;

		if (++group >= ext4_get_groups_count(sb))
			group = 0;

		blkoff = 0;
	}

	if (i >= max) {
		*errp = -ENOSPC;
		return 0;
	}

	block = ext4_group_first_block_no(sb, group) + EXT4_C2B(sbi, i);
	ext4_mb_mark_bb(sb, block, 1, true);
	ar->len = 1;

	*errp = 0;
	return block;
}


/**
 * ext4_mb_new_blocks - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
ext4_fsblk_t ext4_mb_new_blocks(handle_t *handle,
				struct ext4_allocation_request *ar, int *errp)
{
	struct ext4_allocation_context *ac = NULL;
	struct ext4_sb_info *sbi;
	struct super_block *sb;
	ext4_fsblk_t block = 0;
	unsigned int inquota = 0;
	unsigned int reserv_clstrs = 0;
	int retries = 0;
	u64 seq;

	might_sleep();
	sb = ar->inode->i_sb;
	sbi = EXT4_SB(sb);

	trace_ext4_request_blocks(ar);
	if (sbi->s_mount_state & EXT4_FC_REPLAY)
		return ext4_mb_new_blocks_simple(ar, errp);


	if (ext4_is_quota_file(ar->inode))
		ar->flags |= EXT4_MB_USE_ROOT_BLOCKS;

	if ((ar->flags & EXT4_MB_DELALLOC_RESERVED) == 0) {


		while (ar->len &&
			ext4_claim_free_clusters(sbi, ar->len, ar->flags)) {


			cond_resched();
			ar->len = ar->len >> 1;
		}
		if (!ar->len) {
			ext4_mb_show_pa(sb);
			*errp = -ENOSPC;
			return 0;
		}
		reserv_clstrs = ar->len;
		if (ar->flags & EXT4_MB_USE_ROOT_BLOCKS) {
			dquot_alloc_block_nofail(ar->inode,
						 EXT4_C2B(sbi, ar->len));
		} else {
			while (ar->len &&
				dquot_alloc_block(ar->inode,
						  EXT4_C2B(sbi, ar->len))) {

				ar->flags |= EXT4_MB_HINT_NOPREALLOC;
				ar->len--;
			}
		}
		inquota = ar->len;
		if (ar->len == 0) {
			*errp = -EDQUOT;
			goto out;
		}
	}

	ac = kmem_cache_zalloc(ext4_ac_cachep, GFP_NOFS);
	if (!ac) {
		ar->len = 0;
		*errp = -ENOMEM;
		goto out;
	}

	ext4_mb_initialize_context(ac, ar);

	ac->ac_op = EXT4_MB_HISTORY_PREALLOC;
	seq = this_cpu_read(discard_pa_seq);
	if (!ext4_mb_use_preallocated(ac)) {
		ac->ac_op = EXT4_MB_HISTORY_ALLOC;
		ext4_mb_normalize_request(ac, ar);

		*errp = ext4_mb_pa_alloc(ac);
		if (*errp)
			goto errout;
repeat:

		*errp = ext4_mb_regular_allocator(ac);


		if (*errp) {
			ext4_mb_pa_put_free(ac);
			ext4_discard_allocated_blocks(ac);
			goto errout;
		}
		if (ac->ac_status == AC_STATUS_FOUND &&
			ac->ac_o_ex.fe_len >= ac->ac_f_ex.fe_len)
			ext4_mb_pa_put_free(ac);
	}
	if (likely(ac->ac_status == AC_STATUS_FOUND)) {
		*errp = ext4_mb_mark_diskspace_used(ac, handle);
		if (*errp) {
			ext4_discard_allocated_blocks(ac);
			goto errout;
		} else {
			block = ext4_grp_offs_to_block(sb, &ac->ac_b_ex);
			ar->len = ac->ac_b_ex.fe_len;
		}
	} else {
		if (++retries < 3 &&
		    ext4_mb_discard_preallocations_should_retry(sb, ac, &seq))
			goto repeat;


		ext4_mb_pa_put_free(ac);
		*errp = -ENOSPC;
	}

	if (*errp) {
errout:
		ac->ac_b_ex.fe_len = 0;
		ar->len = 0;
		ext4_mb_show_ac(ac);
	}
	ext4_mb_release_context(ac);
	kmem_cache_free(ext4_ac_cachep, ac);
out:
	if (inquota && ar->len < inquota)
		dquot_free_block(ar->inode, EXT4_C2B(sbi, inquota - ar->len));

	if (reserv_clstrs)
		percpu_counter_sub(&sbi->s_dirtyclusters_counter, reserv_clstrs);

	trace_ext4_allocate_blocks(ar, (unsigned long long)block);

	return block;
}


/**
 * ext4_try_merge_freed_extent - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_try_merge_freed_extent(struct ext4_sb_info *sbi,
					struct ext4_free_data *entry,
					struct ext4_free_data *new_entry,
					struct rb_root *entry_rb_root)
{
	if ((entry->efd_tid != new_entry->efd_tid) ||
	    (entry->efd_group != new_entry->efd_group))
		return;
	if (entry->efd_start_cluster + entry->efd_count ==
	    new_entry->efd_start_cluster) {
		new_entry->efd_start_cluster = entry->efd_start_cluster;
		new_entry->efd_count += entry->efd_count;
	} else if (new_entry->efd_start_cluster + new_entry->efd_count ==
		   entry->efd_start_cluster) {
		new_entry->efd_count += entry->efd_count;
	} else
		return;
	spin_lock(&sbi->s_md_lock);
	list_del(&entry->efd_list);
	spin_unlock(&sbi->s_md_lock);
	rb_erase(&entry->efd_node, entry_rb_root);
	kmem_cache_free(ext4_free_data_cachep, entry);
}

/**
 * ext4_mb_free_metadata - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static noinline_for_stack void
ext4_mb_free_metadata(handle_t *handle, struct ext4_buddy *e4b,
		      struct ext4_free_data *new_entry)
{
	ext4_group_t group = e4b->bd_group;
	ext4_grpblk_t cluster;
	ext4_grpblk_t clusters = new_entry->efd_count;
	struct ext4_free_data *entry;
	struct ext4_group_info *db = e4b->bd_info;
	struct super_block *sb = e4b->bd_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct rb_node **n = &db->bb_free_root.rb_node, *node;
	struct rb_node *parent = NULL, *new_node;

	BUG_ON(!ext4_handle_valid(handle));
	BUG_ON(e4b->bd_bitmap_folio == NULL);
	BUG_ON(e4b->bd_buddy_folio == NULL);

	new_node = &new_entry->efd_node;
	cluster = new_entry->efd_start_cluster;

	if (!*n) {


		folio_get(e4b->bd_buddy_folio);
		folio_get(e4b->bd_bitmap_folio);
	}
	while (*n) {
		parent = *n;
		entry = rb_entry(parent, struct ext4_free_data, efd_node);
		if (cluster < entry->efd_start_cluster)
			n = &(*n)->rb_left;
		else if (cluster >= (entry->efd_start_cluster + entry->efd_count))
			n = &(*n)->rb_right;
		else {
			ext4_grp_locked_error(sb, group, 0,
				ext4_group_first_block_no(sb, group) +
				EXT4_C2B(sbi, cluster),
				"Block already on to-be-freed list");
			kmem_cache_free(ext4_free_data_cachep, new_entry);
			return;
		}
	}

	rb_link_node(new_node, parent, n);
	rb_insert_color(new_node, &db->bb_free_root);


	node = rb_prev(new_node);
	if (node) {
		entry = rb_entry(node, struct ext4_free_data, efd_node);
		ext4_try_merge_freed_extent(sbi, entry, new_entry,
					    &(db->bb_free_root));
	}

	node = rb_next(new_node);
	if (node) {
		entry = rb_entry(node, struct ext4_free_data, efd_node);
		ext4_try_merge_freed_extent(sbi, entry, new_entry,
					    &(db->bb_free_root));
	}

	spin_lock(&sbi->s_md_lock);
	list_add_tail(&new_entry->efd_list, &sbi->s_freed_data_list[new_entry->efd_tid & 1]);
	sbi->s_mb_free_pending += clusters;
	spin_unlock(&sbi->s_md_lock);
}

/**
 * ext4_free_blocks_simple - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_free_blocks_simple(struct inode *inode, ext4_fsblk_t block,
					unsigned long count)
{
	struct super_block *sb = inode->i_sb;
	ext4_group_t group;
	ext4_grpblk_t blkoff;

	ext4_get_group_no_and_offset(sb, block, &group, &blkoff);
	ext4_mb_mark_context(NULL, sb, false, group, blkoff, count,
			     EXT4_MB_BITMAP_MARKED_CHECK |
			     EXT4_MB_SYNC_UPDATE,
			     NULL);
}


/**
 * ext4_mb_clear_bb - Implements the mb clear bb operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static void ext4_mb_clear_bb(handle_t *handle, struct inode *inode,
			       ext4_fsblk_t block, unsigned long count,
			       int flags)
{
	struct super_block *sb = inode->i_sb;
	struct ext4_group_info *grp;
	unsigned int overflow;
	ext4_grpblk_t bit;
	ext4_group_t block_group;
	struct ext4_sb_info *sbi;
	struct ext4_buddy e4b;
	unsigned int count_clusters;
	int err = 0;
	int mark_flags = 0;
	ext4_grpblk_t changed;

	sbi = EXT4_SB(sb);

	if (!(flags & EXT4_FREE_BLOCKS_VALIDATED) &&
	    !ext4_inode_block_valid(inode, block, count)) {
		ext4_error(sb, "Freeing blocks in system zone - "
			   "Block = %llu, count = %lu", block, count);

		goto error_out;
	}
	flags |= EXT4_FREE_BLOCKS_VALIDATED;

do_more:
	overflow = 0;
	ext4_get_group_no_and_offset(sb, block, &block_group, &bit);

	grp = ext4_get_group_info(sb, block_group);
	if (unlikely(!grp || EXT4_MB_GRP_BBITMAP_CORRUPT(grp)))
		return;


	if (EXT4_C2B(sbi, bit) + count > EXT4_BLOCKS_PER_GROUP(sb)) {
		overflow = EXT4_C2B(sbi, bit) + count -
			EXT4_BLOCKS_PER_GROUP(sb);
		count -= overflow;

		flags &= ~EXT4_FREE_BLOCKS_VALIDATED;
	}
	count_clusters = EXT4_NUM_B2C(sbi, count);
	trace_ext4_mballoc_free(sb, inode, block_group, bit, count_clusters);


	err = ext4_mb_load_buddy_gfp(sb, block_group, &e4b,
				     GFP_NOFS|__GFP_NOFAIL);
	if (err)
		goto error_out;

	if (!(flags & EXT4_FREE_BLOCKS_VALIDATED) &&
	    !ext4_inode_block_valid(inode, block, count)) {
		ext4_error(sb, "Freeing blocks in system zone - "
			   "Block = %llu, count = %lu", block, count);

		goto error_clean;
	}

#ifdef AGGRESSIVE_CHECK
	mark_flags |= EXT4_MB_BITMAP_MARKED_CHECK;
#endif
	err = ext4_mb_mark_context(handle, sb, false, block_group, bit,
				   count_clusters, mark_flags, &changed);


	if (err && changed == 0)
		goto error_clean;

#ifdef AGGRESSIVE_CHECK
	BUG_ON(changed != count_clusters);
#endif


	if (ext4_handle_valid(handle) &&
	    ((flags & EXT4_FREE_BLOCKS_METADATA) ||
	     !ext4_should_writeback_data(inode))) {
		struct ext4_free_data *new_entry;


		new_entry = kmem_cache_alloc(ext4_free_data_cachep,
				GFP_NOFS|__GFP_NOFAIL);
		new_entry->efd_start_cluster = bit;
		new_entry->efd_group = block_group;
		new_entry->efd_count = count_clusters;
		new_entry->efd_tid = handle->h_transaction->t_tid;

		ext4_lock_group(sb, block_group);
		ext4_mb_free_metadata(handle, &e4b, new_entry);
	} else {
		if (test_opt(sb, DISCARD)) {
			err = ext4_issue_discard(sb, block_group, bit,
						 count_clusters);


			if (err == -EOPNOTSUPP)
				err = 0;
			if (err)
				ext4_msg(sb, KERN_WARNING, "discard request in"
					 " group:%u block:%d count:%lu failed"
					 " with %d", block_group, bit, count,
					 err);
		}

		EXT4_MB_GRP_CLEAR_TRIMMED(e4b.bd_info);

		ext4_lock_group(sb, block_group);
		mb_free_blocks(inode, &e4b, bit, count_clusters);
	}

	ext4_unlock_group(sb, block_group);


	if (!(flags & EXT4_FREE_BLOCKS_RERESERVE_CLUSTER)) {
		if (!(flags & EXT4_FREE_BLOCKS_NO_QUOT_UPDATE))
			dquot_free_block(inode, EXT4_C2B(sbi, count_clusters));
		percpu_counter_add(&sbi->s_freeclusters_counter,
				   count_clusters);
	}

	if (overflow && !err) {
		block += count;
		count = overflow;
		ext4_mb_unload_buddy(&e4b);

		flags &= ~EXT4_FREE_BLOCKS_VALIDATED;
		goto do_more;
	}

error_clean:
	ext4_mb_unload_buddy(&e4b);
error_out:
	ext4_std_error(sb, err);
}


/**
 * ext4_free_blocks - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
void ext4_free_blocks(handle_t *handle, struct inode *inode,
		      struct buffer_head *bh, ext4_fsblk_t block,
		      unsigned long count, int flags)
{
	struct super_block *sb = inode->i_sb;
	unsigned int overflow;
	struct ext4_sb_info *sbi;

	sbi = EXT4_SB(sb);

	if (bh) {
		if (block)
			BUG_ON(block != bh->b_blocknr);
		else
			block = bh->b_blocknr;
	}

	if (sbi->s_mount_state & EXT4_FC_REPLAY) {
		ext4_free_blocks_simple(inode, block, EXT4_NUM_B2C(sbi, count));
		return;
	}

	might_sleep();

	if (!(flags & EXT4_FREE_BLOCKS_VALIDATED) &&
	    !ext4_inode_block_valid(inode, block, count)) {
		ext4_error(sb, "Freeing blocks not in datazone - "
			   "block = %llu, count = %lu", block, count);
		return;
	}
	flags |= EXT4_FREE_BLOCKS_VALIDATED;

	ext4_debug("freeing block %llu\n", block);
	trace_ext4_free_blocks(inode, block, count, flags);

	if (bh && (flags & EXT4_FREE_BLOCKS_FORGET)) {
		BUG_ON(count > 1);

		ext4_forget(handle, flags & EXT4_FREE_BLOCKS_METADATA,
			    inode, bh, block);
	}


	overflow = EXT4_PBLK_COFF(sbi, block);
	if (overflow) {
		if (flags & EXT4_FREE_BLOCKS_NOFREE_FIRST_CLUSTER) {
			overflow = sbi->s_cluster_ratio - overflow;
			block += overflow;
			if (count > overflow)
				count -= overflow;
			else
				return;
		} else {
			block -= overflow;
			count += overflow;
		}

		flags &= ~EXT4_FREE_BLOCKS_VALIDATED;
	}
	overflow = EXT4_LBLK_COFF(sbi, count);
	if (overflow) {
		if (flags & EXT4_FREE_BLOCKS_NOFREE_LAST_CLUSTER) {
			if (count > overflow)
				count -= overflow;
			else
				return;
		} else
			count += sbi->s_cluster_ratio - overflow;

		flags &= ~EXT4_FREE_BLOCKS_VALIDATED;
	}

	if (!bh && (flags & EXT4_FREE_BLOCKS_FORGET)) {
		int i;
		int is_metadata = flags & EXT4_FREE_BLOCKS_METADATA;

		for (i = 0; i < count; i++) {
			cond_resched();
			if (is_metadata)
				bh = sb_find_get_block_nonatomic(inode->i_sb,
								 block + i);
			ext4_forget(handle, is_metadata, inode, bh, block + i);
		}
	}

	ext4_mb_clear_bb(handle, inode, block, count, flags);
}


/**
 * ext4_group_add_blocks - Implements the group add blocks operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int ext4_group_add_blocks(handle_t *handle, struct super_block *sb,
			 ext4_fsblk_t block, unsigned long count)
{
	ext4_group_t block_group;
	ext4_grpblk_t bit;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_buddy e4b;
	int err = 0;
	ext4_fsblk_t first_cluster = EXT4_B2C(sbi, block);
	ext4_fsblk_t last_cluster = EXT4_B2C(sbi, block + count - 1);
	unsigned long cluster_count = last_cluster - first_cluster + 1;
	ext4_grpblk_t changed;

	ext4_debug("Adding block(s) %llu-%llu\n", block, block + count - 1);

	if (cluster_count == 0)
		return 0;

	ext4_get_group_no_and_offset(sb, block, &block_group, &bit);


	if (bit + cluster_count > EXT4_CLUSTERS_PER_GROUP(sb)) {
		ext4_warning(sb, "too many blocks added to group %u",
			     block_group);
		err = -EINVAL;
		goto error_out;
	}

	err = ext4_mb_load_buddy(sb, block_group, &e4b);
	if (err)
		goto error_out;

	if (!ext4_sb_block_valid(sb, NULL, block, count)) {
		ext4_error(sb, "Adding blocks in system zones - "
			   "Block = %llu, count = %lu",
			   block, count);
		err = -EINVAL;
		goto error_clean;
	}

	err = ext4_mb_mark_context(handle, sb, false, block_group, bit,
				   cluster_count, EXT4_MB_BITMAP_MARKED_CHECK,
				   &changed);
	if (err && changed == 0)
		goto error_clean;

	if (changed != cluster_count)
		ext4_error(sb, "bit already cleared in group %u", block_group);

	ext4_lock_group(sb, block_group);
	mb_free_blocks(NULL, &e4b, bit, cluster_count);
	ext4_unlock_group(sb, block_group);
	percpu_counter_add(&sbi->s_freeclusters_counter,
			   changed);

error_clean:
	ext4_mb_unload_buddy(&e4b);
error_out:
	ext4_std_error(sb, err);
	return err;
}


/**
 * __acquires - Implements the acquires operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_trim_extent(struct super_block *sb,
		int start, int count, struct ext4_buddy *e4b)
__releases(bitlock)
__acquires(bitlock)
{
	struct ext4_free_extent ex;
	ext4_group_t group = e4b->bd_group;
	int ret = 0;

	trace_ext4_trim_extent(sb, group, start, count);

	assert_spin_locked(ext4_group_lock_ptr(sb, group));

	ex.fe_start = start;
	ex.fe_group = group;
	ex.fe_len = count;


	mb_mark_used(e4b, &ex);
	ext4_unlock_group(sb, group);
	ret = ext4_issue_discard(sb, group, start, count);
	ext4_lock_group(sb, group);
	mb_free_blocks(NULL, e4b, start, ex.fe_len);
	return ret;
}

/**
 * ext4_last_grp_cluster - Implements the last grp cluster operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static ext4_grpblk_t ext4_last_grp_cluster(struct super_block *sb,
					   ext4_group_t grp)
{
	unsigned long nr_clusters_in_group;

	if (grp < (ext4_get_groups_count(sb) - 1))
		nr_clusters_in_group = EXT4_CLUSTERS_PER_GROUP(sb);
	else
		nr_clusters_in_group = (ext4_blocks_count(EXT4_SB(sb)->s_es) -
					ext4_group_first_block_no(sb, grp))
				       >> EXT4_CLUSTER_BITS(sb);

	return nr_clusters_in_group - 1;
}

/**
 * ext4_trim_interrupted - Implements the trim interrupted operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static bool ext4_trim_interrupted(void)
{
	return fatal_signal_pending(current) || freezing(current);
}

/**
 * __releases - Implements the releases operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static int ext4_try_to_trim_range(struct super_block *sb,
		struct ext4_buddy *e4b, ext4_grpblk_t start,
		ext4_grpblk_t max, ext4_grpblk_t minblocks)
__acquires(ext4_group_lock_ptr(sb, e4b->bd_group))
__releases(ext4_group_lock_ptr(sb, e4b->bd_group))
{
	ext4_grpblk_t next, count, free_count, last, origin_start;
	bool set_trimmed = false;
	void *bitmap;

	if (unlikely(EXT4_MB_GRP_BBITMAP_CORRUPT(e4b->bd_info)))
		return 0;

	last = ext4_last_grp_cluster(sb, e4b->bd_group);
	bitmap = e4b->bd_bitmap;
	if (start == 0 && max >= last)
		set_trimmed = true;
	origin_start = start;
	start = max(e4b->bd_info->bb_first_free, start);
	count = 0;
	free_count = 0;

	while (start <= max) {
		start = mb_find_next_zero_bit(bitmap, max + 1, start);
		if (start > max)
			break;

		next = mb_find_next_bit(bitmap, last + 1, start);
		if (origin_start == 0 && next >= last)
			set_trimmed = true;

		if ((next - start) >= minblocks) {
			int ret = ext4_trim_extent(sb, start, next - start, e4b);

			if (ret && ret != -EOPNOTSUPP)
				return count;
			count += next - start;
		}
		free_count += next - start;
		start = next + 1;

		if (ext4_trim_interrupted())
			return count;

		if (need_resched()) {
			ext4_unlock_group(sb, e4b->bd_group);
			cond_resched();
			ext4_lock_group(sb, e4b->bd_group);
		}

		if ((e4b->bd_info->bb_free - free_count) < minblocks)
			break;
	}

	if (set_trimmed)
		EXT4_MB_GRP_SET_TRIMMED(e4b->bd_info);

	return count;
}


/**
 * ext4_trim_all_free - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static ext4_grpblk_t
ext4_trim_all_free(struct super_block *sb, ext4_group_t group,
		   ext4_grpblk_t start, ext4_grpblk_t max,
		   ext4_grpblk_t minblocks)
{
	struct ext4_buddy e4b;
	int ret;

	trace_ext4_trim_all_free(sb, group, start, max);

	ret = ext4_mb_load_buddy(sb, group, &e4b);
	if (ret) {
		ext4_warning(sb, "Error %d loading buddy information for %u",
			     ret, group);
		return ret;
	}

	ext4_lock_group(sb, group);

	if (!EXT4_MB_GRP_WAS_TRIMMED(e4b.bd_info) ||
	    minblocks < EXT4_SB(sb)->s_last_trim_minblks)
		ret = ext4_try_to_trim_range(sb, &e4b, start, max, minblocks);
	else
		ret = 0;

	ext4_unlock_group(sb, group);
	ext4_mb_unload_buddy(&e4b);

	ext4_debug("trimmed %d blocks in the group %d\n",
		ret, group);

	return ret;
}


/**
 * ext4_trim_fs - Implements the trim fs operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int ext4_trim_fs(struct super_block *sb, struct fstrim_range *range)
{
	unsigned int discard_granularity = bdev_discard_granularity(sb->s_bdev);
	struct ext4_group_info *grp;
	ext4_group_t group, first_group, last_group;
	ext4_grpblk_t cnt = 0, first_cluster, last_cluster;
	uint64_t start, end, minlen, trimmed = 0;
	ext4_fsblk_t first_data_blk =
			le32_to_cpu(EXT4_SB(sb)->s_es->s_first_data_block);
	ext4_fsblk_t max_blks = ext4_blocks_count(EXT4_SB(sb)->s_es);
	int ret = 0;

	start = range->start >> sb->s_blocksize_bits;
	end = start + (range->len >> sb->s_blocksize_bits) - 1;
	minlen = EXT4_NUM_B2C(EXT4_SB(sb),
			      range->minlen >> sb->s_blocksize_bits);

	if (minlen > EXT4_CLUSTERS_PER_GROUP(sb) ||
	    start >= max_blks ||
	    range->len < sb->s_blocksize)
		return -EINVAL;

	if (range->minlen < discard_granularity) {
		minlen = EXT4_NUM_B2C(EXT4_SB(sb),
				discard_granularity >> sb->s_blocksize_bits);
		if (minlen > EXT4_CLUSTERS_PER_GROUP(sb))
			goto out;
	}
	if (end >= max_blks - 1)
		end = max_blks - 1;
	if (end <= first_data_blk)
		goto out;
	if (start < first_data_blk)
		start = first_data_blk;


	ext4_get_group_no_and_offset(sb, (ext4_fsblk_t) start,
				     &first_group, &first_cluster);
	ext4_get_group_no_and_offset(sb, (ext4_fsblk_t) end,
				     &last_group, &last_cluster);


	end = EXT4_CLUSTERS_PER_GROUP(sb) - 1;

	for (group = first_group; group <= last_group; group++) {
		if (ext4_trim_interrupted())
			break;
		grp = ext4_get_group_info(sb, group);
		if (!grp)
			continue;

		if (unlikely(EXT4_MB_GRP_NEED_INIT(grp))) {
			ret = ext4_mb_init_group(sb, group, GFP_NOFS);
			if (ret)
				break;
		}


		if (group == last_group)
			end = last_cluster;
		if (grp->bb_free >= minlen) {
			cnt = ext4_trim_all_free(sb, group, first_cluster,
						 end, minlen);
			if (cnt < 0) {
				ret = cnt;
				break;
			}
			trimmed += cnt;
		}


		first_cluster = 0;
	}

	if (!ret)
		EXT4_SB(sb)->s_last_trim_minblks = minlen;

out:
	range->len = EXT4_C2B(EXT4_SB(sb), trimmed) << sb->s_blocksize_bits;
	return ret;
}


/**
 * ext4_mballoc_query_range - Implements the mballoc query range operation within the multiblock allocator subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
int
ext4_mballoc_query_range(
	struct super_block		*sb,
	ext4_group_t			group,
	ext4_grpblk_t			first,
	ext4_grpblk_t			end,
	ext4_mballoc_query_range_fn	meta_formatter,
	ext4_mballoc_query_range_fn	formatter,
	void				*priv)
{
	void				*bitmap;
	ext4_grpblk_t			start, next;
	struct ext4_buddy		e4b;
	int				error;

	error = ext4_mb_load_buddy(sb, group, &e4b);
	if (error)
		return error;
	bitmap = e4b.bd_bitmap;

	ext4_lock_group(sb, group);

	start = max(e4b.bd_info->bb_first_free, first);
	if (end >= EXT4_CLUSTERS_PER_GROUP(sb))
		end = EXT4_CLUSTERS_PER_GROUP(sb) - 1;
	if (meta_formatter && start != first) {
		if (start > end)
			start = end;
		ext4_unlock_group(sb, group);
		error = meta_formatter(sb, group, first, start - first,
				       priv);
		if (error)
			goto out_unload;
		ext4_lock_group(sb, group);
	}
	while (start <= end) {
		start = mb_find_next_zero_bit(bitmap, end + 1, start);
		if (start > end)
			break;
		next = mb_find_next_bit(bitmap, end + 1, start);

		ext4_unlock_group(sb, group);
		error = formatter(sb, group, start, next - start, priv);
		if (error)
			goto out_unload;
		ext4_lock_group(sb, group);

		start = next + 1;
	}

	ext4_unlock_group(sb, group);
out_unload:
	ext4_mb_unload_buddy(&e4b);

	return error;
}

#ifdef CONFIG_EXT4_KUNIT_TESTS
#include "mballoc-test.c"
#endif
