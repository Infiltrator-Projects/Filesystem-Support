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
