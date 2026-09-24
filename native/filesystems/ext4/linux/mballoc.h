#ifndef INFILTRATR_EXT4_MBALLLOC_H
#define INFILTRATR_EXT4_MBALLLOC_H

#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/proc_fs.h>
#include <linux/quotaops.h>
#include <linux/seq_file.h>
#include <linux/swap.h>
#include <linux/time.h>

#include "ext4.h"
#include "ext4_jbd2.h"

#ifdef CONFIG_EXT4_DEBUG
#define mb_debug(sb, fmt, ...) 	pr_debug("[%s/%d] EXT4-fs (%s): %s: " fmt, 		 current->comm, task_pid_nr(current), (sb)->s_id, 		 __func__, ##__VA_ARGS__)
#else
#define mb_debug(sb, fmt, ...) no_printk(fmt, ##__VA_ARGS__)
#endif

#define EXT4_MB_HISTORY_ALLOC 1
#define EXT4_MB_HISTORY_PREALLOC 2

#define MB_DEFAULT_MAX_TO_SCAN 200
#define MB_DEFAULT_MIN_TO_SCAN 10
#define MB_DEFAULT_STATS 0
#define MB_DEFAULT_STREAM_THRESHOLD 16
#define MB_DEFAULT_ORDER2_REQS 2
#define MB_DEFAULT_GROUP_PREALLOC 512
#define MB_DEFAULT_LINEAR_LIMIT 4
#define MB_DEFAULT_LINEAR_SCAN_THRESHOLD 16
#define MB_DEFAULT_BEST_AVAIL_TRIM_ORDER 3

#define MB_NUM_ORDERS(sb) ((sb)->s_blocksize_bits + 2)

struct ext4_free_data {
	struct list_head efd_list;
	struct rb_node efd_node;
	ext4_group_t efd_group;
	ext4_grpblk_t efd_start_cluster;
	ext4_grpblk_t efd_count;
	tid_t efd_tid;
};

enum ext4_prealloc_kind {
	MB_INODE_PA = 0,
	MB_GROUP_PA = 1,
};

struct ext4_prealloc_space {
	union {
		struct rb_node inode_node;
		struct list_head lg_list;
	} pa_node;
	struct list_head pa_group_list;
	union {
		struct list_head pa_tmp_list;
		struct rcu_head pa_rcu;
	} u;
	spinlock_t pa_lock;
	atomic_t pa_count;
	unsigned int pa_deleted;
	ext4_fsblk_t pa_pstart;
	ext4_lblk_t pa_lstart;
	ext4_grpblk_t pa_len;
	ext4_grpblk_t pa_free;
	unsigned short pa_type;
	union {
		rwlock_t *inode_lock;
		spinlock_t *lg_lock;
	} pa_node_lock;
	struct inode *pa_inode;
};

struct ext4_free_extent {
	ext4_lblk_t fe_logical;
	ext4_grpblk_t fe_start;
	ext4_group_t fe_group;
	ext4_grpblk_t fe_len;
};

#define PREALLOC_TB_SIZE 10

struct ext4_locality_group {
	struct mutex lg_mutex;
	struct list_head lg_prealloc_list[PREALLOC_TB_SIZE];
	spinlock_t lg_prealloc_lock;
};

struct ext4_allocation_context {
	struct inode *ac_inode;
	struct super_block *ac_sb;
	struct ext4_free_extent ac_o_ex;
	struct ext4_free_extent ac_g_ex;
	struct ext4_free_extent ac_b_ex;
	struct ext4_free_extent ac_f_ex;
	ext4_grpblk_t ac_orig_goal_len;
	ext4_group_t ac_prefetch_grp;
	unsigned int ac_prefetch_ios;
	unsigned int ac_prefetch_nr;
	int ac_first_err;
	__u32 ac_flags;
	__u16 ac_groups_scanned;
	__u16 ac_found;
	__u16 ac_cX_found[EXT4_MB_NUM_CRS];
	__u16 ac_tail;
	__u16 ac_buddy;
	__u8 ac_status;
	__u8 ac_criteria;
	__u8 ac_2order;
	__u8 ac_op;
	struct ext4_buddy *ac_e4b;
	struct folio *ac_bitmap_folio;
	struct folio *ac_buddy_folio;
	struct ext4_prealloc_space *ac_pa;
	struct ext4_locality_group *ac_lg;
};

enum ext4_allocation_search_state {
	AC_STATUS_CONTINUE = 1,
	AC_STATUS_FOUND = 2,
	AC_STATUS_BREAK = 3,
};

struct ext4_buddy {
	struct folio *bd_buddy_folio;
	void *bd_buddy;
	struct folio *bd_bitmap_folio;
	void *bd_bitmap;
	struct ext4_group_info *bd_info;
	struct super_block *bd_sb;
	__u16 bd_blkbits;
	ext4_group_t bd_group;
};

static inline ext4_fsblk_t ext4_grp_offs_to_block(
	struct super_block *sb, const struct ext4_free_extent *extent)
{
	return ext4_group_first_block_no(sb, extent->fe_group) +
	       ((ext4_fsblk_t)extent->fe_start <<
		EXT4_SB(sb)->s_cluster_bits);
}

static inline loff_t extent_logical_end(
	const struct ext4_sb_info *sbi,
	const struct ext4_free_extent *extent)
{
	return (loff_t)extent->fe_logical +
	       EXT4_C2B(sbi, extent->fe_len);
}

static inline loff_t pa_logical_end(
	const struct ext4_sb_info *sbi,
	const struct ext4_prealloc_space *pa)
{
	return (loff_t)pa->pa_lstart +
	       EXT4_C2B(sbi, pa->pa_len);
}

typedef int (*ext4_mballoc_query_range_fn)(
	struct super_block *sb,
	ext4_group_t group,
	ext4_grpblk_t start,
	ext4_grpblk_t length,
	void *private_data);

int ext4_mballoc_query_range(
	struct super_block *sb,
	ext4_group_t group,
	ext4_grpblk_t start,
	ext4_grpblk_t end,
	ext4_mballoc_query_range_fn metadata_formatter,
	ext4_mballoc_query_range_fn free_space_formatter,
	void *private_data);

#endif
