// SPDX-License-Identifier: GPL-2.0

/*
 * EXT4 — Extent-status cache
 *
 * Purpose:
 *   Maintains the in-memory logical-range cache used to accelerate extent state queries and delayed-allocation tracking.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   The cache may be discarded and rebuilt; it must never become more authoritative than persistent extent and allocation state.
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

#include <linux/list_sort.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include "ext4.h"

#include <trace/events/ext4.h>


static struct kmem_cache *ext4_es_cachep;
static struct kmem_cache *ext4_pending_cachep;

static int __es_insert_extent(struct inode *inode, struct extent_status *newes,
			      struct extent_status *prealloc);
static int __es_remove_extent(struct inode *inode, ext4_lblk_t lblk,
			      ext4_lblk_t end, int *reserved,
			      struct extent_status *prealloc);
static int es_reclaim_extents(struct ext4_inode_info *ei, int *nr_to_scan);
static int __es_shrink(struct ext4_sb_info *sbi, int nr_to_scan,
		       struct ext4_inode_info *locked_ei);
static int __revise_pending(struct inode *inode, ext4_lblk_t lblk,
			    ext4_lblk_t len,
			    struct pending_reservation **prealloc);


/**
 * ext4_init_es - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int __init ext4_init_es(void)
{
	ext4_es_cachep = KMEM_CACHE(extent_status, SLAB_RECLAIM_ACCOUNT);
	if (ext4_es_cachep == NULL)
		return -ENOMEM;
	return 0;
}


/**
 * ext4_exit_es - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_exit_es(void)
{
	kmem_cache_destroy(ext4_es_cachep);
}


/**
 * ext4_es_init_tree - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_es_init_tree(struct ext4_es_tree *tree)
{
	tree->root = RB_ROOT;
	tree->cache_es = NULL;
}

#ifdef ES_DEBUG__


/**
 * ext4_es_print_tree - Implements the es print tree operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_es_print_tree(struct inode *inode)
{
	struct ext4_es_tree *tree;
	struct rb_node *node;

	printk(KERN_DEBUG "status extents for inode %lu:", inode->i_ino);
	tree = &EXT4_I(inode)->i_es_tree;
	node = rb_first(&tree->root);
	while (node) {
		struct extent_status *es;
		es = rb_entry(node, struct extent_status, rb_node);
		printk(KERN_DEBUG " [%u/%u) %llu %x",
		       es->es_lblk, es->es_len,
		       ext4_es_pblock(es), ext4_es_status(es));
		node = rb_next(node);
	}
	printk(KERN_DEBUG "\n");
}
#else
#define ext4_es_print_tree(inode)
#endif


/**
 * ext4_es_end - Implements the es end operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline ext4_lblk_t ext4_es_end(struct extent_status *es)
{
	BUG_ON(es->es_lblk + es->es_len < es->es_lblk);
	return es->es_lblk + es->es_len - 1;
}


/**
 * __es_tree_search - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct extent_status *__es_tree_search(struct rb_root *root,
					      ext4_lblk_t lblk)
{
	struct rb_node *node = root->rb_node;
	struct extent_status *es = NULL;

	while (node) {
		es = rb_entry(node, struct extent_status, rb_node);
		if (lblk < es->es_lblk)
			node = node->rb_left;
		else if (lblk > ext4_es_end(es))
			node = node->rb_right;
		else
			return es;
	}

	if (es && lblk < es->es_lblk)
		return es;

	if (es && lblk > ext4_es_end(es)) {
		node = rb_next(&es->rb_node);
		return node ? rb_entry(node, struct extent_status, rb_node) :
			      NULL;
	}

	return NULL;
}


/**
 * __es_find_extent_range - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void __es_find_extent_range(struct inode *inode,
				   int (*matching_fn)(struct extent_status *es),
				   ext4_lblk_t lblk, ext4_lblk_t end,
				   struct extent_status *es)
{
	struct ext4_es_tree *tree = NULL;
	struct extent_status *es1 = NULL;
	struct rb_node *node;

	WARN_ON(es == NULL);
	WARN_ON(end < lblk);

	tree = &EXT4_I(inode)->i_es_tree;


	es->es_lblk = es->es_len = es->es_pblk = 0;
	es1 = READ_ONCE(tree->cache_es);
	if (es1 && in_range(lblk, es1->es_lblk, es1->es_len)) {
		es_debug("%u cached by [%u/%u) %llu %x\n",
			 lblk, es1->es_lblk, es1->es_len,
			 ext4_es_pblock(es1), ext4_es_status(es1));
		goto out;
	}

	es1 = __es_tree_search(&tree->root, lblk);

out:
	if (es1 && !matching_fn(es1)) {
		while ((node = rb_next(&es1->rb_node)) != NULL) {
			es1 = rb_entry(node, struct extent_status, rb_node);
			if (es1->es_lblk > end) {
				es1 = NULL;
				break;
			}
			if (matching_fn(es1))
				break;
		}
	}

	if (es1 && matching_fn(es1)) {
		WRITE_ONCE(tree->cache_es, es1);
		es->es_lblk = es1->es_lblk;
		es->es_len = es1->es_len;
		es->es_pblk = es1->es_pblk;
	}

}


/**
 * ext4_es_find_extent_range - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_es_find_extent_range(struct inode *inode,
			       int (*matching_fn)(struct extent_status *es),
			       ext4_lblk_t lblk, ext4_lblk_t end,
			       struct extent_status *es)
{
	es->es_lblk = es->es_len = es->es_pblk = 0;

	if (EXT4_SB(inode->i_sb)->s_mount_state & EXT4_FC_REPLAY)
		return;

	trace_ext4_es_find_extent_range_enter(inode, lblk);

	read_lock(&EXT4_I(inode)->i_es_lock);
	__es_find_extent_range(inode, matching_fn, lblk, end, es);
	read_unlock(&EXT4_I(inode)->i_es_lock);

	trace_ext4_es_find_extent_range_exit(inode, es);
}


/**
 * __es_scan_range - Implements the es scan range operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool __es_scan_range(struct inode *inode,
			    int (*matching_fn)(struct extent_status *es),
			    ext4_lblk_t start, ext4_lblk_t end)
{
	struct extent_status es;

	__es_find_extent_range(inode, matching_fn, start, end, &es);
	if (es.es_len == 0)
		return false;
	else if (es.es_lblk <= start &&
		 start < es.es_lblk + es.es_len)
		return true;
	else if (start <= es.es_lblk && es.es_lblk <= end)
		return true;
	else
		return false;
}


/**
 * ext4_es_scan_range - Implements the es scan range operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
bool ext4_es_scan_range(struct inode *inode,
			int (*matching_fn)(struct extent_status *es),
			ext4_lblk_t lblk, ext4_lblk_t end)
{
	bool ret;

	if (EXT4_SB(inode->i_sb)->s_mount_state & EXT4_FC_REPLAY)
		return false;

	read_lock(&EXT4_I(inode)->i_es_lock);
	ret = __es_scan_range(inode, matching_fn, lblk, end);
	read_unlock(&EXT4_I(inode)->i_es_lock);

	return ret;
}


/**
 * __es_scan_clu - Implements the es scan clu operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool __es_scan_clu(struct inode *inode,
			  int (*matching_fn)(struct extent_status *es),
			  ext4_lblk_t lblk)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	ext4_lblk_t lblk_start, lblk_end;

	lblk_start = EXT4_LBLK_CMASK(sbi, lblk);
	lblk_end = lblk_start + sbi->s_cluster_ratio - 1;

	return __es_scan_range(inode, matching_fn, lblk_start, lblk_end);
}


/**
 * ext4_es_scan_clu - Implements the es scan clu operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
bool ext4_es_scan_clu(struct inode *inode,
		      int (*matching_fn)(struct extent_status *es),
		      ext4_lblk_t lblk)
{
	bool ret;

	if (EXT4_SB(inode->i_sb)->s_mount_state & EXT4_FC_REPLAY)
		return false;

	read_lock(&EXT4_I(inode)->i_es_lock);
	ret = __es_scan_clu(inode, matching_fn, lblk);
	read_unlock(&EXT4_I(inode)->i_es_lock);

	return ret;
}


/**
 * ext4_es_list_add - Implements the es list add operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_es_list_add(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

	if (!list_empty(&ei->i_es_list))
		return;

	spin_lock(&sbi->s_es_lock);
	if (list_empty(&ei->i_es_list)) {
		list_add_tail(&ei->i_es_list, &sbi->s_es_list);
		sbi->s_es_nr_inode++;
	}
	spin_unlock(&sbi->s_es_lock);
}


/**
 * ext4_es_list_del - Implements the es list del operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_es_list_del(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

	spin_lock(&sbi->s_es_lock);
	if (!list_empty(&ei->i_es_list)) {
		list_del_init(&ei->i_es_list);
		sbi->s_es_nr_inode--;
		WARN_ON_ONCE(sbi->s_es_nr_inode < 0);
	}
	spin_unlock(&sbi->s_es_lock);
}


/**
 * __alloc_pending - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline struct pending_reservation *__alloc_pending(bool nofail)
{
	if (!nofail)
		return kmem_cache_alloc(ext4_pending_cachep, GFP_ATOMIC);

	return kmem_cache_zalloc(ext4_pending_cachep, GFP_KERNEL | __GFP_NOFAIL);
}


/**
 * __free_pending - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void __free_pending(struct pending_reservation *pr)
{
	kmem_cache_free(ext4_pending_cachep, pr);
}


/**
 * ext4_es_must_keep - Implements the es must keep operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline bool ext4_es_must_keep(struct extent_status *es)
{

	if (ext4_es_is_delayed(es))
		return true;

	return false;
}


/**
 * __es_alloc_extent - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline struct extent_status *__es_alloc_extent(bool nofail)
{
	if (!nofail)
		return kmem_cache_alloc(ext4_es_cachep, GFP_ATOMIC);

	return kmem_cache_zalloc(ext4_es_cachep, GFP_KERNEL | __GFP_NOFAIL);
}


/**
 * ext4_es_init_extent - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_es_init_extent(struct inode *inode, struct extent_status *es,
		ext4_lblk_t lblk, ext4_lblk_t len, ext4_fsblk_t pblk)
{
	es->es_lblk = lblk;
	es->es_len = len;
	es->es_pblk = pblk;


	if (!ext4_es_must_keep(es)) {
		if (!EXT4_I(inode)->i_es_shk_nr++)
			ext4_es_list_add(inode);
		percpu_counter_inc(&EXT4_SB(inode->i_sb)->
					s_es_stats.es_stats_shk_cnt);
	}

	EXT4_I(inode)->i_es_all_nr++;
	percpu_counter_inc(&EXT4_SB(inode->i_sb)->s_es_stats.es_stats_all_cnt);
}


/**
 * __es_free_extent - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void __es_free_extent(struct extent_status *es)
{
	kmem_cache_free(ext4_es_cachep, es);
}


/**
 * ext4_es_free_extent - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_es_free_extent(struct inode *inode, struct extent_status *es)
{
	EXT4_I(inode)->i_es_all_nr--;
	percpu_counter_dec(&EXT4_SB(inode->i_sb)->s_es_stats.es_stats_all_cnt);


	if (!ext4_es_must_keep(es)) {
		BUG_ON(EXT4_I(inode)->i_es_shk_nr == 0);
		if (!--EXT4_I(inode)->i_es_shk_nr)
			ext4_es_list_del(inode);
		percpu_counter_dec(&EXT4_SB(inode->i_sb)->
					s_es_stats.es_stats_shk_cnt);
	}

	__es_free_extent(es);
}


/**
 * ext4_es_can_be_merged - Implements the es can be merged operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_es_can_be_merged(struct extent_status *es1,
				 struct extent_status *es2)
{
	if (ext4_es_type(es1) != ext4_es_type(es2))
		return 0;

	if (((__u64) es1->es_len) + es2->es_len > EXT_MAX_BLOCKS) {
		pr_warn("ES assertion failed when merging extents. "
			"The sum of lengths of es1 (%d) and es2 (%d) "
			"is bigger than allowed file size (%d)\n",
			es1->es_len, es2->es_len, EXT_MAX_BLOCKS);
		WARN_ON(1);
		return 0;
	}

	if (((__u64) es1->es_lblk) + es1->es_len != es2->es_lblk)
		return 0;

	if ((ext4_es_is_written(es1) || ext4_es_is_unwritten(es1)) &&
	    (ext4_es_pblock(es1) + es1->es_len == ext4_es_pblock(es2)))
		return 1;

	if (ext4_es_is_hole(es1))
		return 1;


	if (ext4_es_is_delayed(es1))
		return 1;

	return 0;
}


/**
 * ext4_es_try_to_merge_left - Implements the es try to merge left operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct extent_status *
ext4_es_try_to_merge_left(struct inode *inode, struct extent_status *es)
{
	struct ext4_es_tree *tree = &EXT4_I(inode)->i_es_tree;
	struct extent_status *es1;
	struct rb_node *node;

	node = rb_prev(&es->rb_node);
	if (!node)
		return es;

	es1 = rb_entry(node, struct extent_status, rb_node);
	if (ext4_es_can_be_merged(es1, es)) {
		es1->es_len += es->es_len;
		if (ext4_es_is_referenced(es))
			ext4_es_set_referenced(es1);
		rb_erase(&es->rb_node, &tree->root);
		ext4_es_free_extent(inode, es);
		es = es1;
	}

	return es;
}


/**
 * ext4_es_try_to_merge_right - Implements the es try to merge right operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct extent_status *
ext4_es_try_to_merge_right(struct inode *inode, struct extent_status *es)
{
	struct ext4_es_tree *tree = &EXT4_I(inode)->i_es_tree;
	struct extent_status *es1;
	struct rb_node *node;

	node = rb_next(&es->rb_node);
	if (!node)
		return es;

	es1 = rb_entry(node, struct extent_status, rb_node);
	if (ext4_es_can_be_merged(es, es1)) {
		es->es_len += es1->es_len;
		if (ext4_es_is_referenced(es1))
			ext4_es_set_referenced(es);
		rb_erase(node, &tree->root);
		ext4_es_free_extent(inode, es1);
	}

	return es;
}

#ifdef ES_AGGRESSIVE_TEST
#include "ext4_extents.h"


/**
 * ext4_es_insert_extent_ext_check - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_es_insert_extent_ext_check(struct inode *inode,
					    struct extent_status *es)
{
	struct ext4_ext_path *path = NULL;
	struct ext4_extent *ex;
	ext4_lblk_t ee_block;
	ext4_fsblk_t ee_start;
	unsigned short ee_len;
	int depth, ee_status, es_status;

	path = ext4_find_extent(inode, es->es_lblk, NULL, EXT4_EX_NOCACHE);
	if (IS_ERR(path))
		return;

	depth = ext_depth(inode);
	ex = path[depth].p_ext;

	if (ex) {

		ee_block = le32_to_cpu(ex->ee_block);
		ee_start = ext4_ext_pblock(ex);
		ee_len = ext4_ext_get_actual_len(ex);

		ee_status = ext4_ext_is_unwritten(ex) ? 1 : 0;
		es_status = ext4_es_is_unwritten(es) ? 1 : 0;


		if (!ext4_es_is_written(es) && !ext4_es_is_unwritten(es)) {
			if (in_range(es->es_lblk, ee_block, ee_len)) {
				pr_warn("ES insert assertion failed for "
					"inode: %lu we can find an extent "
					"at block [%d/%d/%llu/%c], but we "
					"want to add a delayed/hole extent "
					"[%d/%d/%llu/%x]\n",
					inode->i_ino, ee_block, ee_len,
					ee_start, ee_status ? 'u' : 'w',
					es->es_lblk, es->es_len,
					ext4_es_pblock(es), ext4_es_status(es));
			}
			goto out;
		}


		if (es->es_lblk < ee_block ||
		    ext4_es_pblock(es) != ee_start + es->es_lblk - ee_block) {
			pr_warn("ES insert assertion failed for inode: %lu "
				"ex_status [%d/%d/%llu/%c] != "
				"es_status [%d/%d/%llu/%c]\n", inode->i_ino,
				ee_block, ee_len, ee_start,
				ee_status ? 'u' : 'w', es->es_lblk, es->es_len,
				ext4_es_pblock(es), es_status ? 'u' : 'w');
			goto out;
		}

		if (ee_status ^ es_status) {
			pr_warn("ES insert assertion failed for inode: %lu "
				"ex_status [%d/%d/%llu/%c] != "
				"es_status [%d/%d/%llu/%c]\n", inode->i_ino,
				ee_block, ee_len, ee_start,
				ee_status ? 'u' : 'w', es->es_lblk, es->es_len,
				ext4_es_pblock(es), es_status ? 'u' : 'w');
		}
	} else {


		if (!ext4_es_is_delayed(es) && !ext4_es_is_hole(es)) {
			pr_warn("ES insert assertion failed for inode: %lu "
				"can't find an extent at block %d but we want "
				"to add a written/unwritten extent "
				"[%d/%d/%llu/%x]\n", inode->i_ino,
				es->es_lblk, es->es_lblk, es->es_len,
				ext4_es_pblock(es), ext4_es_status(es));
		}
	}
out:
	ext4_free_ext_path(path);
}


/**
 * ext4_es_insert_extent_ind_check - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_es_insert_extent_ind_check(struct inode *inode,
					    struct extent_status *es)
{
	struct ext4_map_blocks map;
	int retval;


	map.m_lblk = es->es_lblk;
	map.m_len = es->es_len;

	retval = ext4_ind_map_blocks(NULL, inode, &map, 0);
	if (retval > 0) {
		if (ext4_es_is_delayed(es) || ext4_es_is_hole(es)) {


			pr_warn("ES insert assertion failed for inode: %lu "
				"We can find blocks but we want to add a "
				"delayed/hole extent [%d/%d/%llu/%x]\n",
				inode->i_ino, es->es_lblk, es->es_len,
				ext4_es_pblock(es), ext4_es_status(es));
			return;
		} else if (ext4_es_is_written(es)) {
			if (retval != es->es_len) {
				pr_warn("ES insert assertion failed for "
					"inode: %lu retval %d != es_len %d\n",
					inode->i_ino, retval, es->es_len);
				return;
			}
			if (map.m_pblk != ext4_es_pblock(es)) {
				pr_warn("ES insert assertion failed for "
					"inode: %lu m_pblk %llu != "
					"es_pblk %llu\n",
					inode->i_ino, map.m_pblk,
					ext4_es_pblock(es));
				return;
			}
		} else {


			BUG();
		}
	} else if (retval == 0) {
		if (ext4_es_is_written(es)) {
			pr_warn("ES insert assertion failed for inode: %lu "
				"We can't find the block but we want to add "
				"a written extent [%d/%d/%llu/%x]\n",
				inode->i_ino, es->es_lblk, es->es_len,
				ext4_es_pblock(es), ext4_es_status(es));
			return;
		}
	}
}


/**
 * ext4_es_insert_extent_check - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_es_insert_extent_check(struct inode *inode,
					       struct extent_status *es)
{


	BUG_ON(!rwsem_is_locked(&EXT4_I(inode)->i_data_sem));
	if (ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		ext4_es_insert_extent_ext_check(inode, es);
	else
		ext4_es_insert_extent_ind_check(inode, es);
}
#else


/**
 * ext4_es_insert_extent_check - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext4_es_insert_extent_check(struct inode *inode,
					       struct extent_status *es)
{
}
#endif


/**
 * __es_insert_extent - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __es_insert_extent(struct inode *inode, struct extent_status *newes,
			      struct extent_status *prealloc)
{
	struct ext4_es_tree *tree = &EXT4_I(inode)->i_es_tree;
	struct rb_node **p = &tree->root.rb_node;
	struct rb_node *parent = NULL;
	struct extent_status *es;

	while (*p) {
		parent = *p;
		es = rb_entry(parent, struct extent_status, rb_node);

		if (newes->es_lblk < es->es_lblk) {
			if (ext4_es_can_be_merged(newes, es)) {


				es->es_lblk = newes->es_lblk;
				es->es_len += newes->es_len;
				if (ext4_es_is_written(es) ||
				    ext4_es_is_unwritten(es))
					ext4_es_store_pblock(es,
							     newes->es_pblk);
				es = ext4_es_try_to_merge_left(inode, es);
				goto out;
			}
			p = &(*p)->rb_left;
		} else if (newes->es_lblk > ext4_es_end(es)) {
			if (ext4_es_can_be_merged(es, newes)) {
				es->es_len += newes->es_len;
				es = ext4_es_try_to_merge_right(inode, es);
				goto out;
			}
			p = &(*p)->rb_right;
		} else {
			BUG();
			return -EINVAL;
		}
	}

	if (prealloc)
		es = prealloc;
	else
		es = __es_alloc_extent(false);
	if (!es)
		return -ENOMEM;
	ext4_es_init_extent(inode, es, newes->es_lblk, newes->es_len,
			    newes->es_pblk);

	rb_link_node(&es->rb_node, parent, p);
	rb_insert_color(&es->rb_node, &tree->root);

out:
	tree->cache_es = es;
	return 0;
}


/**
 * ext4_es_insert_extent - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_es_insert_extent(struct inode *inode, ext4_lblk_t lblk,
			   ext4_lblk_t len, ext4_fsblk_t pblk,
			   unsigned int status, int flags)
{
	struct extent_status newes;
	ext4_lblk_t end = lblk + len - 1;
	int err1 = 0, err2 = 0, err3 = 0;
	int resv_used = 0, pending = 0;
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct extent_status *es1 = NULL;
	struct extent_status *es2 = NULL;
	struct pending_reservation *pr = NULL;
	bool revise_pending = false;

	if (EXT4_SB(inode->i_sb)->s_mount_state & EXT4_FC_REPLAY)
		return;

	es_debug("add [%u/%u) %llu %x %x to extent status tree of inode %lu\n",
		 lblk, len, pblk, status, flags, inode->i_ino);

	if (!len)
		return;

	BUG_ON(end < lblk);
	WARN_ON_ONCE(status & EXTENT_STATUS_DELAYED);

	newes.es_lblk = lblk;
	newes.es_len = len;
	ext4_es_store_pblock_status(&newes, pblk, status);
	trace_ext4_es_insert_extent(inode, &newes);

	ext4_es_insert_extent_check(inode, &newes);

	revise_pending = sbi->s_cluster_ratio > 1 &&
			 test_opt(inode->i_sb, DELALLOC) &&
			 (status & (EXTENT_STATUS_WRITTEN |
				    EXTENT_STATUS_UNWRITTEN));
retry:
	if (err1 && !es1)
		es1 = __es_alloc_extent(true);
	if ((err1 || err2) && !es2)
		es2 = __es_alloc_extent(true);
	if ((err1 || err2 || err3 < 0) && revise_pending && !pr)
		pr = __alloc_pending(true);
	write_lock(&EXT4_I(inode)->i_es_lock);

	err1 = __es_remove_extent(inode, lblk, end, &resv_used, es1);
	if (err1 != 0)
		goto error;

	if (es1) {
		if (!es1->es_len)
			__es_free_extent(es1);
		es1 = NULL;
	}

	err2 = __es_insert_extent(inode, &newes, es2);
	if (err2 == -ENOMEM && !ext4_es_must_keep(&newes))
		err2 = 0;
	if (err2 != 0)
		goto error;

	if (es2) {
		if (!es2->es_len)
			__es_free_extent(es2);
		es2 = NULL;
	}

	if (revise_pending) {
		err3 = __revise_pending(inode, lblk, len, &pr);
		if (err3 < 0)
			goto error;
		if (pr) {
			__free_pending(pr);
			pr = NULL;
		}
		pending = err3;
	}
error:
	write_unlock(&EXT4_I(inode)->i_es_lock);


	resv_used += pending;
	if (resv_used)
		ext4_da_update_reserve_space(inode, resv_used,
				flags & EXT4_GET_BLOCKS_DELALLOC_RESERVE);

	if (err1 || err2 || err3 < 0)
		goto retry;

	ext4_es_print_tree(inode);
	return;
}


/**
 * ext4_es_cache_extent - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_es_cache_extent(struct inode *inode, ext4_lblk_t lblk,
			  ext4_lblk_t len, ext4_fsblk_t pblk,
			  unsigned int status)
{
	struct extent_status *es;
	struct extent_status newes;
	ext4_lblk_t end = lblk + len - 1;

	if (EXT4_SB(inode->i_sb)->s_mount_state & EXT4_FC_REPLAY)
		return;

	newes.es_lblk = lblk;
	newes.es_len = len;
	ext4_es_store_pblock_status(&newes, pblk, status);
	trace_ext4_es_cache_extent(inode, &newes);

	if (!len)
		return;

	BUG_ON(end < lblk);

	write_lock(&EXT4_I(inode)->i_es_lock);

	es = __es_tree_search(&EXT4_I(inode)->i_es_tree.root, lblk);
	if (!es || es->es_lblk > end)
		__es_insert_extent(inode, &newes, NULL);
	write_unlock(&EXT4_I(inode)->i_es_lock);
}


/**
 * ext4_es_lookup_extent - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_es_lookup_extent(struct inode *inode, ext4_lblk_t lblk,
			  ext4_lblk_t *next_lblk,
			  struct extent_status *es)
{
	struct ext4_es_tree *tree;
	struct ext4_es_stats *stats;
	struct extent_status *es1 = NULL;
	struct rb_node *node;
	int found = 0;

	if (EXT4_SB(inode->i_sb)->s_mount_state & EXT4_FC_REPLAY)
		return 0;

	trace_ext4_es_lookup_extent_enter(inode, lblk);
	es_debug("lookup extent in block %u\n", lblk);

	tree = &EXT4_I(inode)->i_es_tree;
	read_lock(&EXT4_I(inode)->i_es_lock);


	es->es_lblk = es->es_len = es->es_pblk = 0;
	es1 = READ_ONCE(tree->cache_es);
	if (es1 && in_range(lblk, es1->es_lblk, es1->es_len)) {
		es_debug("%u cached by [%u/%u)\n",
			 lblk, es1->es_lblk, es1->es_len);
		found = 1;
		goto out;
	}

	node = tree->root.rb_node;
	while (node) {
		es1 = rb_entry(node, struct extent_status, rb_node);
		if (lblk < es1->es_lblk)
			node = node->rb_left;
		else if (lblk > ext4_es_end(es1))
			node = node->rb_right;
		else {
			found = 1;
			break;
		}
	}

out:
	stats = &EXT4_SB(inode->i_sb)->s_es_stats;
	if (found) {
		BUG_ON(!es1);
		es->es_lblk = es1->es_lblk;
		es->es_len = es1->es_len;
		es->es_pblk = es1->es_pblk;
		if (!ext4_es_is_referenced(es1))
			ext4_es_set_referenced(es1);
		percpu_counter_inc(&stats->es_stats_cache_hits);
		if (next_lblk) {
			node = rb_next(&es1->rb_node);
			if (node) {
				es1 = rb_entry(node, struct extent_status,
					       rb_node);
				*next_lblk = es1->es_lblk;
			} else
				*next_lblk = 0;
		}
	} else {
		percpu_counter_inc(&stats->es_stats_cache_misses);
	}

	read_unlock(&EXT4_I(inode)->i_es_lock);

	trace_ext4_es_lookup_extent_exit(inode, es, found);
	return found;
}


/**
 * struct rsvd_count - Private EXT4 state/data structure used by extent-status cache.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct rsvd_count {
	int ndelayed;
	bool first_do_lblk_found;
	ext4_lblk_t first_do_lblk;
	ext4_lblk_t last_do_lblk;
	struct extent_status *left_es;
	bool partial;
	ext4_lblk_t lclu;
};


/**
 * init_rsvd - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void init_rsvd(struct inode *inode, ext4_lblk_t lblk,
		      struct extent_status *es, struct rsvd_count *rc)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct rb_node *node;

	rc->ndelayed = 0;


	if (sbi->s_cluster_ratio > 1) {
		rc->first_do_lblk_found = false;
		if (lblk > es->es_lblk) {
			rc->left_es = es;
		} else {
			node = rb_prev(&es->rb_node);
			rc->left_es = node ? rb_entry(node,
						      struct extent_status,
						      rb_node) : NULL;
		}
		rc->partial = false;
	}
}


/**
 * count_rsvd - Computes derived filesystem state used for validation, accounting or policy decisions.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void count_rsvd(struct inode *inode, ext4_lblk_t lblk, long len,
		       struct extent_status *es, struct rsvd_count *rc)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	ext4_lblk_t i, end, nclu;

	if (!ext4_es_is_delayed(es))
		return;

	WARN_ON(len <= 0);

	if (sbi->s_cluster_ratio == 1) {
		rc->ndelayed += (int) len;
		return;
	}


	i = (lblk < es->es_lblk) ? es->es_lblk : lblk;
	end = lblk + (ext4_lblk_t) len - 1;
	end = (end > ext4_es_end(es)) ? ext4_es_end(es) : end;


	if (!rc->first_do_lblk_found) {
		rc->first_do_lblk = i;
		rc->first_do_lblk_found = true;
	}


	rc->last_do_lblk = end;


	if (rc->partial && (rc->lclu != EXT4_B2C(sbi, i))) {
		rc->ndelayed++;
		rc->partial = false;
	}


	if (EXT4_LBLK_COFF(sbi, i) != 0) {
		if (end >= EXT4_LBLK_CFILL(sbi, i)) {
			rc->ndelayed++;
			rc->partial = false;
			i = EXT4_LBLK_CFILL(sbi, i) + 1;
		}
	}


	if ((i + sbi->s_cluster_ratio - 1) <= end) {
		nclu = (end - i + 1) >> sbi->s_cluster_bits;
		rc->ndelayed += nclu;
		i += nclu << sbi->s_cluster_bits;
	}


	if (!rc->partial && i <= end) {
		rc->partial = true;
		rc->lclu = EXT4_B2C(sbi, i);
	}
}


/**
 * __pr_tree_search - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct pending_reservation *__pr_tree_search(struct rb_root *root,
						    ext4_lblk_t lclu)
{
	struct rb_node *node = root->rb_node;
	struct pending_reservation *pr = NULL;

	while (node) {
		pr = rb_entry(node, struct pending_reservation, rb_node);
		if (lclu < pr->lclu)
			node = node->rb_left;
		else if (lclu > pr->lclu)
			node = node->rb_right;
		else
			return pr;
	}
	if (pr && lclu < pr->lclu)
		return pr;
	if (pr && lclu > pr->lclu) {
		node = rb_next(&pr->rb_node);
		return node ? rb_entry(node, struct pending_reservation,
				       rb_node) : NULL;
	}
	return NULL;
}


/**
 * get_rsvd - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static unsigned int get_rsvd(struct inode *inode, ext4_lblk_t end,
			     struct extent_status *right_es,
			     struct rsvd_count *rc)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct pending_reservation *pr;
	struct ext4_pending_tree *tree = &EXT4_I(inode)->i_pending_tree;
	struct rb_node *node;
	ext4_lblk_t first_lclu, last_lclu;
	bool left_delayed, right_delayed, count_pending;
	struct extent_status *es;

	if (sbi->s_cluster_ratio > 1) {

		if (rc->partial)
			rc->ndelayed++;

		if (rc->ndelayed == 0)
			return 0;

		first_lclu = EXT4_B2C(sbi, rc->first_do_lblk);
		last_lclu = EXT4_B2C(sbi, rc->last_do_lblk);


		left_delayed = right_delayed = false;

		es = rc->left_es;
		while (es && ext4_es_end(es) >=
		       EXT4_LBLK_CMASK(sbi, rc->first_do_lblk)) {
			if (ext4_es_is_delayed(es)) {
				rc->ndelayed--;
				left_delayed = true;
				break;
			}
			node = rb_prev(&es->rb_node);
			if (!node)
				break;
			es = rb_entry(node, struct extent_status, rb_node);
		}
		if (right_es && (!left_delayed || first_lclu != last_lclu)) {
			if (end < ext4_es_end(right_es)) {
				es = right_es;
			} else {
				node = rb_next(&right_es->rb_node);
				es = node ? rb_entry(node, struct extent_status,
						     rb_node) : NULL;
			}
			while (es && es->es_lblk <=
			       EXT4_LBLK_CFILL(sbi, rc->last_do_lblk)) {
				if (ext4_es_is_delayed(es)) {
					rc->ndelayed--;
					right_delayed = true;
					break;
				}
				node = rb_next(&es->rb_node);
				if (!node)
					break;
				es = rb_entry(node, struct extent_status,
					      rb_node);
			}
		}


		if (first_lclu == last_lclu) {
			if (left_delayed | right_delayed)
				count_pending = false;
			else
				count_pending = true;
		} else {
			if (left_delayed)
				first_lclu++;
			if (right_delayed)
				last_lclu--;
			if (first_lclu <= last_lclu)
				count_pending = true;
			else
				count_pending = false;
		}


		if (count_pending) {
			pr = __pr_tree_search(&tree->root, first_lclu);
			while (pr && pr->lclu <= last_lclu) {
				rc->ndelayed--;
				node = rb_next(&pr->rb_node);
				rb_erase(&pr->rb_node, &tree->root);
				__free_pending(pr);
				if (!node)
					break;
				pr = rb_entry(node, struct pending_reservation,
					      rb_node);
			}
		}
	}
	return rc->ndelayed;
}


/**
 * __es_remove_extent - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __es_remove_extent(struct inode *inode, ext4_lblk_t lblk,
			      ext4_lblk_t end, int *reserved,
			      struct extent_status *prealloc)
{
	struct ext4_es_tree *tree = &EXT4_I(inode)->i_es_tree;
	struct rb_node *node;
	struct extent_status *es;
	struct extent_status orig_es;
	ext4_lblk_t len1, len2;
	ext4_fsblk_t block;
	int err = 0;
	bool count_reserved = true;
	struct rsvd_count rc;

	if (reserved == NULL || !test_opt(inode->i_sb, DELALLOC))
		count_reserved = false;

	es = __es_tree_search(&tree->root, lblk);
	if (!es)
		goto out;
	if (es->es_lblk > end)
		goto out;


	tree->cache_es = NULL;
	if (count_reserved)
		init_rsvd(inode, lblk, es, &rc);

	orig_es.es_lblk = es->es_lblk;
	orig_es.es_len = es->es_len;
	orig_es.es_pblk = es->es_pblk;

	len1 = lblk > es->es_lblk ? lblk - es->es_lblk : 0;
	len2 = ext4_es_end(es) > end ? ext4_es_end(es) - end : 0;
	if (len1 > 0)
		es->es_len = len1;
	if (len2 > 0) {
		if (len1 > 0) {
			struct extent_status newes;

			newes.es_lblk = end + 1;
			newes.es_len = len2;
			block = 0x7FDEADBEEFULL;
			if (ext4_es_is_written(&orig_es) ||
			    ext4_es_is_unwritten(&orig_es))
				block = ext4_es_pblock(&orig_es) +
					orig_es.es_len - len2;
			ext4_es_store_pblock_status(&newes, block,
						    ext4_es_status(&orig_es));
			err = __es_insert_extent(inode, &newes, prealloc);
			if (err) {
				if (!ext4_es_must_keep(&newes))
					return 0;

				es->es_lblk = orig_es.es_lblk;
				es->es_len = orig_es.es_len;
				goto out;
			}
		} else {
			es->es_lblk = end + 1;
			es->es_len = len2;
			if (ext4_es_is_written(es) ||
			    ext4_es_is_unwritten(es)) {
				block = orig_es.es_pblk + orig_es.es_len - len2;
				ext4_es_store_pblock(es, block);
			}
		}
		if (count_reserved)
			count_rsvd(inode, orig_es.es_lblk + len1,
				   orig_es.es_len - len1 - len2, &orig_es, &rc);
		goto out_get_reserved;
	}

	if (len1 > 0) {
		if (count_reserved)
			count_rsvd(inode, lblk, orig_es.es_len - len1,
				   &orig_es, &rc);
		node = rb_next(&es->rb_node);
		if (node)
			es = rb_entry(node, struct extent_status, rb_node);
		else
			es = NULL;
	}

	while (es && ext4_es_end(es) <= end) {
		if (count_reserved)
			count_rsvd(inode, es->es_lblk, es->es_len, es, &rc);
		node = rb_next(&es->rb_node);
		rb_erase(&es->rb_node, &tree->root);
		ext4_es_free_extent(inode, es);
		if (!node) {
			es = NULL;
			break;
		}
		es = rb_entry(node, struct extent_status, rb_node);
	}

	if (es && es->es_lblk < end + 1) {
		ext4_lblk_t orig_len = es->es_len;

		len1 = ext4_es_end(es) - end;
		if (count_reserved)
			count_rsvd(inode, es->es_lblk, orig_len - len1,
				   es, &rc);
		es->es_lblk = end + 1;
		es->es_len = len1;
		if (ext4_es_is_written(es) || ext4_es_is_unwritten(es)) {
			block = es->es_pblk + orig_len - len1;
			ext4_es_store_pblock(es, block);
		}
	}

out_get_reserved:
	if (count_reserved)
		*reserved = get_rsvd(inode, end, es, &rc);
out:
	return err;
}


/**
 * ext4_es_remove_extent - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_es_remove_extent(struct inode *inode, ext4_lblk_t lblk,
			   ext4_lblk_t len)
{
	ext4_lblk_t end;
	int err = 0;
	int reserved = 0;
	struct extent_status *es = NULL;

	if (EXT4_SB(inode->i_sb)->s_mount_state & EXT4_FC_REPLAY)
		return;

	trace_ext4_es_remove_extent(inode, lblk, len);
	es_debug("remove [%u/%u) from extent status tree of inode %lu\n",
		 lblk, len, inode->i_ino);

	if (!len)
		return;

	end = lblk + len - 1;
	BUG_ON(end < lblk);

retry:
	if (err && !es)
		es = __es_alloc_extent(true);


	write_lock(&EXT4_I(inode)->i_es_lock);
	err = __es_remove_extent(inode, lblk, end, &reserved, es);

	if (es) {
		if (!es->es_len)
			__es_free_extent(es);
		es = NULL;
	}
	write_unlock(&EXT4_I(inode)->i_es_lock);
	if (err)
		goto retry;

	ext4_es_print_tree(inode);
	ext4_da_release_space(inode, reserved);
	return;
}


/**
 * __es_shrink - Implements the es shrink operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __es_shrink(struct ext4_sb_info *sbi, int nr_to_scan,
		       struct ext4_inode_info *locked_ei)
{
	struct ext4_inode_info *ei;
	struct ext4_es_stats *es_stats;
	ktime_t start_time;
	u64 scan_time;
	int nr_to_walk;
	int nr_shrunk = 0;
	int retried = 0, nr_skipped = 0;

	es_stats = &sbi->s_es_stats;
	start_time = ktime_get();

retry:
	spin_lock(&sbi->s_es_lock);
	nr_to_walk = sbi->s_es_nr_inode;
	while (nr_to_walk-- > 0) {
		if (list_empty(&sbi->s_es_list)) {
			spin_unlock(&sbi->s_es_lock);
			goto out;
		}
		ei = list_first_entry(&sbi->s_es_list, struct ext4_inode_info,
				      i_es_list);

		list_move_tail(&ei->i_es_list, &sbi->s_es_list);


		if (!retried && ext4_test_inode_state(&ei->vfs_inode,
						EXT4_STATE_EXT_PRECACHED)) {
			nr_skipped++;
			continue;
		}

		if (ei == locked_ei || !write_trylock(&ei->i_es_lock)) {
			nr_skipped++;
			continue;
		}


		spin_unlock(&sbi->s_es_lock);

		nr_shrunk += es_reclaim_extents(ei, &nr_to_scan);
		write_unlock(&ei->i_es_lock);

		if (nr_to_scan <= 0)
			goto out;
		spin_lock(&sbi->s_es_lock);
	}
	spin_unlock(&sbi->s_es_lock);


	if ((nr_shrunk == 0) && nr_skipped && !retried) {
		retried++;
		goto retry;
	}

	if (locked_ei && nr_shrunk == 0)
		nr_shrunk = es_reclaim_extents(locked_ei, &nr_to_scan);

out:
	scan_time = ktime_to_ns(ktime_sub(ktime_get(), start_time));
	if (likely(es_stats->es_stats_scan_time))
		es_stats->es_stats_scan_time = (scan_time +
				es_stats->es_stats_scan_time*3) / 4;
	else
		es_stats->es_stats_scan_time = scan_time;
	if (scan_time > es_stats->es_stats_max_scan_time)
		es_stats->es_stats_max_scan_time = scan_time;
	if (likely(es_stats->es_stats_shrunk))
		es_stats->es_stats_shrunk = (nr_shrunk +
				es_stats->es_stats_shrunk*3) / 4;
	else
		es_stats->es_stats_shrunk = nr_shrunk;

	trace_ext4_es_shrink(sbi->s_sb, nr_shrunk, scan_time,
			     nr_skipped, retried);
	return nr_shrunk;
}


/**
 * ext4_es_count - Computes derived filesystem state used for validation, accounting or policy decisions.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static unsigned long ext4_es_count(struct shrinker *shrink,
				   struct shrink_control *sc)
{
	unsigned long nr;
	struct ext4_sb_info *sbi;

	sbi = shrink->private_data;
	nr = percpu_counter_read_positive(&sbi->s_es_stats.es_stats_shk_cnt);
	trace_ext4_es_shrink_count(sbi->s_sb, sc->nr_to_scan, nr);
	return nr;
}


/**
 * ext4_es_scan - Implements the es scan operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static unsigned long ext4_es_scan(struct shrinker *shrink,
				  struct shrink_control *sc)
{
	struct ext4_sb_info *sbi = shrink->private_data;
	int nr_to_scan = sc->nr_to_scan;
	int ret, nr_shrunk;

	ret = percpu_counter_read_positive(&sbi->s_es_stats.es_stats_shk_cnt);
	trace_ext4_es_shrink_scan_enter(sbi->s_sb, nr_to_scan, ret);

	nr_shrunk = __es_shrink(sbi, nr_to_scan, NULL);

	ret = percpu_counter_read_positive(&sbi->s_es_stats.es_stats_shk_cnt);
	trace_ext4_es_shrink_scan_exit(sbi->s_sb, nr_shrunk, ret);
	return nr_shrunk;
}


/**
 * ext4_seq_es_shrinker_info_show - Implements the seq es shrinker info show operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_seq_es_shrinker_info_show(struct seq_file *seq, void *v)
{
	struct ext4_sb_info *sbi = EXT4_SB((struct super_block *) seq->private);
	struct ext4_es_stats *es_stats = &sbi->s_es_stats;
	struct ext4_inode_info *ei, *max = NULL;
	unsigned int inode_cnt = 0;

	if (v != SEQ_START_TOKEN)
		return 0;


	spin_lock(&sbi->s_es_lock);
	list_for_each_entry(ei, &sbi->s_es_list, i_es_list) {
		inode_cnt++;
		if (max && max->i_es_all_nr < ei->i_es_all_nr)
			max = ei;
		else if (!max)
			max = ei;
	}
	spin_unlock(&sbi->s_es_lock);

	seq_printf(seq, "stats:\n  %lld objects\n  %lld reclaimable objects\n",
		   percpu_counter_sum_positive(&es_stats->es_stats_all_cnt),
		   percpu_counter_sum_positive(&es_stats->es_stats_shk_cnt));
	seq_printf(seq, "  %lld/%lld cache hits/misses\n",
		   percpu_counter_sum_positive(&es_stats->es_stats_cache_hits),
		   percpu_counter_sum_positive(&es_stats->es_stats_cache_misses));
	if (inode_cnt)
		seq_printf(seq, "  %d inodes on list\n", inode_cnt);

	seq_printf(seq, "average:\n  %llu us scan time\n",
	    div_u64(es_stats->es_stats_scan_time, 1000));
	seq_printf(seq, "  %lu shrunk objects\n", es_stats->es_stats_shrunk);
	if (inode_cnt)
		seq_printf(seq,
		    "maximum:\n  %lu inode (%u objects, %u reclaimable)\n"
		    "  %llu us max scan time\n",
		    max->vfs_inode.i_ino, max->i_es_all_nr, max->i_es_shk_nr,
		    div_u64(es_stats->es_stats_max_scan_time, 1000));

	return 0;
}


/**
 * ext4_es_register_shrinker - Implements the es register shrinker operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_es_register_shrinker(struct ext4_sb_info *sbi)
{
	int err;


	BUILD_BUG_ON(ES_SHIFT < 48);
	INIT_LIST_HEAD(&sbi->s_es_list);
	sbi->s_es_nr_inode = 0;
	spin_lock_init(&sbi->s_es_lock);
	sbi->s_es_stats.es_stats_shrunk = 0;
	err = percpu_counter_init(&sbi->s_es_stats.es_stats_cache_hits, 0,
				  GFP_KERNEL);
	if (err)
		return err;
	err = percpu_counter_init(&sbi->s_es_stats.es_stats_cache_misses, 0,
				  GFP_KERNEL);
	if (err)
		goto err1;
	sbi->s_es_stats.es_stats_scan_time = 0;
	sbi->s_es_stats.es_stats_max_scan_time = 0;
	err = percpu_counter_init(&sbi->s_es_stats.es_stats_all_cnt, 0, GFP_KERNEL);
	if (err)
		goto err2;
	err = percpu_counter_init(&sbi->s_es_stats.es_stats_shk_cnt, 0, GFP_KERNEL);
	if (err)
		goto err3;

	sbi->s_es_shrinker = shrinker_alloc(0, "ext4-es:%s", sbi->s_sb->s_id);
	if (!sbi->s_es_shrinker) {
		err = -ENOMEM;
		goto err4;
	}

	sbi->s_es_shrinker->scan_objects = ext4_es_scan;
	sbi->s_es_shrinker->count_objects = ext4_es_count;
	sbi->s_es_shrinker->private_data = sbi;

	shrinker_register(sbi->s_es_shrinker);

	return 0;
err4:
	percpu_counter_destroy(&sbi->s_es_stats.es_stats_shk_cnt);
err3:
	percpu_counter_destroy(&sbi->s_es_stats.es_stats_all_cnt);
err2:
	percpu_counter_destroy(&sbi->s_es_stats.es_stats_cache_misses);
err1:
	percpu_counter_destroy(&sbi->s_es_stats.es_stats_cache_hits);
	return err;
}


/**
 * ext4_es_unregister_shrinker - Implements the es unregister shrinker operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_es_unregister_shrinker(struct ext4_sb_info *sbi)
{
	percpu_counter_destroy(&sbi->s_es_stats.es_stats_cache_hits);
	percpu_counter_destroy(&sbi->s_es_stats.es_stats_cache_misses);
	percpu_counter_destroy(&sbi->s_es_stats.es_stats_all_cnt);
	percpu_counter_destroy(&sbi->s_es_stats.es_stats_shk_cnt);
	shrinker_free(sbi->s_es_shrinker);
}


/**
 * es_do_reclaim_extents - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int es_do_reclaim_extents(struct ext4_inode_info *ei, ext4_lblk_t end,
				 int *nr_to_scan, int *nr_shrunk)
{
	struct inode *inode = &ei->vfs_inode;
	struct ext4_es_tree *tree = &ei->i_es_tree;
	struct extent_status *es;
	struct rb_node *node;

	es = __es_tree_search(&tree->root, ei->i_es_shrink_lblk);
	if (!es)
		goto out_wrap;

	while (*nr_to_scan > 0) {
		if (es->es_lblk > end) {
			ei->i_es_shrink_lblk = end + 1;
			return 0;
		}

		(*nr_to_scan)--;
		node = rb_next(&es->rb_node);

		if (ext4_es_must_keep(es))
			goto next;
		if (ext4_es_is_referenced(es)) {
			ext4_es_clear_referenced(es);
			goto next;
		}

		rb_erase(&es->rb_node, &tree->root);
		ext4_es_free_extent(inode, es);
		(*nr_shrunk)++;
next:
		if (!node)
			goto out_wrap;
		es = rb_entry(node, struct extent_status, rb_node);
	}
	ei->i_es_shrink_lblk = es->es_lblk;
	return 1;
out_wrap:
	ei->i_es_shrink_lblk = 0;
	return 0;
}


/**
 * es_reclaim_extents - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int es_reclaim_extents(struct ext4_inode_info *ei, int *nr_to_scan)
{
	struct inode *inode = &ei->vfs_inode;
	int nr_shrunk = 0;
	ext4_lblk_t start = ei->i_es_shrink_lblk;
	static DEFINE_RATELIMIT_STATE(_rs, DEFAULT_RATELIMIT_INTERVAL,
				      DEFAULT_RATELIMIT_BURST);

	if (ei->i_es_shk_nr == 0)
		return 0;

	if (ext4_test_inode_state(inode, EXT4_STATE_EXT_PRECACHED) &&
	    __ratelimit(&_rs))
		ext4_warning(inode->i_sb, "forced shrink of precached extents");

	if (!es_do_reclaim_extents(ei, EXT_MAX_BLOCKS, nr_to_scan, &nr_shrunk) &&
	    start != 0)
		es_do_reclaim_extents(ei, start - 1, nr_to_scan, &nr_shrunk);

	ei->i_es_tree.cache_es = NULL;
	return nr_shrunk;
}


/**
 * ext4_clear_inode_es - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_clear_inode_es(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct extent_status *es;
	struct ext4_es_tree *tree;
	struct rb_node *node;

	write_lock(&ei->i_es_lock);
	tree = &EXT4_I(inode)->i_es_tree;
	tree->cache_es = NULL;
	node = rb_first(&tree->root);
	while (node) {
		es = rb_entry(node, struct extent_status, rb_node);
		node = rb_next(node);
		if (!ext4_es_must_keep(es)) {
			rb_erase(&es->rb_node, &tree->root);
			ext4_es_free_extent(inode, es);
		}
	}
	ext4_clear_inode_state(inode, EXT4_STATE_EXT_PRECACHED);
	write_unlock(&ei->i_es_lock);
}

#ifdef ES_DEBUG__


/**
 * ext4_print_pending_tree - Implements the print pending tree operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_print_pending_tree(struct inode *inode)
{
	struct ext4_pending_tree *tree;
	struct rb_node *node;
	struct pending_reservation *pr;

	printk(KERN_DEBUG "pending reservations for inode %lu:", inode->i_ino);
	tree = &EXT4_I(inode)->i_pending_tree;
	node = rb_first(&tree->root);
	while (node) {
		pr = rb_entry(node, struct pending_reservation, rb_node);
		printk(KERN_DEBUG " %u", pr->lclu);
		node = rb_next(node);
	}
	printk(KERN_DEBUG "\n");
}
#else
#define ext4_print_pending_tree(inode)
#endif


/**
 * ext4_init_pending - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int __init ext4_init_pending(void)
{
	ext4_pending_cachep = KMEM_CACHE(pending_reservation, SLAB_RECLAIM_ACCOUNT);
	if (ext4_pending_cachep == NULL)
		return -ENOMEM;
	return 0;
}


/**
 * ext4_exit_pending - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_exit_pending(void)
{
	kmem_cache_destroy(ext4_pending_cachep);
}


/**
 * ext4_init_pending_tree - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_init_pending_tree(struct ext4_pending_tree *tree)
{
	tree->root = RB_ROOT;
}


/**
 * __get_pending - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct pending_reservation *__get_pending(struct inode *inode,
						 ext4_lblk_t lclu)
{
	struct ext4_pending_tree *tree;
	struct rb_node *node;
	struct pending_reservation *pr = NULL;

	tree = &EXT4_I(inode)->i_pending_tree;
	node = (&tree->root)->rb_node;

	while (node) {
		pr = rb_entry(node, struct pending_reservation, rb_node);
		if (lclu < pr->lclu)
			node = node->rb_left;
		else if (lclu > pr->lclu)
			node = node->rb_right;
		else if (lclu == pr->lclu)
			return pr;
	}
	return NULL;
}


/**
 * __insert_pending - Implements the insert pending operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __insert_pending(struct inode *inode, ext4_lblk_t lblk,
			    struct pending_reservation **prealloc)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_pending_tree *tree = &EXT4_I(inode)->i_pending_tree;
	struct rb_node **p = &tree->root.rb_node;
	struct rb_node *parent = NULL;
	struct pending_reservation *pr;
	ext4_lblk_t lclu;
	int ret = 0;

	lclu = EXT4_B2C(sbi, lblk);

	while (*p) {
		parent = *p;
		pr = rb_entry(parent, struct pending_reservation, rb_node);

		if (lclu < pr->lclu) {
			p = &(*p)->rb_left;
		} else if (lclu > pr->lclu) {
			p = &(*p)->rb_right;
		} else {

			goto out;
		}
	}

	if (likely(*prealloc == NULL)) {
		pr = __alloc_pending(false);
		if (!pr) {
			ret = -ENOMEM;
			goto out;
		}
	} else {
		pr = *prealloc;
		*prealloc = NULL;
	}
	pr->lclu = lclu;

	rb_link_node(&pr->rb_node, parent, p);
	rb_insert_color(&pr->rb_node, &tree->root);
	ret = 1;

out:
	return ret;
}


/**
 * __remove_pending - Implements the remove pending operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void __remove_pending(struct inode *inode, ext4_lblk_t lblk)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct pending_reservation *pr;
	struct ext4_pending_tree *tree;

	pr = __get_pending(inode, EXT4_B2C(sbi, lblk));
	if (pr != NULL) {
		tree = &EXT4_I(inode)->i_pending_tree;
		rb_erase(&pr->rb_node, &tree->root);
		__free_pending(pr);
	}
}


/**
 * ext4_remove_pending - Implements the remove pending operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_remove_pending(struct inode *inode, ext4_lblk_t lblk)
{
	struct ext4_inode_info *ei = EXT4_I(inode);

	write_lock(&ei->i_es_lock);
	__remove_pending(inode, lblk);
	write_unlock(&ei->i_es_lock);
}


/**
 * ext4_is_pending - Implements the is pending operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
bool ext4_is_pending(struct inode *inode, ext4_lblk_t lblk)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_inode_info *ei = EXT4_I(inode);
	bool ret;

	read_lock(&ei->i_es_lock);
	ret = (bool)(__get_pending(inode, EXT4_B2C(sbi, lblk)) != NULL);
	read_unlock(&ei->i_es_lock);

	return ret;
}


/**
 * ext4_es_insert_delayed_extent - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_es_insert_delayed_extent(struct inode *inode, ext4_lblk_t lblk,
				   ext4_lblk_t len, bool lclu_allocated,
				   bool end_allocated)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct extent_status newes;
	ext4_lblk_t end = lblk + len - 1;
	int err1 = 0, err2 = 0, err3 = 0;
	struct extent_status *es1 = NULL;
	struct extent_status *es2 = NULL;
	struct pending_reservation *pr1 = NULL;
	struct pending_reservation *pr2 = NULL;

	if (EXT4_SB(inode->i_sb)->s_mount_state & EXT4_FC_REPLAY)
		return;

	es_debug("add [%u/%u) delayed to extent status tree of inode %lu\n",
		 lblk, len, inode->i_ino);
	if (!len)
		return;

	WARN_ON_ONCE((EXT4_B2C(sbi, lblk) == EXT4_B2C(sbi, end)) &&
		     end_allocated);

	newes.es_lblk = lblk;
	newes.es_len = len;
	ext4_es_store_pblock_status(&newes, ~0, EXTENT_STATUS_DELAYED);
	trace_ext4_es_insert_delayed_extent(inode, &newes, lclu_allocated,
					    end_allocated);

	ext4_es_insert_extent_check(inode, &newes);

retry:
	if (err1 && !es1)
		es1 = __es_alloc_extent(true);
	if ((err1 || err2) && !es2)
		es2 = __es_alloc_extent(true);
	if (err1 || err2 || err3 < 0) {
		if (lclu_allocated && !pr1)
			pr1 = __alloc_pending(true);
		if (end_allocated && !pr2)
			pr2 = __alloc_pending(true);
	}
	write_lock(&EXT4_I(inode)->i_es_lock);

	err1 = __es_remove_extent(inode, lblk, end, NULL, es1);
	if (err1 != 0)
		goto error;

	if (es1) {
		if (!es1->es_len)
			__es_free_extent(es1);
		es1 = NULL;
	}

	err2 = __es_insert_extent(inode, &newes, es2);
	if (err2 != 0)
		goto error;

	if (es2) {
		if (!es2->es_len)
			__es_free_extent(es2);
		es2 = NULL;
	}

	if (lclu_allocated) {
		err3 = __insert_pending(inode, lblk, &pr1);
		if (err3 < 0)
			goto error;
		if (pr1) {
			__free_pending(pr1);
			pr1 = NULL;
		}
	}
	if (end_allocated) {
		err3 = __insert_pending(inode, end, &pr2);
		if (err3 < 0)
			goto error;
		if (pr2) {
			__free_pending(pr2);
			pr2 = NULL;
		}
	}
error:
	write_unlock(&EXT4_I(inode)->i_es_lock);
	if (err1 || err2 || err3 < 0)
		goto retry;

	ext4_es_print_tree(inode);
	ext4_print_pending_tree(inode);
	return;
}


/**
 * __revise_pending - Implements the revise pending operation within the extent-status cache subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __revise_pending(struct inode *inode, ext4_lblk_t lblk,
			    ext4_lblk_t len,
			    struct pending_reservation **prealloc)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	ext4_lblk_t end = lblk + len - 1;
	ext4_lblk_t first, last;
	bool f_del = false, l_del = false;
	int pendings = 0;
	int ret = 0;

	if (len == 0)
		return 0;


	if (EXT4_B2C(sbi, lblk) == EXT4_B2C(sbi, end)) {
		first = EXT4_LBLK_CMASK(sbi, lblk);
		if (first != lblk)
			f_del = __es_scan_range(inode, &ext4_es_is_delayed,
						first, lblk - 1);
		if (f_del) {
			ret = __insert_pending(inode, first, prealloc);
			if (ret < 0)
				goto out;
			pendings += ret;
		} else {
			last = EXT4_LBLK_CMASK(sbi, end) +
			       sbi->s_cluster_ratio - 1;
			if (last != end)
				l_del = __es_scan_range(inode,
							&ext4_es_is_delayed,
							end + 1, last);
			if (l_del) {
				ret = __insert_pending(inode, last, prealloc);
				if (ret < 0)
					goto out;
				pendings += ret;
			} else
				__remove_pending(inode, last);
		}
	} else {
		first = EXT4_LBLK_CMASK(sbi, lblk);
		if (first != lblk)
			f_del = __es_scan_range(inode, &ext4_es_is_delayed,
						first, lblk - 1);
		if (f_del) {
			ret = __insert_pending(inode, first, prealloc);
			if (ret < 0)
				goto out;
			pendings += ret;
		} else
			__remove_pending(inode, first);

		last = EXT4_LBLK_CMASK(sbi, end) + sbi->s_cluster_ratio - 1;
		if (last != end)
			l_del = __es_scan_range(inode, &ext4_es_is_delayed,
						end + 1, last);
		if (l_del) {
			ret = __insert_pending(inode, last, prealloc);
			if (ret < 0)
				goto out;
			pendings += ret;
		} else
			__remove_pending(inode, last);
	}
out:
	return (ret < 0) ? ret : pendings;
}
