// SPDX-License-Identifier: GPL-2.0

/*
 * EXT4 — Extent-status interfaces
 *
 * Purpose:
 *   Defines extent-status states, range representation and cache operations shared by EXT4 mapping/allocation paths.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Status transitions describe logical range state and must agree with delayed allocation, unwritten extents and durable mapping state.
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

#ifndef _EXT4_EXTENTS_STATUS_H
#define _EXT4_EXTENTS_STATUS_H


#ifdef ES_DEBUG__
#define es_debug(fmt, ...)	printk(fmt, ##__VA_ARGS__)
#else
#define es_debug(fmt, ...)	no_printk(fmt, ##__VA_ARGS__)
#endif


#define ES_AGGRESSIVE_TEST__


/**
 * no_printk - Implements the no printk operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
enum {
	ES_WRITTEN_B,
	ES_UNWRITTEN_B,
	ES_DELAYED_B,
	ES_HOLE_B,
	ES_REFERENCED_B,
	ES_FLAGS
};

#define ES_SHIFT (sizeof(ext4_fsblk_t)*8 - ES_FLAGS)
#define ES_MASK (~((ext4_fsblk_t)0) << ES_SHIFT)


#define EXTENT_STATUS_WRITTEN	(1 << ES_WRITTEN_B)
#define EXTENT_STATUS_UNWRITTEN (1 << ES_UNWRITTEN_B)
#define EXTENT_STATUS_DELAYED	(1 << ES_DELAYED_B)
#define EXTENT_STATUS_HOLE	(1 << ES_HOLE_B)
#define EXTENT_STATUS_REFERENCED	(1 << ES_REFERENCED_B)

#define ES_TYPE_MASK	((ext4_fsblk_t)(EXTENT_STATUS_WRITTEN | \
			  EXTENT_STATUS_UNWRITTEN | \
			  EXTENT_STATUS_DELAYED | \
			  EXTENT_STATUS_HOLE))

#define ES_TYPE_VALID(type)	((type) && !((type) & ((type) - 1)))

struct ext4_sb_info;
struct ext4_extent;

/**
 * struct extent_status - Private EXT4 state/data structure used by extent-status interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct extent_status {
	struct rb_node rb_node;
	ext4_lblk_t es_lblk;
	ext4_lblk_t es_len;
	ext4_fsblk_t es_pblk;
};

/**
 * struct ext4_es_tree - Private EXT4 state/data structure used by extent-status interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_es_tree {
	struct rb_root root;
	struct extent_status *cache_es;
};

/**
 * struct ext4_es_stats - Private EXT4 state/data structure used by extent-status interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_es_stats {
	unsigned long es_stats_shrunk;
	struct percpu_counter es_stats_cache_hits;
	struct percpu_counter es_stats_cache_misses;
	u64 es_stats_scan_time;
	u64 es_stats_max_scan_time;
	struct percpu_counter es_stats_all_cnt;
	struct percpu_counter es_stats_shk_cnt;
};


/**
 * struct pending_reservation - Private EXT4 state/data structure used by extent-status interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct pending_reservation {
	struct rb_node rb_node;
	ext4_lblk_t lclu;
};

/**
 * struct ext4_pending_tree - Private EXT4 state/data structure used by extent-status interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_pending_tree {
	struct rb_root root;
};

extern int __init ext4_init_es(void);
extern void ext4_exit_es(void);
extern void ext4_es_init_tree(struct ext4_es_tree *tree);

extern void ext4_es_insert_extent(struct inode *inode, ext4_lblk_t lblk,
				  ext4_lblk_t len, ext4_fsblk_t pblk,
				  unsigned int status, int flags);
extern void ext4_es_cache_extent(struct inode *inode, ext4_lblk_t lblk,
				 ext4_lblk_t len, ext4_fsblk_t pblk,
				 unsigned int status);
extern void ext4_es_remove_extent(struct inode *inode, ext4_lblk_t lblk,
				  ext4_lblk_t len);
extern void ext4_es_find_extent_range(struct inode *inode,
				      int (*match_fn)(struct extent_status *es),
				      ext4_lblk_t lblk, ext4_lblk_t end,
				      struct extent_status *es);
extern int ext4_es_lookup_extent(struct inode *inode, ext4_lblk_t lblk,
				 ext4_lblk_t *next_lblk,
				 struct extent_status *es);
extern bool ext4_es_scan_range(struct inode *inode,
			       int (*matching_fn)(struct extent_status *es),
			       ext4_lblk_t lblk, ext4_lblk_t end);
extern bool ext4_es_scan_clu(struct inode *inode,
			     int (*matching_fn)(struct extent_status *es),
			     ext4_lblk_t lblk);

/**
 * ext4_es_status - Implements the es status operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline unsigned int ext4_es_status(struct extent_status *es)
{
	return es->es_pblk >> ES_SHIFT;
}

/**
 * ext4_es_type - Implements the es type operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline unsigned int ext4_es_type(struct extent_status *es)
{
	return (es->es_pblk >> ES_SHIFT) & ES_TYPE_MASK;
}

/**
 * ext4_es_is_written - Implements the es is written operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_es_is_written(struct extent_status *es)
{
	return (ext4_es_type(es) & EXTENT_STATUS_WRITTEN) != 0;
}

/**
 * ext4_es_is_unwritten - Implements the es is unwritten operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_es_is_unwritten(struct extent_status *es)
{
	return (ext4_es_type(es) & EXTENT_STATUS_UNWRITTEN) != 0;
}

/**
 * ext4_es_is_delayed - Implements the es is delayed operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_es_is_delayed(struct extent_status *es)
{
	return (ext4_es_type(es) & EXTENT_STATUS_DELAYED) != 0;
}

/**
 * ext4_es_is_hole - Implements the es is hole operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_es_is_hole(struct extent_status *es)
{
	return (ext4_es_type(es) & EXTENT_STATUS_HOLE) != 0;
}

/**
 * ext4_es_is_mapped - Implements the es is mapped operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_es_is_mapped(struct extent_status *es)
{
	return (ext4_es_is_written(es) || ext4_es_is_unwritten(es));
}

/**
 * ext4_es_set_referenced - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_es_set_referenced(struct extent_status *es)
{
	es->es_pblk |= ((ext4_fsblk_t)EXTENT_STATUS_REFERENCED) << ES_SHIFT;
}

/**
 * ext4_es_clear_referenced - Implements the es clear referenced operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_es_clear_referenced(struct extent_status *es)
{
	es->es_pblk &= ~(((ext4_fsblk_t)EXTENT_STATUS_REFERENCED) << ES_SHIFT);
}

/**
 * ext4_es_is_referenced - Implements the es is referenced operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline int ext4_es_is_referenced(struct extent_status *es)
{
	return (ext4_es_status(es) & EXTENT_STATUS_REFERENCED) != 0;
}

/**
 * ext4_es_pblock - Implements the es pblock operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline ext4_fsblk_t ext4_es_pblock(struct extent_status *es)
{
	return es->es_pblk & ~ES_MASK;
}

/**
 * ext4_es_show_pblock - Implements the es show pblock operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline ext4_fsblk_t ext4_es_show_pblock(struct extent_status *es)
{
	ext4_fsblk_t pblock = ext4_es_pblock(es);
	return pblock == ~ES_MASK ? 0 : pblock;
}

/**
 * ext4_es_store_pblock - Implements the es store pblock operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_es_store_pblock(struct extent_status *es,
					ext4_fsblk_t pb)
{
	ext4_fsblk_t block;

	block = (pb & ~ES_MASK) | (es->es_pblk & ES_MASK);
	es->es_pblk = block;
}

/**
 * ext4_es_store_pblock_status - Implements the es store pblock status operation within the extent-status interfaces subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline void ext4_es_store_pblock_status(struct extent_status *es,
					       ext4_fsblk_t pb,
					       unsigned int status)
{
	WARN_ON_ONCE(!ES_TYPE_VALID(status & ES_TYPE_MASK));

	es->es_pblk = (((ext4_fsblk_t)status << ES_SHIFT) & ES_MASK) |
		      (pb & ~ES_MASK);
}

extern int ext4_es_register_shrinker(struct ext4_sb_info *sbi);
extern void ext4_es_unregister_shrinker(struct ext4_sb_info *sbi);

extern int ext4_seq_es_shrinker_info_show(struct seq_file *seq, void *v);

extern int __init ext4_init_pending(void);
extern void ext4_exit_pending(void);
extern void ext4_init_pending_tree(struct ext4_pending_tree *tree);
extern void ext4_remove_pending(struct inode *inode, ext4_lblk_t lblk);
extern bool ext4_is_pending(struct inode *inode, ext4_lblk_t lblk);
extern void ext4_es_insert_delayed_extent(struct inode *inode, ext4_lblk_t lblk,
					  ext4_lblk_t len, bool lclu_allocated,
					  bool end_allocated);
extern void ext4_clear_inode_es(struct inode *inode);

#endif
