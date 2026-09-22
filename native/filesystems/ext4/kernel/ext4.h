// SPDX-License-Identifier: GPL-2.0
/*
 *  ext4.h
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/include/linux/minix_fs.h
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

/*
 * EXT4 — EXT4 shared model
 *
 * Purpose:
 *   Defines EXT4 feature bits, on-disk structures, in-memory state and private interfaces shared across the full driver.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Feature masks and disk-layout declarations are compatibility contracts; changes require explicit reasoning about old media and read-only versus read-write support.
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

#ifndef _EXT4_H
#define _EXT4_H

#include <linux/refcount.h>
#include <linux/types.h>
#include <linux/blkdev.h>
#include <linux/magic.h>
#include <linux/jbd2.h>
#include <linux/quota.h>
#include <linux/rwsem.h>
#include <linux/rbtree.h>
#include <linux/seqlock.h>
#include <linux/mutex.h>
#include <linux/timer.h>
#include <linux/wait.h>
#include <linux/sched/signal.h>
#include <linux/blockgroup_lock.h>
#include <linux/percpu_counter.h>
#include <linux/ratelimit.h>
#include <crypto/hash.h>
#include <linux/falloc.h>
#include <linux/percpu-rwsem.h>
#include <linux/fiemap.h>
#ifdef __KERNEL__
#include <linux/compat.h>
#endif
#include <uapi/linux/ext4.h>

#include <linux/fscrypt.h>
#include <linux/fsverity.h>

#include <linux/compiler.h>


#define AGGRESSIVE_CHECK__


#define DOUBLE_CHECK__


#undef EXT4FS_DEBUG


#ifdef EXT4FS_DEBUG
#define ext4_debug(f, a...)						\
/**
 * ext4_debug - Implements the debug operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
	do {								\
		printk(KERN_DEBUG "EXT4-fs DEBUG (%s, %d): %s:",	\
			__FILE__, __LINE__, __func__);			\
		printk(KERN_DEBUG f, ## a);				\
	}/**
 * ASSERT - Implements the ASSERT operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)
#else
#define ext4_debug(fmt, ...)	no_printk(fmt, ##__VA_ARGS__)
#endif


#define EXT_DEBUG__


#ifdef CONFIG_EXT4_DEBUG
#define ext_debug(ino, fmt, ...)					\
	pr_debug("[%s/%d] EXT4-fs (%s): ino %lu: (%s, %d): %s:" fmt,	\
		 current->comm, task_pid_nr(current),			\
		 ino->i_sb->s_id, ino->i_ino, __FILE__, __LINE__,	\
		 __func__, ##__VA_ARGS__)
#else
#define ext_debug(ino, fmt, ...)	no_printk(fmt, ##__VA_ARGS__)
#endif

#define ASSERT(assert)						\
do {									\
	if (unlikely(!(assert))) {					\
		printk(KERN_EMERG					\
		       "Assertion failure in %s() at %s:%d: '%s'\n",	\
		       __func__, __FILE__, __LINE__, #assert);		\
		BUG();							\
	}								\
} while (0)


typedef int ext4_grpblk_t;


typedef unsigned long long ext4_fsblk_t;


typedef __u32 ext4_lblk_t;


typedef unsigned int ext4_group_t;

/**
 * enum SHIFT_DIRECTION - Private EXT4 state/value set used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
enum SHIFT_DIRECTION {
	SHIFT_LEFT = 0,
	SHIFT_RIGHT,
};


/**
 * enum criteria - Private EXT4 state/value set used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
enum criteria {


	CR_POWER2_ALIGNED,


	CR_GOAL_LEN_FAST,


	CR_BEST_AVAIL_LEN,


	CR_GOAL_LEN_SLOW,


	CR_ANY_FREE,


	EXT4_MB_NUM_CRS
};


#define EXT4_MB_HINT_MERGE		0x0001

#define EXT4_MB_HINT_RESERVED		0x0002

#define EXT4_MB_HINT_METADATA		0x0004

#define EXT4_MB_HINT_FIRST		0x0008

#define EXT4_MB_HINT_BEST		0x0010

#define EXT4_MB_HINT_DATA		0x0020

#define EXT4_MB_HINT_NOPREALLOC		0x0040

#define EXT4_MB_HINT_GROUP_ALLOC	0x0080

#define EXT4_MB_HINT_GOAL_ONLY		0x0100

#define EXT4_MB_HINT_TRY_GOAL		0x0200

#define EXT4_MB_DELALLOC_RESERVED	0x0400

#define EXT4_MB_STREAM_ALLOC		0x0800

#define EXT4_MB_USE_ROOT_BLOCKS		0x1000

#define EXT4_MB_USE_RESERVED		0x2000

#define EXT4_MB_STRICT_CHECK		0x4000

/**
 * struct ext4_allocation_request - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_allocation_request {

	struct inode *inode;

	unsigned int len;

	ext4_lblk_t logical;

	ext4_lblk_t lleft;

	ext4_lblk_t lright;

	ext4_fsblk_t goal;

	ext4_fsblk_t pleft;

	ext4_fsblk_t pright;

	unsigned int flags;
};


#define EXT4_MAP_NEW		BIT(BH_New)
#define EXT4_MAP_MAPPED		BIT(BH_Mapped)
#define EXT4_MAP_UNWRITTEN	BIT(BH_Unwritten)
#define EXT4_MAP_BOUNDARY	BIT(BH_Boundary)
#define EXT4_MAP_DELAYED	BIT(BH_Delay)
#define EXT4_MAP_FLAGS		(EXT4_MAP_NEW | EXT4_MAP_MAPPED |\
/**
 * EXT4_MAP_FLAGS - Implements the EXT4 MAP FLAGS operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
				 EXT4_MAP_UNWRITTEN | EXT4_MAP_BOUNDARY |\
				 EXT4_MAP_DELAYED)

struct ext4_map_blocks {
	ext4_fsblk_t m_pblk;
	ext4_lblk_t m_lblk;
	unsigned int m_len;
	unsigned int m_flags;
};


/**
 * struct ext4_system_blocks - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_system_blocks {
	struct rb_root root;
	struct rcu_head rcu;
};


#define EXT4_IO_END_UNWRITTEN	0x0001
#define EXT4_IO_END_FAILED	0x0002

/**
 * struct ext4_io_end_vec - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_io_end_vec {
	struct list_head list;
	loff_t offset;
	ssize_t size;
};


/**
 * struct ext4_io_end - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
typedef struct ext4_io_end {
	struct list_head	list;
	handle_t		*handle;

	struct inode		*inode;
	struct bio		*bio;

	unsigned int		flag;
	refcount_t		count;
	struct list_head	list_vec;
} ext4_io_end_t;

/**
 * struct ext4_io_submit - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_io_submit {
	struct writeback_control *io_wbc;
	struct bio		*io_bio;
	ext4_io_end_t		*io_end;
	sector_t		io_next_block;
};


#define	EXT4_BAD_INO		 1
#define EXT4_ROOT_INO		 2
#define EXT4_USR_QUOTA_INO	 3
#define EXT4_GRP_QUOTA_INO	 4
#define EXT4_BOOT_LOADER_INO	 5
#define EXT4_UNDEL_DIR_INO	 6
#define EXT4_RESIZE_INO		 7
#define EXT4_JOURNAL_INO	 8


#define EXT4_GOOD_OLD_FIRST_INO	11


#define EXT4_LINK_MAX		65000


#define EXT4_MIN_BLOCK_SIZE		1024
#define	EXT4_MAX_BLOCK_SIZE		65536
#define EXT4_MIN_BLOCK_LOG_SIZE		10
#define EXT4_MAX_BLOCK_LOG_SIZE		16
#define EXT4_MAX_CLUSTER_LOG_SIZE	30
#ifdef __KERNEL__
# define EXT4_BLOCK_SIZE(s)		((s)->s_blocksize)
#else
# define EXT4_BLOCK_SIZE(s)		(EXT4_MIN_BLOCK_SIZE << (s)->s_log_block_size)
#endif
#define	EXT4_ADDR_PER_BLOCK(s)		(EXT4_BLOCK_SIZE(s) / sizeof(__u32))
#define EXT4_CLUSTER_SIZE(s)		(EXT4_BLOCK_SIZE(s) << \
					 EXT4_SB(s)->s_cluster_bits)
#ifdef __KERNEL__
# define EXT4_BLOCK_SIZE_BITS(s)	((s)->s_blocksize_bits)
# define EXT4_CLUSTER_BITS(s)		(EXT4_SB(s)->s_cluster_bits)
#else
# define EXT4_BLOCK_SIZE_BITS(s)	((s)->s_log_block_size + 10)
#endif
#ifdef __KERNEL__
#define	EXT4_ADDR_PER_BLOCK_BITS(s)	(EXT4_SB(s)->s_addr_per_block_bits)
#define EXT4_INODE_SIZE(s)		(EXT4_SB(s)->s_inode_size)
#define EXT4_FIRST_INO(s)		(EXT4_SB(s)->s_first_ino)
#else
#define EXT4_INODE_SIZE(s)	(((s)->s_rev_level == EXT4_GOOD_OLD_REV) ? \
				 EXT4_GOOD_OLD_INODE_SIZE : \
				 (s)->s_inode_size)
#define EXT4_FIRST_INO(s)	(((s)->s_rev_level == EXT4_GOOD_OLD_REV) ? \
				 EXT4_GOOD_OLD_FIRST_INO : \
				 (s)->s_first_ino)
#endif
#define EXT4_BLOCK_ALIGN(size, blkbits)		ALIGN((size), (1 << (blkbits)))
#define EXT4_MAX_BLOCKS(size, offset, blkbits) \
	((EXT4_BLOCK_ALIGN(size + offset, blkbits) >> blkbits) - (offset >> \
								  blkbits))
#define EXT4_B_TO_LBLK(inode, offset) \
	(round_up((offset), i_blocksize(inode)) >> (inode)->i_blkbits)


#define EXT4_B2C(sbi, blk)	((blk) >> (sbi)->s_cluster_bits)

#define EXT4_C2B(sbi, cluster)	((cluster) << (sbi)->s_cluster_bits)

#define EXT4_NUM_B2C(sbi, blks)	(((blks) + (sbi)->s_cluster_ratio - 1) >> \
				 (sbi)->s_cluster_bits)

#define EXT4_PBLK_CMASK(s, pblk) ((pblk) &				\
				  ~((ext4_fsblk_t) (s)->s_cluster_ratio - 1))
#define EXT4_LBLK_CMASK(s, lblk) ((lblk) &				\
				  ~((ext4_lblk_t) (s)->s_cluster_ratio - 1))

#define EXT4_LBLK_CFILL(sbi, lblk) ((lblk) |				\
				    ((ext4_lblk_t) (sbi)->s_cluster_ratio - 1))

#define EXT4_PBLK_COFF(s, pblk) ((pblk) &				\
				 ((ext4_fsblk_t) (s)->s_cluster_ratio - 1))
#define EXT4_LBLK_COFF(s, lblk) ((lblk) &				\
				 ((ext4_lblk_t) (s)->s_cluster_ratio - 1))


struct ext4_group_desc
{
	__le32	bg_block_bitmap_lo;
	__le32	bg_inode_bitmap_lo;
	__le32	bg_inode_table_lo;
	__le16	bg_free_blocks_count_lo;
	__le16	bg_free_inodes_count_lo;
	__le16	bg_used_dirs_count_lo;
	__le16	bg_flags;
	__le32  bg_exclude_bitmap_lo;
	__le16  bg_block_bitmap_csum_lo;
	__le16  bg_inode_bitmap_csum_lo;
	__le16  bg_itable_unused_lo;
	__le16  bg_checksum;
	__le32	bg_block_bitmap_hi;
	__le32	bg_inode_bitmap_hi;
	__le32	bg_inode_table_hi;
	__le16	bg_free_blocks_count_hi;
	__le16	bg_free_inodes_count_hi;
	__le16	bg_used_dirs_count_hi;
	__le16  bg_itable_unused_hi;
	__le32  bg_exclude_bitmap_hi;
	__le16  bg_block_bitmap_csum_hi;
	__le16  bg_inode_bitmap_csum_hi;
	__u32   bg_reserved;
};

#define EXT4_BG_INODE_BITMAP_CSUM_HI_END	\
	(offsetof(struct ext4_group_desc, bg_inode_bitmap_csum_hi) + \
	 sizeof(__le16))
#define EXT4_BG_BLOCK_BITMAP_CSUM_HI_END	\
	(offsetof(struct ext4_group_desc, bg_block_bitmap_csum_hi) + \
	 sizeof(__le16))


struct flex_groups {
	atomic64_t	free_clusters;
	atomic_t	free_inodes;
	atomic_t	used_dirs;
};

#define EXT4_BG_INODE_UNINIT	0x0001
#define EXT4_BG_BLOCK_UNINIT	0x0002
#define EXT4_BG_INODE_ZEROED	0x0004


#define EXT4_MIN_DESC_SIZE		32
#define EXT4_MIN_DESC_SIZE_64BIT	64
#define	EXT4_MAX_DESC_SIZE		EXT4_MIN_BLOCK_SIZE
#define EXT4_DESC_SIZE(s)		(EXT4_SB(s)->s_desc_size)
#ifdef __KERNEL__
# define EXT4_BLOCKS_PER_GROUP(s)	(EXT4_SB(s)->s_blocks_per_group)
# define EXT4_CLUSTERS_PER_GROUP(s)	(EXT4_SB(s)->s_clusters_per_group)
# define EXT4_DESC_PER_BLOCK(s)		(EXT4_SB(s)->s_desc_per_block)
# define EXT4_INODES_PER_GROUP(s)	(EXT4_SB(s)->s_inodes_per_group)
# define EXT4_DESC_PER_BLOCK_BITS(s)	(EXT4_SB(s)->s_desc_per_block_bits)
#else
# define EXT4_BLOCKS_PER_GROUP(s)	((s)->s_blocks_per_group)
# define EXT4_DESC_PER_BLOCK(s)		(EXT4_BLOCK_SIZE(s) / EXT4_DESC_SIZE(s))
# define EXT4_INODES_PER_GROUP(s)	((s)->s_inodes_per_group)
#endif


#define	EXT4_NDIR_BLOCKS		12
#define	EXT4_IND_BLOCK			EXT4_NDIR_BLOCKS
#define	EXT4_DIND_BLOCK			(EXT4_IND_BLOCK + 1)
#define	EXT4_TIND_BLOCK			(EXT4_DIND_BLOCK + 1)
#define	EXT4_N_BLOCKS			(EXT4_TIND_BLOCK + 1)


#define	EXT4_SECRM_FL			0x00000001
#define	EXT4_UNRM_FL			0x00000002
#define	EXT4_COMPR_FL			0x00000004
#define EXT4_SYNC_FL			0x00000008
#define EXT4_IMMUTABLE_FL		0x00000010
#define EXT4_APPEND_FL			0x00000020
#define EXT4_NODUMP_FL			0x00000040
#define EXT4_NOATIME_FL			0x00000080

#define EXT4_DIRTY_FL			0x00000100
#define EXT4_COMPRBLK_FL		0x00000200
#define EXT4_NOCOMPR_FL			0x00000400

#define EXT4_ENCRYPT_FL			0x00000800

#define EXT4_INDEX_FL			0x00001000
#define EXT4_IMAGIC_FL			0x00002000
#define EXT4_JOURNAL_DATA_FL		0x00004000
#define EXT4_NOTAIL_FL			0x00008000
#define EXT4_DIRSYNC_FL			0x00010000
#define EXT4_TOPDIR_FL			0x00020000
#define EXT4_HUGE_FILE_FL               0x00040000
#define EXT4_EXTENTS_FL			0x00080000
#define EXT4_VERITY_FL			0x00100000
#define EXT4_EA_INODE_FL	        0x00200000


#define EXT4_DAX_FL			0x02000000

#define EXT4_INLINE_DATA_FL		0x10000000
#define EXT4_PROJINHERIT_FL		0x20000000
#define EXT4_CASEFOLD_FL		0x40000000
#define EXT4_RESERVED_FL		0x80000000


#define EXT4_FL_USER_MODIFIABLE		(EXT4_SECRM_FL | \
/**
 * ext4_mask_flags - Implements the mask flags operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
					 EXT4_UNRM_FL | \
					 EXT4_COMPR_FL | \
					 EXT4_SYNC_FL | \
					 EXT4_IMMUTABLE_FL | \
					 EXT4_APPEND_FL | \
					 EXT4_NODUMP_FL | \
					 EXT4_NOATIME_FL | \
					 EXT4_JOURNAL_DATA_FL | \
					 EXT4_NOTAIL_FL | \
					 EXT4_DIRSYNC_FL | \
					 EXT4_TOPDIR_FL | \
					 EXT4_EXTENTS_FL | \
					 0x00400000                         | \
					 EXT4_DAX_FL | \
					 EXT4_PROJINHERIT_FL | \
					 EXT4_CASEFOLD_FL)


#define EXT4_FL_USER_VISIBLE		(EXT4_FL_USER_MODIFIABLE | \
					 EXT4_DIRTY_FL | \
					 EXT4_COMPRBLK_FL | \
					 EXT4_NOCOMPR_FL | \
					 EXT4_ENCRYPT_FL | \
					 EXT4_INDEX_FL | \
					 EXT4_VERITY_FL | \
					 EXT4_INLINE_DATA_FL)


#define EXT4_FL_INHERITED (EXT4_SECRM_FL | EXT4_UNRM_FL | EXT4_COMPR_FL |\
			   EXT4_SYNC_FL | EXT4_NODUMP_FL | EXT4_NOATIME_FL |\
			   EXT4_NOCOMPR_FL | EXT4_JOURNAL_DATA_FL |\
			   EXT4_NOTAIL_FL | EXT4_DIRSYNC_FL |\
			   EXT4_PROJINHERIT_FL | EXT4_CASEFOLD_FL |\
			   EXT4_DAX_FL)


#define EXT4_REG_FLMASK (~(EXT4_DIRSYNC_FL | EXT4_TOPDIR_FL | EXT4_CASEFOLD_FL |\
			   EXT4_PROJINHERIT_FL))


#define EXT4_OTHER_FLMASK (EXT4_NODUMP_FL | EXT4_NOATIME_FL)


#define EXT4_FL_SHOULD_SWAP (EXT4_HUGE_FILE_FL | EXT4_EXTENTS_FL)


#define EXT4_DAX_MUT_EXCL (EXT4_VERITY_FL | EXT4_ENCRYPT_FL |\
			   EXT4_JOURNAL_DATA_FL | EXT4_INLINE_DATA_FL)


static inline __u32 ext4_mask_flags(umode_t mode, __u32 flags)
{
	if (S_ISDIR(mode))
		return flags;
	else if (S_ISREG(mode))
		return flags & EXT4_REG_FLMASK;
	else
		return flags & EXT4_OTHER_FLMASK;
}


enum {
	EXT4_INODE_SECRM	= 0,
	EXT4_INODE_UNRM		= 1,
	EXT4_INODE_COMPR	= 2,
	EXT4_INODE_SYNC		= 3,
	EXT4_INODE_IMMUTABLE	= 4,
	EXT4_INODE_APPEND	= 5,
	EXT4_INODE_NODUMP	= 6,
	EXT4_INODE_NOATIME	= 7,

	EXT4_INODE_DIRTY	= 8,
	EXT4_INODE_COMPRBLK	= 9,
	EXT4_INODE_NOCOMPR	= 10,
	EXT4_INODE_ENCRYPT	= 11,

	EXT4_INODE_INDEX	= 12,
	EXT4_INODE_IMAGIC	= 13,
	EXT4_INODE_JOURNAL_DATA	= 14,
	EXT4_INODE_NOTAIL	= 15,
	EXT4_INODE_DIRSYNC	= 16,
	EXT4_INODE_TOPDIR	= 17,
	EXT4_INODE_HUGE_FILE	= 18,
	EXT4_INODE_EXTENTS	= 19,
	EXT4_INODE_VERITY	= 20,
	EXT4_INODE_EA_INODE	= 21,

	EXT4_INODE_DAX		= 25,
	EXT4_INODE_INLINE_DATA	= 28,
	EXT4_INODE_PROJINHERIT	= 29,
	EXT4_INODE_CASEFOLD	= 30,
	EXT4_INODE_RESERVED	= 31,
};


#define TEST_FLAG_VALUE(FLAG) (EXT4_##FLAG##_FL == (1U << EXT4_INODE_##FLAG))
#define CHECK_FLAG_VALUE(FLAG) BUILD_BUG_ON(!TEST_FLAG_VALUE(FLAG))

static inline void ext4_check_flag_values(void)
{
	CHECK_FLAG_VALUE(SECRM);
	CHECK_FLAG_VALUE(UNRM);
	CHECK_FLAG_VALUE(COMPR);
	CHECK_FLAG_VALUE(SYNC);
	CHECK_FLAG_VALUE(IMMUTABLE);
	CHECK_FLAG_VALUE(APPEND);
	CHECK_FLAG_VALUE(NODUMP);
	CHECK_FLAG_VALUE(NOATIME);
	CHECK_FLAG_VALUE(DIRTY);
	CHECK_FLAG_VALUE(COMPRBLK);
	CHECK_FLAG_VALUE(NOCOMPR);
	CHECK_FLAG_VALUE(ENCRYPT);
	CHECK_FLAG_VALUE(INDEX);
	CHECK_FLAG_VALUE(IMAGIC);
	CHECK_FLAG_VALUE(JOURNAL_DATA);
	CHECK_FLAG_VALUE(NOTAIL);
	CHECK_FLAG_VALUE(DIRSYNC);
	CHECK_FLAG_VALUE(TOPDIR);
	CHECK_FLAG_VALUE(HUGE_FILE);
	CHECK_FLAG_VALUE(EXTENTS);
	CHECK_FLAG_VALUE(VERITY);
	CHECK_FLAG_VALUE(EA_INODE);
	CHECK_FLAG_VALUE(INLINE_DATA);
	CHECK_FLAG_VALUE(PROJINHERIT);
	CHECK_FLAG_VALUE(CASEFOLD);
	CHECK_FLAG_VALUE(RESERVED);
}

#if defined(__KERNEL__) && defined(CONFIG_COMPAT)
struct compat_ext4_new_group_input {
	u32 group;
	compat_u64 block_bitmap;
	compat_u64 inode_bitmap;
	compat_u64 inode_table;
	u32 blocks_count;
	u16 reserved_blocks;
	u16 unused;
};
#endif


/**
 * struct ext4_new_group_data - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_new_group_data {
	__u32 group;
	__u64 block_bitmap;
	__u64 inode_bitmap;
	__u64 inode_table;
	__u32 blocks_count;
	__u16 reserved_blocks;
	__u16 mdata_blocks;
	__u32 free_clusters_count;
};


enum {
	BLOCK_BITMAP = 0,
	INODE_BITMAP,
	INODE_TABLE,
	GROUP_TABLE_COUNT,
};


#define EXT4_GET_BLOCKS_CREATE			0x0001

#define EXT4_GET_BLOCKS_UNWRIT_EXT		0x0002
#define EXT4_GET_BLOCKS_CREATE_UNWRIT_EXT	(EXT4_GET_BLOCKS_UNWRIT_EXT|\
/**
 * _IOW - Implements the IOW operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
						 EXT4_GET_BLOCKS_CREATE)


#define EXT4_GET_BLOCKS_DELALLOC_RESERVE	0x0004


#define EXT4_GET_BLOCKS_PRE_IO			0x0008
#define EXT4_GET_BLOCKS_CONVERT			0x0010
#define EXT4_GET_BLOCKS_IO_CREATE_EXT		(EXT4_GET_BLOCKS_PRE_IO|\
					 EXT4_GET_BLOCKS_CREATE_UNWRIT_EXT)

#define EXT4_GET_BLOCKS_IO_CONVERT_EXT		(EXT4_GET_BLOCKS_CONVERT|\
					 EXT4_GET_BLOCKS_CREATE_UNWRIT_EXT)


#define EXT4_GET_BLOCKS_METADATA_NOFAIL		0x0020

#define EXT4_GET_BLOCKS_NO_NORMALIZE		0x0040

#define EXT4_GET_BLOCKS_CONVERT_UNWRITTEN	0x0100

#define EXT4_GET_BLOCKS_ZERO			0x0200
#define EXT4_GET_BLOCKS_CREATE_ZERO		(EXT4_GET_BLOCKS_CREATE |\
					EXT4_GET_BLOCKS_ZERO)


#define EXT4_GET_BLOCKS_IO_SUBMIT		0x0400

#define EXT4_GET_BLOCKS_CACHED_NOWAIT		0x0800


#define EXT4_EX_NOCACHE				0x40000000
#define EXT4_EX_FORCE_CACHE			0x20000000
#define EXT4_EX_NOFAIL				0x10000000


#define EXT4_FREE_BLOCKS_METADATA		0x0001
#define EXT4_FREE_BLOCKS_FORGET			0x0002
#define EXT4_FREE_BLOCKS_VALIDATED		0x0004
#define EXT4_FREE_BLOCKS_NO_QUOT_UPDATE		0x0008
#define EXT4_FREE_BLOCKS_NOFREE_FIRST_CLUSTER	0x0010
#define EXT4_FREE_BLOCKS_NOFREE_LAST_CLUSTER	0x0020
#define EXT4_FREE_BLOCKS_RERESERVE_CLUSTER      0x0040

#if defined(__KERNEL__) && defined(CONFIG_COMPAT)


#define EXT4_IOC32_GETVERSION		_IOR('f', 3, int)
#define EXT4_IOC32_SETVERSION		_IOW('f', 4, int)
#define EXT4_IOC32_GETRSVSZ		_IOR('f', 5, int)
#define EXT4_IOC32_SETRSVSZ		_IOW('f', 6, int)
#define EXT4_IOC32_GROUP_EXTEND		_IOW('f', 7, unsigned int)
#define EXT4_IOC32_GROUP_ADD		_IOW('f', 8, struct compat_ext4_new_group_input)
#define EXT4_IOC32_GETVERSION_OLD	FS_IOC32_GETVERSION
#define EXT4_IOC32_SETVERSION_OLD	FS_IOC32_SETVERSION
#endif


#define EXT4_MAX_BLOCK_FILE_PHYS	0xFFFFFFFF


#define EXT4_MAX_LOGICAL_BLOCK		0xFFFFFFFE


struct ext4_inode {
	__le16	i_mode;
	__le16	i_uid;
	__le32	i_size_lo;
	__le32	i_atime;
	__le32	i_ctime;
	__le32	i_mtime;
	__le32	i_dtime;
	__le16	i_gid;
	__le16	i_links_count;
	__le32	i_blocks_lo;
	__le32	i_flags;
	union {
		struct {
			__le32  l_i_version;
		} linux1;
		struct {
			__u32  h_i_translator;
		} hurd1;
		struct {
			__u32  m_i_reserved1;
		} masix1;
	} osd1;
	__le32	i_block[EXT4_N_BLOCKS];
	__le32	i_generation;
	__le32	i_file_acl_lo;
	__le32	i_size_high;
	__le32	i_obso_faddr;
	union {
		struct {
			__le16	l_i_blocks_high;
			__le16	l_i_file_acl_high;
			__le16	l_i_uid_high;
			__le16	l_i_gid_high;
			__le16	l_i_checksum_lo;
			__le16	l_i_reserved;
		} linux2;
		struct {
			__le16	h_i_reserved1;
			__u16	h_i_mode_high;
			__u16	h_i_uid_high;
			__u16	h_i_gid_high;
			__u32	h_i_author;
		} hurd2;
		struct {
			__le16	h_i_reserved1;
			__le16	m_i_file_acl_high;
			__u32	m_i_reserved2[2];
		} masix2;
	} osd2;
	__le16	i_extra_isize;
	__le16	i_checksum_hi;
	__le32  i_ctime_extra;
	__le32  i_mtime_extra;
	__le32  i_atime_extra;
	__le32  i_crtime;
	__le32  i_crtime_extra;
	__le32  i_version_hi;
	__le32	i_projid;
};

#define EXT4_EPOCH_BITS 2
#define EXT4_EPOCH_MASK ((1 << EXT4_EPOCH_BITS) - 1)
#define EXT4_NSEC_MASK  (~0UL << EXT4_EPOCH_BITS)


#define EXT4_FITS_IN_INODE(ext4_inode, einode, field)	\
	((offsetof(typeof(*ext4_inode), field) +	\
	  sizeof((ext4_inode)->field))			\
	<= (EXT4_GOOD_OLD_INODE_SIZE +			\
	    (einode)->i_extra_isize))			\


static inline __le32 ext4_encode_extra_time(struct timespec64 ts)
{
	u32 extra = ((ts.tv_sec - (s32)ts.tv_sec) >> 32) & EXT4_EPOCH_MASK;
	return cpu_to_le32(extra | (ts.tv_nsec << EXT4_EPOCH_BITS));
}

/**
 * ext4_decode_extra_time - Implements the decode extra time operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline struct timespec64 ext4_decode_extra_time(__le32 base,
						       __le32 extra)
{
	struct timespec64 ts = { .tv_sec = (signed)le32_to_cpu(base) };

	if (unlikely(extra & cpu_to_le32(EXT4_EPOCH_MASK)))
		ts.tv_sec += (u64)(le32_to_cpu(extra) & EXT4_EPOCH_MASK) << 32;
	ts.tv_nsec = (le32_to_cpu(extra) & EXT4_NSEC_MASK) >> EXT4_EPOCH_BITS;
	return ts;
}

#define EXT4_INODE_SET_XTIME_VAL(xtime, inode, raw_inode, ts)			\
/**
 * EXT4_INODE_SET_XTIME_VAL - Implements the EXT4 INODE SET XTIME VAL operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
do {										\
	if (EXT4_FITS_IN_INODE(raw_inode, EXT4_I(inode), xtime ## _extra)) {	\
		(raw_inode)->xtime = cpu_to_le32((ts).tv_sec);			\
		(raw_inode)->xtime ## _extra = ext4_encode_extra_time(ts);	\
	} else									\
		(raw_inode)->xtime = cpu_to_le32(clamp_t(int32_t, (ts).tv_sec, S32_MIN, S32_MAX));	\
} while (0)

#define EXT4_INODE_SET_ATIME(inode, raw_inode)						\
	EXT4_INODE_SET_XTIME_VAL(i_atime, inode, raw_inode, inode_get_atime(inode))

#define EXT4_INODE_SET_MTIME(inode, raw_inode)						\
	EXT4_INODE_SET_XTIME_VAL(i_mtime, inode, raw_inode, inode_get_mtime(inode))

#define EXT4_INODE_SET_CTIME(inode, raw_inode)						\
	EXT4_INODE_SET_XTIME_VAL(i_ctime, inode, raw_inode, inode_get_ctime(inode))

#define EXT4_EINODE_SET_XTIME(xtime, einode, raw_inode)				\
	if (EXT4_FITS_IN_INODE(raw_inode, einode, xtime))			\
		EXT4_INODE_SET_XTIME_VAL(xtime, &((einode)->vfs_inode),		\
					 raw_inode, (einode)->xtime)

#define EXT4_INODE_GET_XTIME_VAL(xtime, inode, raw_inode)			\
	(EXT4_FITS_IN_INODE(raw_inode, EXT4_I(inode), xtime ## _extra) ?	\
		ext4_decode_extra_time((raw_inode)->xtime,				\
				       (raw_inode)->xtime ## _extra) :		\
		(struct timespec64) {						\
			.tv_sec = (signed)le32_to_cpu((raw_inode)->xtime)	\
		}/**
 * EXT4_INODE_GET_ATIME - Implements the EXT4 INODE GET ATIME operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
)

#define EXT4_INODE_GET_ATIME(inode, raw_inode)					\
do {										\
	inode_set_atime_to_ts(inode,						\
		EXT4_INODE_GET_XTIME_VAL(i_atime, inode, raw_inode));		\
}/**
 * EXT4_INODE_GET_MTIME - Implements the EXT4 INODE GET MTIME operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)

#define EXT4_INODE_GET_MTIME(inode, raw_inode)					\
do {										\
	inode_set_mtime_to_ts(inode,						\
		EXT4_INODE_GET_XTIME_VAL(i_mtime, inode, raw_inode));		\
}/**
 * EXT4_INODE_GET_CTIME - Implements the EXT4 INODE GET CTIME operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)

#define EXT4_INODE_GET_CTIME(inode, raw_inode)					\
do {										\
	inode_set_ctime_to_ts(inode,						\
		EXT4_INODE_GET_XTIME_VAL(i_ctime, inode, raw_inode));		\
}/**
 * EXT4_EINODE_GET_XTIME - Implements the EXT4 EINODE GET XTIME operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)

#define EXT4_EINODE_GET_XTIME(xtime, einode, raw_inode)				\
do {										\
	if (EXT4_FITS_IN_INODE(raw_inode, einode, xtime)) 			\
		(einode)->xtime =						\
			EXT4_INODE_GET_XTIME_VAL(xtime, &(einode->vfs_inode),	\
						 raw_inode);			\
	else									\
		(einode)->xtime = (struct timespec64){0, 0};			\
} while (0)

#define i_disk_version osd1.linux1.l_i_version

#if defined(__KERNEL__) || defined(__linux__)
#define i_reserved1	osd1.linux1.l_i_reserved1
#define i_file_acl_high	osd2.linux2.l_i_file_acl_high
#define i_blocks_high	osd2.linux2.l_i_blocks_high
#define i_uid_low	i_uid
#define i_gid_low	i_gid
#define i_uid_high	osd2.linux2.l_i_uid_high
#define i_gid_high	osd2.linux2.l_i_gid_high
#define i_checksum_lo	osd2.linux2.l_i_checksum_lo

#elif defined(__GNU__)

#define i_translator	osd1.hurd1.h_i_translator
#define i_uid_high	osd2.hurd2.h_i_uid_high
#define i_gid_high	osd2.hurd2.h_i_gid_high
#define i_author	osd2.hurd2.h_i_author

#elif defined(__masix__)

#define i_reserved1	osd1.masix1.m_i_reserved1
#define i_file_acl_high	osd2.masix2.m_i_file_acl_high
#define i_reserved2	osd2.masix2.m_i_reserved2

#endif

#include "extents_status.h"
#include "fast_commit.h"


enum {
	I_DATA_SEM_NORMAL = 0,
	I_DATA_SEM_OTHER,
	I_DATA_SEM_QUOTA,
	I_DATA_SEM_EA
};


/**
 * struct ext4_inode_info - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_inode_info {
	__le32	i_data[15];
	__u32	i_dtime;
	ext4_fsblk_t	i_file_acl;


	ext4_group_t	i_block_group;
	ext4_lblk_t	i_dir_start_lookup;
#if (BITS_PER_LONG < 64)
	unsigned long	i_state_flags;
#endif
	unsigned long	i_flags;


	struct rw_semaphore xattr_sem;


	union {
		struct list_head i_orphan;
		unsigned int i_orphan_idx;
	};


	struct list_head i_fc_dilist;
	struct list_head i_fc_list;


	ext4_lblk_t i_fc_lblk_start;


	ext4_lblk_t i_fc_lblk_len;


	atomic_t  i_fc_updates;
	atomic_t i_unwritten;


	wait_queue_head_t i_fc_wait;


	struct mutex i_fc_lock;


	loff_t	i_disksize;


	struct rw_semaphore i_data_sem;
	struct inode vfs_inode;
	struct jbd2_inode *jinode;

	spinlock_t i_raw_lock;


	struct timespec64 i_crtime;


	atomic_t i_prealloc_active;


	unsigned int i_reserved_data_blocks;
	struct rb_root i_prealloc_node;
	rwlock_t i_prealloc_lock;


	struct ext4_es_tree i_es_tree;
	rwlock_t i_es_lock;
	struct list_head i_es_list;
	unsigned int i_es_all_nr;
	unsigned int i_es_shk_nr;
	ext4_lblk_t i_es_shrink_lblk;


	ext4_group_t	i_last_alloc_group;


	struct ext4_pending_tree i_pending_tree;


	__u16 i_extra_isize;


	u16 i_inline_off;
	u16 i_inline_size;

#ifdef CONFIG_QUOTA

	qsize_t i_reserved_quota;
#endif


	spinlock_t i_completed_io_lock;


	struct list_head i_rsv_conversion_list;
	struct work_struct i_rsv_conversion_work;

	spinlock_t i_block_reservation_lock;


	tid_t i_sync_tid;
	tid_t i_datasync_tid;

#ifdef CONFIG_QUOTA
	struct dquot __rcu *i_dquot[MAXQUOTAS];
#endif


	__u32 i_csum_seed;

	kprojid_t i_projid;
};


#define	EXT4_VALID_FS			0x0001
#define	EXT4_ERROR_FS			0x0002
#define	EXT4_ORPHAN_FS			0x0004
#define EXT4_FC_REPLAY			0x0020


#define EXT4_FLAGS_SIGNED_HASH		0x0001
#define EXT4_FLAGS_UNSIGNED_HASH	0x0002
#define EXT4_FLAGS_TEST_FILESYS		0x0004


#define EXT4_MOUNT_NO_MBCACHE		0x00001
#define EXT4_MOUNT_GRPID		0x00004
#define EXT4_MOUNT_DEBUG		0x00008
#define EXT4_MOUNT_ERRORS_CONT		0x00010
#define EXT4_MOUNT_ERRORS_RO		0x00020
#define EXT4_MOUNT_ERRORS_PANIC		0x00040
#define EXT4_MOUNT_ERRORS_MASK		0x00070
#define EXT4_MOUNT_MINIX_DF		0x00080
#define EXT4_MOUNT_NOLOAD		0x00100
#ifdef CONFIG_FS_DAX
#define EXT4_MOUNT_DAX_ALWAYS		0x00200
#else
#define EXT4_MOUNT_DAX_ALWAYS		0
#endif
#define EXT4_MOUNT_DATA_FLAGS		0x00C00
#define EXT4_MOUNT_JOURNAL_DATA		0x00400
#define EXT4_MOUNT_ORDERED_DATA		0x00800
#define EXT4_MOUNT_WRITEBACK_DATA	0x00C00
#define EXT4_MOUNT_UPDATE_JOURNAL	0x01000
#define EXT4_MOUNT_NO_UID32		0x02000
#define EXT4_MOUNT_XATTR_USER		0x04000
#define EXT4_MOUNT_POSIX_ACL		0x08000
#define EXT4_MOUNT_NO_AUTO_DA_ALLOC	0x10000
#define EXT4_MOUNT_BARRIER		0x20000
#define EXT4_MOUNT_QUOTA		0x40000
#define EXT4_MOUNT_USRQUOTA		0x80000


#define EXT4_MOUNT_GRPQUOTA		0x100000


#define EXT4_MOUNT_PRJQUOTA		0x200000

#define EXT4_MOUNT_DIOREAD_NOLOCK	0x400000
#define EXT4_MOUNT_JOURNAL_CHECKSUM	0x800000
#define EXT4_MOUNT_JOURNAL_ASYNC_COMMIT	0x1000000
#define EXT4_MOUNT_WARN_ON_ERROR	0x2000000
#define EXT4_MOUNT_NO_PREFETCH_BLOCK_BITMAPS 0x4000000
#define EXT4_MOUNT_DELALLOC		0x8000000
#define EXT4_MOUNT_DATA_ERR_ABORT	0x10000000
#define EXT4_MOUNT_BLOCK_VALIDITY	0x20000000
#define EXT4_MOUNT_DISCARD		0x40000000
#define EXT4_MOUNT_INIT_INODE_TABLE	0x80000000


#define EXT4_MOUNT2_EXPLICIT_DELALLOC	0x00000001

#define EXT4_MOUNT2_STD_GROUP_SIZE	0x00000002


#define EXT4_MOUNT2_HURD_COMPAT		0x00000004

#define EXT4_MOUNT2_EXPLICIT_JOURNAL_CHECKSUM	0x00000008


#define EXT4_MOUNT2_JOURNAL_FAST_COMMIT	0x00000010
#define EXT4_MOUNT2_DAX_NEVER		0x00000020
#define EXT4_MOUNT2_DAX_INODE		0x00000040
#define EXT4_MOUNT2_MB_OPTIMIZE_SCAN	0x00000080


#define EXT4_MOUNT2_ABORT		0x00000100

#define clear_opt(sb, opt)		EXT4_SB(sb)->s_mount_opt &= \
						~EXT4_MOUNT_##opt
#define set_opt(sb, opt)		EXT4_SB(sb)->s_mount_opt |= \
						EXT4_MOUNT_##opt
#define test_opt(sb, opt)		(EXT4_SB(sb)->s_mount_opt & \
					 EXT4_MOUNT_##opt)

#define clear_opt2(sb, opt)		EXT4_SB(sb)->s_mount_opt2 &= \
						~EXT4_MOUNT2_##opt
#define set_opt2(sb, opt)		EXT4_SB(sb)->s_mount_opt2 |= \
						EXT4_MOUNT2_##opt
#define test_opt2(sb, opt)		(EXT4_SB(sb)->s_mount_opt2 & \
					 EXT4_MOUNT2_##opt)

#define ext4_test_and_set_bit		__test_and_set_bit_le
#define ext4_set_bit			__set_bit_le
#define ext4_test_and_clear_bit		__test_and_clear_bit_le
#define ext4_clear_bit			__clear_bit_le
#define ext4_test_bit			test_bit_le
#define ext4_find_next_zero_bit		find_next_zero_bit_le
#define ext4_find_next_bit		find_next_bit_le

extern void mb_set_bits(void *bm, int cur, int len);


#define EXT4_DFL_MAX_MNT_COUNT		20
#define EXT4_DFL_CHECKINTERVAL		0


#define EXT4_ERRORS_CONTINUE		1
#define EXT4_ERRORS_RO			2
#define EXT4_ERRORS_PANIC		3
#define EXT4_ERRORS_DEFAULT		EXT4_ERRORS_CONTINUE


#define EXT4_CRC32C_CHKSUM		1

#define EXT4_LABEL_MAX			16


/**
 * struct ext4_super_block - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_super_block {
      	__le32	s_inodes_count;
	__le32	s_blocks_count_lo;
	__le32	s_r_blocks_count_lo;
	__le32	s_free_blocks_count_lo;
      	__le32	s_free_inodes_count;
	__le32	s_first_data_block;
	__le32	s_log_block_size;
	__le32	s_log_cluster_size;
      	__le32	s_blocks_per_group;
	__le32	s_clusters_per_group;
	__le32	s_inodes_per_group;
	__le32	s_mtime;
      	__le32	s_wtime;
	__le16	s_mnt_count;
	__le16	s_max_mnt_count;
	__le16	s_magic;
	__le16	s_state;
	__le16	s_errors;
	__le16	s_minor_rev_level;
      	__le32	s_lastcheck;
	__le32	s_checkinterval;
	__le32	s_creator_os;
	__le32	s_rev_level;
      	__le16	s_def_resuid;
	__le16	s_def_resgid;


	__le32	s_first_ino;
	__le16  s_inode_size;
	__le16	s_block_group_nr;
	__le32	s_feature_compat;
      	__le32	s_feature_incompat;
	__le32	s_feature_ro_compat;
      	__u8	s_uuid[16];
      	char	s_volume_name[EXT4_LABEL_MAX] __nonstring;
      	char	s_last_mounted[64] __nonstring;
      	__le32	s_algorithm_usage_bitmap;


	__u8	s_prealloc_blocks;
	__u8	s_prealloc_dir_blocks;
	__le16	s_reserved_gdt_blocks;


      	__u8	s_journal_uuid[16];
      	__le32	s_journal_inum;
	__le32	s_journal_dev;
	__le32	s_last_orphan;
	__le32	s_hash_seed[4];
	__u8	s_def_hash_version;
	__u8	s_jnl_backup_type;
	__le16  s_desc_size;
       	__le32	s_default_mount_opts;
	__le32	s_first_meta_bg;
	__le32	s_mkfs_time;
	__le32	s_jnl_blocks[17];

       	__le32	s_blocks_count_hi;
	__le32	s_r_blocks_count_hi;
	__le32	s_free_blocks_count_hi;
	__le16	s_min_extra_isize;
	__le16	s_want_extra_isize;
	__le32	s_flags;
	__le16  s_raid_stride;
	__le16  s_mmp_update_interval;
	__le64  s_mmp_block;
	__le32  s_raid_stripe_width;
	__u8	s_log_groups_per_flex;
	__u8	s_checksum_type;
	__u8	s_encryption_level;
	__u8	s_reserved_pad;
	__le64	s_kbytes_written;
	__le32	s_snapshot_inum;
	__le32	s_snapshot_id;
	__le64	s_snapshot_r_blocks_count;

	__le32	s_snapshot_list;

#define EXT4_S_ERR_START offsetof(struct ext4_super_block, s_error_count)
	__le32	s_error_count;
	__le32	s_first_error_time;
	__le32	s_first_error_ino;
	__le64	s_first_error_block;
	__u8	s_first_error_func[32] __nonstring;
	__le32	s_first_error_line;
	__le32	s_last_error_time;
	__le32	s_last_error_ino;
	__le32	s_last_error_line;
	__le64	s_last_error_block;
	__u8	s_last_error_func[32] __nonstring;
#define EXT4_S_ERR_END offsetof(struct ext4_super_block, s_mount_opts)
	__u8	s_mount_opts[64];
	__le32	s_usr_quota_inum;
	__le32	s_grp_quota_inum;
	__le32	s_overhead_clusters;
	__le32	s_backup_bgs[2];
	__u8	s_encrypt_algos[4];
	__u8	s_encrypt_pw_salt[16];
	__le32	s_lpf_ino;
	__le32	s_prj_quota_inum;
	__le32	s_checksum_seed;
	__u8	s_wtime_hi;
	__u8	s_mtime_hi;
	__u8	s_mkfs_time_hi;
	__u8	s_lastcheck_hi;
	__u8	s_first_error_time_hi;
	__u8	s_last_error_time_hi;
	__u8	s_first_error_errcode;
	__u8    s_last_error_errcode;
	__le16  s_encoding;
	__le16  s_encoding_flags;
	__le32  s_orphan_file_inum;
	__le32	s_reserved[94];
	__le32	s_checksum;
};

#define EXT4_S_ERR_LEN (EXT4_S_ERR_END - EXT4_S_ERR_START)

#ifdef __KERNEL__


#define EXT4_MAXQUOTAS 3

#define EXT4_ENC_UTF8_12_1	1


/**
 * EXT4_S_ERR_LEN - Implements the EXT4 S ERR LEN operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
enum ext4_journal_trigger_type {
	EXT4_JTR_ORPHAN_FILE,
	EXT4_JTR_NONE
};

#define EXT4_JOURNAL_TRIGGER_COUNT EXT4_JTR_NONE

/**
 * struct ext4_journal_trigger - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_journal_trigger {
	struct jbd2_buffer_trigger_type tr_triggers;
	struct super_block *sb;
};

/**
 * EXT4_TRIGGER - Implements the EXT4 TRIGGER operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline struct ext4_journal_trigger *EXT4_TRIGGER(
				struct jbd2_buffer_trigger_type *trigger)
{
	return container_of(trigger, struct ext4_journal_trigger, tr_triggers);
}

#define EXT4_ORPHAN_BLOCK_MAGIC 0x0b10ca04


/**
 * struct ext4_orphan_block_tail - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_orphan_block_tail {
	__le32 ob_magic;
	__le32 ob_checksum;
};

/**
 * ext4_inodes_per_orphan_block - Implements the inodes per orphan block operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_inodes_per_orphan_block(struct super_block *sb)
{
	return (sb->s_blocksize - sizeof(struct ext4_orphan_block_tail)) /
			sizeof(u32);
}

/**
 * struct ext4_orphan_block - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_orphan_block {
	atomic_t ob_free_entries;
	struct buffer_head *ob_bh;
};


/**
 * struct ext4_orphan_info - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_orphan_info {
	int of_blocks;
	__u32 of_csum_seed;
	struct ext4_orphan_block *of_binfo;

};


/**
 * struct ext4_sb_info - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_sb_info {
	unsigned long s_desc_size;
	unsigned long s_inodes_per_block;
	unsigned long s_blocks_per_group;
	unsigned long s_clusters_per_group;
	unsigned long s_inodes_per_group;
	unsigned long s_itb_per_group;
	unsigned long s_gdb_count;
	unsigned long s_desc_per_block;
	ext4_group_t s_groups_count;
	ext4_group_t s_blockfile_groups;
	unsigned long s_overhead;
	unsigned int s_cluster_ratio;
	unsigned int s_cluster_bits;
	loff_t s_bitmap_maxbytes;
	struct buffer_head * s_sbh;
	struct ext4_super_block *s_es;

	struct buffer_head * __rcu *s_group_desc;
	unsigned int s_mount_opt;
	unsigned int s_mount_opt2;
	unsigned long s_mount_flags;
	unsigned int s_def_mount_opt;
	unsigned int s_def_mount_opt2;
	ext4_fsblk_t s_sb_block;
	atomic64_t s_resv_clusters;
	kuid_t s_resuid;
	kgid_t s_resgid;
	unsigned short s_mount_state;
	unsigned short s_pad;
	int s_addr_per_block_bits;
	int s_desc_per_block_bits;
	int s_inode_size;
	int s_first_ino;
	unsigned int s_inode_readahead_blks;
	unsigned int s_inode_goal;
	u32 s_hash_seed[4];
	int s_def_hash_version;
	int s_hash_unsigned;
	struct percpu_counter s_freeclusters_counter;
	struct percpu_counter s_freeinodes_counter;
	struct percpu_counter s_dirs_counter;
	struct percpu_counter s_dirtyclusters_counter;
	struct percpu_counter s_sra_exceeded_retry_limit;
	struct blockgroup_lock *s_blockgroup_lock;
	struct proc_dir_entry *s_proc;
	struct kobject s_kobj;
	struct completion s_kobj_unregister;
	struct mutex s_error_notify_mutex;
	struct super_block *s_sb;
	struct buffer_head *s_mmp_bh;


	struct journal_s *s_journal;
	unsigned long s_ext4_flags;
	struct mutex s_orphan_lock;
	struct list_head s_orphan;

	struct ext4_orphan_info s_orphan_info;
	unsigned long s_commit_interval;
	u32 s_max_batch_time;
	u32 s_min_batch_time;
	struct file *s_journal_bdev_file;
#ifdef CONFIG_QUOTA

	char __rcu *s_qf_names[EXT4_MAXQUOTAS];
	int s_jquota_fmt;
#endif
	unsigned int s_want_extra_isize;
	struct ext4_system_blocks __rcu *s_system_blks;

#ifdef EXTENTS_STATS

	unsigned long s_ext_min;
	unsigned long s_ext_max;
	unsigned long s_depth_max;
	spinlock_t s_ext_stats_lock;
	unsigned long s_ext_blocks;
	unsigned long s_ext_extents;
#endif


	struct ext4_group_info ** __rcu *s_group_info;
	struct inode *s_buddy_cache;
	spinlock_t s_md_lock;
	unsigned short *s_mb_offsets;
	unsigned int *s_mb_maxs;
	unsigned int s_group_info_size;
	unsigned int s_mb_free_pending;
	struct list_head s_freed_data_list[2];

	struct list_head s_discard_list;
	struct work_struct s_discard_work;
	atomic_t s_retry_alloc_pending;
	struct xarray *s_mb_avg_fragment_size;
	struct xarray *s_mb_largest_free_orders;


	unsigned long s_stripe;
	unsigned int s_mb_max_linear_groups;
	unsigned int s_mb_stream_request;
	unsigned int s_mb_max_to_scan;
	unsigned int s_mb_min_to_scan;
	unsigned int s_mb_stats;
	unsigned int s_mb_order2_reqs;
	unsigned int s_mb_group_prealloc;
	unsigned int s_max_dir_size_kb;

	unsigned long s_mb_last_group;
	unsigned long s_mb_last_start;
	unsigned int s_mb_prefetch;
	unsigned int s_mb_prefetch_limit;
	unsigned int s_mb_best_avail_max_trim_order;


	atomic_t s_bal_reqs;
	atomic_t s_bal_success;
	atomic_t s_bal_allocated;
	atomic_t s_bal_ex_scanned;
	atomic_t s_bal_cX_ex_scanned[EXT4_MB_NUM_CRS];
	atomic_t s_bal_groups_scanned;
	atomic_t s_bal_goals;
	atomic_t s_bal_len_goals;
	atomic_t s_bal_breaks;
	atomic_t s_bal_2orders;
	atomic64_t s_bal_cX_groups_considered[EXT4_MB_NUM_CRS];
	atomic64_t s_bal_cX_hits[EXT4_MB_NUM_CRS];
	atomic64_t s_bal_cX_failed[EXT4_MB_NUM_CRS];
	atomic_t s_mb_buddies_generated;
	atomic64_t s_mb_generation_time;
	atomic_t s_mb_lost_chunks;
	atomic_t s_mb_preallocated;
	atomic_t s_mb_discarded;
	atomic_t s_lock_busy;


	struct ext4_locality_group __percpu *s_locality_groups;


	unsigned long s_sectors_written_start;
	u64 s_kbytes_written;


	unsigned int s_extent_max_zeroout_kb;

	unsigned int s_log_groups_per_flex;
	struct flex_groups * __rcu *s_flex_groups;
	ext4_group_t s_flex_groups_allocated;


	struct workqueue_struct *rsv_conversion_wq;


	struct timer_list s_err_report;


	struct ext4_li_request *s_li_request;

	unsigned int s_li_wait_mult;


	struct task_struct *s_mmp_tsk;


	unsigned long s_last_trim_minblks;


	struct crypto_shash *s_chksum_driver;


	__u32 s_csum_seed;


	struct shrinker *s_es_shrinker;
	struct list_head s_es_list;
	long s_es_nr_inode;
	struct ext4_es_stats s_es_stats;
	struct mb_cache *s_ea_block_cache;
	struct mb_cache *s_ea_inode_cache;
	spinlock_t s_es_lock ____cacheline_aligned_in_smp;


	struct ext4_journal_trigger s_journal_triggers[EXT4_JOURNAL_TRIGGER_COUNT];


	struct ratelimit_state s_err_ratelimit_state;
	struct ratelimit_state s_warning_ratelimit_state;
	struct ratelimit_state s_msg_ratelimit_state;
	atomic_t s_warning_count;
	atomic_t s_msg_count;


	struct fscrypt_dummy_policy s_dummy_enc_policy;


	struct percpu_rw_semaphore s_writepages_rwsem;
	struct dax_device *s_daxdev;
	u64 s_dax_part_off;
#ifdef CONFIG_EXT4_DEBUG
	unsigned long s_simulate_fail;
#endif

	errseq_t s_bdev_wb_err;
	spinlock_t s_bdev_wb_lock;


	spinlock_t s_error_lock;
	int s_add_error_count;
	int s_first_error_code;
	__u32 s_first_error_line;
	__u32 s_first_error_ino;
	__u64 s_first_error_block;
	const char *s_first_error_func;
	time64_t s_first_error_time;
	int s_last_error_code;
	__u32 s_last_error_line;
	__u32 s_last_error_ino;
	__u64 s_last_error_block;
	const char *s_last_error_func;
	time64_t s_last_error_time;


	struct work_struct s_sb_upd_work;


	atomic_t s_fc_subtid;


#define FC_Q_MAIN	0
#define FC_Q_STAGING	1
	struct list_head s_fc_q[2];


	struct list_head s_fc_dentry_q[2];
	unsigned int s_fc_bytes;


	spinlock_t s_fc_lock;
	struct buffer_head *s_fc_bh;
	struct ext4_fc_stats s_fc_stats;
	tid_t s_fc_ineligible_tid;
#ifdef CONFIG_EXT4_DEBUG
	int s_fc_debug_max_replay;
#endif
	struct ext4_fc_replay_state s_fc_replay_state;
};

/**
 * EXT4_SB - Implements the EXT4 SB operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline struct ext4_sb_info *EXT4_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}
/**
 * EXT4_I - Implements the EXT4 I operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline struct ext4_inode_info *EXT4_I(struct inode *inode)
{
	return container_of(inode, struct ext4_inode_info, vfs_inode);
}

/**
 * ext4_writepages_down_read - Reads or materialises filesystem state for validation or higher-level processing.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_writepages_down_read(struct super_block *sb)
{
	percpu_down_read(&EXT4_SB(sb)->s_writepages_rwsem);
	return memalloc_nofs_save();
}

/**
 * ext4_writepages_up_read - Reads or materialises filesystem state for validation or higher-level processing.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_writepages_up_read(struct super_block *sb, int ctx)
{
	memalloc_nofs_restore(ctx);
	percpu_up_read(&EXT4_SB(sb)->s_writepages_rwsem);
}

/**
 * ext4_writepages_down_write - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_writepages_down_write(struct super_block *sb)
{
	percpu_down_write(&EXT4_SB(sb)->s_writepages_rwsem);
	return memalloc_nofs_save();
}

/**
 * ext4_writepages_up_write - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_writepages_up_write(struct super_block *sb, int ctx)
{
	memalloc_nofs_restore(ctx);
	percpu_up_write(&EXT4_SB(sb)->s_writepages_rwsem);
}

/**
 * ext4_valid_inum - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_valid_inum(struct super_block *sb, unsigned long ino)
{
	return ino == EXT4_ROOT_INO ||
		(ino >= EXT4_FIRST_INO(sb) &&
		 ino <= le32_to_cpu(EXT4_SB(sb)->s_es->s_inodes_count));
}


#define sbi_array_rcu_deref(sbi, field, index)				   \
/**
 * sbi_array_rcu_deref - Implements the sbi array rcu deref operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
({									   \
	typeof(*((sbi)->field)) _v;					   \
	rcu_read_lock();						   \
	_v = ((typeof(_v)*)rcu_dereference((sbi)->field))[index];	   \
	rcu_read_unlock();						   \
	_v;								   \
})


enum {
	EXT4_MF_MNTDIR_SAMPLED,
	EXT4_MF_FC_INELIGIBLE,
	EXT4_MF_JOURNAL_DESTROY
};

/**
 * ext4_set_mount_flag - Implements a mount-path operation for the owning filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_set_mount_flag(struct super_block *sb, int bit)
{
	set_bit(bit, &EXT4_SB(sb)->s_mount_flags);
}

/**
 * ext4_clear_mount_flag - Implements a mount-path operation for the owning filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_clear_mount_flag(struct super_block *sb, int bit)
{
	clear_bit(bit, &EXT4_SB(sb)->s_mount_flags);
}

/**
 * ext4_test_mount_flag - Implements a mount-path operation for the owning filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_test_mount_flag(struct super_block *sb, int bit)
{
	return test_bit(bit, &EXT4_SB(sb)->s_mount_flags);
}


#define EXT4_SIM_BBITMAP_EIO	1
#define EXT4_SIM_BBITMAP_CRC	2
#define EXT4_SIM_IBITMAP_EIO	3
#define EXT4_SIM_IBITMAP_CRC	4
#define EXT4_SIM_INODE_EIO	5
#define EXT4_SIM_INODE_CRC	6
#define EXT4_SIM_DIRBLOCK_EIO	7
#define EXT4_SIM_DIRBLOCK_CRC	8

/**
 * ext4_simulate_fail - Implements the simulate fail operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_simulate_fail(struct super_block *sb,
				     unsigned long code)
{
#ifdef CONFIG_EXT4_DEBUG
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	if (unlikely(sbi->s_simulate_fail == code)) {
		sbi->s_simulate_fail = 0;
		return true;
	}
#endif
	return false;
}


#define EXT4_ERR_UNKNOWN	 1
#define EXT4_ERR_EIO		 2
#define EXT4_ERR_ENOMEM		 3
#define EXT4_ERR_EFSBADCRC	 4
#define EXT4_ERR_EFSCORRUPTED	 5
#define EXT4_ERR_ENOSPC		 6
#define EXT4_ERR_ENOKEY		 7
#define EXT4_ERR_EROFS		 8
#define EXT4_ERR_EFBIG		 9
#define EXT4_ERR_EEXIST		10
#define EXT4_ERR_ERANGE		11
#define EXT4_ERR_EOVERFLOW	12
#define EXT4_ERR_EBUSY		13
#define EXT4_ERR_ENOTDIR	14
#define EXT4_ERR_ENOTEMPTY	15
#define EXT4_ERR_ESHUTDOWN	16
#define EXT4_ERR_EFAULT		17


enum {
	EXT4_STATE_NEW,
	EXT4_STATE_XATTR,
	EXT4_STATE_NO_EXPAND,
	EXT4_STATE_DA_ALLOC_CLOSE,
	EXT4_STATE_EXT_MIGRATE,
	EXT4_STATE_NEWENTRY,
	EXT4_STATE_MAY_INLINE_DATA,
	EXT4_STATE_EXT_PRECACHED,
	EXT4_STATE_LUSTRE_EA_INODE,
	EXT4_STATE_VERITY_IN_PROGRESS,
	EXT4_STATE_FC_COMMITTING,
	EXT4_STATE_ORPHAN_FILE,
};

#define EXT4_INODE_BIT_FNS(name, field, offset)				\
/**
 * name - Implements the name operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_test_inode_##name(struct inode *inode, int bit)	\
{									\
	return test_bit(bit + (offset), &EXT4_I(inode)->i_##field);	\
}/**
 * name - Implements the name operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
									\
static inline void ext4_set_inode_##name(struct inode *inode, int bit)	\
{									\
	set_bit(bit + (offset), &EXT4_I(inode)->i_##field);		\
}/**
 * name - Implements the name operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
									\
static inline void ext4_clear_inode_##name(struct inode *inode, int bit) \
{									\
	clear_bit(bit + (offset), &EXT4_I(inode)->i_##field);		\
}


static inline int ext4_test_inode_flag(struct inode *inode, int bit);
static inline void ext4_set_inode_flag(struct inode *inode, int bit);
static inline void ext4_clear_inode_flag(struct inode *inode, int bit);
EXT4_INODE_BIT_FNS(flag, flags, 0)


static inline int ext4_test_inode_state(struct inode *inode, int bit);
static inline void ext4_set_inode_state(struct inode *inode, int bit);
static inline void ext4_clear_inode_state(struct inode *inode, int bit);
#if (BITS_PER_LONG < 64)
/**
 * ext4_clear_state_flags - Implements the clear state flags operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
EXT4_INODE_BIT_FNS(state, state_flags, 0)

static inline void ext4_clear_state_flags(struct ext4_inode_info *ei)
{
	(ei)->i_state_flags = 0;
}
#else
/**
 * ext4_clear_state_flags - Implements the clear state flags operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
EXT4_INODE_BIT_FNS(state, flags, 32)

static inline void ext4_clear_state_flags(struct ext4_inode_info *ei)
{

}
#endif
#else


#define EXT4_SB(sb)	(sb)
#endif

/**
 * ext4_verity_in_progress - Implements the verity in progress operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_verity_in_progress(struct inode *inode)
{
	return IS_ENABLED(CONFIG_FS_VERITY) &&
	       ext4_test_inode_state(inode, EXT4_STATE_VERITY_IN_PROGRESS);
}

#define NEXT_ORPHAN(inode) EXT4_I(inode)->i_dtime


/**
 * ext4_inode_orphan_tracked - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_inode_orphan_tracked(struct inode *inode)
{
	return ext4_test_inode_state(inode, EXT4_STATE_ORPHAN_FILE) ||
		!list_empty(&EXT4_I(inode)->i_orphan);
}


#define EXT4_OS_LINUX		0
#define EXT4_OS_HURD		1
#define EXT4_OS_MASIX		2
#define EXT4_OS_FREEBSD		3
#define EXT4_OS_LITES		4


#define EXT4_GOOD_OLD_REV	0
#define EXT4_DYNAMIC_REV	1

#define EXT4_MAX_SUPP_REV	EXT4_DYNAMIC_REV

#define EXT4_GOOD_OLD_INODE_SIZE 128

#define EXT4_EXTRA_TIMESTAMP_MAX	(((s64)1 << 34) - 1  + S32_MIN)
#define EXT4_NON_EXTRA_TIMESTAMP_MAX	S32_MAX
#define EXT4_TIMESTAMP_MIN		S32_MIN


#define EXT4_FEATURE_COMPAT_DIR_PREALLOC	0x0001
#define EXT4_FEATURE_COMPAT_IMAGIC_INODES	0x0002
#define EXT4_FEATURE_COMPAT_HAS_JOURNAL		0x0004
#define EXT4_FEATURE_COMPAT_EXT_ATTR		0x0008
#define EXT4_FEATURE_COMPAT_RESIZE_INODE	0x0010
#define EXT4_FEATURE_COMPAT_DIR_INDEX		0x0020
#define EXT4_FEATURE_COMPAT_SPARSE_SUPER2	0x0200


#define EXT4_FEATURE_COMPAT_FAST_COMMIT		0x0400
#define EXT4_FEATURE_COMPAT_STABLE_INODES	0x0800
#define EXT4_FEATURE_COMPAT_ORPHAN_FILE		0x1000

#define EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER	0x0001
#define EXT4_FEATURE_RO_COMPAT_LARGE_FILE	0x0002
#define EXT4_FEATURE_RO_COMPAT_BTREE_DIR	0x0004
#define EXT4_FEATURE_RO_COMPAT_HUGE_FILE        0x0008
#define EXT4_FEATURE_RO_COMPAT_GDT_CSUM		0x0010
#define EXT4_FEATURE_RO_COMPAT_DIR_NLINK	0x0020
#define EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE	0x0040
#define EXT4_FEATURE_RO_COMPAT_QUOTA		0x0100
#define EXT4_FEATURE_RO_COMPAT_BIGALLOC		0x0200


#define EXT4_FEATURE_RO_COMPAT_METADATA_CSUM	0x0400
#define EXT4_FEATURE_RO_COMPAT_READONLY		0x1000
#define EXT4_FEATURE_RO_COMPAT_PROJECT		0x2000
#define EXT4_FEATURE_RO_COMPAT_VERITY		0x8000
#define EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT	0x10000


#define EXT4_FEATURE_INCOMPAT_COMPRESSION	0x0001
#define EXT4_FEATURE_INCOMPAT_FILETYPE		0x0002
#define EXT4_FEATURE_INCOMPAT_RECOVER		0x0004
#define EXT4_FEATURE_INCOMPAT_JOURNAL_DEV	0x0008
#define EXT4_FEATURE_INCOMPAT_META_BG		0x0010
#define EXT4_FEATURE_INCOMPAT_EXTENTS		0x0040
#define EXT4_FEATURE_INCOMPAT_64BIT		0x0080
#define EXT4_FEATURE_INCOMPAT_MMP               0x0100
#define EXT4_FEATURE_INCOMPAT_FLEX_BG		0x0200
#define EXT4_FEATURE_INCOMPAT_EA_INODE		0x0400
#define EXT4_FEATURE_INCOMPAT_DIRDATA		0x1000
#define EXT4_FEATURE_INCOMPAT_CSUM_SEED		0x2000
#define EXT4_FEATURE_INCOMPAT_LARGEDIR		0x4000
#define EXT4_FEATURE_INCOMPAT_INLINE_DATA	0x8000
#define EXT4_FEATURE_INCOMPAT_ENCRYPT		0x10000
#define EXT4_FEATURE_INCOMPAT_CASEFOLD		0x20000

extern void ext4_update_dynamic_rev(struct super_block *sb);

#define EXT4_FEATURE_COMPAT_FUNCS(name, flagname) \
/**
 * name - Implements the name operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_has_feature_##name(struct super_block *sb) \
{ \
	return ((EXT4_SB(sb)->s_es->s_feature_compat & \
		cpu_to_le32(EXT4_FEATURE_COMPAT_##flagname)) != 0); \
}/**
 * name - Implements the name operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 \
static inline void ext4_set_feature_##name(struct super_block *sb) \
{ \
	ext4_update_dynamic_rev(sb); \
	EXT4_SB(sb)->s_es->s_feature_compat |= \
		cpu_to_le32(EXT4_FEATURE_COMPAT_##flagname); \
}/**
 * name - Implements the name operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 \
static inline void ext4_clear_feature_##name(struct super_block *sb) \
{ \
	EXT4_SB(sb)->s_es->s_feature_compat &= \
		~cpu_to_le32(EXT4_FEATURE_COMPAT_##flagname); \
}

#define EXT4_FEATURE_RO_COMPAT_FUNCS(name, flagname) \
/**
 * name - Implements the name operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_has_feature_##name(struct super_block *sb) \
{ \
	return ((EXT4_SB(sb)->s_es->s_feature_ro_compat & \
		cpu_to_le32(EXT4_FEATURE_RO_COMPAT_##flagname)) != 0); \
}/**
 * name - Implements the name operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 \
static inline void ext4_set_feature_##name(struct super_block *sb) \
{ \
	ext4_update_dynamic_rev(sb); \
	EXT4_SB(sb)->s_es->s_feature_ro_compat |= \
		cpu_to_le32(EXT4_FEATURE_RO_COMPAT_##flagname); \
}/**
 * name - Implements the name operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 \
static inline void ext4_clear_feature_##name(struct super_block *sb) \
{ \
	EXT4_SB(sb)->s_es->s_feature_ro_compat &= \
		~cpu_to_le32(EXT4_FEATURE_RO_COMPAT_##flagname); \
}

#define EXT4_FEATURE_INCOMPAT_FUNCS(name, flagname) \
/**
 * name - Implements the name operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_has_feature_##name(struct super_block *sb) \
{ \
	return ((EXT4_SB(sb)->s_es->s_feature_incompat & \
		cpu_to_le32(EXT4_FEATURE_INCOMPAT_##flagname)) != 0); \
}/**
 * name - Implements the name operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 \
static inline void ext4_set_feature_##name(struct super_block *sb) \
{ \
	ext4_update_dynamic_rev(sb); \
	EXT4_SB(sb)->s_es->s_feature_incompat |= \
		cpu_to_le32(EXT4_FEATURE_INCOMPAT_##flagname); \
}/**
 * name - Implements the name operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 \
static inline void ext4_clear_feature_##name(struct super_block *sb) \
{ \
	EXT4_SB(sb)->s_es->s_feature_incompat &= \
		~cpu_to_le32(EXT4_FEATURE_INCOMPAT_##flagname); \
}

/**
 * _compat_features - Implements the compat features operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
EXT4_FEATURE_COMPAT_FUNCS(dir_prealloc,		DIR_PREALLOC)
EXT4_FEATURE_COMPAT_FUNCS(imagic_inodes,	IMAGIC_INODES)
EXT4_FEATURE_COMPAT_FUNCS(journal,		HAS_JOURNAL)
EXT4_FEATURE_COMPAT_FUNCS(xattr,		EXT_ATTR)
EXT4_FEATURE_COMPAT_FUNCS(resize_inode,		RESIZE_INODE)
EXT4_FEATURE_COMPAT_FUNCS(dir_index,		DIR_INDEX)
EXT4_FEATURE_COMPAT_FUNCS(sparse_super2,	SPARSE_SUPER2)
EXT4_FEATURE_COMPAT_FUNCS(fast_commit,		FAST_COMMIT)
EXT4_FEATURE_COMPAT_FUNCS(stable_inodes,	STABLE_INODES)
EXT4_FEATURE_COMPAT_FUNCS(orphan_file,		ORPHAN_FILE)

EXT4_FEATURE_RO_COMPAT_FUNCS(sparse_super,	SPARSE_SUPER)
EXT4_FEATURE_RO_COMPAT_FUNCS(large_file,	LARGE_FILE)
EXT4_FEATURE_RO_COMPAT_FUNCS(btree_dir,		BTREE_DIR)
EXT4_FEATURE_RO_COMPAT_FUNCS(huge_file,		HUGE_FILE)
EXT4_FEATURE_RO_COMPAT_FUNCS(gdt_csum,		GDT_CSUM)
EXT4_FEATURE_RO_COMPAT_FUNCS(dir_nlink,		DIR_NLINK)
EXT4_FEATURE_RO_COMPAT_FUNCS(extra_isize,	EXTRA_ISIZE)
EXT4_FEATURE_RO_COMPAT_FUNCS(quota,		QUOTA)
EXT4_FEATURE_RO_COMPAT_FUNCS(bigalloc,		BIGALLOC)
EXT4_FEATURE_RO_COMPAT_FUNCS(metadata_csum,	METADATA_CSUM)
EXT4_FEATURE_RO_COMPAT_FUNCS(readonly,		READONLY)
EXT4_FEATURE_RO_COMPAT_FUNCS(project,		PROJECT)
EXT4_FEATURE_RO_COMPAT_FUNCS(verity,		VERITY)
EXT4_FEATURE_RO_COMPAT_FUNCS(orphan_present,	ORPHAN_PRESENT)

EXT4_FEATURE_INCOMPAT_FUNCS(compression,	COMPRESSION)
EXT4_FEATURE_INCOMPAT_FUNCS(filetype,		FILETYPE)
EXT4_FEATURE_INCOMPAT_FUNCS(journal_needs_recovery,	RECOVER)
EXT4_FEATURE_INCOMPAT_FUNCS(journal_dev,	JOURNAL_DEV)
EXT4_FEATURE_INCOMPAT_FUNCS(meta_bg,		META_BG)
EXT4_FEATURE_INCOMPAT_FUNCS(extents,		EXTENTS)
EXT4_FEATURE_INCOMPAT_FUNCS(64bit,		64BIT)
EXT4_FEATURE_INCOMPAT_FUNCS(mmp,		MMP)
EXT4_FEATURE_INCOMPAT_FUNCS(flex_bg,		FLEX_BG)
EXT4_FEATURE_INCOMPAT_FUNCS(ea_inode,		EA_INODE)
EXT4_FEATURE_INCOMPAT_FUNCS(dirdata,		DIRDATA)
EXT4_FEATURE_INCOMPAT_FUNCS(csum_seed,		CSUM_SEED)
EXT4_FEATURE_INCOMPAT_FUNCS(largedir,		LARGEDIR)
EXT4_FEATURE_INCOMPAT_FUNCS(inline_data,	INLINE_DATA)
EXT4_FEATURE_INCOMPAT_FUNCS(encrypt,		ENCRYPT)
EXT4_FEATURE_INCOMPAT_FUNCS(casefold,		CASEFOLD)

#define EXT4_FEATURE_COMPAT_SUPP	(EXT4_FEATURE_COMPAT_EXT_ATTR| \
					 EXT4_FEATURE_COMPAT_ORPHAN_FILE)
#define EXT4_FEATURE_INCOMPAT_SUPP	(EXT4_FEATURE_INCOMPAT_FILETYPE| \
					 EXT4_FEATURE_INCOMPAT_RECOVER| \
					 EXT4_FEATURE_INCOMPAT_META_BG| \
					 EXT4_FEATURE_INCOMPAT_EXTENTS| \
					 EXT4_FEATURE_INCOMPAT_64BIT| \
					 EXT4_FEATURE_INCOMPAT_FLEX_BG| \
					 EXT4_FEATURE_INCOMPAT_EA_INODE| \
					 EXT4_FEATURE_INCOMPAT_MMP | \
					 EXT4_FEATURE_INCOMPAT_INLINE_DATA | \
					 EXT4_FEATURE_INCOMPAT_ENCRYPT | \
					 EXT4_FEATURE_INCOMPAT_CASEFOLD | \
					 EXT4_FEATURE_INCOMPAT_CSUM_SEED | \
					 EXT4_FEATURE_INCOMPAT_LARGEDIR)
#define EXT4_FEATURE_RO_COMPAT_SUPP	(EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER| \
					 EXT4_FEATURE_RO_COMPAT_LARGE_FILE| \
					 EXT4_FEATURE_RO_COMPAT_GDT_CSUM| \
					 EXT4_FEATURE_RO_COMPAT_DIR_NLINK | \
					 EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE | \
					 EXT4_FEATURE_RO_COMPAT_BTREE_DIR |\
					 EXT4_FEATURE_RO_COMPAT_HUGE_FILE |\
					 EXT4_FEATURE_RO_COMPAT_BIGALLOC |\
					 EXT4_FEATURE_RO_COMPAT_METADATA_CSUM|\
					 EXT4_FEATURE_RO_COMPAT_QUOTA |\
					 EXT4_FEATURE_RO_COMPAT_PROJECT |\
					 EXT4_FEATURE_RO_COMPAT_VERITY |\
					 EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT)

#define EXTN_FEATURE_FUNCS(ver) \
static inline bool ext4_has_unknown_ext##ver##_compat_features(struct super_block *sb) \
{ \
	return ((EXT4_SB(sb)->s_es->s_feature_compat & \
		cpu_to_le32(~EXT##ver##_FEATURE_COMPAT_SUPP)) != 0); \
}/**
 * _ro_compat_features - Implements the ro compat features operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 \
static inline bool ext4_has_unknown_ext##ver##_ro_compat_features(struct super_block *sb) \
{ \
	return ((EXT4_SB(sb)->s_es->s_feature_ro_compat & \
		cpu_to_le32(~EXT##ver##_FEATURE_RO_COMPAT_SUPP)) != 0); \
}/**
 * _incompat_features - Implements the incompat features operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 \
static inline bool ext4_has_unknown_ext##ver##_incompat_features(struct super_block *sb) \
{ \
	return ((EXT4_SB(sb)->s_es->s_feature_incompat & \
		cpu_to_le32(~EXT##ver##_FEATURE_INCOMPAT_SUPP)) != 0); \
}

/**
 * ext4_has_compat_features - Implements the has compat features operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
EXTN_FEATURE_FUNCS(2)
EXTN_FEATURE_FUNCS(3)
EXTN_FEATURE_FUNCS(4)

static inline bool ext4_has_compat_features(struct super_block *sb)
{
	return (EXT4_SB(sb)->s_es->s_feature_compat != 0);
}
/**
 * ext4_has_ro_compat_features - Implements the has ro compat features operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_has_ro_compat_features(struct super_block *sb)
{
	return (EXT4_SB(sb)->s_es->s_feature_ro_compat != 0);
}
/**
 * ext4_has_incompat_features - Implements the has incompat features operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_has_incompat_features(struct super_block *sb)
{
	return (EXT4_SB(sb)->s_es->s_feature_incompat != 0);
}

extern int ext4_feature_set_ok(struct super_block *sb, int readonly);


#define EXT4_FLAGS_RESIZING	0
#define EXT4_FLAGS_SHUTDOWN	1
#define EXT4_FLAGS_BDEV_IS_DAX	2

/**
 * ext4_forced_shutdown - Implements the forced shutdown operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_forced_shutdown(struct super_block *sb)
{
	return test_bit(EXT4_FLAGS_SHUTDOWN, &EXT4_SB(sb)->s_ext4_flags);
}


#define	EXT4_DEF_RESUID		0
#define	EXT4_DEF_RESGID		0


#define	EXT4_DEF_PROJID		0

#define EXT4_DEF_INODE_READAHEAD_BLKS	32


#define EXT4_DEFM_DEBUG		0x0001
#define EXT4_DEFM_BSDGROUPS	0x0002
#define EXT4_DEFM_XATTR_USER	0x0004
#define EXT4_DEFM_ACL		0x0008
#define EXT4_DEFM_UID16		0x0010
#define EXT4_DEFM_JMODE		0x0060
#define EXT4_DEFM_JMODE_DATA	0x0020
#define EXT4_DEFM_JMODE_ORDERED	0x0040
#define EXT4_DEFM_JMODE_WBACK	0x0060
#define EXT4_DEFM_NOBARRIER	0x0100
#define EXT4_DEFM_BLOCK_VALIDITY 0x0200
#define EXT4_DEFM_DISCARD	0x0400
#define EXT4_DEFM_NODELALLOC	0x0800


#define EXT4_DEF_MIN_BATCH_TIME	0
#define EXT4_DEF_MAX_BATCH_TIME	15000


#define EXT4_FLEX_SIZE_DIR_ALLOC_SCHEME	4


#define EXT4_NAME_LEN 255


#define EXT4_BASE_DIR_LEN (sizeof(struct ext4_dir_entry_2) - EXT4_NAME_LEN)

/**
 * EXT4_BASE_DIR_LEN - Implements the EXT4 BASE DIR LEN operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
struct ext4_dir_entry {
	__le32	inode;
	__le16	rec_len;
	__le16	name_len;
	char	name[EXT4_NAME_LEN];
};


/**
 * struct ext4_dir_entry_hash - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_dir_entry_hash {
	__le32 hash;
	__le32 minor_hash;
};


/**
 * struct ext4_dir_entry_2 - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_dir_entry_2 {
	__le32	inode;
	__le16	rec_len;
	__u8	name_len;
	__u8	file_type;
	char	name[EXT4_NAME_LEN];
};


#define EXT4_DIRENT_HASHES(entry) \
/**
 * ext4_hash_in_dirent - Implements the hash in dirent operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
	((struct ext4_dir_entry_hash *) \
		(((void *)(entry)) + \
		((8 + (entry)->name_len + EXT4_DIR_ROUND) & ~EXT4_DIR_ROUND)))
#define EXT4_DIRENT_HASH(entry) le32_to_cpu(EXT4_DIRENT_HASHES(entry)->hash)
#define EXT4_DIRENT_MINOR_HASH(entry) \
		le32_to_cpu(EXT4_DIRENT_HASHES(entry)->minor_hash)

static inline bool ext4_hash_in_dirent(const struct inode *inode)
{
	return IS_CASEFOLDED(inode) && IS_ENCRYPTED(inode);
}


/**
 * struct ext4_dir_entry_tail - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_dir_entry_tail {
	__le32	det_reserved_zero1;
	__le16	det_rec_len;
	__u8	det_reserved_zero2;
	__u8	det_reserved_ft;
	__le32	det_checksum;
};

#define EXT4_DIRENT_TAIL(block, blocksize) \
/**
 * ext4_dir_rec_len - Implements the dir rec len operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
	((struct ext4_dir_entry_tail *)(((void *)(block)) + \
					((blocksize) - \
					 sizeof(struct ext4_dir_entry_tail))))


#define EXT4_FT_UNKNOWN		0
#define EXT4_FT_REG_FILE	1
#define EXT4_FT_DIR		2
#define EXT4_FT_CHRDEV		3
#define EXT4_FT_BLKDEV		4
#define EXT4_FT_FIFO		5
#define EXT4_FT_SOCK		6
#define EXT4_FT_SYMLINK		7

#define EXT4_FT_MAX		8

#define EXT4_FT_DIR_CSUM	0xDE


#define EXT4_DIR_PAD			4
#define EXT4_DIR_ROUND			(EXT4_DIR_PAD - 1)
#define EXT4_MAX_REC_LEN		((1<<16)-1)


static inline unsigned int ext4_dir_rec_len(__u8 name_len,
						const struct inode *dir)
{
	int rec_len = (name_len + 8 + EXT4_DIR_ROUND);

	if (dir && ext4_hash_in_dirent(dir))
		rec_len += sizeof(struct ext4_dir_entry_hash);
	return (rec_len & ~EXT4_DIR_ROUND);
}


/**
 * ext4_rec_len_from_disk - Implements the rec len from disk operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline unsigned int
ext4_rec_len_from_disk(__le16 dlen, unsigned blocksize)
{
	unsigned len = le16_to_cpu(dlen);

#if (PAGE_SIZE >= 65536)
	if (len == EXT4_MAX_REC_LEN || len == 0)
		return blocksize;
	return (len & 65532) | ((len & 3) << 16);
#else
	return len;
#endif
}

/**
 * ext4_rec_len_to_disk - Implements the rec len to disk operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline __le16 ext4_rec_len_to_disk(unsigned len, unsigned blocksize)
{
	BUG_ON((len > blocksize) || (blocksize > (1 << 18)) || (len & 3));
#if (PAGE_SIZE >= 65536)
	if (len < 65536)
		return cpu_to_le16(len);
	if (len == blocksize) {
		if (blocksize == 65536)
			return cpu_to_le16(EXT4_MAX_REC_LEN);
		else
			return cpu_to_le16(0);
	}
	return cpu_to_le16((len & 65532) | ((len >> 16) & 3));
#else
	return cpu_to_le16(len);
#endif
}


#define is_dx(dir) (ext4_has_feature_dir_index((dir)->i_sb) && \
		    ext4_test_inode_flag((dir), EXT4_INODE_INDEX))
#define EXT4_DIR_LINK_MAX(dir) unlikely((dir)->i_nlink >= EXT4_LINK_MAX && \
		    !(ext4_has_feature_dir_nlink((dir)->i_sb) && is_dx(dir)))
#define EXT4_DIR_LINK_EMPTY(dir) ((dir)->i_nlink == 2 || (dir)->i_nlink == 1)


#define DX_HASH_LEGACY			0
#define DX_HASH_HALF_MD4		1
#define DX_HASH_TEA			2
#define DX_HASH_LEGACY_UNSIGNED		3
#define DX_HASH_HALF_MD4_UNSIGNED	4
#define DX_HASH_TEA_UNSIGNED		5
#define DX_HASH_SIPHASH			6
#define DX_HASH_LAST 			DX_HASH_SIPHASH

static inline u32 ext4_chksum(struct ext4_sb_info *sbi, u32 crc,
			      const void *address, unsigned int length)
{
	struct {
		struct shash_desc shash;
		char ctx[4];
	} desc;

	BUG_ON(crypto_shash_descsize(sbi->s_chksum_driver)!=sizeof(desc.ctx));

	desc.shash.tfm = sbi->s_chksum_driver;
	*(u32 *)desc.ctx = crc;

	BUG_ON(crypto_shash_update(&desc.shash, address, length));

	return *(u32 *)desc.ctx;
}

#ifdef __KERNEL__


/**
 * struct dx_hash_info - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct dx_hash_info
{
	u32		hash;
	u32		minor_hash;
	int		hash_version;
	u32		*seed;
};


#define EXT4_HTREE_EOF_32BIT   ((1UL  << (32 - 1)) - 1)
#define EXT4_HTREE_EOF_64BIT   ((1ULL << (64 - 1)) - 1)


#define HASH_NB_ALWAYS		1

/**
 * EXT4_HTREE_EOF_64BIT - Implements the EXT4 HTREE EOF 64BIT operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
struct ext4_filename {
	const struct qstr *usr_fname;
	struct fscrypt_str disk_name;
	struct dx_hash_info hinfo;
#ifdef CONFIG_FS_ENCRYPTION
	struct fscrypt_str crypto_buf;
#endif
#if IS_ENABLED(CONFIG_UNICODE)
	struct qstr cf_name;
#endif
};

#define fname_name(p) ((p)->disk_name.name)
#define fname_usr_name(p) ((p)->usr_fname->name)
#define fname_len(p)  ((p)->disk_name.len)


struct ext4_iloc
{
	struct buffer_head *bh;
	unsigned long offset;
	ext4_group_t block_group;
};

/**
 * ext4_raw_inode - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline struct ext4_inode *ext4_raw_inode(struct ext4_iloc *iloc)
{
	return (struct ext4_inode *) (iloc->bh->b_data + iloc->offset);
}

/**
 * ext4_is_quota_file - Implements the is quota file operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_is_quota_file(struct inode *inode)
{
	return IS_NOQUOTA(inode) &&
	       !(EXT4_I(inode)->i_flags & EXT4_EA_INODE_FL);
}


/**
 * struct dir_private_info - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct dir_private_info {
	struct rb_root	root;
	struct rb_node	*curr_node;
	struct fname	*extra_fname;
	loff_t		last_pos;
	__u32		curr_hash;
	__u32		curr_minor_hash;
	__u32		next_hash;
	u64		cookie;
	bool		initialized;
};


/**
 * ext4_group_first_block_no - Implements the group first block no operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline ext4_fsblk_t
ext4_group_first_block_no(struct super_block *sb, ext4_group_t group_no)
{
	return group_no * (ext4_fsblk_t)EXT4_BLOCKS_PER_GROUP(sb) +
		le32_to_cpu(EXT4_SB(sb)->s_es->s_first_data_block);
}


#define ERR_BAD_DX_DIR	(-(MAX_ERRNO - 1))


#define	EXT4_HTREE_LEVEL_COMPAT	2
#define	EXT4_HTREE_LEVEL	3

/**
 * ext4_dir_htree_level - Implements the dir htree level operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_dir_htree_level(struct super_block *sb)
{
	return ext4_has_feature_largedir(sb) ?
		EXT4_HTREE_LEVEL : EXT4_HTREE_LEVEL_COMPAT;
}


#define EXT4_DEF_LI_WAIT_MULT			10
#define EXT4_DEF_LI_MAX_START_DELAY		5
#define EXT4_LAZYINIT_QUIT			0x0001
#define EXT4_LAZYINIT_RUNNING			0x0002


/**
 * struct ext4_lazy_init - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_lazy_init {
	unsigned long		li_state;
	struct list_head	li_request_list;
	struct mutex		li_list_mtx;
};

/**
 * enum ext4_li_mode - Private EXT4 state/value set used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
enum ext4_li_mode {
	EXT4_LI_MODE_PREFETCH_BBITMAP,
	EXT4_LI_MODE_ITABLE,
};

/**
 * struct ext4_li_request - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_li_request {
	struct super_block	*lr_super;
	enum ext4_li_mode	lr_mode;
	ext4_group_t		lr_first_not_zeroed;
	ext4_group_t		lr_next_group;
	struct list_head	lr_request;
	unsigned long		lr_next_sched;
	unsigned long		lr_timeout;
};

/**
 * struct ext4_features - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_features {
	struct kobject f_kobj;
	struct completion f_kobj_unregister;
};


#define EXT4_MMP_MAGIC     0x004D4D50U
#define EXT4_MMP_SEQ_CLEAN 0xFF4D4D50U
#define EXT4_MMP_SEQ_FSCK  0xE24D4D50U
#define EXT4_MMP_SEQ_MAX   0xE24D4D4FU

/**
 * struct mmp_struct - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct mmp_struct {
	__le32	mmp_magic;
	__le32	mmp_seq;


	__le64	mmp_time;
	char	mmp_nodename[64];
	char	mmp_bdevname[32];


	__le16	mmp_check_interval;

	__le16	mmp_pad1;
	__le32	mmp_pad2[226];
	__le32	mmp_checksum;
};


/**
 * struct mmpd_data - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct mmpd_data {
	struct buffer_head *bh;
	struct super_block *sb;
};


#define EXT4_MMP_CHECK_MULT		2UL


#define EXT4_MMP_MIN_CHECK_INTERVAL	5UL


#define EXT4_MMP_MAX_CHECK_INTERVAL	300UL


# define NORET_TYPE
# define ATTRIB_NORET	__attribute__((noreturn))
# define NORET_AND	noreturn,


extern unsigned int ext4_count_free(char *bitmap, unsigned numchars);
void ext4_inode_bitmap_csum_set(struct super_block *sb,
				struct ext4_group_desc *gdp,
				struct buffer_head *bh);
int ext4_inode_bitmap_csum_verify(struct super_block *sb,
				  struct ext4_group_desc *gdp,
				  struct buffer_head *bh);
void ext4_block_bitmap_csum_set(struct super_block *sb,
				struct ext4_group_desc *gdp,
				struct buffer_head *bh);
int ext4_block_bitmap_csum_verify(struct super_block *sb,
				  struct ext4_group_desc *gdp,
				  struct buffer_head *bh);


extern void ext4_get_group_no_and_offset(struct super_block *sb,
					 ext4_fsblk_t blocknr,
					 ext4_group_t *blockgrpp,
					 ext4_grpblk_t *offsetp);
extern ext4_group_t ext4_get_group_number(struct super_block *sb,
					  ext4_fsblk_t block);

extern int ext4_bg_has_super(struct super_block *sb, ext4_group_t group);
extern unsigned long ext4_bg_num_gdb(struct super_block *sb,
			ext4_group_t group);
extern ext4_fsblk_t ext4_new_meta_blocks(handle_t *handle, struct inode *inode,
					 ext4_fsblk_t goal,
					 unsigned int flags,
					 unsigned long *count,
					 int *errp);
extern int ext4_claim_free_clusters(struct ext4_sb_info *sbi,
				    s64 nclusters, unsigned int flags);
extern ext4_fsblk_t ext4_count_free_clusters(struct super_block *);
extern struct ext4_group_desc * ext4_get_group_desc(struct super_block * sb,
						    ext4_group_t block_group,
						    struct buffer_head ** bh);
extern struct ext4_group_info *ext4_get_group_info(struct super_block *sb,
						   ext4_group_t group);
extern int ext4_should_retry_alloc(struct super_block *sb, int *retries);

extern struct buffer_head *ext4_read_block_bitmap_nowait(struct super_block *sb,
						ext4_group_t block_group,
						bool ignore_locked);
extern int ext4_wait_block_bitmap(struct super_block *sb,
				  ext4_group_t block_group,
				  struct buffer_head *bh);
extern struct buffer_head *ext4_read_block_bitmap(struct super_block *sb,
						  ext4_group_t block_group);
extern unsigned ext4_free_clusters_after_init(struct super_block *sb,
					      ext4_group_t block_group,
					      struct ext4_group_desc *gdp);
ext4_fsblk_t ext4_inode_to_goal_block(struct inode *);

#if IS_ENABLED(CONFIG_UNICODE)
extern int ext4_fname_setup_ci_filename(struct inode *dir,
					const struct qstr *iname,
					struct ext4_filename *fname);

/**
 * ext4_fname_free_ci_filename - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_fname_free_ci_filename(struct ext4_filename *fname)
{
	kfree(fname->cf_name.name);
	fname->cf_name.name = NULL;
}
#else
/**
 * ext4_fname_setup_ci_filename - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_fname_setup_ci_filename(struct inode *dir,
					       const struct qstr *iname,
					       struct ext4_filename *fname)
{
	return 0;
}

/**
 * ext4_fname_free_ci_filename - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_fname_free_ci_filename(struct ext4_filename *fname)
{
}
#endif


#ifdef CONFIG_FS_ENCRYPTION
extern const struct fscrypt_operations ext4_cryptops;

int ext4_fname_setup_filename(struct inode *dir, const struct qstr *iname,
			      int lookup, struct ext4_filename *fname);

int ext4_fname_prepare_lookup(struct inode *dir, struct dentry *dentry,
			      struct ext4_filename *fname);

void ext4_fname_free_filename(struct ext4_filename *fname);

int ext4_ioctl_get_encryption_pwsalt(struct file *filp, void __user *arg);

#else
/**
 * ext4_fname_setup_filename - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_fname_setup_filename(struct inode *dir,
					    const struct qstr *iname,
					    int lookup,
					    struct ext4_filename *fname)
{
	fname->usr_fname = iname;
	fname->disk_name.name = (unsigned char *) iname->name;
	fname->disk_name.len = iname->len;

	return ext4_fname_setup_ci_filename(dir, iname, fname);
}

/**
 * ext4_fname_prepare_lookup - Locates filesystem state without changing the authoritative persistent representation unless the surrounding API explicitly permits it.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_fname_prepare_lookup(struct inode *dir,
					    struct dentry *dentry,
					    struct ext4_filename *fname)
{
	return ext4_fname_setup_filename(dir, &dentry->d_name, 1, fname);
}

/**
 * ext4_fname_free_filename - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_fname_free_filename(struct ext4_filename *fname)
{
	ext4_fname_free_ci_filename(fname);
}

/**
 * ext4_ioctl_get_encryption_pwsalt - Handles a filesystem-specific control operation exposed through the file API.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_ioctl_get_encryption_pwsalt(struct file *filp,
						   void __user *arg)
{
	return -EOPNOTSUPP;
}
#endif


extern int __ext4_check_dir_entry(const char *, unsigned int, struct inode *,
				  struct file *,
				  struct ext4_dir_entry_2 *,
				  struct buffer_head *, char *, int,
				  unsigned int);
#define ext4_check_dir_entry(dir, filp, de, bh, buf, size, offset) \
	unlikely(__ext4_check_dir_entry(__func__, __LINE__, (dir), (filp), \
				(de), (bh), (buf), (size), (offset)))
extern int ext4_htree_store_dirent(struct file *dir_file, __u32 hash,
				__u32 minor_hash,
				struct ext4_dir_entry_2 *dirent,
				struct fscrypt_str *ent_name);
extern void ext4_htree_free_dir_info(struct dir_private_info *p);
extern int ext4_find_dest_de(struct inode *dir, struct inode *inode,
			     struct buffer_head *bh,
			     void *buf, int buf_size,
			     struct ext4_filename *fname,
			     struct ext4_dir_entry_2 **dest_de);
void ext4_insert_dentry(struct inode *dir, struct inode *inode,
			struct ext4_dir_entry_2 *de,
			int buf_size,
			struct ext4_filename *fname);
/**
 * ext4_update_dx_flag - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_update_dx_flag(struct inode *inode)
{
	if (!ext4_has_feature_dir_index(inode->i_sb) &&
	    ext4_test_inode_flag(inode, EXT4_INODE_INDEX)) {

		WARN_ON_ONCE(ext4_has_feature_metadata_csum(inode->i_sb));
		ext4_clear_inode_flag(inode, EXT4_INODE_INDEX);
	}
}
static const unsigned char ext4_filetype_table[] = {
	DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, DT_SOCK, DT_LNK
};

/**
 * get_dtype - Implements the get dtype operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline  unsigned char get_dtype(struct super_block *sb, int filetype)
{
	if (!ext4_has_feature_filetype(sb) || filetype >= EXT4_FT_MAX)
		return DT_UNKNOWN;

	return ext4_filetype_table[filetype];
}
extern int ext4_check_all_de(struct inode *dir, struct buffer_head *bh,
			     void *buf, int buf_size);


extern int ext4_sync_file(struct file *, loff_t, loff_t, int);


extern int ext4fs_dirhash(const struct inode *dir, const char *name, int len,
			  struct dx_hash_info *hinfo);


extern int ext4_mark_inode_used(struct super_block *sb, int ino);
extern struct inode *__ext4_new_inode(struct mnt_idmap *, handle_t *,
				      struct inode *, umode_t,
				      const struct qstr *qstr, __u32 goal,
				      uid_t *owner, __u32 i_flags,
				      int handle_type, unsigned int line_no,
				      int nblocks);

#define ext4_new_inode(handle, dir, mode, qstr, goal, owner, i_flags)          \
	__ext4_new_inode(&nop_mnt_idmap, (handle), (dir), (mode), (qstr),      \
			 (goal), (owner), i_flags, 0, 0, 0)
#define ext4_new_inode_start_handle(idmap, dir, mode, qstr, goal, owner, \
				    type, nblocks)		    \
	__ext4_new_inode((idmap), NULL, (dir), (mode), (qstr), (goal), (owner), \
			 0, (type), __LINE__, (nblocks))


extern void ext4_free_inode(handle_t *, struct inode *);
extern struct inode * ext4_orphan_get(struct super_block *, unsigned long);
extern unsigned long ext4_count_free_inodes(struct super_block *);
extern unsigned long ext4_count_dirs(struct super_block *);
extern void ext4_mark_bitmap_end(int start_bit, int end_bit, char *bitmap);
extern int ext4_init_inode_table(struct super_block *sb,
				 ext4_group_t group, int barrier);
extern void ext4_end_bitmap_read(struct buffer_head *bh, int uptodate);


int ext4_fc_info_show(struct seq_file *seq, void *v);
void ext4_fc_init(struct super_block *sb, journal_t *journal);
void ext4_fc_init_inode(struct inode *inode);
void ext4_fc_track_range(handle_t *handle, struct inode *inode, ext4_lblk_t start,
			 ext4_lblk_t end);
void __ext4_fc_track_unlink(handle_t *handle, struct inode *inode,
	struct dentry *dentry);
void __ext4_fc_track_link(handle_t *handle, struct inode *inode,
	struct dentry *dentry);
void ext4_fc_track_unlink(handle_t *handle, struct dentry *dentry);
void ext4_fc_track_link(handle_t *handle, struct dentry *dentry);
void __ext4_fc_track_create(handle_t *handle, struct inode *inode,
			    struct dentry *dentry);
void ext4_fc_track_create(handle_t *handle, struct dentry *dentry);
void ext4_fc_track_inode(handle_t *handle, struct inode *inode);
void ext4_fc_mark_ineligible(struct super_block *sb, int reason, handle_t *handle);
void ext4_fc_start_update(struct inode *inode);
void ext4_fc_stop_update(struct inode *inode);
void ext4_fc_del(struct inode *inode);
bool ext4_fc_replay_check_excluded(struct super_block *sb, ext4_fsblk_t block);
void ext4_fc_replay_cleanup(struct super_block *sb);
int ext4_fc_commit(journal_t *journal, tid_t commit_tid);
int __init ext4_fc_init_dentry_cache(void);
void ext4_fc_destroy_dentry_cache(void);
int ext4_fc_record_regions(struct super_block *sb, int ino,
			   ext4_lblk_t lblk, ext4_fsblk_t pblk,
			   int len, int replay);


extern const struct seq_operations ext4_mb_seq_groups_ops;
extern const struct seq_operations ext4_mb_seq_structs_summary_ops;
extern int ext4_seq_mb_stats_show(struct seq_file *seq, void *offset);
extern int ext4_mb_init(struct super_block *);
extern void ext4_mb_release(struct super_block *);
extern ext4_fsblk_t ext4_mb_new_blocks(handle_t *,
				struct ext4_allocation_request *, int *);
extern void ext4_discard_preallocations(struct inode *);
extern int __init ext4_init_mballoc(void);
extern void ext4_exit_mballoc(void);
extern ext4_group_t ext4_mb_prefetch(struct super_block *sb,
				     ext4_group_t group,
				     unsigned int nr, int *cnt);
extern void ext4_mb_prefetch_fini(struct super_block *sb, ext4_group_t group,
				  unsigned int nr);

extern void ext4_free_blocks(handle_t *handle, struct inode *inode,
			     struct buffer_head *bh, ext4_fsblk_t block,
			     unsigned long count, int flags);
extern int ext4_mb_alloc_groupinfo(struct super_block *sb,
				   ext4_group_t ngroups);
extern int ext4_mb_add_groupinfo(struct super_block *sb,
		ext4_group_t i, struct ext4_group_desc *desc);
extern int ext4_group_add_blocks(handle_t *handle, struct super_block *sb,
				ext4_fsblk_t block, unsigned long count);
extern int ext4_trim_fs(struct super_block *, struct fstrim_range *);
extern void ext4_process_freed_data(struct super_block *sb, tid_t commit_tid);
extern void ext4_mb_mark_bb(struct super_block *sb, ext4_fsblk_t block,
			    int len, bool state);
/**
 * ext4_mb_cr_expensive - Implements the mb cr expensive operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_mb_cr_expensive(enum criteria cr)
{
	return cr >= CR_GOAL_LEN_SLOW;
}


void ext4_inode_csum_set(struct inode *inode, struct ext4_inode *raw,
			 struct ext4_inode_info *ei);
int ext4_inode_is_fast_symlink(struct inode *inode);
struct buffer_head *ext4_getblk(handle_t *, struct inode *, ext4_lblk_t, int);
struct buffer_head *ext4_bread(handle_t *, struct inode *, ext4_lblk_t, int);
int ext4_bread_batch(struct inode *inode, ext4_lblk_t block, int bh_count,
		     bool wait, struct buffer_head **bhs);
int ext4_get_block_unwritten(struct inode *inode, sector_t iblock,
			     struct buffer_head *bh_result, int create);
int ext4_get_block(struct inode *inode, sector_t iblock,
		   struct buffer_head *bh_result, int create);
int ext4_da_get_block_prep(struct inode *inode, sector_t iblock,
			   struct buffer_head *bh, int create);
int ext4_walk_page_buffers(handle_t *handle,
			   struct inode *inode,
			   struct buffer_head *head,
			   unsigned from,
			   unsigned to,
			   int *partial,
			   int (*fn)(handle_t *handle, struct inode *inode,
				     struct buffer_head *bh));
int do_journal_get_write_access(handle_t *handle, struct inode *inode,
				struct buffer_head *bh);
#define FALL_BACK_TO_NONDELALLOC 1
#define CONVERT_INLINE_DATA	 2

typedef enum {
	EXT4_IGET_NORMAL =	0,
	EXT4_IGET_SPECIAL =	0x0001,
	EXT4_IGET_HANDLE = 	0x0002,
	EXT4_IGET_BAD =		0x0004,
	EXT4_IGET_EA_INODE =	0x0008
} ext4_iget_flags;

extern struct inode *__ext4_iget(struct super_block *sb, unsigned long ino,
				 ext4_iget_flags flags, const char *function,
				 unsigned int line);

#define ext4_iget(sb, ino, flags) \
	__ext4_iget((sb), (ino), (flags), __func__, __LINE__)

extern int  ext4_write_inode(struct inode *, struct writeback_control *);
extern int  ext4_setattr(struct mnt_idmap *, struct dentry *,
			 struct iattr *);
extern u32  ext4_dio_alignment(struct inode *inode);
extern int  ext4_getattr(struct mnt_idmap *, const struct path *,
			 struct kstat *, u32, unsigned int);
extern void ext4_evict_inode(struct inode *);
extern void ext4_clear_inode(struct inode *);
extern int  ext4_file_getattr(struct mnt_idmap *, const struct path *,
			      struct kstat *, u32, unsigned int);
extern void ext4_dirty_inode(struct inode *, int);
extern int ext4_change_inode_journal_flag(struct inode *, int);
extern int ext4_get_inode_loc(struct inode *, struct ext4_iloc *);
extern int ext4_get_fc_inode_loc(struct super_block *sb, unsigned long ino,
			  struct ext4_iloc *iloc);
extern int ext4_inode_attach_jinode(struct inode *inode);
extern int ext4_can_truncate(struct inode *inode);
extern int ext4_truncate(struct inode *);
extern int ext4_break_layouts(struct inode *);
extern int ext4_truncate_page_cache_block_range(struct inode *inode,
						loff_t start, loff_t end);
extern int ext4_punch_hole(struct file *file, loff_t offset, loff_t length);
extern void ext4_set_inode_flags(struct inode *, bool init);
extern int ext4_alloc_da_blocks(struct inode *inode);
extern void ext4_set_aops(struct inode *inode);
extern int ext4_writepage_trans_blocks(struct inode *);
extern int ext4_normal_submit_inode_data_buffers(struct jbd2_inode *jinode);
extern int ext4_chunk_trans_blocks(struct inode *, int nrblocks);
extern int ext4_zero_partial_blocks(handle_t *handle, struct inode *inode,
			     loff_t lstart, loff_t lend);
extern vm_fault_t ext4_page_mkwrite(struct vm_fault *vmf);
extern qsize_t *ext4_get_reserved_space(struct inode *inode);
extern int ext4_get_projid(struct inode *inode, kprojid_t *projid);
extern void ext4_da_release_space(struct inode *inode, int to_free);
extern void ext4_da_update_reserve_space(struct inode *inode,
					int used, int quota_claim);
extern int ext4_issue_zeroout(struct inode *inode, ext4_lblk_t lblk,
			      ext4_fsblk_t pblk, ext4_lblk_t len);


extern int ext4_ind_map_blocks(handle_t *handle, struct inode *inode,
				struct ext4_map_blocks *map, int flags);
extern int ext4_ind_trans_blocks(struct inode *inode, int nrblocks);
extern void ext4_ind_truncate(handle_t *, struct inode *inode);
extern int ext4_ind_remove_space(handle_t *handle, struct inode *inode,
				 ext4_lblk_t start, ext4_lblk_t end);


extern long ext4_ioctl(struct file *, unsigned int, unsigned long);
extern long ext4_compat_ioctl(struct file *, unsigned int, unsigned long);
int ext4_fileattr_set(struct mnt_idmap *idmap,
		      struct dentry *dentry, struct fileattr *fa);
int ext4_fileattr_get(struct dentry *dentry, struct fileattr *fa);
extern void ext4_reset_inode_seed(struct inode *inode);
int ext4_update_overhead(struct super_block *sb, bool force);
int ext4_force_shutdown(struct super_block *sb, u32 flags);


extern int ext4_ext_migrate(struct inode *);
extern int ext4_ind_migrate(struct inode *inode);


extern int ext4_init_new_dir(handle_t *handle, struct inode *dir,
			     struct inode *inode);
extern int ext4_dirblock_csum_verify(struct inode *inode,
				     struct buffer_head *bh);
extern int ext4_htree_fill_tree(struct file *dir_file, __u32 start_hash,
				__u32 start_minor_hash, __u32 *next_hash);
extern int ext4_search_dir(struct buffer_head *bh,
			   char *search_buf,
			   int buf_size,
			   struct inode *dir,
			   struct ext4_filename *fname,
			   unsigned int offset,
			   struct ext4_dir_entry_2 **res_dir);
extern int ext4_generic_delete_entry(struct inode *dir,
				     struct ext4_dir_entry_2 *de_del,
				     struct buffer_head *bh,
				     void *entry_buf,
				     int buf_size,
				     int csum_size);
extern bool ext4_empty_dir(struct inode *inode);


extern void ext4_kvfree_array_rcu(void *to_free);
extern int ext4_group_add(struct super_block *sb,
				struct ext4_new_group_data *input);
extern int ext4_group_extend(struct super_block *sb,
				struct ext4_super_block *es,
				ext4_fsblk_t n_blocks_count);
extern int ext4_resize_fs(struct super_block *sb, ext4_fsblk_t n_blocks_count);
extern unsigned int ext4_list_backups(struct super_block *sb,
				      unsigned int *three, unsigned int *five,
				      unsigned int *seven);


extern struct buffer_head *ext4_sb_bread(struct super_block *sb,
					 sector_t block, blk_opf_t op_flags);
extern struct buffer_head *ext4_sb_bread_unmovable(struct super_block *sb,
						   sector_t block);
extern struct buffer_head *ext4_sb_bread_nofail(struct super_block *sb,
						sector_t block);
extern void ext4_read_bh_nowait(struct buffer_head *bh, blk_opf_t op_flags,
				bh_end_io_t *end_io, bool simu_fail);
extern int ext4_read_bh(struct buffer_head *bh, blk_opf_t op_flags,
			bh_end_io_t *end_io, bool simu_fail);
extern int ext4_read_bh_lock(struct buffer_head *bh, blk_opf_t op_flags, bool wait);
extern void ext4_sb_breadahead_unmovable(struct super_block *sb, sector_t block);
extern int ext4_seq_options_show(struct seq_file *seq, void *offset);
extern int ext4_calculate_overhead(struct super_block *sb);
extern __le32 ext4_superblock_csum(struct super_block *sb,
				   struct ext4_super_block *es);
extern void ext4_superblock_csum_set(struct super_block *sb);
extern int ext4_alloc_flex_bg_array(struct super_block *sb,
				    ext4_group_t ngroup);
extern const char *ext4_decode_error(struct super_block *sb, int errno,
				     char nbuf[16]);
extern void ext4_mark_group_bitmap_corrupted(struct super_block *sb,
					     ext4_group_t block_group,
					     unsigned int flags);
extern unsigned int ext4_num_base_meta_blocks(struct super_block *sb,
					      ext4_group_t block_group);

extern __printf(7, 8)
void __ext4_error(struct super_block *, const char *, unsigned int, bool,
		  int, __u64, const char *, ...);
extern __printf(6, 7)
void __ext4_error_inode(struct inode *, const char *, unsigned int,
			ext4_fsblk_t, int, const char *, ...);
extern __printf(5, 6)
void __ext4_error_file(struct file *, const char *, unsigned int, ext4_fsblk_t,
		     const char *, ...);
extern void __ext4_std_error(struct super_block *, const char *,
			     unsigned int, int);
extern __printf(4, 5)
void __ext4_warning(struct super_block *, const char *, unsigned int,
		    const char *, ...);
extern __printf(4, 5)
void __ext4_warning_inode(const struct inode *inode, const char *function,
			  unsigned int line, const char *fmt, ...);
extern __printf(3, 4)
void __ext4_msg(struct super_block *, const char *, const char *, ...);
extern void __dump_mmp_msg(struct super_block *, struct mmp_struct *mmp,
			   const char *, unsigned int, const char *);
extern __printf(7, 8)
void __ext4_grp_locked_error(const char *, unsigned int,
			     struct super_block *, ext4_group_t,
			     unsigned long, ext4_fsblk_t,
			     const char *, ...);

#define EXT4_ERROR_INODE(inode, fmt, a...) \
/**
 * ext4_error_inode - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
	ext4_error_inode((inode), __func__, __LINE__, 0, (fmt), ## a)

#define EXT4_ERROR_INODE_ERR(inode, err, fmt, a...)			\
	__ext4_error_inode((inode), __func__, __LINE__, 0, (err), (fmt), ## a)

#define ext4_error_inode_block(inode, block, err, fmt, a...)		\
	__ext4_error_inode((inode), __func__, __LINE__, (block), (err),	\
			   (fmt), ## a)

#define EXT4_ERROR_FILE(file, block, fmt, a...)				\
	ext4_error_file((file), __func__, __LINE__, (block), (fmt), ## a)

#define ext4_abort(sb, err, fmt, a...)					\
	__ext4_error((sb), __func__, __LINE__, true, (err), 0, (fmt), ## a)

#ifdef CONFIG_PRINTK

#define ext4_error_inode(inode, func, line, block, fmt, ...)		\
	__ext4_error_inode(inode, func, line, block, 0, fmt, ##__VA_ARGS__)
#define ext4_error_inode_err(inode, func, line, block, err, fmt, ...)	\
	__ext4_error_inode((inode), (func), (line), (block), 		\
			   (err), (fmt), ##__VA_ARGS__)
#define ext4_error_file(file, func, line, block, fmt, ...)		\
	__ext4_error_file(file, func, line, block, fmt, ##__VA_ARGS__)
#define ext4_error(sb, fmt, ...)					\
	__ext4_error((sb), __func__, __LINE__, false, 0, 0, (fmt),	\
		##__VA_ARGS__)
#define ext4_error_err(sb, err, fmt, ...)				\
	__ext4_error((sb), __func__, __LINE__, false, (err), 0, (fmt),	\
		##__VA_ARGS__)
#define ext4_warning(sb, fmt, ...)					\
	__ext4_warning(sb, __func__, __LINE__, fmt, ##__VA_ARGS__)
#define ext4_warning_inode(inode, fmt, ...)				\
	__ext4_warning_inode(inode, __func__, __LINE__, fmt, ##__VA_ARGS__)
#define ext4_msg(sb, level, fmt, ...)				\
	__ext4_msg(sb, level, fmt, ##__VA_ARGS__)
#define dump_mmp_msg(sb, mmp, msg)					\
	__dump_mmp_msg(sb, mmp, __func__, __LINE__, msg)
#define ext4_grp_locked_error(sb, grp, ino, block, fmt, ...)		\
	__ext4_grp_locked_error(__func__, __LINE__, sb, grp, ino, block, \
				fmt, ##__VA_ARGS__)

#else

#define ext4_error_inode(inode, func, line, block, fmt, ...)		\
do {									\
	no_printk(fmt, ##__VA_ARGS__);					\
	__ext4_error_inode(inode, "", 0, block, 0, " ");		\
}/**
 * ext4_error_inode_err - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)
#define ext4_error_inode_err(inode, func, line, block, err, fmt, ...)	\
do {									\
	no_printk(fmt, ##__VA_ARGS__);					\
	__ext4_error_inode(inode, "", 0, block, err, " ");		\
}/**
 * ext4_error_file - Implements the error file operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)
#define ext4_error_file(file, func, line, block, fmt, ...)		\
do {									\
	no_printk(fmt, ##__VA_ARGS__);					\
	__ext4_error_file(file, "", 0, block, " ");			\
}/**
 * ext4_error - Implements the error operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)
#define ext4_error(sb, fmt, ...)					\
do {									\
	no_printk(fmt, ##__VA_ARGS__);					\
	__ext4_error(sb, "", 0, false, 0, 0, " ");			\
}/**
 * ext4_error_err - Implements the error err operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)
#define ext4_error_err(sb, err, fmt, ...)				\
do {									\
	no_printk(fmt, ##__VA_ARGS__);					\
	__ext4_error(sb, "", 0, false, err, 0, " ");			\
}/**
 * ext4_warning - Implements the warning operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)
#define ext4_warning(sb, fmt, ...)					\
do {									\
	no_printk(fmt, ##__VA_ARGS__);					\
	__ext4_warning(sb, "", 0, " ");					\
}/**
 * ext4_warning_inode - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)
#define ext4_warning_inode(inode, fmt, ...)				\
do {									\
	no_printk(fmt, ##__VA_ARGS__);					\
	__ext4_warning_inode(inode, "", 0, " ");			\
}/**
 * ext4_msg - Implements the msg operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)
#define ext4_msg(sb, level, fmt, ...)					\
do {									\
	no_printk(fmt, ##__VA_ARGS__);					\
	__ext4_msg(sb, "", " ");					\
}/**
 * ext4_grp_locked_error - Implements the grp locked error operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)
#define dump_mmp_msg(sb, mmp, msg)					\
	__dump_mmp_msg(sb, mmp, "", 0, "")
#define ext4_grp_locked_error(sb, grp, ino, block, fmt, ...)		\
do {									\
	no_printk(fmt, ##__VA_ARGS__);				\
	__ext4_grp_locked_error("", 0, sb, grp, ino, block, " ");	\
} while (0)

#endif

extern ext4_fsblk_t ext4_block_bitmap(struct super_block *sb,
				      struct ext4_group_desc *bg);
extern ext4_fsblk_t ext4_inode_bitmap(struct super_block *sb,
				      struct ext4_group_desc *bg);
extern ext4_fsblk_t ext4_inode_table(struct super_block *sb,
				     struct ext4_group_desc *bg);
extern __u32 ext4_free_group_clusters(struct super_block *sb,
				      struct ext4_group_desc *bg);
extern __u32 ext4_free_inodes_count(struct super_block *sb,
				 struct ext4_group_desc *bg);
extern __u32 ext4_used_dirs_count(struct super_block *sb,
				struct ext4_group_desc *bg);
extern __u32 ext4_itable_unused_count(struct super_block *sb,
				   struct ext4_group_desc *bg);
extern void ext4_block_bitmap_set(struct super_block *sb,
				  struct ext4_group_desc *bg, ext4_fsblk_t blk);
extern void ext4_inode_bitmap_set(struct super_block *sb,
				  struct ext4_group_desc *bg, ext4_fsblk_t blk);
extern void ext4_inode_table_set(struct super_block *sb,
				 struct ext4_group_desc *bg, ext4_fsblk_t blk);
extern void ext4_free_group_clusters_set(struct super_block *sb,
					 struct ext4_group_desc *bg,
					 __u32 count);
extern void ext4_free_inodes_set(struct super_block *sb,
				struct ext4_group_desc *bg, __u32 count);
extern void ext4_used_dirs_set(struct super_block *sb,
				struct ext4_group_desc *bg, __u32 count);
extern void ext4_itable_unused_set(struct super_block *sb,
				   struct ext4_group_desc *bg, __u32 count);
extern int ext4_group_desc_csum_verify(struct super_block *sb, __u32 group,
				       struct ext4_group_desc *gdp);
extern void ext4_group_desc_csum_set(struct super_block *sb, __u32 group,
				     struct ext4_group_desc *gdp);
extern int ext4_register_li_request(struct super_block *sb,
				    ext4_group_t first_not_zeroed);

/**
 * ext4_has_metadata_csum - Implements the has metadata csum operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_has_metadata_csum(struct super_block *sb)
{
	WARN_ON_ONCE(ext4_has_feature_metadata_csum(sb) &&
		     !EXT4_SB(sb)->s_chksum_driver);

	return ext4_has_feature_metadata_csum(sb) &&
	       (EXT4_SB(sb)->s_chksum_driver != NULL);
}

/**
 * ext4_has_group_desc_csum - Implements the has group desc csum operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_has_group_desc_csum(struct super_block *sb)
{
	return ext4_has_feature_gdt_csum(sb) || ext4_has_metadata_csum(sb);
}

#define ext4_read_incompat_64bit_val(es, name) \
/**
 * ext4_blocks_count - Computes derived filesystem state used for validation, accounting or policy decisions.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
	(((es)->s_feature_incompat & cpu_to_le32(EXT4_FEATURE_INCOMPAT_64BIT) \
		? (ext4_fsblk_t)le32_to_cpu(es->name##_hi) << 32 : 0) | \
		le32_to_cpu(es->name##_lo))

static inline ext4_fsblk_t ext4_blocks_count(struct ext4_super_block *es)
{
	return ext4_read_incompat_64bit_val(es, s_blocks_count);
}

/**
 * ext4_r_blocks_count - Computes derived filesystem state used for validation, accounting or policy decisions.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline ext4_fsblk_t ext4_r_blocks_count(struct ext4_super_block *es)
{
	return ext4_read_incompat_64bit_val(es, s_r_blocks_count);
}

/**
 * ext4_free_blocks_count - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline ext4_fsblk_t ext4_free_blocks_count(struct ext4_super_block *es)
{
	return ext4_read_incompat_64bit_val(es, s_free_blocks_count);
}

/**
 * ext4_blocks_count_set - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_blocks_count_set(struct ext4_super_block *es,
					 ext4_fsblk_t blk)
{
	es->s_blocks_count_lo = cpu_to_le32((u32)blk);
	es->s_blocks_count_hi = cpu_to_le32(blk >> 32);
}

/**
 * ext4_free_blocks_count_set - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_free_blocks_count_set(struct ext4_super_block *es,
					      ext4_fsblk_t blk)
{
	es->s_free_blocks_count_lo = cpu_to_le32((u32)blk);
	es->s_free_blocks_count_hi = cpu_to_le32(blk >> 32);
}

/**
 * ext4_r_blocks_count_set - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_r_blocks_count_set(struct ext4_super_block *es,
					   ext4_fsblk_t blk)
{
	es->s_r_blocks_count_lo = cpu_to_le32((u32)blk);
	es->s_r_blocks_count_hi = cpu_to_le32(blk >> 32);
}

/**
 * ext4_isize - Implements the isize operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline loff_t ext4_isize(struct super_block *sb,
				struct ext4_inode *raw_inode)
{
	if (ext4_has_feature_largedir(sb) ||
	    S_ISREG(le16_to_cpu(raw_inode->i_mode)))
		return ((loff_t)le32_to_cpu(raw_inode->i_size_high) << 32) |
			le32_to_cpu(raw_inode->i_size_lo);

	return (loff_t) le32_to_cpu(raw_inode->i_size_lo);
}

/**
 * ext4_isize_set - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_isize_set(struct ext4_inode *raw_inode, loff_t i_size)
{
	raw_inode->i_size_lo = cpu_to_le32(i_size);
	raw_inode->i_size_high = cpu_to_le32(i_size >> 32);
}


/**
 * ext4_get_groups_count - Computes derived filesystem state used for validation, accounting or policy decisions.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline ext4_group_t ext4_get_groups_count(struct super_block *sb)
{
	ext4_group_t	ngroups = EXT4_SB(sb)->s_groups_count;

	smp_rmb();
	return ngroups;
}

/**
 * ext4_flex_group - Implements the flex group operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline ext4_group_t ext4_flex_group(struct ext4_sb_info *sbi,
					     ext4_group_t block_group)
{
	return block_group >> sbi->s_log_groups_per_flex;
}

/**
 * ext4_flex_bg_size - Implements the flex bg size operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline unsigned int ext4_flex_bg_size(struct ext4_sb_info *sbi)
{
	return 1 << sbi->s_log_groups_per_flex;
}

/**
 * ext4_get_maxbytes - Implements the get maxbytes operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline loff_t ext4_get_maxbytes(struct inode *inode)
{
	if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		return inode->i_sb->s_maxbytes;
	return EXT4_SB(inode->i_sb)->s_bitmap_maxbytes;
}

#define ext4_std_error(sb, errno)				\
/**
 * ext4_std_error - Implements the std error operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
do {								\
	if ((errno))						\
		__ext4_std_error((sb), __func__, __LINE__, (errno));	\
}/**
 * ext4_update_i_disksize - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
 while (0)

#ifdef CONFIG_SMP


#define EXT4_FREECLUSTERS_WATERMARK (4 * (percpu_counter_batch * nr_cpu_ids))
#else
#define EXT4_FREECLUSTERS_WATERMARK 0
#endif


static inline void ext4_update_i_disksize(struct inode *inode, loff_t newsize)
{
	WARN_ON_ONCE(S_ISREG(inode->i_mode) &&
		     !inode_is_locked(inode));
	down_write(&EXT4_I(inode)->i_data_sem);
	if (newsize > EXT4_I(inode)->i_disksize)
		WRITE_ONCE(EXT4_I(inode)->i_disksize, newsize);
	up_write(&EXT4_I(inode)->i_data_sem);
}


/**
 * ext4_update_inode_size - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_update_inode_size(struct inode *inode, loff_t newsize)
{
	int changed = 0;

	if (newsize > inode->i_size) {
		i_size_write(inode, newsize);
		changed = 1;
	}
	if (newsize > EXT4_I(inode)->i_disksize) {
		ext4_update_i_disksize(inode, newsize);
		changed |= 2;
	}
	return changed;
}

int ext4_update_disksize_before_punch(struct inode *inode, loff_t offset,
				      loff_t len);

/**
 * struct ext4_group_info - Private EXT4 state/data structure used by ext4 shared model.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_group_info {
	unsigned long   bb_state;
#ifdef AGGRESSIVE_CHECK
	unsigned long	bb_check_counter;
#endif
	struct rb_root  bb_free_root;
	ext4_grpblk_t	bb_first_free;
	ext4_grpblk_t	bb_free;
	ext4_grpblk_t	bb_fragments;
	int		bb_avg_fragment_size_order;

	ext4_grpblk_t	bb_largest_free_order;
	ext4_group_t	bb_group;
	struct          list_head bb_prealloc_list;
#ifdef DOUBLE_CHECK
	void            *bb_bitmap;
#endif
	struct rw_semaphore alloc_sem;
	ext4_grpblk_t	bb_counters[];


};

#define EXT4_GROUP_INFO_NEED_INIT_BIT		0
#define EXT4_GROUP_INFO_WAS_TRIMMED_BIT		1
#define EXT4_GROUP_INFO_BBITMAP_CORRUPT_BIT	2
#define EXT4_GROUP_INFO_IBITMAP_CORRUPT_BIT	3
#define EXT4_GROUP_INFO_BBITMAP_CORRUPT		\
/**
 * ext4_group_lock_ptr - Implements the group lock ptr operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
	(1 << EXT4_GROUP_INFO_BBITMAP_CORRUPT_BIT)
#define EXT4_GROUP_INFO_IBITMAP_CORRUPT		\
	(1 << EXT4_GROUP_INFO_IBITMAP_CORRUPT_BIT)
#define EXT4_GROUP_INFO_BBITMAP_READ_BIT	4

#define EXT4_MB_GRP_NEED_INIT(grp)	\
	(test_bit(EXT4_GROUP_INFO_NEED_INIT_BIT, &((grp)->bb_state)))
#define EXT4_MB_GRP_BBITMAP_CORRUPT(grp)	\
	(test_bit(EXT4_GROUP_INFO_BBITMAP_CORRUPT_BIT, &((grp)->bb_state)))
#define EXT4_MB_GRP_IBITMAP_CORRUPT(grp)	\
	(test_bit(EXT4_GROUP_INFO_IBITMAP_CORRUPT_BIT, &((grp)->bb_state)))

#define EXT4_MB_GRP_WAS_TRIMMED(grp)	\
	(test_bit(EXT4_GROUP_INFO_WAS_TRIMMED_BIT, &((grp)->bb_state)))
#define EXT4_MB_GRP_SET_TRIMMED(grp)	\
	(set_bit(EXT4_GROUP_INFO_WAS_TRIMMED_BIT, &((grp)->bb_state)))
#define EXT4_MB_GRP_CLEAR_TRIMMED(grp)	\
	(clear_bit(EXT4_GROUP_INFO_WAS_TRIMMED_BIT, &((grp)->bb_state)))
#define EXT4_MB_GRP_TEST_AND_SET_READ(grp)	\
	(test_and_set_bit(EXT4_GROUP_INFO_BBITMAP_READ_BIT, &((grp)->bb_state)))

#define EXT4_MAX_CONTENTION		8
#define EXT4_CONTENTION_THRESHOLD	2

static inline spinlock_t *ext4_group_lock_ptr(struct super_block *sb,
					      ext4_group_t group)
{
	return bgl_lock_ptr(EXT4_SB(sb)->s_blockgroup_lock, group);
}


/**
 * ext4_fs_is_busy - Implements the fs is busy operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_fs_is_busy(struct ext4_sb_info *sbi)
{
	return (atomic_read(&sbi->s_lock_busy) > EXT4_CONTENTION_THRESHOLD);
}

/**
 * ext4_try_lock_group - Implements the try lock group operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_try_lock_group(struct super_block *sb, ext4_group_t group)
{
	if (!spin_trylock(ext4_group_lock_ptr(sb, group)))
		return false;


	atomic_add_unless(&EXT4_SB(sb)->s_lock_busy, -1, 0);
	return true;
}

/**
 * ext4_lock_group - Implements the lock group operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_lock_group(struct super_block *sb, ext4_group_t group)
{
	if (!ext4_try_lock_group(sb, group)) {


		atomic_add_unless(&EXT4_SB(sb)->s_lock_busy, 1,
				  EXT4_MAX_CONTENTION);
		spin_lock(ext4_group_lock_ptr(sb, group));
	}
}

/**
 * ext4_unlock_group - Implements the unlock group operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_unlock_group(struct super_block *sb,
					ext4_group_t group)
{
	spin_unlock(ext4_group_lock_ptr(sb, group));
}

#ifdef CONFIG_QUOTA
/**
 * ext4_quota_capable - Implements the quota capable operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_quota_capable(struct super_block *sb)
{
	return (test_opt(sb, QUOTA) || ext4_has_feature_quota(sb));
}

/**
 * ext4_is_quota_journalled - Implements the is quota journalled operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline bool ext4_is_quota_journalled(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);

	return (ext4_has_feature_quota(sb) ||
		sbi->s_qf_names[USRQUOTA] || sbi->s_qf_names[GRPQUOTA]);
}
int ext4_enable_quotas(struct super_block *sb);
#endif


#define ext4_check_indirect_blockref(inode, bh)				\
	ext4_check_blockref(__func__, __LINE__, inode,			\
			    (__le32 *)(bh)->b_data,			\
			    EXT4_ADDR_PER_BLOCK((inode)->i_sb))

#define ext4_ind_check_inode(inode)					\
	ext4_check_blockref(__func__, __LINE__, inode,			\
			    EXT4_I(inode)->i_data,			\
			    EXT4_NDIR_BLOCKS)


extern const struct file_operations ext4_dir_operations;


extern const struct inode_operations ext4_file_inode_operations;
extern const struct file_operations ext4_file_operations;
extern loff_t ext4_llseek(struct file *file, loff_t offset, int origin);


extern int ext4_get_max_inline_size(struct inode *inode);
extern int ext4_find_inline_data_nolock(struct inode *inode);
extern int ext4_destroy_inline_data(handle_t *handle, struct inode *inode);

int ext4_readpage_inline(struct inode *inode, struct folio *folio);
extern int ext4_try_to_write_inline_data(struct address_space *mapping,
					 struct inode *inode,
					 loff_t pos, unsigned len,
					 struct folio **foliop);
int ext4_write_inline_data_end(struct inode *inode, loff_t pos, unsigned len,
			       unsigned copied, struct folio *folio);
extern int ext4_da_write_inline_data_begin(struct address_space *mapping,
					   struct inode *inode,
					   loff_t pos, unsigned len,
					   struct folio **foliop,
					   void **fsdata);
extern int ext4_try_add_inline_entry(handle_t *handle,
				     struct ext4_filename *fname,
				     struct inode *dir, struct inode *inode);
extern int ext4_try_create_inline_dir(handle_t *handle,
				      struct inode *parent,
				      struct inode *inode);
extern int ext4_read_inline_dir(struct file *filp,
				struct dir_context *ctx,
				int *has_inline_data);
extern int ext4_inlinedir_to_tree(struct file *dir_file,
				  struct inode *dir, ext4_lblk_t block,
				  struct dx_hash_info *hinfo,
				  __u32 start_hash, __u32 start_minor_hash,
				  int *has_inline_data);
extern struct buffer_head *ext4_find_inline_entry(struct inode *dir,
					struct ext4_filename *fname,
					struct ext4_dir_entry_2 **res_dir,
					int *has_inline_data);
extern int ext4_delete_inline_entry(handle_t *handle,
				    struct inode *dir,
				    struct ext4_dir_entry_2 *de_del,
				    struct buffer_head *bh,
				    int *has_inline_data);
extern bool empty_inline_dir(struct inode *dir, int *has_inline_data);
extern struct buffer_head *ext4_get_first_inline_block(struct inode *inode,
					struct ext4_dir_entry_2 **parent_de,
					int *retval);
extern void *ext4_read_inline_link(struct inode *inode);

struct iomap;
extern int ext4_inline_data_iomap(struct inode *inode, struct iomap *iomap);

extern int ext4_inline_data_truncate(struct inode *inode, int *has_inline);

extern int ext4_convert_inline_data(struct inode *inode);

/**
 * ext4_has_inline_data - Implements the has inline data operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_has_inline_data(struct inode *inode)
{
	return ext4_test_inode_flag(inode, EXT4_INODE_INLINE_DATA) &&
	       EXT4_I(inode)->i_inline_off;
}


extern const struct inode_operations ext4_dir_inode_operations;
extern const struct inode_operations ext4_special_inode_operations;
extern struct dentry *ext4_get_parent(struct dentry *child);
extern struct ext4_dir_entry_2 *ext4_init_dot_dotdot(struct inode *inode,
				 struct ext4_dir_entry_2 *de,
				 int blocksize, int csum_size,
				 unsigned int parent_ino, int dotdot_real_len);
extern void ext4_initialize_dirent_tail(struct buffer_head *bh,
					unsigned int blocksize);
extern int ext4_handle_dirty_dirblock(handle_t *handle, struct inode *inode,
				      struct buffer_head *bh);
extern int __ext4_unlink(struct inode *dir, const struct qstr *d_name,
			 struct inode *inode, struct dentry *dentry);
extern int __ext4_link(struct inode *dir, struct inode *inode,
		       struct dentry *dentry);

#define S_SHIFT 12
static const unsigned char ext4_type_by_mode[(S_IFMT >> S_SHIFT) + 1] = {
	[S_IFREG >> S_SHIFT]	= EXT4_FT_REG_FILE,
	[S_IFDIR >> S_SHIFT]	= EXT4_FT_DIR,
	[S_IFCHR >> S_SHIFT]	= EXT4_FT_CHRDEV,
	[S_IFBLK >> S_SHIFT]	= EXT4_FT_BLKDEV,
	[S_IFIFO >> S_SHIFT]	= EXT4_FT_FIFO,
	[S_IFSOCK >> S_SHIFT]	= EXT4_FT_SOCK,
	[S_IFLNK >> S_SHIFT]	= EXT4_FT_SYMLINK,
};

/**
 * ext4_set_de_type - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_set_de_type(struct super_block *sb,
				struct ext4_dir_entry_2 *de,
				umode_t mode) {
	if (ext4_has_feature_filetype(sb))
		de->file_type = ext4_type_by_mode[(mode & S_IFMT)>>S_SHIFT];
}


extern int ext4_mpage_readpages(struct inode *inode,
		struct readahead_control *rac, struct folio *folio);
extern int __init ext4_init_post_read_processing(void);
extern void ext4_exit_post_read_processing(void);


extern const struct inode_operations ext4_encrypted_symlink_inode_operations;
extern const struct inode_operations ext4_symlink_inode_operations;
extern const struct inode_operations ext4_fast_symlink_inode_operations;


extern void ext4_notify_error_sysfs(struct ext4_sb_info *sbi);
extern int ext4_register_sysfs(struct super_block *sb);
extern void ext4_unregister_sysfs(struct super_block *sb);
extern int __init ext4_init_sysfs(void);
extern void ext4_exit_sysfs(void);


extern void ext4_release_system_zone(struct super_block *sb);
extern int ext4_setup_system_zone(struct super_block *sb);
extern int __init ext4_init_system_zone(void);
extern void ext4_exit_system_zone(void);
extern int ext4_inode_block_valid(struct inode *inode,
				  ext4_fsblk_t start_blk,
				  unsigned int count);
extern int ext4_check_blockref(const char *, unsigned int,
			       struct inode *, __le32 *, unsigned int);
extern int ext4_sb_block_valid(struct super_block *sb, struct inode *inode,
				ext4_fsblk_t start_blk, unsigned int count);


struct ext4_ext_path;
struct ext4_extent;


#define EXT_MAX_BLOCKS	0xffffffff

extern void ext4_ext_tree_init(handle_t *handle, struct inode *inode);
extern int ext4_ext_index_trans_blocks(struct inode *inode, int extents);
extern int ext4_ext_map_blocks(handle_t *handle, struct inode *inode,
			       struct ext4_map_blocks *map, int flags);
extern int ext4_ext_truncate(handle_t *, struct inode *);
extern int ext4_ext_remove_space(struct inode *inode, ext4_lblk_t start,
				 ext4_lblk_t end);
extern void ext4_ext_init(struct super_block *);
extern void ext4_ext_release(struct super_block *);
extern long ext4_fallocate(struct file *file, int mode, loff_t offset,
			  loff_t len);
extern int ext4_convert_unwritten_extents(handle_t *handle, struct inode *inode,
					  loff_t offset, ssize_t len);
extern int ext4_convert_unwritten_io_end_vec(handle_t *handle,
					     ext4_io_end_t *io_end);
extern int ext4_map_blocks(handle_t *handle, struct inode *inode,
			   struct ext4_map_blocks *map, int flags);
extern int ext4_ext_calc_credits_for_single_extent(struct inode *inode,
						   int num,
						   struct ext4_ext_path *path);
extern struct ext4_ext_path *ext4_ext_insert_extent(
				handle_t *handle, struct inode *inode,
				struct ext4_ext_path *path,
				struct ext4_extent *newext, int gb_flags);
extern struct ext4_ext_path *ext4_find_extent(struct inode *, ext4_lblk_t,
					      struct ext4_ext_path *,
					      int flags);
extern void ext4_free_ext_path(struct ext4_ext_path *);
extern int ext4_ext_check_inode(struct inode *inode);
extern ext4_lblk_t ext4_ext_next_allocated_block(struct ext4_ext_path *path);
extern int ext4_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
			__u64 start, __u64 len);
extern int ext4_get_es_cache(struct inode *inode,
			     struct fiemap_extent_info *fieinfo,
			     __u64 start, __u64 len);
extern int ext4_ext_precache(struct inode *inode);
extern int ext4_swap_extents(handle_t *handle, struct inode *inode1,
				struct inode *inode2, ext4_lblk_t lblk1,
			     ext4_lblk_t lblk2,  ext4_lblk_t count,
			     int mark_unwritten,int *err);
extern int ext4_clu_mapped(struct inode *inode, ext4_lblk_t lclu);
extern int ext4_datasem_ensure_credits(handle_t *handle, struct inode *inode,
				       int check_cred, int restart_cred,
				       int revoke_cred);
extern void ext4_ext_replay_shrink_inode(struct inode *inode, ext4_lblk_t end);
extern int ext4_ext_replay_set_iblocks(struct inode *inode);
extern int ext4_ext_replay_update_ex(struct inode *inode, ext4_lblk_t start,
		int len, int unwritten, ext4_fsblk_t pblk);
extern int ext4_ext_clear_bb(struct inode *inode);


extern void ext4_double_down_write_data_sem(struct inode *first,
					    struct inode *second);
extern void ext4_double_up_write_data_sem(struct inode *orig_inode,
					  struct inode *donor_inode);
extern int ext4_move_extents(struct file *o_filp, struct file *d_filp,
			     __u64 start_orig, __u64 start_donor,
			     __u64 len, __u64 *moved_len);


extern int __init ext4_init_pageio(void);
extern void ext4_exit_pageio(void);
extern ext4_io_end_t *ext4_init_io_end(struct inode *inode, gfp_t flags);
extern ext4_io_end_t *ext4_get_io_end(ext4_io_end_t *io_end);
extern int ext4_put_io_end(ext4_io_end_t *io_end);
extern void ext4_put_io_end_defer(ext4_io_end_t *io_end);
extern void ext4_io_submit_init(struct ext4_io_submit *io,
				struct writeback_control *wbc);
extern void ext4_end_io_rsv_work(struct work_struct *work);
extern void ext4_io_submit(struct ext4_io_submit *io);
int ext4_bio_write_folio(struct ext4_io_submit *io, struct folio *page,
		size_t len);
extern struct ext4_io_end_vec *ext4_alloc_io_end_vec(ext4_io_end_t *io_end);
extern struct ext4_io_end_vec *ext4_last_io_end_vec(ext4_io_end_t *io_end);


extern int ext4_multi_mount_protect(struct super_block *, ext4_fsblk_t);


extern void ext4_stop_mmpd(struct ext4_sb_info *sbi);


extern const struct fsverity_operations ext4_verityops;


extern int ext4_orphan_add(handle_t *, struct inode *);
extern int ext4_orphan_del(handle_t *, struct inode *);
extern void ext4_orphan_cleanup(struct super_block *sb,
				struct ext4_super_block *es);
extern void ext4_release_orphan_info(struct super_block *sb);
extern int ext4_init_orphan_info(struct super_block *sb);
extern int ext4_orphan_file_empty(struct super_block *sb);
extern void ext4_orphan_file_block_trigger(
				struct jbd2_buffer_trigger_type *triggers,
				struct buffer_head *bh,
				void *data, size_t size);


#define BH_BITMAP_UPTODATE BH_JBDPrivateStart

/**
 * bitmap_uptodate - Implements the bitmap uptodate operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int bitmap_uptodate(struct buffer_head *bh)
{
	return (buffer_uptodate(bh) &&
			test_bit(BH_BITMAP_UPTODATE, &(bh)->b_state));
}
/**
 * set_bitmap_uptodate - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void set_bitmap_uptodate(struct buffer_head *bh)
{
	set_bit(BH_BITMAP_UPTODATE, &(bh)->b_state);
}


#define EXT4_WQ_HASH_SZ		37
#define ext4_ioend_wq(v)   (&ext4__ioend_wq[((unsigned long)(v)) %\
					    EXT4_WQ_HASH_SZ])
extern wait_queue_head_t ext4__ioend_wq[EXT4_WQ_HASH_SZ];

extern int ext4_resize_begin(struct super_block *sb);
extern int ext4_resize_end(struct super_block *sb, bool update_backups);

/**
 * ext4_set_io_unwritten_flag - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_set_io_unwritten_flag(struct inode *inode,
					      struct ext4_io_end *io_end)
{
	if (!(io_end->flag & EXT4_IO_END_UNWRITTEN)) {
		io_end->flag |= EXT4_IO_END_UNWRITTEN;
		atomic_inc(&EXT4_I(inode)->i_unwritten);
	}
}

/**
 * ext4_clear_io_unwritten_flag - Implements the clear io unwritten flag operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_clear_io_unwritten_flag(ext4_io_end_t *io_end)
{
	struct inode *inode = io_end->inode;

	if (io_end->flag & EXT4_IO_END_UNWRITTEN) {
		io_end->flag &= ~EXT4_IO_END_UNWRITTEN;

		if (atomic_dec_and_test(&EXT4_I(inode)->i_unwritten))
			wake_up_all(ext4_ioend_wq(inode));
	}
}

extern const struct iomap_ops ext4_iomap_ops;
extern const struct iomap_ops ext4_iomap_overwrite_ops;
extern const struct iomap_ops ext4_iomap_report_ops;

/**
 * ext4_buffer_uptodate - Implements the buffer uptodate operation within the ext4 shared model subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_buffer_uptodate(struct buffer_head *bh)
{


	if (buffer_write_io_error(bh))
		set_buffer_uptodate(bh);
	return buffer_uptodate(bh);
}

extern int ext4_block_write_begin(handle_t *handle, struct folio *folio,
				  loff_t pos, unsigned len,
				  get_block_t *get_block);
#endif

#define EFSBADCRC	EBADMSG
#define EFSCORRUPTED	EUCLEAN

#endif
