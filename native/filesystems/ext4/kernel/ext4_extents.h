// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2003-2006, Cluster File Systems, Inc, info@clusterfs.com
 * Written by Alex Tomas <alex@clusterfs.com>
 */

/*
 * EXT4 — Extent-tree representation
 *
 * Purpose:
 *   Defines the persistent and in-memory structures used by EXT4 extent trees.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Extent headers, indexes and leaves are on-disk data structures; depth, entry counts and logical/physical ranges require validation before traversal.
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

#ifndef _EXT4_EXTENTS
#define _EXT4_EXTENTS

#include "ext4.h"


#define AGGRESSIVE_TEST_


#define EXTENTS_STATS__


#define CHECK_BINSEARCH__


#define EXT_STATS_


/**
 * struct ext4_extent_tail - Private EXT4 state/data structure used by extent-tree representation.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_extent_tail {
	__le32	et_checksum;
};


/**
 * struct ext4_extent - Private EXT4 state/data structure used by extent-tree representation.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_extent {
	__le32	ee_block;
	__le16	ee_len;
	__le16	ee_start_hi;
	__le32	ee_start_lo;
};


/**
 * struct ext4_extent_idx - Private EXT4 state/data structure used by extent-tree representation.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_extent_idx {
	__le32	ei_block;
	__le32	ei_leaf_lo;

	__le16	ei_leaf_hi;
	__u16	ei_unused;
};


/**
 * struct ext4_extent_header - Private EXT4 state/data structure used by extent-tree representation.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_extent_header {
	__le16	eh_magic;
	__le16	eh_entries;
	__le16	eh_max;
	__le16	eh_depth;
	__le32	eh_generation;
};

#define EXT4_EXT_MAGIC		cpu_to_le16(0xf30a)
#define EXT4_MAX_EXTENT_DEPTH 5

#define EXT4_EXTENT_TAIL_OFFSET(hdr) \
	(sizeof(struct ext4_extent_header) + \
	 (sizeof(struct ext4_extent) * le16_to_cpu((hdr)->eh_max)))

/**
 * find_ext4_extent_tail - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline struct ext4_extent_tail *
find_ext4_extent_tail(struct ext4_extent_header *eh)
{
	return (struct ext4_extent_tail *)(((void *)eh) +
					   EXT4_EXTENT_TAIL_OFFSET(eh));
}


/**
 * struct ext4_ext_path - Private EXT4 state/data structure used by extent-tree representation.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_ext_path {
	ext4_fsblk_t			p_block;
	__u16				p_depth;
	__u16				p_maxdepth;
	struct ext4_extent		*p_ext;
	struct ext4_extent_idx		*p_idx;
	struct ext4_extent_header	*p_hdr;
	struct buffer_head		*p_bh;
};


/**
 * struct partial_cluster - Private EXT4 state/data structure used by extent-tree representation.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct partial_cluster {
	ext4_fsblk_t pclu;
	ext4_lblk_t lblk;
	enum {initial, tofree, nofree} state;
};


#define EXT_INIT_MAX_LEN	(1UL << 15)
#define EXT_UNWRITTEN_MAX_LEN	(EXT_INIT_MAX_LEN - 1)


#define EXT_FIRST_EXTENT(__hdr__) \
	((struct ext4_extent *) (((char *) (__hdr__)) +		\
				 sizeof(struct ext4_extent_header)))
#define EXT_FIRST_INDEX(__hdr__) \
	((struct ext4_extent_idx *) (((char *) (__hdr__)) +	\
				     sizeof(struct ext4_extent_header)))
#define EXT_HAS_FREE_INDEX(__path__) \
	(le16_to_cpu((__path__)->p_hdr->eh_entries) \
				     < le16_to_cpu((__path__)->p_hdr->eh_max))
#define EXT_LAST_EXTENT(__hdr__) \
	(EXT_FIRST_EXTENT((__hdr__)) + le16_to_cpu((__hdr__)->eh_entries) - 1)
#define EXT_LAST_INDEX(__hdr__) \
	(EXT_FIRST_INDEX((__hdr__)) + le16_to_cpu((__hdr__)->eh_entries) - 1)
#define EXT_MAX_EXTENT(__hdr__)	\
	((le16_to_cpu((__hdr__)->eh_max)) ? \
	((EXT_FIRST_EXTENT((__hdr__)) + le16_to_cpu((__hdr__)->eh_max) - 1)) \
					: NULL)
#define EXT_MAX_INDEX(__hdr__) \
	((le16_to_cpu((__hdr__)->eh_max)) ? \
	((EXT_FIRST_INDEX((__hdr__)) + le16_to_cpu((__hdr__)->eh_max) - 1)) \
					: NULL)

/**
 * ext_inode_hdr - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline struct ext4_extent_header *ext_inode_hdr(struct inode *inode)
{
	return (struct ext4_extent_header *) EXT4_I(inode)->i_data;
}

/**
 * ext_block_hdr - Implements the ext block hdr operation within the extent-tree representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline struct ext4_extent_header *ext_block_hdr(struct buffer_head *bh)
{
	return (struct ext4_extent_header *) bh->b_data;
}

/**
 * ext_depth - Implements the ext depth operation within the extent-tree representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline unsigned short ext_depth(struct inode *inode)
{
	return le16_to_cpu(ext_inode_hdr(inode)->eh_depth);
}

/**
 * ext4_ext_mark_unwritten - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_ext_mark_unwritten(struct ext4_extent *ext)
{

	BUG_ON((le16_to_cpu(ext->ee_len) & ~EXT_INIT_MAX_LEN) == 0);
	ext->ee_len |= cpu_to_le16(EXT_INIT_MAX_LEN);
}

/**
 * ext4_ext_is_unwritten - Implements the ext is unwritten operation within the extent-tree representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_ext_is_unwritten(struct ext4_extent *ext)
{

	return (le16_to_cpu(ext->ee_len) > EXT_INIT_MAX_LEN);
}

/**
 * ext4_ext_get_actual_len - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext4_ext_get_actual_len(struct ext4_extent *ext)
{
	return (le16_to_cpu(ext->ee_len) <= EXT_INIT_MAX_LEN ?
		le16_to_cpu(ext->ee_len) :
		(le16_to_cpu(ext->ee_len) - EXT_INIT_MAX_LEN));
}

/**
 * ext4_ext_mark_initialized - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_ext_mark_initialized(struct ext4_extent *ext)
{
	ext->ee_len = cpu_to_le16(ext4_ext_get_actual_len(ext));
}


/**
 * ext4_ext_pblock - Implements the ext pblock operation within the extent-tree representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline ext4_fsblk_t ext4_ext_pblock(struct ext4_extent *ex)
{
	ext4_fsblk_t block;

	block = le32_to_cpu(ex->ee_start_lo);
	block |= ((ext4_fsblk_t) le16_to_cpu(ex->ee_start_hi) << 31) << 1;
	return block;
}


/**
 * ext4_idx_pblock - Implements the idx pblock operation within the extent-tree representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline ext4_fsblk_t ext4_idx_pblock(struct ext4_extent_idx *ix)
{
	ext4_fsblk_t block;

	block = le32_to_cpu(ix->ei_leaf_lo);
	block |= ((ext4_fsblk_t) le16_to_cpu(ix->ei_leaf_hi) << 31) << 1;
	return block;
}


/**
 * ext4_ext_store_pblock - Implements the ext store pblock operation within the extent-tree representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_ext_store_pblock(struct ext4_extent *ex,
					 ext4_fsblk_t pb)
{
	ex->ee_start_lo = cpu_to_le32((unsigned long) (pb & 0xffffffff));
	ex->ee_start_hi = cpu_to_le16((unsigned long) ((pb >> 31) >> 1) &
				      0xffff);
}


/**
 * ext4_idx_store_pblock - Implements the idx store pblock operation within the extent-tree representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_idx_store_pblock(struct ext4_extent_idx *ix,
					 ext4_fsblk_t pb)
{
	ix->ei_leaf_lo = cpu_to_le32((unsigned long) (pb & 0xffffffff));
	ix->ei_leaf_hi = cpu_to_le16((unsigned long) ((pb >> 31) >> 1) &
				     0xffff);
}

#endif
