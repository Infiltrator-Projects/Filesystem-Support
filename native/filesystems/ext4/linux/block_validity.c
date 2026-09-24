#include <linux/fs.h>
#include <linux/slab.h>

#include "ext4.h"

struct ext4_system_zone {
	struct rb_node node;
	ext4_fsblk_t start;
	unsigned int length;
	u32 owner_ino;
};

static struct kmem_cache *ext4_system_zone_cache;

int __init ext4_init_system_zone(void)
{
	ext4_system_zone_cache = KMEM_CACHE(ext4_system_zone, 0);
	return ext4_system_zone_cache ? 0 : -ENOMEM;
}

void ext4_exit_system_zone(void)
{
	rcu_barrier();
	kmem_cache_destroy(ext4_system_zone_cache);
}

static void ext4_system_zones_free(struct ext4_system_blocks *zones)
{
	struct ext4_system_zone *zone;
	struct ext4_system_zone *next;

	rbtree_postorder_for_each_entry_safe(zone, next, &zones->root, node)
		kmem_cache_free(ext4_system_zone_cache, zone);
}

static bool ext4_system_zone_adjacent(const struct ext4_system_zone *left,
				      const struct ext4_system_zone *right)
{
	return left->owner_ino == right->owner_ino &&
	       right->start >= left->start &&
	       right->start - left->start == left->length;
}

static int ext4_system_zone_add(struct ext4_system_blocks *zones,
				ext4_fsblk_t start,
				unsigned int length,
				u32 owner_ino)
{
	struct rb_node **link = &zones->root.rb_node;
	struct rb_node *parent = NULL;
	struct ext4_system_zone *zone;
	struct ext4_system_zone *created;
	struct rb_node *previous_node;
	struct rb_node *next_node;
	unsigned int merged_length;
	ext4_fsblk_t merged_start;

	if (length == 0)
		return -EFSCORRUPTED;

	while (*link) {
		parent = *link;
		zone = rb_entry(parent, struct ext4_system_zone, node);

		if (start < zone->start) {
			link = &parent->rb_left;
		} else if (start - zone->start >= zone->length) {
			link = &parent->rb_right;
		} else {
			return -EFSCORRUPTED;
		}
	}

	created = kmem_cache_alloc(ext4_system_zone_cache, GFP_KERNEL);
	if (!created)
		return -ENOMEM;

	created->start = start;
	created->length = length;
	created->owner_ino = owner_ino;
	rb_link_node(&created->node, parent, link);
	rb_insert_color(&created->node, &zones->root);

	previous_node = rb_prev(&created->node);
	if (previous_node) {
		zone = rb_entry(previous_node, struct ext4_system_zone, node);
		if (ext4_system_zone_adjacent(zone, created)) {
			if (zone->length > UINT_MAX - created->length)
				goto overflow;

			merged_start = zone->start;
			merged_length = zone->length + created->length;
			rb_erase(previous_node, &zones->root);
			kmem_cache_free(ext4_system_zone_cache, zone);
			created->start = merged_start;
			created->length = merged_length;
		}
	}

	next_node = rb_next(&created->node);
	if (next_node) {
		zone = rb_entry(next_node, struct ext4_system_zone, node);
		if (ext4_system_zone_adjacent(created, zone)) {
			if (zone->length > UINT_MAX - created->length)
				goto overflow;

			created->length += zone->length;
			rb_erase(next_node, &zones->root);
			kmem_cache_free(ext4_system_zone_cache, zone);
		}
	}

	return 0;

overflow:
	/*
	 * The tree must not retain a partially accepted range on an arithmetic
	 * failure. Remove the just-created node before reporting corruption.
	 */
	rb_erase(&created->node, &zones->root);
	kmem_cache_free(ext4_system_zone_cache, created);
	return -EFSCORRUPTED;
}

static int ext4_protect_special_inode(struct super_block *sb,
				      struct ext4_system_blocks *zones,
				      u32 ino)
{
	struct inode *inode;
	struct ext4_map_blocks map;
	u32 logical = 0;
	u32 block_count;
	int mapped;
	int err = 0;

	if (ino < EXT4_ROOT_INO ||
	    ino > le32_to_cpu(EXT4_SB(sb)->s_es->s_inodes_count))
		return -EINVAL;

	inode = ext4_iget(sb, ino, EXT4_IGET_SPECIAL);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	block_count = (inode->i_size + sb->s_blocksize - 1) >>
		      sb->s_blocksize_bits;

	while (logical < block_count) {
		cond_resched();
		map.m_lblk = logical;
		map.m_len = block_count - logical;

		mapped = ext4_map_blocks(NULL, inode, &map, 0);
		if (mapped < 0) {
			err = mapped;
			break;
		}
		if (mapped == 0) {
			logical++;
			continue;
		}

		err = ext4_system_zone_add(
			zones, map.m_pblk, mapped, ino);
		if (err)
			break;
		logical += mapped;
	}

	iput(inode);
	return err;
}

static void ext4_system_zone_rcu_free(struct rcu_head *rcu)
{
	struct ext4_system_blocks *zones =
		container_of(rcu, struct ext4_system_blocks, rcu);

	ext4_system_zones_free(zones);
	kfree(zones);
}

int ext4_setup_system_zone(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_system_blocks *zones;
	const ext4_group_t groups = ext4_get_groups_count(sb);
	ext4_group_t group;
	int err;

	zones = kzalloc(sizeof(*zones), GFP_KERNEL);
	if (!zones)
		return -ENOMEM;

	for (group = 0; group < groups; ++group) {
		struct ext4_group_desc *desc;
		unsigned int base_metadata;

		cond_resched();
		base_metadata = ext4_num_base_meta_blocks(sb, group);
		if (base_metadata) {
			err = ext4_system_zone_add(
				zones, ext4_group_first_block_no(sb, group),
				base_metadata, 0);
			if (err)
				goto fail;
		}

		desc = ext4_get_group_desc(sb, group, NULL);
		if (!desc) {
			err = -EFSCORRUPTED;
			goto fail;
		}

		err = ext4_system_zone_add(
			zones, ext4_block_bitmap(sb, desc), 1, 0);
		if (err)
			goto fail;

		err = ext4_system_zone_add(
			zones, ext4_inode_bitmap(sb, desc), 1, 0);
		if (err)
			goto fail;

		err = ext4_system_zone_add(
			zones, ext4_inode_table(sb, desc),
			sbi->s_itb_per_group, 0);
		if (err)
			goto fail;
	}

	if (ext4_has_feature_journal(sb) && sbi->s_es->s_journal_inum) {
		err = ext4_protect_special_inode(
			sb, zones, le32_to_cpu(sbi->s_es->s_journal_inum));
		if (err)
			goto fail;
	}

	rcu_assign_pointer(sbi->s_system_blks, zones);
	return 0;

fail:
	ext4_system_zones_free(zones);
	kfree(zones);
	return err;
}

void ext4_release_system_zone(struct super_block *sb)
{
	struct ext4_system_blocks *zones;

	zones = rcu_dereference_protected(
		EXT4_SB(sb)->s_system_blks,
		lockdep_is_held(&sb->s_umount));
	rcu_assign_pointer(EXT4_SB(sb)->s_system_blks, NULL);

	if (zones)
		call_rcu(&zones->rcu, ext4_system_zone_rcu_free);
}

int ext4_sb_block_valid(struct super_block *sb, struct inode *inode,
			ext4_fsblk_t start, unsigned int count)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	const ext4_fsblk_t total = ext4_blocks_count(sbi->s_es);
	struct ext4_system_blocks *zones;
	struct rb_node *node;
	ext4_fsblk_t last;
	int valid = 1;

	if (count == 0 ||
	    start <= le32_to_cpu(sbi->s_es->s_first_data_block) ||
	    start >= total ||
	    count > total - start)
		return 0;

	last = start + count - 1;
	rcu_read_lock();
	zones = rcu_dereference(sbi->s_system_blks);
	if (!zones)
		goto out;

	node = zones->root.rb_node;
	while (node) {
		struct ext4_system_zone *zone =
			rb_entry(node, struct ext4_system_zone, node);

		if (last < zone->start) {
			node = node->rb_left;
		} else if (start >= zone->start &&
			   start - zone->start >= zone->length) {
			node = node->rb_right;
		} else {
			valid = inode && zone->owner_ino == inode->i_ino;
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
	unsigned int i;

	if (journal && inode == journal->j_inode)
		return 0;

	for (i = 0; i < count; ++i) {
		const ext4_fsblk_t block = le32_to_cpu(refs[i]);

		if (block && !ext4_inode_block_valid(inode, block, 1)) {
			ext4_error_inode(
				inode, function, line, block,
				"invalid block");
			return -EFSCORRUPTED;
		}
	}

	return 0;
}

void __dump_mmp_msg(struct super_block *sb, struct mmp_struct *mmp,
		    const char *function, unsigned int line,
		    const char *msg);
