/* Infiltrator Filesystem Support — SFS Linux mapping and object engine adapter.
 * Extent traversal, node-tree handling and object-container traversal are kept
 * together as one host-facing mapping responsibility.
 */

#include <linux/buffer_head.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include "asfs_fs.h"

static int sfs_validate_btree(
    struct super_block *sb,
    const struct BTreeContainer *tree,
    u32 *capacity)
{
    return ifs_sfs_validate_btree_layout(
        sb->s_blocksize,
        be16_to_cpu(tree->nodecount),
        tree->nodesize,
        tree->isleaf == TRUE,
        capacity) == 0 ? 0 : -EUCLEAN;
}

static struct BNode *sfs_bnode_at(
    struct BTreeContainer *tree,
    u32 index)
{
    return (struct BNode *)
        ((u8 *)tree->bnode +
         index * tree->nodesize);
}

static const struct BNode *sfs_bnode_at_const(
    const struct BTreeContainer *tree,
    u32 index)
{
    return (const struct BNode *)
        ((const u8 *)tree->bnode +
         index * tree->nodesize);
}

static u32 sfs_select_bnode_index(
    const struct BTreeContainer *tree,
    u32 key)
{
    u32 count = be16_to_cpu(tree->nodecount);
    u32 index;

    if (count == 0U)
        return 0U;

    for (index = count; index > 0U; --index) {
        const struct BNode *node =
            sfs_bnode_at_const(tree, index - 1U);

        if (key >= be32_to_cpu(node->key))
            return index - 1U;
    }

    return 0U;
}

static int sfs_find_leaf(
    struct super_block *sb,
    u32 key,
    struct buffer_head **returned_bh,
    struct BNode **returned_node)
{
    u32 block = ASFS_SB(sb)->extentbnoderoot;
    u32 budget = ASFS_SB(sb)->totalblocks;

    if (!returned_bh || !returned_node)
        return -EINVAL;

    *returned_bh = NULL;
    *returned_node = NULL;

    while (block != 0U) {
        struct buffer_head *bh;
        struct fsBNodeContainer *container;
        struct BTreeContainer *tree;
        u32 capacity;
        u32 count;

        if (budget-- == 0U)
            return -EUCLEAN;

        bh = asfs_breadcheck(
            sb, block, ASFS_BNODECONTAINER_ID);
        if (!bh)
            return -EIO;

        container =
            (struct fsBNodeContainer *)bh->b_data;
        tree = &container->btc;

        if (sfs_validate_btree(
                sb, tree, &capacity) != 0) {
            asfs_brelse(bh);
            return -EUCLEAN;
        }

        count = be16_to_cpu(tree->nodecount);
        if (tree->isleaf == TRUE) {
            *returned_bh = bh;
            if (count != 0U)
                *returned_node = sfs_bnode_at(
                    tree,
                    sfs_select_bnode_index(
                        tree, key));
            return 0;
        }

        if (count == 0U ||
            tree->nodesize != sizeof(struct BNode)) {
            asfs_brelse(bh);
            return -EUCLEAN;
        }

        {
            const struct BNode *node =
                sfs_bnode_at_const(
                    tree,
                    sfs_select_bnode_index(
                        tree, key));
            const u32 next =
                be32_to_cpu(node->data);

            asfs_brelse(bh);
            if (next == 0U ||
                next >= ASFS_SB(sb)->totalblocks)
                return -EUCLEAN;
            block = next;
        }
    }

    return -EUCLEAN;
}

int asfs_getextent(
    struct super_block *sb,
    u32 key,
    struct buffer_head **returned_bh,
    struct fsExtentBNode **returned_extent)
{
    struct BNode *node = NULL;
    int result;

    if (!returned_bh || !returned_extent)
        return -EINVAL;

    *returned_bh = NULL;
    *returned_extent = NULL;

    result = sfs_find_leaf(
        sb, key, returned_bh, &node);
    if (result != 0)
        return result;

    if (!node || be32_to_cpu(node->key) != key) {
        asfs_brelse(*returned_bh);
        *returned_bh = NULL;
        return -ENOENT;
    }

    *returned_extent =
        (struct fsExtentBNode *)node;
    if (ifs_sfs_validate_extent(
            be32_to_cpu((*returned_extent)->key),
            be32_to_cpu((*returned_extent)->next),
            be16_to_cpu((*returned_extent)->blocks),
            ASFS_SB(sb)->totalblocks) != 0) {
        asfs_brelse(*returned_bh);
        *returned_bh = NULL;
        *returned_extent = NULL;
        return -EUCLEAN;
    }

    return 0;
}

#ifdef CONFIG_ASFS_RW

static struct BNode *sfs_insert_sorted(
    struct BTreeContainer *tree,
    u32 capacity,
    u32 key)
{
    u32 count = be16_to_cpu(tree->nodecount);
    u32 index;

    if (count >= capacity)
        return NULL;

    index = 0U;
    while (index < count &&
           key > be32_to_cpu(
               sfs_bnode_at(tree, index)->key))
        index++;

    if (index < count &&
        be32_to_cpu(
            sfs_bnode_at(tree, index)->key) == key)
        return ERR_PTR(-EEXIST);

    if (index < count) {
        memmove(
            (u8 *)sfs_bnode_at(tree, index + 1U),
            (u8 *)sfs_bnode_at(tree, index),
            (count - index) * tree->nodesize);
    }

    memset(
        sfs_bnode_at(tree, index),
        0, tree->nodesize);
    sfs_bnode_at(tree, index)->key =
        cpu_to_be32(key);
    tree->nodecount = cpu_to_be16(count + 1U);
    return sfs_bnode_at(tree, index);
}

static int sfs_promote_full_root(
    struct super_block *sb,
    struct buffer_head *root_bh)
{
    struct fsBNodeContainer *root =
        (struct fsBNodeContainer *)root_bh->b_data;
    struct BTreeContainer *tree = &root->btc;
    struct buffer_head *copy_bh;
    struct fsBNodeContainer *copy;
    const u32 root_block =
        be32_to_cpu(root->bheader.ownblock);
    const u32 count =
        be16_to_cpu(tree->nodecount);
    u32 first_key;
    u32 copy_block;
    int result;

    if (count == 0U)
        return -EUCLEAN;

    first_key =
        be32_to_cpu(tree->bnode[0].key);

    result = asfs_allocadminspace(
        sb, &copy_block);
    if (result != 0)
        return result;

    copy_bh = asfs_getzeroblk(
        sb, copy_block);
    if (!copy_bh) {
        (void)asfs_freeadminspace(
            sb, copy_block);
        return -EIO;
    }

    copy =
        (struct fsBNodeContainer *)copy_bh->b_data;
    memcpy(copy, root, sb->s_blocksize);
    copy->bheader.ownblock =
        cpu_to_be32(copy_block);
    asfs_bstore(sb, copy_bh);
    asfs_brelse(copy_bh);

    memset(root_bh->b_data, 0, sb->s_blocksize);
    root =
        (struct fsBNodeContainer *)root_bh->b_data;
    root->bheader.id =
        cpu_to_be32(ASFS_BNODECONTAINER_ID);
    root->bheader.ownblock =
        cpu_to_be32(root_block);
    root->btc.isleaf = FALSE;
    root->btc.nodesize = sizeof(struct BNode);
    root->btc.nodecount = cpu_to_be16(1U);
    root->btc.bnode[0].key =
        cpu_to_be32(first_key);
    root->btc.bnode[0].data =
        cpu_to_be32(copy_block);
    asfs_bstore(sb, root_bh);
    return 0;
}

static int sfs_split_child(
    struct super_block *sb,
    struct buffer_head *parent_bh,
    u32 child_index,
    struct buffer_head *child_bh,
    u32 *right_block_out,
    u32 *right_key_out)
{
    struct fsBNodeContainer *parent_container =
        (struct fsBNodeContainer *)parent_bh->b_data;
    struct BTreeContainer *parent =
        &parent_container->btc;
    struct fsBNodeContainer *child_container =
        (struct fsBNodeContainer *)child_bh->b_data;
    struct BTreeContainer *child =
        &child_container->btc;
    struct buffer_head *right_bh;
    struct fsBNodeContainer *right_container;
    struct BTreeContainer *right;
    u32 parent_capacity;
    u32 child_capacity;
    u32 child_count;
    u32 left_count;
    u32 right_count;
    u32 right_block;
    u32 right_key;
    struct BNode *parent_node;
    int result;

    if (!right_block_out || !right_key_out ||
        sfs_validate_btree(
            sb, parent, &parent_capacity) != 0 ||
        sfs_validate_btree(
            sb, child, &child_capacity) != 0 ||
        parent->isleaf == TRUE ||
        child_index >=
            be16_to_cpu(parent->nodecount) ||
        be16_to_cpu(parent->nodecount) >=
            parent_capacity)
        return -EUCLEAN;

    child_count =
        be16_to_cpu(child->nodecount);
    if (child_count != child_capacity ||
        child_count < 2U)
        return -EUCLEAN;

    left_count = child_count / 2U;
    right_count = child_count - left_count;

    result = asfs_allocadminspace(
        sb, &right_block);
    if (result != 0)
        return result;

    right_bh = asfs_getzeroblk(
        sb, right_block);
    if (!right_bh) {
        (void)asfs_freeadminspace(
            sb, right_block);
        return -EIO;
    }

    right_container =
        (struct fsBNodeContainer *)
            right_bh->b_data;
    right_container->bheader.id =
        cpu_to_be32(ASFS_BNODECONTAINER_ID);
    right_container->bheader.ownblock =
        cpu_to_be32(right_block);
    right = &right_container->btc;
    right->isleaf = child->isleaf;
    right->nodesize = child->nodesize;
    right->nodecount =
        cpu_to_be16(right_count);
    memcpy(
        right->bnode,
        (u8 *)child->bnode +
            left_count * child->nodesize,
        right_count * child->nodesize);

    right_key =
        be32_to_cpu(right->bnode[0].key);

    memset(
        (u8 *)child->bnode +
            left_count * child->nodesize,
        0,
        right_count * child->nodesize);
    child->nodecount =
        cpu_to_be16(left_count);

    parent_node = sfs_insert_sorted(
        parent, parent_capacity, right_key);
    if (IS_ERR(parent_node) || !parent_node) {
        memcpy(
            (u8 *)child->bnode +
                left_count * child->nodesize,
            right->bnode,
            right_count * child->nodesize);
        child->nodecount =
            cpu_to_be16(child_count);
        asfs_brelse(right_bh);
        (void)asfs_freeadminspace(
            sb, right_block);
        return IS_ERR(parent_node)
            ? PTR_ERR(parent_node) : -EUCLEAN;
    }

    parent_node->data =
        cpu_to_be32(right_block);
    sfs_bnode_at(parent, child_index)->key =
        child->bnode[0].key;

    asfs_bstore(sb, child_bh);
    asfs_bstore(sb, right_bh);
    asfs_bstore(sb, parent_bh);
    asfs_brelse(right_bh);

    *right_block_out = right_block;
    *right_key_out = right_key;
    return 0;
}

static int sfs_create_extent_node(
    struct super_block *sb,
    u32 key,
    struct buffer_head **returned_bh,
    struct fsExtentBNode **returned_extent)
{
    const u32 root_block =
        ASFS_SB(sb)->extentbnoderoot;
    u32 block = root_block;
    u32 budget = ASFS_SB(sb)->totalblocks;

    if (!returned_bh || !returned_extent)
        return -EINVAL;

    *returned_bh = NULL;
    *returned_extent = NULL;

restart:
    block = root_block;

    while (block != 0U) {
        struct buffer_head *bh;
        struct fsBNodeContainer *container;
        struct BTreeContainer *tree;
        u32 capacity;
        u32 count;

        if (budget-- == 0U)
            return -EUCLEAN;

        bh = asfs_breadcheck(
            sb, block, ASFS_BNODECONTAINER_ID);
        if (!bh)
            return -EIO;

        container =
            (struct fsBNodeContainer *)bh->b_data;
        tree = &container->btc;
        if (sfs_validate_btree(
                sb, tree, &capacity) != 0) {
            asfs_brelse(bh);
            return -EUCLEAN;
        }

        count = be16_to_cpu(tree->nodecount);
        if (count >= capacity) {
            int result;

            if (block != root_block) {
                asfs_brelse(bh);
                return -EUCLEAN;
            }

            result = sfs_promote_full_root(
                sb, bh);
            asfs_brelse(bh);
            if (result != 0)
                return result;
            goto restart;
        }

        if (tree->isleaf == TRUE) {
            struct BNode *node =
                sfs_insert_sorted(
                    tree, capacity, key);

            if (IS_ERR(node)) {
                int result = PTR_ERR(node);
                asfs_brelse(bh);
                return result;
            }
            if (!node) {
                asfs_brelse(bh);
                return -EUCLEAN;
            }

            asfs_bstore(sb, bh);
            *returned_bh = bh;
            *returned_extent =
                (struct fsExtentBNode *)node;
            return 0;
        }

        if (count == 0U ||
            tree->nodesize != sizeof(struct BNode)) {
            asfs_brelse(bh);
            return -EUCLEAN;
        }

        {
            u32 child_index =
                sfs_select_bnode_index(
                    tree, key);
            struct BNode *child_node =
                sfs_bnode_at(
                    tree, child_index);
            u32 child_block =
                be32_to_cpu(child_node->data);
            struct buffer_head *child_bh;
            struct fsBNodeContainer *child_container;
            struct BTreeContainer *child_tree;
            u32 child_capacity;
            u32 right_block = 0U;
            u32 right_key = 0U;
            int result;

            if (child_block == 0U ||
                child_block >=
                    ASFS_SB(sb)->totalblocks) {
                asfs_brelse(bh);
                return -EUCLEAN;
            }

            child_bh = asfs_breadcheck(
                sb, child_block,
                ASFS_BNODECONTAINER_ID);
            if (!child_bh) {
                asfs_brelse(bh);
                return -EIO;
            }

            child_container =
                (struct fsBNodeContainer *)
                    child_bh->b_data;
            child_tree =
                &child_container->btc;
            if (sfs_validate_btree(
                    sb, child_tree,
                    &child_capacity) != 0) {
                asfs_brelse(child_bh);
                asfs_brelse(bh);
                return -EUCLEAN;
            }

            if (be16_to_cpu(
                    child_tree->nodecount) >=
                child_capacity) {
                result = sfs_split_child(
                    sb, bh, child_index,
                    child_bh,
                    &right_block, &right_key);
                if (result != 0) {
                    asfs_brelse(child_bh);
                    asfs_brelse(bh);
                    return result;
                }

                if (key >= right_key)
                    child_block = right_block;
            }

            asfs_brelse(child_bh);
            asfs_brelse(bh);
            block = child_block;
        }
    }

    return -EUCLEAN;
}

static int sfs_find_parent(
    struct super_block *sb,
    u32 child_block,
    u32 child_first_key,
    struct buffer_head **parent_bh,
    u32 *parent_index)
{
    u32 block = ASFS_SB(sb)->extentbnoderoot;
    u32 budget = ASFS_SB(sb)->totalblocks;

    if (!parent_bh || !parent_index)
        return -EINVAL;

    *parent_bh = NULL;
    *parent_index = 0U;

    if (child_block == block)
        return 0;

    while (block != 0U) {
        struct buffer_head *bh;
        struct fsBNodeContainer *container;
        struct BTreeContainer *tree;
        u32 capacity;
        u32 count;
        u32 index;

        if (budget-- == 0U)
            return -EUCLEAN;

        bh = asfs_breadcheck(
            sb, block, ASFS_BNODECONTAINER_ID);
        if (!bh)
            return -EIO;

        container =
            (struct fsBNodeContainer *)bh->b_data;
        tree = &container->btc;
        if (sfs_validate_btree(
                sb, tree, &capacity) != 0 ||
            tree->isleaf == TRUE) {
            asfs_brelse(bh);
            return -EUCLEAN;
        }

        count = be16_to_cpu(tree->nodecount);
        for (index = 0U; index < count; ++index) {
            if (be32_to_cpu(
                    sfs_bnode_at(
                        tree, index)->data) ==
                child_block) {
                *parent_bh = bh;
                *parent_index = index;
                return 0;
            }
        }

        if (count == 0U) {
            asfs_brelse(bh);
            return -EUCLEAN;
        }

        index = sfs_select_bnode_index(
            tree, child_first_key);
        block = be32_to_cpu(
            sfs_bnode_at(tree, index)->data);
        asfs_brelse(bh);

        if (block == 0U ||
            block >= ASFS_SB(sb)->totalblocks)
            return -EUCLEAN;
    }

    return -EUCLEAN;
}

static void sfs_remove_bnode_at(
    struct BTreeContainer *tree,
    u32 index)
{
    u32 count =
        be16_to_cpu(tree->nodecount);

    if (index >= count)
        return;

    if (index + 1U < count) {
        memmove(
            sfs_bnode_at(tree, index),
            sfs_bnode_at(tree, index + 1U),
            (count - index - 1U) *
                tree->nodesize);
    }

    count--;
    memset(
        sfs_bnode_at(tree, count),
        0, tree->nodesize);
    tree->nodecount = cpu_to_be16(count);
}

static int sfs_update_first_key_upward(
    struct super_block *sb,
    u32 child_block,
    u32 old_key,
    u32 new_key)
{
    u32 budget = ASFS_SB(sb)->totalblocks;

    while (child_block !=
           ASFS_SB(sb)->extentbnoderoot) {
        struct buffer_head *parent_bh = NULL;
        struct fsBNodeContainer *parent_container;
        struct BTreeContainer *parent;
        u32 index;
        u32 parent_block;
        u32 parent_old_key;
        int result;

        if (budget-- == 0U)
            return -EUCLEAN;

        result = sfs_find_parent(
            sb, child_block, old_key,
            &parent_bh, &index);
        if (result != 0)
            return result;
        if (!parent_bh)
            return -EUCLEAN;

        parent_container =
            (struct fsBNodeContainer *)
                parent_bh->b_data;
        parent = &parent_container->btc;
        parent_block =
            be32_to_cpu(
                parent_container->
                    bheader.ownblock);
        parent_old_key =
            be32_to_cpu(parent->bnode[0].key);

        sfs_bnode_at(parent, index)->key =
            cpu_to_be32(new_key);
        asfs_bstore(sb, parent_bh);

        if (index != 0U) {
            asfs_brelse(parent_bh);
            return 0;
        }

        asfs_brelse(parent_bh);
        child_block = parent_block;
        old_key = parent_old_key;
    }

    return 0;
}

static int sfs_collapse_root_if_possible(
    struct super_block *sb)
{
    const u32 root_block =
        ASFS_SB(sb)->extentbnoderoot;
    struct buffer_head *root_bh;
    struct fsBNodeContainer *root;
    struct BTreeContainer *tree;
    u32 capacity;
    u32 child_block;
    struct buffer_head *child_bh;

    root_bh = asfs_breadcheck(
        sb, root_block,
        ASFS_BNODECONTAINER_ID);
    if (!root_bh)
        return -EIO;

    root =
        (struct fsBNodeContainer *)
            root_bh->b_data;
    tree = &root->btc;
    if (sfs_validate_btree(
            sb, tree, &capacity) != 0) {
        asfs_brelse(root_bh);
        return -EUCLEAN;
    }

    if (tree->isleaf == TRUE ||
        be16_to_cpu(tree->nodecount) != 1U) {
        asfs_brelse(root_bh);
        return 0;
    }

    child_block =
        be32_to_cpu(tree->bnode[0].data);
    if (child_block == 0U ||
        child_block >= ASFS_SB(sb)->totalblocks) {
        asfs_brelse(root_bh);
        return -EUCLEAN;
    }

    child_bh = asfs_breadcheck(
        sb, child_block,
        ASFS_BNODECONTAINER_ID);
    if (!child_bh) {
        asfs_brelse(root_bh);
        return -EIO;
    }

    memcpy(
        root_bh->b_data,
        child_bh->b_data,
        sb->s_blocksize);
    root =
        (struct fsBNodeContainer *)
            root_bh->b_data;
    root->bheader.ownblock =
        cpu_to_be32(root_block);
    asfs_bstore(sb, root_bh);
    asfs_brelse(child_bh);
    asfs_brelse(root_bh);

    return asfs_freeadminspace(
        sb, child_block);
}

static int sfs_remove_empty_container(
    struct super_block *sb,
    u32 child_block,
    u32 old_first_key)
{
    const u32 root_block =
        ASFS_SB(sb)->extentbnoderoot;
    u32 budget = ASFS_SB(sb)->totalblocks;

    while (child_block != root_block) {
        struct buffer_head *parent_bh = NULL;
        struct fsBNodeContainer *parent_container;
        struct BTreeContainer *parent;
        u32 parent_index;
        u32 parent_block;
        u32 parent_old_key;
        u32 parent_count;
        int result;

        if (budget-- == 0U)
            return -EUCLEAN;

        result = sfs_find_parent(
            sb, child_block, old_first_key,
            &parent_bh, &parent_index);
        if (result != 0)
            return result;
        if (!parent_bh)
            return -EUCLEAN;

        parent_container =
            (struct fsBNodeContainer *)
                parent_bh->b_data;
        parent = &parent_container->btc;
        parent_block =
            be32_to_cpu(
                parent_container->
                    bheader.ownblock);
        parent_old_key =
            be32_to_cpu(parent->bnode[0].key);

        sfs_remove_bnode_at(
            parent, parent_index);
        parent_count =
            be16_to_cpu(parent->nodecount);
        asfs_bstore(sb, parent_bh);

        result = asfs_freeadminspace(
            sb, child_block);
        if (result != 0) {
            asfs_brelse(parent_bh);
            return result;
        }

        if (parent_block == root_block) {
            if (parent_count == 0U) {
                parent->isleaf = TRUE;
                parent->nodesize =
                    IFS_SFS_BTREE_EXTENT_NODE_MIN_SIZE;
                asfs_bstore(sb, parent_bh);
                asfs_brelse(parent_bh);
                return 0;
            }

            asfs_brelse(parent_bh);
            return sfs_collapse_root_if_possible(
                sb);
        }

        if (parent_count != 0U) {
            const u32 new_first_key =
                be32_to_cpu(
                    parent->bnode[0].key);
            const bool first_changed =
                parent_index == 0U;

            asfs_brelse(parent_bh);
            if (first_changed)
                return sfs_update_first_key_upward(
                    sb, parent_block,
                    parent_old_key,
                    new_first_key);
            return 0;
        }

        asfs_brelse(parent_bh);
        child_block = parent_block;
        old_first_key = parent_old_key;
    }

    return 0;
}

int asfs_deletebnode(
    struct super_block *sb,
    struct buffer_head *bh,
    u32 key)
{
    struct fsBNodeContainer *container;
    struct BTreeContainer *tree;
    u32 capacity;
    u32 count;
    u32 index;
    u32 old_first_key;
    u32 block;

    if (!bh)
        return -EINVAL;

    container =
        (struct fsBNodeContainer *)bh->b_data;
    tree = &container->btc;
    if (sfs_validate_btree(
            sb, tree, &capacity) != 0 ||
        tree->isleaf != TRUE)
        return -EUCLEAN;

    count = be16_to_cpu(tree->nodecount);
    if (count == 0U)
        return -ENOENT;

    old_first_key =
        be32_to_cpu(tree->bnode[0].key);
    block =
        be32_to_cpu(container->bheader.ownblock);

    for (index = 0U; index < count; ++index) {
        if (be32_to_cpu(
                sfs_bnode_at(
                    tree, index)->key) == key)
            break;
    }
    if (index == count)
        return -ENOENT;

    sfs_remove_bnode_at(tree, index);
    asfs_bstore(sb, bh);

    count = be16_to_cpu(tree->nodecount);
    if (count == 0U) {
        if (block ==
            ASFS_SB(sb)->extentbnoderoot)
            return 0;
        return sfs_remove_empty_container(
            sb, block, old_first_key);
    }

    if (index == 0U) {
        return sfs_update_first_key_upward(
            sb, block, old_first_key,
            be32_to_cpu(tree->bnode[0].key));
    }

    return 0;
}

int asfs_deleteextents(
    struct super_block *sb,
    u32 key)
{
    u32 budget = ASFS_SB(sb)->totalblocks;

    while (key != 0U) {
        struct buffer_head *bh = NULL;
        struct fsExtentBNode *extent = NULL;
        u32 next;
        u32 extent_key;
        u16 blocks;
        int result;

        if (budget-- == 0U)
            return -EUCLEAN;

        result = asfs_getextent(
            sb, key, &bh, &extent);
        if (result != 0)
            return result;

        next = be32_to_cpu(extent->next);
        extent_key = be32_to_cpu(extent->key);
        blocks = be16_to_cpu(extent->blocks);

        result = asfs_freespace(
            sb, extent_key, blocks);
        if (result == 0)
            result = asfs_deletebnode(
                sb, bh, extent_key);

        asfs_brelse(bh);
        if (result != 0)
            return result;

        key = next;
    }

    return 0;
}

int asfs_addblocks(
    struct super_block *sb,
    u16 blocks,
    u32 new_space,
    u32 object_node,
    u32 *last_extent)
{
    struct buffer_head *bh = NULL;
    struct fsExtentBNode *extent = NULL;
    int result;

    if (!last_extent || blocks == 0U ||
        ifs_sfs_validate_extent(
            new_space, 0U, blocks,
            ASFS_SB(sb)->totalblocks) != 0)
        return -EINVAL;

    if (*last_extent != 0U) {
        const u32 previous_key =
            *last_extent;
        u32 previous_end;
        u32 previous_blocks;

        result = asfs_getextent(
            sb, previous_key, &bh, &extent);
        if (result != 0)
            return result;

        previous_blocks =
            be16_to_cpu(extent->blocks);
        previous_end =
            be32_to_cpu(extent->key) +
            previous_blocks;

        if (previous_end == new_space &&
            previous_blocks + (u32)blocks <=
                0xffffU) {
            extent->blocks = cpu_to_be16(
                previous_blocks + blocks);
            asfs_bstore(sb, bh);
            asfs_brelse(bh);
            ASFS_SB(sb)->
                block_rovingblockptr =
                new_space + blocks;
            return 0;
        }

        asfs_brelse(bh);
        bh = NULL;
        extent = NULL;

        result = sfs_create_extent_node(
            sb, new_space,
            &bh, &extent);
        if (result != 0)
            return result;

        extent->key = cpu_to_be32(new_space);
        extent->prev =
            cpu_to_be32(previous_key);
        extent->next = 0U;
        extent->blocks =
            cpu_to_be16(blocks);
        asfs_bstore(sb, bh);
        asfs_brelse(bh);
        bh = NULL;
        extent = NULL;

        result = asfs_getextent(
            sb, previous_key,
            &bh, &extent);
        if (result != 0) {
            struct buffer_head *cleanup_bh = NULL;
            struct fsExtentBNode *cleanup_extent = NULL;

            if (asfs_getextent(
                    sb, new_space,
                    &cleanup_bh,
                    &cleanup_extent) == 0) {
                (void)asfs_deletebnode(
                    sb, cleanup_bh,
                    new_space);
                asfs_brelse(cleanup_bh);
            }
            return result;
        }

        extent->next =
            cpu_to_be32(new_space);
        asfs_bstore(sb, bh);
        asfs_brelse(bh);

        *last_extent = new_space;
        ASFS_SB(sb)->block_rovingblockptr =
            new_space + blocks;
        return 0;
    }

    result = sfs_create_extent_node(
        sb, new_space, &bh, &extent);
    if (result != 0)
        return result;

    extent->key = cpu_to_be32(new_space);
    extent->prev =
        cpu_to_be32(object_node | MSB_MASK);
    extent->next = 0U;
    extent->blocks = cpu_to_be16(blocks);
    asfs_bstore(sb, bh);
    asfs_brelse(bh);

    *last_extent = new_space;
    ASFS_SB(sb)->block_rovingblockptr =
        new_space + blocks;
    return 0;
}

#endif


/* ===== node tree ===== */
#include <linux/buffer_head.h>
#include <linux/errno.h>
#include <linux/overflow.h>
#include <linux/string.h>
#include "asfs_fs.h"

static u32 sfs_node_leaf_capacity(struct super_block *sb)
{
    if (sb->s_blocksize <= sizeof(struct fsNodeContainer))
        return 0U;

    return (sb->s_blocksize -
            sizeof(struct fsNodeContainer)) /
           NODE_STRUCT_SIZE;
}

static u32 sfs_node_index_capacity(struct super_block *sb)
{
    if (sb->s_blocksize <= sizeof(struct fsNodeContainer))
        return 0U;

    return (sb->s_blocksize -
            sizeof(struct fsNodeContainer)) /
           sizeof(u32);
}

static int sfs_decode_child_pointer(
    struct super_block *sb,
    u32 raw,
    u32 *child_block,
    bool *full)
{
    const unsigned int shift =
        sb->s_blocksize_bits - ASFS_BLCKFACCURACY;
    u32 block;

    if (!child_block || !full ||
        sb->s_blocksize_bits < ASFS_BLCKFACCURACY ||
        raw == 0U)
        return -EUCLEAN;

    block = raw >> shift;
    if (block == 0U || block >= ASFS_SB(sb)->totalblocks)
        return -EUCLEAN;

    *child_block = block;
    *full = (raw & 1U) != 0U;
    return 0;
}

static int sfs_encode_child_pointer(
    struct super_block *sb,
    u32 child_block,
    bool full,
    u32 *raw)
{
    const unsigned int shift =
        sb->s_blocksize_bits - ASFS_BLCKFACCURACY;
    u32 encoded;

    if (!raw ||
        sb->s_blocksize_bits < ASFS_BLCKFACCURACY ||
        child_block == 0U ||
        child_block >= ASFS_SB(sb)->totalblocks ||
        check_shl_overflow(child_block, shift, &encoded))
        return -EINVAL;

    *raw = encoded | (full ? 1U : 0U);
    return 0;
}

static bool sfs_index_container_full(
    struct super_block *sb,
    const struct fsNodeContainer *container)
{
    const u32 capacity = sfs_node_index_capacity(sb);
    u32 index;

    if (capacity == 0U)
        return true;

    for (index = 0U; index < capacity; ++index) {
        const u32 raw = be32_to_cpu(container->node[index]);

        if (raw == 0U || (raw & 1U) == 0U)
            return false;
    }

    return true;
}

static bool sfs_index_container_empty(
    struct super_block *sb,
    const struct fsNodeContainer *container)
{
    const u32 capacity = sfs_node_index_capacity(sb);
    u32 index;

    for (index = 0U; index < capacity; ++index) {
        if (container->node[index] != 0U)
            return false;
    }

    return true;
}

static int sfs_find_parent_container(
    struct super_block *sb,
    u32 child_block,
    u32 child_node_number,
    struct buffer_head **parent_bh,
    u32 *parent_slot)
{
    u32 block = ASFS_SB(sb)->objectnoderoot;
    u32 budget = ASFS_SB(sb)->totalblocks;

    if (!parent_bh || !parent_slot)
        return -EINVAL;

    *parent_bh = NULL;
    *parent_slot = 0U;

    if (child_block == block)
        return 0;

    while (block != 0U) {
        struct buffer_head *bh;
        struct fsNodeContainer *container;
        u32 slot;
        u32 raw;
        u32 next;
        bool full;

        if (budget-- == 0U)
            return -EUCLEAN;

        bh = asfs_breadcheck(
            sb, block, ASFS_NODECONTAINER_ID);
        if (!bh)
            return -EIO;

        container =
            (struct fsNodeContainer *)bh->b_data;
        if (be32_to_cpu(container->nodes) <= 1U) {
            asfs_brelse(bh);
            return -EUCLEAN;
        }

        if (ifs_sfs_node_index_slot(
                sb->s_blocksize,
                be32_to_cpu(container->nodenumber),
                be32_to_cpu(container->nodes),
                child_node_number,
                &slot) != 0 ||
            slot >= sfs_node_index_capacity(sb)) {
            asfs_brelse(bh);
            return -EUCLEAN;
        }

        raw = be32_to_cpu(container->node[slot]);
        if (sfs_decode_child_pointer(
                sb, raw, &next, &full) != 0) {
            asfs_brelse(bh);
            return -EUCLEAN;
        }

        if (next == child_block) {
            *parent_bh = bh;
            *parent_slot = slot;
            return 0;
        }

        asfs_brelse(bh);
        block = next;
    }

    return -EUCLEAN;
}

int asfs_getnode(
    struct super_block *sb,
    u32 node_number,
    struct buffer_head **returned_bh,
    struct fsObjectNode **returned_node)
{
    u32 block;
    u32 budget;

    if (!returned_bh || !returned_node)
        return -EINVAL;

    *returned_bh = NULL;
    *returned_node = NULL;
    block = ASFS_SB(sb)->objectnoderoot;
    budget = ASFS_SB(sb)->totalblocks;

    while (block != 0U) {
        struct buffer_head *bh;
        struct fsNodeContainer *container;
        u32 nodes;
        u32 base;

        if (budget-- == 0U)
            return -EUCLEAN;

        bh = asfs_breadcheck(
            sb, block, ASFS_NODECONTAINER_ID);
        if (!bh)
            return -EIO;

        container =
            (struct fsNodeContainer *)bh->b_data;
        nodes = be32_to_cpu(container->nodes);
        base = be32_to_cpu(container->nodenumber);

        if (nodes == 1U) {
            u32 slot;

            if (ifs_sfs_node_leaf_slot(
                    sb->s_blocksize, base,
                    node_number, &slot) != 0 ||
                slot >= sfs_node_leaf_capacity(sb)) {
                asfs_brelse(bh);
                return -EUCLEAN;
            }

            *returned_node =
                (struct fsObjectNode *)
                    ((u8 *)container->node +
                     slot * NODE_STRUCT_SIZE);
            *returned_bh = bh;
            return 0;
        }

        if (nodes > 1U) {
            u32 slot;
            u32 next;
            bool full;

            if (ifs_sfs_node_index_slot(
                    sb->s_blocksize, base, nodes,
                    node_number, &slot) != 0 ||
                slot >= sfs_node_index_capacity(sb) ||
                sfs_decode_child_pointer(
                    sb,
                    be32_to_cpu(container->node[slot]),
                    &next, &full) != 0) {
                asfs_brelse(bh);
                return -EUCLEAN;
            }

            asfs_brelse(bh);
            block = next;
            continue;
        }

        asfs_brelse(bh);
        return -EUCLEAN;
    }

    return -EUCLEAN;
}

#ifdef CONFIG_ASFS_RW

static int sfs_propagate_full_state(
    struct super_block *sb,
    u32 child_block,
    u32 child_node_number,
    bool full)
{
    u32 budget = ASFS_SB(sb)->totalblocks;

    while (child_block != ASFS_SB(sb)->objectnoderoot) {
        struct buffer_head *parent_bh = NULL;
        struct fsNodeContainer *parent;
        u32 slot;
        u32 raw;
        u32 parent_block;
        u32 parent_node_number;
        bool old_full;
        bool propagate;
        int result;

        if (budget-- == 0U)
            return -EUCLEAN;

        result = sfs_find_parent_container(
            sb, child_block, child_node_number,
            &parent_bh, &slot);
        if (result != 0)
            return result;
        if (!parent_bh)
            return -EUCLEAN;

        parent =
            (struct fsNodeContainer *)parent_bh->b_data;
        raw = be32_to_cpu(parent->node[slot]);
        old_full = (raw & 1U) != 0U;
        if (full)
            raw |= 1U;
        else
            raw &= ~1U;
        parent->node[slot] = cpu_to_be32(raw);
        asfs_bstore(sb, parent_bh);

        propagate = full
            ? sfs_index_container_full(sb, parent)
            : old_full;
        parent_block =
            be32_to_cpu(parent->bheader.ownblock);
        parent_node_number =
            be32_to_cpu(parent->nodenumber);
        asfs_brelse(parent_bh);

        if (!propagate)
            return 0;

        child_block = parent_block;
        child_node_number = parent_node_number;
    }

    return 0;
}

static int sfs_create_node_container(
    struct super_block *sb,
    u32 node_number,
    u32 nodes_per_entry,
    u32 *returned_block)
{
    struct buffer_head *bh;
    struct fsNodeContainer *container;
    u32 block;
    int result;

    if (!returned_block || nodes_per_entry == 0U)
        return -EINVAL;

    result = asfs_allocadminspace(sb, &block);
    if (result != 0)
        return result;

    bh = asfs_getzeroblk(sb, block);
    if (!bh) {
        (void)asfs_freeadminspace(sb, block);
        return -EIO;
    }

    container =
        (struct fsNodeContainer *)bh->b_data;
    container->bheader.id =
        cpu_to_be32(ASFS_NODECONTAINER_ID);
    container->bheader.ownblock =
        cpu_to_be32(block);
    container->nodenumber =
        cpu_to_be32(node_number);
    container->nodes =
        cpu_to_be32(nodes_per_entry);
    asfs_bstore(sb, bh);
    asfs_brelse(bh);

    *returned_block = block;
    return 0;
}

static int sfs_add_node_level(struct super_block *sb)
{
    const u32 root_block = ASFS_SB(sb)->objectnoderoot;
    const u32 index_capacity =
        sfs_node_index_capacity(sb);
    const u32 leaf_capacity =
        sfs_node_leaf_capacity(sb);
    struct buffer_head *root_bh;
    struct buffer_head *copy_bh;
    struct fsNodeContainer *root;
    struct fsNodeContainer *copy;
    u32 old_nodes;
    u32 new_nodes;
    u32 copy_block;
    u32 encoded;
    int result;

    if (index_capacity == 0U || leaf_capacity == 0U)
        return -EUCLEAN;

    root_bh = asfs_breadcheck(
        sb, root_block, ASFS_NODECONTAINER_ID);
    if (!root_bh)
        return -EIO;

    root = (struct fsNodeContainer *)root_bh->b_data;
    old_nodes = be32_to_cpu(root->nodes);
    if (old_nodes == 0U) {
        asfs_brelse(root_bh);
        return -EUCLEAN;
    }

    result = asfs_allocadminspace(sb, &copy_block);
    if (result != 0) {
        asfs_brelse(root_bh);
        return result;
    }

    copy_bh = asfs_getzeroblk(sb, copy_block);
    if (!copy_bh) {
        asfs_brelse(root_bh);
        (void)asfs_freeadminspace(sb, copy_block);
        return -EIO;
    }

    copy = (struct fsNodeContainer *)copy_bh->b_data;
    memcpy(copy, root, sb->s_blocksize);
    copy->bheader.ownblock = cpu_to_be32(copy_block);
    asfs_bstore(sb, copy_bh);
    asfs_brelse(copy_bh);

    if (old_nodes == 1U) {
        new_nodes = leaf_capacity;
    } else if (check_mul_overflow(
                   old_nodes, index_capacity,
                   &new_nodes)) {
        asfs_brelse(root_bh);
        (void)asfs_freeadminspace(sb, copy_block);
        return -EOVERFLOW;
    }

    result = sfs_encode_child_pointer(
        sb, copy_block, true, &encoded);
    if (result != 0) {
        asfs_brelse(root_bh);
        (void)asfs_freeadminspace(sb, copy_block);
        return result;
    }

    memset(root->node, 0,
           sb->s_blocksize -
               sizeof(struct fsNodeContainer));
    root->nodes = cpu_to_be32(new_nodes);
    root->node[0] = cpu_to_be32(encoded);
    asfs_bstore(sb, root_bh);
    asfs_brelse(root_bh);
    return 0;
}

int asfs_createnode(
    struct super_block *sb,
    struct buffer_head **returned_bh,
    struct fsNode **returned_node,
    u32 *returned_node_number)
{
    const u32 root_block = ASFS_SB(sb)->objectnoderoot;
    const u32 leaf_capacity =
        sfs_node_leaf_capacity(sb);
    const u32 index_capacity =
        sfs_node_index_capacity(sb);
    u32 block = root_block;
    u32 budget = ASFS_SB(sb)->totalblocks;

    if (!returned_bh || !returned_node ||
        !returned_node_number ||
        leaf_capacity == 0U ||
        index_capacity == 0U)
        return -EINVAL;

    *returned_bh = NULL;
    *returned_node = NULL;
    *returned_node_number = 0U;

restart:
    block = root_block;

    while (block != 0U) {
        struct buffer_head *bh;
        struct fsNodeContainer *container;
        const u32 nodes =
            0U; /* set after buffer validation */

        (void)nodes;
        if (budget-- == 0U)
            return -EUCLEAN;

        bh = asfs_breadcheck(
            sb, block, ASFS_NODECONTAINER_ID);
        if (!bh)
            return -EIO;

        container =
            (struct fsNodeContainer *)bh->b_data;

        if (be32_to_cpu(container->nodes) == 1U) {
            u32 slot;

            for (slot = 0U;
                 slot < leaf_capacity;
                 ++slot) {
                struct fsNode *node =
                    (struct fsNode *)
                        ((u8 *)container->node +
                         slot * NODE_STRUCT_SIZE);

                if (node->data == 0U) {
                    u32 after;
                    bool has_more = false;

                    *returned_node = node;
                    *returned_node_number =
                        be32_to_cpu(
                            container->nodenumber) +
                        slot;
                    *returned_bh = bh;

                    for (after = slot + 1U;
                         after < leaf_capacity;
                         ++after) {
                        struct fsNode *candidate =
                            (struct fsNode *)
                                ((u8 *)container->node +
                                 after *
                                     NODE_STRUCT_SIZE);
                        if (candidate->data == 0U) {
                            has_more = true;
                            break;
                        }
                    }

                    if (!has_more) {
                        const int result =
                            sfs_propagate_full_state(
                                sb, block,
                                be32_to_cpu(
                                    container->
                                        nodenumber),
                                true);
                        if (result != 0) {
                            *returned_bh = NULL;
                            *returned_node = NULL;
                            asfs_brelse(bh);
                            return result;
                        }
                    }
                    return 0;
                }
            }

            {
                const u32 node_number =
                    be32_to_cpu(container->nodenumber);

                asfs_brelse(bh);
                if (block != root_block) {
                    int result =
                        sfs_propagate_full_state(
                            sb, block,
                            node_number,
                            true);
                    if (result != 0)
                        return result;
                    goto restart;
                }

                {
                    int result =
                        sfs_add_node_level(sb);
                    if (result != 0)
                        return result;
                    goto restart;
                }
            }
        }

        if (be32_to_cpu(container->nodes) > 1U) {
            const u32 nodes_per_child =
                be32_to_cpu(container->nodes);
            const u32 base =
                be32_to_cpu(container->nodenumber);
            u32 slot;
            u32 empty_slot = index_capacity;

            for (slot = 0U;
                 slot < index_capacity;
                 ++slot) {
                const u32 raw =
                    be32_to_cpu(
                        container->node[slot]);

                if (raw == 0U) {
                    if (empty_slot ==
                        index_capacity)
                        empty_slot = slot;
                    continue;
                }

                if ((raw & 1U) == 0U) {
                    u32 next;
                    bool full;
                    int result =
                        sfs_decode_child_pointer(
                            sb, raw,
                            &next, &full);

                    if (result != 0) {
                        asfs_brelse(bh);
                        return result;
                    }

                    asfs_brelse(bh);
                    block = next;
                    goto continue_descent;
                }
            }

            if (empty_slot < index_capacity) {
                u32 child_nodes;
                u32 child_node_number;
                u32 child_block;
                u32 encoded;
                int result;

                if (nodes_per_child ==
                    leaf_capacity) {
                    child_nodes = 1U;
                } else {
                    if (nodes_per_child %
                            index_capacity !=
                        0U) {
                        asfs_brelse(bh);
                        return -EUCLEAN;
                    }
                    child_nodes =
                        nodes_per_child /
                        index_capacity;
                    if (child_nodes == 0U) {
                        asfs_brelse(bh);
                        return -EUCLEAN;
                    }
                }

                if (check_mul_overflow(
                        empty_slot,
                        nodes_per_child,
                        &child_node_number) ||
                    check_add_overflow(
                        base,
                        child_node_number,
                        &child_node_number)) {
                    asfs_brelse(bh);
                    return -EOVERFLOW;
                }

                result =
                    sfs_create_node_container(
                        sb, child_node_number,
                        child_nodes, &child_block);
                if (result != 0) {
                    asfs_brelse(bh);
                    return result;
                }

                result = sfs_encode_child_pointer(
                    sb, child_block, false,
                    &encoded);
                if (result != 0) {
                    (void)asfs_freeadminspace(
                        sb, child_block);
                    asfs_brelse(bh);
                    return result;
                }

                container->node[empty_slot] =
                    cpu_to_be32(encoded);
                asfs_bstore(sb, bh);
                asfs_brelse(bh);
                block = child_block;
                goto continue_descent;
            }

            {
                const u32 node_number =
                    be32_to_cpu(
                        container->nodenumber);

                asfs_brelse(bh);
                if (block == root_block) {
                    int result =
                        sfs_add_node_level(sb);
                    if (result != 0)
                        return result;
                } else {
                    int result =
                        sfs_propagate_full_state(
                            sb, block,
                            node_number, true);
                    if (result != 0)
                        return result;
                }
                goto restart;
            }
        }

        asfs_brelse(bh);
        return -EUCLEAN;

continue_descent:
        continue;
    }

    return -EUCLEAN;
}

static int sfs_unlink_empty_container(
    struct super_block *sb,
    u32 child_block,
    u32 child_node_number)
{
    const u32 root_block =
        ASFS_SB(sb)->objectnoderoot;
    u32 budget = ASFS_SB(sb)->totalblocks;

    while (child_block != root_block) {
        struct buffer_head *parent_bh = NULL;
        struct fsNodeContainer *parent;
        u32 slot;
        u32 parent_block;
        u32 parent_node_number;
        int result;

        if (budget-- == 0U)
            return -EUCLEAN;

        result = sfs_find_parent_container(
            sb, child_block, child_node_number,
            &parent_bh, &slot);
        if (result != 0)
            return result;
        if (!parent_bh)
            return -EUCLEAN;

        parent =
            (struct fsNodeContainer *)
                parent_bh->b_data;
        parent_block =
            be32_to_cpu(
                parent->bheader.ownblock);
        parent_node_number =
            be32_to_cpu(parent->nodenumber);

        parent->node[slot] = 0U;
        asfs_bstore(sb, parent_bh);

        result =
            asfs_freeadminspace(
                sb, child_block);
        if (result != 0) {
            asfs_brelse(parent_bh);
            return result;
        }

        if (!sfs_index_container_empty(
                sb, parent) ||
            parent_block == root_block) {
            asfs_brelse(parent_bh);
            return sfs_propagate_full_state(
                sb, parent_block,
                parent_node_number, false);
        }

        asfs_brelse(parent_bh);
        child_block = parent_block;
        child_node_number =
            parent_node_number;
    }

    return 0;
}

int asfs_deletenode(
    struct super_block *sb,
    u32 object_node)
{
    const u32 leaf_capacity =
        sfs_node_leaf_capacity(sb);
    struct buffer_head *bh = NULL;
    struct fsObjectNode *object = NULL;
    struct fsNodeContainer *container;
    u32 empty_count = 0U;
    u32 index;
    u32 block;
    u32 node_number;
    int result;

    result = asfs_getnode(
        sb, object_node, &bh, &object);
    if (result != 0)
        return result;

    container =
        (struct fsNodeContainer *)bh->b_data;
    block =
        be32_to_cpu(container->bheader.ownblock);
    node_number =
        be32_to_cpu(container->nodenumber);

    object->node.data = 0U;
    object->next = 0U;
    object->hash16 = 0U;

    for (index = 0U;
         index < leaf_capacity;
         ++index) {
        const struct fsNode *node =
            (const struct fsNode *)
                ((const u8 *)container->node +
                 index * NODE_STRUCT_SIZE);
        if (node->data == 0U)
            empty_count++;
    }

    asfs_bstore(sb, bh);

    if (empty_count == 1U) {
        result = sfs_propagate_full_state(
            sb, block, node_number, false);
    } else if (empty_count == leaf_capacity &&
               block != ASFS_SB(sb)->
                   objectnoderoot) {
        result = sfs_unlink_empty_container(
            sb, block, node_number);
    } else {
        result = 0;
    }

    asfs_brelse(bh);
    return result;
}

#endif


/* ===== object containers ===== */
#include <linux/buffer_head.h>
#include <linux/errno.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/overflow.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "asfs_fs.h"

static u32 sfs_object_fixed_bytes(void)
{
    return (u32)offsetof(struct fsObject, name);
}

static int sfs_object_record_bytes(
    size_t name_length,
    u32 *record_bytes)
{
    size_t raw;

    if (!record_bytes ||
        name_length > ASFS_MAXFN ||
        check_add_overflow(
            (size_t)sfs_object_fixed_bytes(),
            name_length + 2U, &raw))
        return -EINVAL;

    raw = ALIGN(raw, 2U);
    if (raw > U32_MAX)
        return -EOVERFLOW;

    *record_bytes = (u32)raw;
    return 0;
}

struct fsObject *asfs_nextobject(
    struct super_block *sb,
    struct fsObjectContainer *container,
    struct fsObject *object)
{
    u8 *block_end;
    u8 *object_start;
    u8 *tail;
    ifs_sfs_u32 record_bytes = 0U;
    ifs_sfs_u32 name_bytes = 0U;
    IfsSfsObjectRecordStatus status;

    if (!asfs_object_slot_fits(
            sb, container, object))
        return NULL;

    block_end =
        (u8 *)container + sb->s_blocksize;
    object_start = (u8 *)object;
    tail = object->name;

    status = ifs_sfs_object_record_layout(
        tail,
        (ifs_sfs_u32)(block_end - tail),
        (ifs_sfs_u32)(tail - object_start),
        &record_bytes, &name_bytes);
    if (status != IFS_SFS_OBJECT_RECORD_OK ||
        record_bytes >
            (ifs_sfs_u32)(
                block_end - object_start))
        return NULL;

    return (struct fsObject *)
        (object_start + record_bytes);
}

struct fsObject *asfs_find_obj_by_name(
    struct super_block *sb,
    struct fsObjectContainer *container,
    u8 *name)
{
    struct fsObject *object =
        &container->object[0];

    while (asfs_object_slot_fits(
               sb, container, object) &&
           be32_to_cpu(object->objectnode) != 0U) {
        struct fsObject *next =
            asfs_nextobject(
                sb, container, object);

        if (!next)
            return ERR_PTR(-EUCLEAN);

        if (asfs_namecmp(
                object->name, name,
                (ASFS_SB(sb)->flags &
                 ASFS_ROOTBITS_CASESENSITIVE) != 0,
                NULL) == 0)
            return object;

        object = next;
    }

    return NULL;
}

#ifdef CONFIG_ASFS_RW

static struct fsObject *sfs_find_object_by_node(
    struct super_block *sb,
    struct fsObjectContainer *container,
    u32 object_node)
{
    struct fsObject *object =
        &container->object[0];

    while (asfs_object_slot_fits(
               sb, container, object) &&
           be32_to_cpu(object->objectnode) != 0U) {
        struct fsObject *next =
            asfs_nextobject(
                sb, container, object);

        if (!next)
            return ERR_PTR(-EUCLEAN);

        if (be32_to_cpu(
                object->objectnode) ==
            object_node)
            return object;

        object = next;
    }

    return NULL;
}

int asfs_readobject(
    struct super_block *sb,
    u32 object_node,
    struct buffer_head **returned_bh,
    struct fsObject **returned_object)
{
    struct buffer_head *node_bh = NULL;
    struct fsObjectNode *node = NULL;
    u32 container_block;
    int result;

    if (!returned_bh || !returned_object)
        return -EINVAL;

    *returned_bh = NULL;
    *returned_object = NULL;

    result = asfs_getnode(
        sb, object_node,
        &node_bh, &node);
    if (result != 0)
        return result;

    container_block =
        be32_to_cpu(node->node.data);
    asfs_brelse(node_bh);

    if (container_block == 0U ||
        container_block >=
            ASFS_SB(sb)->totalblocks)
        return -EUCLEAN;

    *returned_bh = asfs_breadcheck(
        sb, container_block,
        ASFS_OBJECTCONTAINER_ID);
    if (!*returned_bh)
        return -EIO;

    *returned_object =
        sfs_find_object_by_node(
            sb,
            (struct fsObjectContainer *)
                (*returned_bh)->b_data,
            object_node);
    if (IS_ERR(*returned_object)) {
        result = PTR_ERR(*returned_object);
        *returned_object = NULL;
        asfs_brelse(*returned_bh);
        *returned_bh = NULL;
        return result;
    }
    if (!*returned_object) {
        asfs_brelse(*returned_bh);
        *returned_bh = NULL;
        return -ENOENT;
    }

    return 0;
}

static int sfs_remove_object_container(
    struct super_block *sb,
    struct buffer_head *container_bh)
{
    struct fsObjectContainer *container =
        (struct fsObjectContainer *)
            container_bh->b_data;
    const u32 own_block =
        be32_to_cpu(
            container->bheader.ownblock);
    const u32 next =
        be32_to_cpu(container->next);
    const u32 previous =
        be32_to_cpu(container->previous);
    const u32 parent =
        be32_to_cpu(container->parent);
    struct buffer_head *link_bh;
    int result;

    if (own_block == 0U ||
        own_block >= ASFS_SB(sb)->totalblocks ||
        next == own_block ||
        previous == own_block)
        return -EUCLEAN;

    if (next != 0U) {
        struct fsObjectContainer *next_container;

        if (next >= ASFS_SB(sb)->totalblocks)
            return -EUCLEAN;

        link_bh = asfs_breadcheck(
            sb, next,
            ASFS_OBJECTCONTAINER_ID);
        if (!link_bh)
            return -EIO;

        next_container =
            (struct fsObjectContainer *)
                link_bh->b_data;
        next_container->previous =
            cpu_to_be32(previous);
        asfs_bstore(sb, link_bh);
        asfs_brelse(link_bh);
    }

    if (previous != 0U) {
        struct fsObjectContainer *previous_container;

        if (previous >= ASFS_SB(sb)->totalblocks)
            return -EUCLEAN;

        link_bh = asfs_breadcheck(
            sb, previous,
            ASFS_OBJECTCONTAINER_ID);
        if (!link_bh)
            return -EIO;

        previous_container =
            (struct fsObjectContainer *)
                link_bh->b_data;
        previous_container->next =
            cpu_to_be32(next);
        asfs_bstore(sb, link_bh);
        asfs_brelse(link_bh);
    } else {
        struct fsObject *parent_object = NULL;

        result = asfs_readobject(
            sb, parent, &link_bh,
            &parent_object);
        if (result != 0)
            return result;

        if ((parent_object->bits &
             OTYPE_DIR) == 0U) {
            asfs_brelse(link_bh);
            return -EUCLEAN;
        }

        parent_object->
            object.dir.firstdirblock =
            cpu_to_be32(next);
        asfs_bstore(sb, link_bh);
        asfs_brelse(link_bh);
    }

    return asfs_freeadminspace(
        sb, own_block);
}

static int sfs_adjust_recycled_info(
    struct super_block *sb,
    s32 deleted_files,
    s32 deleted_blocks)
{
    struct buffer_head *bh;
    struct fsRootInfo *root_info;
    u32 files;
    u32 blocks;

    if (sb->s_blocksize <
        sizeof(struct fsRootInfo))
        return -EUCLEAN;

    bh = asfs_breadcheck(
        sb, ASFS_SB(sb)->rootobjectcontainer,
        ASFS_OBJECTCONTAINER_ID);
    if (!bh)
        return -EIO;

    root_info = (struct fsRootInfo *)
        ((u8 *)bh->b_data +
         sb->s_blocksize -
         sizeof(struct fsRootInfo));

    if (ifs_sfs_adjust_counter(
            be32_to_cpu(
                root_info->deletedfiles),
            deleted_files, &files) != 0 ||
        ifs_sfs_adjust_counter(
            be32_to_cpu(
                root_info->deletedblocks),
            deleted_blocks, &blocks) != 0) {
        asfs_brelse(bh);
        return -EUCLEAN;
    }

    root_info->deletedfiles =
        cpu_to_be32(files);
    root_info->deletedblocks =
        cpu_to_be32(blocks);
    asfs_bstore(sb, bh);
    asfs_brelse(bh);
    return 0;
}

static int sfs_remove_packed_object(
    struct super_block *sb,
    struct buffer_head *bh,
    struct fsObject *object)
{
    struct fsObjectContainer *container =
        (struct fsObjectContainer *)bh->b_data;
    struct fsObject *first =
        &container->object[0];
    struct fsObject *next;
    u8 *block_end =
        (u8 *)container + sb->s_blocksize;
    size_t record_bytes;

    if (!asfs_object_slot_fits(
            sb, container, object))
        return -EUCLEAN;

    if (be32_to_cpu(container->parent) ==
        ASFS_RECYCLEDNODE) {
        const u32 file_blocks =
            DIV_ROUND_UP(
                be32_to_cpu(
                    object->object.file.size),
                sb->s_blocksize);
        int result =
            sfs_adjust_recycled_info(
                sb, -1, -(s32)file_blocks);

        if (result != 0)
            return result;
    }

    next = asfs_nextobject(
        sb, container, object);
    if (!next)
        return -EUCLEAN;

    if (object == first &&
        asfs_object_slot_fits(
            sb, container, next) &&
        be32_to_cpu(next->objectnode) == 0U)
        return sfs_remove_object_container(
            sb, bh);

    record_bytes =
        (size_t)((u8 *)next -
                 (u8 *)object);
    if (record_bytes == 0U ||
        (u8 *)next > block_end)
        return -EUCLEAN;

    memmove(
        object, next,
        (size_t)(block_end -
                 (u8 *)next));
    memset(
        block_end - record_bytes,
        0, record_bytes);
    asfs_bstore(sb, bh);
    return 0;
}

static int sfs_dehash_object(
    struct super_block *sb,
    u32 object_node,
    u8 *name,
    u32 parent_node)
{
    struct buffer_head *parent_bh = NULL;
    struct fsObject *parent = NULL;
    struct buffer_head *hash_bh = NULL;
    struct fsHashTable *table;
    struct buffer_head *target_bh = NULL;
    struct fsObjectNode *target = NULL;
    u32 hash_block;
    u16 hash;
    u16 chain;
    u32 cursor_node;
    u32 replacement;
    u32 budget = ASFS_SB(sb)->totalblocks;
    int result;

    result = asfs_readobject(
        sb, parent_node,
        &parent_bh, &parent);
    if (result != 0)
        return result;

    hash_block =
        be32_to_cpu(
            parent->object.dir.hashtable);
    asfs_brelse(parent_bh);
    if (hash_block == 0U)
        return 0;

    hash_bh = asfs_breadcheck(
        sb, hash_block, ASFS_HASHTABLE_ID);
    if (!hash_bh)
        return -EIO;

    result = asfs_getnode(
        sb, object_node,
        &target_bh, &target);
    if (result != 0) {
        asfs_brelse(hash_bh);
        return result;
    }

    table =
        (struct fsHashTable *)hash_bh->b_data;
    hash = asfs_hash(
        name,
        (ASFS_SB(sb)->flags &
         ASFS_ROOTBITS_CASESENSITIVE) != 0);
    chain = HASHCHAIN(hash);
    cursor_node =
        be32_to_cpu(
            table->hashentry[chain]);
    replacement =
        be32_to_cpu(target->next);

    if (cursor_node == object_node) {
        table->hashentry[chain] =
            cpu_to_be32(replacement);
        asfs_bstore(sb, hash_bh);
        asfs_brelse(target_bh);
        asfs_brelse(hash_bh);
        return 0;
    }

    asfs_brelse(target_bh);
    target_bh = NULL;

    while (cursor_node != 0U) {
        struct buffer_head *node_bh = NULL;
        struct fsObjectNode *node = NULL;
        u32 next;

        if (budget-- == 0U) {
            result = -EUCLEAN;
            goto out;
        }

        result = asfs_getnode(
            sb, cursor_node,
            &node_bh, &node);
        if (result != 0)
            goto out;

        next = be32_to_cpu(node->next);
        if (next == object_node) {
            node->next =
                cpu_to_be32(replacement);
            asfs_bstore(sb, node_bh);
            asfs_brelse(node_bh);
            result = 0;
            goto out;
        }

        asfs_brelse(node_bh);
        cursor_node = next;
    }

    result = -EUCLEAN;

out:
    asfs_brelse(hash_bh);
    return result;
}

static int sfs_remove_object(
    struct super_block *sb,
    struct buffer_head *bh,
    struct fsObject *object,
    bool delete_node)
{
    struct fsObjectContainer *container =
        (struct fsObjectContainer *)bh->b_data;
    const u32 node =
        be32_to_cpu(object->objectnode);
    int result;

    result = sfs_dehash_object(
        sb, node, object->name,
        be32_to_cpu(container->parent));
    if (result != 0)
        return result;

    result = sfs_remove_packed_object(
        sb, bh, object);
    if (result != 0)
        return result;

    if (delete_node)
        return asfs_deletenode(sb, node);

    return 0;
}

int asfs_deleteobject(
    struct super_block *sb,
    struct buffer_head *bh,
    struct fsObject *object)
{
    u8 bits;
    u32 auxiliary;
    int result;

    if (!bh || !object)
        return -EINVAL;

    if ((object->bits & OTYPE_DIR) != 0U &&
        object->object.dir.firstdirblock != 0U)
        return -ENOTEMPTY;

    bits = object->bits;
    auxiliary =
        (bits & OTYPE_DIR) != 0U
        ? be32_to_cpu(
            object->object.dir.hashtable)
        : be32_to_cpu(
            object->object.file.data);

    result = sfs_remove_object(
        sb, bh, object, true);
    if (result != 0)
        return result;

    if ((bits & OTYPE_LINK) != 0U &&
        (bits & OTYPE_HARDLINK) == 0U) {
        return auxiliary != 0U
            ? asfs_freeadminspace(
                sb, auxiliary)
            : 0;
    }

    if ((bits & OTYPE_DIR) != 0U) {
        return auxiliary != 0U
            ? asfs_freeadminspace(
                sb, auxiliary)
            : 0;
    }

    if (auxiliary != 0U)
        return asfs_deleteextents(
            sb, auxiliary);

    return 0;
}

static u8 *sfs_object_container_end(
    struct super_block *sb,
    struct fsObjectContainer *container)
{
    struct fsObject *object =
        &container->object[0];
    u32 budget =
        sb->s_blocksize /
        max_t(u32, sfs_object_fixed_bytes(), 1U);

    while (budget-- != 0U &&
           asfs_object_slot_fits(
               sb, container, object) &&
           be32_to_cpu(
               object->objectnode) != 0U) {
        object = asfs_nextobject(
            sb, container, object);
        if (!object)
            return NULL;
    }

    if (!asfs_object_slot_fits(
            sb, container, object))
        return NULL;

    return (u8 *)object;
}

static int sfs_find_object_space(
    struct super_block *sb,
    struct buffer_head **io_bh,
    struct fsObject **io_object,
    u32 bytes_needed)
{
    struct buffer_head *parent_bh;
    struct fsObject *parent;
    u32 block;
    u32 budget = ASFS_SB(sb)->totalblocks;

    if (!io_bh || !*io_bh ||
        !io_object || !*io_object ||
        bytes_needed == 0U ||
        bytes_needed > sb->s_blocksize)
        return -EINVAL;

    parent_bh = *io_bh;
    parent = *io_object;
    block =
        be32_to_cpu(
            parent->object.dir.firstdirblock);

    while (block != 0U) {
        struct buffer_head *bh;
        struct fsObjectContainer *container;
        u8 *end;
        u32 next;

        if (budget-- == 0U)
            return -EUCLEAN;

        bh = asfs_breadcheck(
            sb, block,
            ASFS_OBJECTCONTAINER_ID);
        if (!bh)
            return -EIO;

        container =
            (struct fsObjectContainer *)
                bh->b_data;
        end = sfs_object_container_end(
            sb, container);
        if (!end) {
            asfs_brelse(bh);
            return -EUCLEAN;
        }

        if ((size_t)(
                (u8 *)container +
                sb->s_blocksize - end) >=
            bytes_needed) {
            *io_bh = bh;
            *io_object =
                (struct fsObject *)end;
            return 0;
        }

        next = be32_to_cpu(container->next);
        asfs_brelse(bh);
        block = next;
    }

    {
        const u32 old_head =
            be32_to_cpu(
                parent->
                    object.dir.firstdirblock);
        struct buffer_head *new_bh;
        struct fsObjectContainer *new_container;
        struct buffer_head *old_head_bh = NULL;
        u32 new_block;
        int result;

        result = asfs_allocadminspace(
            sb, &new_block);
        if (result != 0)
            return result;

        new_bh = asfs_getzeroblk(
            sb, new_block);
        if (!new_bh) {
            (void)asfs_freeadminspace(
                sb, new_block);
            return -EIO;
        }

        if (old_head != 0U) {
            old_head_bh = asfs_breadcheck(
                sb, old_head,
                ASFS_OBJECTCONTAINER_ID);
            if (!old_head_bh) {
                asfs_brelse(new_bh);
                (void)asfs_freeadminspace(
                    sb, new_block);
                return -EIO;
            }
        }

        new_container =
            (struct fsObjectContainer *)
                new_bh->b_data;
        new_container->bheader.id =
            cpu_to_be32(
                ASFS_OBJECTCONTAINER_ID);
        new_container->bheader.ownblock =
            cpu_to_be32(new_block);
        new_container->parent =
            parent->objectnode;
        new_container->next =
            cpu_to_be32(old_head);
        new_container->previous = 0U;
        asfs_bstore(sb, new_bh);

        parent->
            object.dir.firstdirblock =
            cpu_to_be32(new_block);
        asfs_bstore(sb, parent_bh);

        if (old_head_bh) {
            struct fsObjectContainer *old =
                (struct fsObjectContainer *)
                    old_head_bh->b_data;

            old->previous =
                cpu_to_be32(new_block);
            asfs_bstore(sb, old_head_bh);
            asfs_brelse(old_head_bh);
        }

        *io_bh = new_bh;
        *io_object =
            &new_container->object[0];
        return 0;
    }
}

static int sfs_publish_hash_link(
    struct super_block *sb,
    u32 hash_block,
    struct buffer_head *node_bh,
    struct fsObjectNode *node,
    u32 node_number,
    u8 *name)
{
    struct buffer_head *hash_bh;
    struct fsHashTable *table;
    u16 hash;
    u16 chain;
    u32 old_head;

    if (hash_block == 0U) {
        node->next = 0U;
        node->hash16 = 0U;
        asfs_bstore(sb, node_bh);
        return 0;
    }

    hash_bh = asfs_breadcheck(
        sb, hash_block, ASFS_HASHTABLE_ID);
    if (!hash_bh)
        return -EIO;

    table =
        (struct fsHashTable *)hash_bh->b_data;
    hash = asfs_hash(
        name,
        (ASFS_SB(sb)->flags &
         ASFS_ROOTBITS_CASESENSITIVE) != 0);
    chain = HASHCHAIN(hash);
    old_head =
        be32_to_cpu(
            table->hashentry[chain]);

    node->next = cpu_to_be32(old_head);
    node->hash16 = cpu_to_be16(hash);
    asfs_bstore(sb, node_bh);

    table->hashentry[chain] =
        cpu_to_be32(node_number);
    asfs_bstore(sb, hash_bh);
    asfs_brelse(hash_bh);
    return 0;
}

int asfs_createobject(
    struct super_block *sb,
    struct buffer_head **io_bh,
    struct fsObject **io_object,
    struct fsObject *template,
    u8 *object_name,
    int force)
{
    struct buffer_head *parent_bh;
    struct fsObject *parent;
    struct buffer_head *node_bh = NULL;
    struct fsObjectNode *node = NULL;
    struct fsObjectNode saved_node;
    struct fsObject *destination = NULL;
    u32 parent_hash;
    u32 node_number = 0U;
    u32 auxiliary_block = 0U;
    u32 record_bytes;
    size_t name_length;
    bool created_node = false;
    bool saved_node_valid = false;
    int result;

    if (!io_bh || !*io_bh ||
        !io_object || !*io_object ||
        !template || !object_name)
        return -EINVAL;

    parent_bh = *io_bh;
    parent = *io_object;
    if ((parent->bits & OTYPE_DIR) == 0U)
        return -ENOTDIR;

    name_length =
        strnlen(
            (const char *)object_name,
            ASFS_MAXFN + 1U);
    if (name_length > ASFS_MAXFN)
        return -ENAMETOOLONG;

    result = sfs_object_record_bytes(
        name_length, &record_bytes);
    if (result != 0)
        return result;

    parent_hash =
        be32_to_cpu(
            parent->object.dir.hashtable);

    if (!force &&
        !ifs_sfs_has_allocation_headroom(
            ASFS_SB(sb)->freeblocks,
            1U, ASFS_ALWAYSFREE))
        return -ENOSPC;
    if (!force &&
        be32_to_cpu(parent->objectnode) ==
            ASFS_RECYCLEDNODE)
        return -EINVAL;

    result = sfs_find_object_space(
        sb, io_bh, io_object,
        record_bytes);
    if (result != 0)
        return result;

    destination = *io_object;
    memset(destination, 0, record_bytes);
    memcpy(
        destination, template,
        sfs_object_fixed_bytes());
    memcpy(
        destination->name,
        object_name, name_length);
    destination->name[name_length] = 0U;
    destination->name[name_length + 1U] = 0U;

    if (destination->objectnode != 0U) {
        node_number =
            be32_to_cpu(
                destination->objectnode);
        result = asfs_getnode(
            sb, node_number,
            &node_bh, &node);
        if (result != 0)
            goto rollback_record;

        saved_node = *node;
        saved_node_valid = true;
    } else {
        result = asfs_createnode(
            sb, &node_bh,
            (struct fsNode **)&node,
            &node_number);
        if (result != 0)
            goto rollback_record;

        created_node = true;
        destination->objectnode =
            cpu_to_be32(node_number);
    }

    if ((destination->bits & OTYPE_DIR) != 0U &&
        destination->object.dir.hashtable == 0U) {
        struct buffer_head *aux_bh;
        struct fsHashTable *table;

        result = asfs_allocadminspace(
            sb, &auxiliary_block);
        if (result != 0)
            goto rollback_node;

        aux_bh = asfs_getzeroblk(
            sb, auxiliary_block);
        if (!aux_bh) {
            result = -EIO;
            goto rollback_node;
        }

        table =
            (struct fsHashTable *)aux_bh->b_data;
        table->bheader.id =
            cpu_to_be32(ASFS_HASHTABLE_ID);
        table->bheader.ownblock =
            cpu_to_be32(auxiliary_block);
        table->parent =
            destination->objectnode;
        asfs_bstore(sb, aux_bh);
        asfs_brelse(aux_bh);
        destination->object.dir.hashtable =
            cpu_to_be32(auxiliary_block);
    } else if ((destination->bits &
                (OTYPE_LINK |
                 OTYPE_HARDLINK)) ==
               OTYPE_LINK &&
               destination->object.file.data == 0U) {
        struct buffer_head *aux_bh;
        struct fsSoftLink *link;

        result = asfs_allocadminspace(
            sb, &auxiliary_block);
        if (result != 0)
            goto rollback_node;

        aux_bh = asfs_getzeroblk(
            sb, auxiliary_block);
        if (!aux_bh) {
            result = -EIO;
            goto rollback_node;
        }

        link =
            (struct fsSoftLink *)aux_bh->b_data;
        link->bheader.id =
            cpu_to_be32(ASFS_SOFTLINK_ID);
        link->bheader.ownblock =
            cpu_to_be32(auxiliary_block);
        link->parent =
            destination->objectnode;
        asfs_bstore(sb, aux_bh);
        asfs_brelse(aux_bh);
        destination->object.file.data =
            cpu_to_be32(auxiliary_block);
    }

    node->node.data =
        ((struct fsBlockHeader *)
             (*io_bh)->b_data)->ownblock;

    asfs_bstore(sb, *io_bh);
    result = sfs_publish_hash_link(
        sb, parent_hash,
        node_bh, node,
        node_number, object_name);
    if (result != 0)
        goto rollback_node;

    asfs_brelse(node_bh);
    return 0;

rollback_node:
    if (saved_node_valid && node_bh) {
        *node = saved_node;
        asfs_bstore(sb, node_bh);
    }
    asfs_brelse(node_bh);

    if (created_node &&
        asfs_deletenode(
            sb, node_number) != 0)
        result = -EUCLEAN;

    if (auxiliary_block != 0U &&
        asfs_freeadminspace(
            sb, auxiliary_block) != 0)
        result = -EUCLEAN;

rollback_record:
    if (destination) {
        memset(
            destination, 0, record_bytes);
        asfs_bstore(sb, *io_bh);
    }
    return result;
}

int asfs_addblockstofile(
    struct super_block *sb,
    struct buffer_head *object_bh,
    struct fsObject *object,
    u32 blocks,
    u32 *new_space,
    u32 *added_blocks)
{
    struct buffer_head *extent_bh = NULL;
    struct fsExtentBNode *extent = NULL;
    u32 last_extent =
        be32_to_cpu(
            object->object.file.data);
    u32 search_start = 0U;
    u32 found_block = 0U;
    u32 found_blocks = 0U;
    u32 budget = ASFS_SB(sb)->totalblocks;
    int result;

    if (!new_space || !added_blocks ||
        blocks == 0U || blocks > 0xffffU)
        return -EINVAL;

    *new_space = 0U;
    *added_blocks = 0U;

    if (last_extent != 0U) {
        while (last_extent != 0U) {
            u32 next;

            if (budget-- == 0U)
                return -EUCLEAN;

            result = asfs_getextent(
                sb, last_extent,
                &extent_bh, &extent);
            if (result != 0)
                return result;

            next = be32_to_cpu(extent->next);
            if (next == 0U) {
                search_start =
                    be32_to_cpu(extent->key) +
                    be16_to_cpu(extent->blocks);
                last_extent =
                    be32_to_cpu(extent->key);
                asfs_brelse(extent_bh);
                extent_bh = NULL;
                break;
            }

            last_extent = next;
            asfs_brelse(extent_bh);
            extent_bh = NULL;
        }
    }

    result = asfs_findspace(
        sb, blocks,
        search_start, search_start,
        &found_block, &found_blocks);
    if (result != 0)
        return result;

    result = asfs_markspace(
        sb, found_block, found_blocks);
    if (result != 0)
        return result;

    {
        u32 new_last = last_extent;

        result = asfs_addblocks(
            sb, (u16)found_blocks,
            found_block,
            be32_to_cpu(object->objectnode),
            &new_last);
        if (result != 0) {
            struct buffer_head *orphan_bh = NULL;
            struct fsExtentBNode *orphan = NULL;
            int cleanup = asfs_getextent(
                sb, found_block,
                &orphan_bh, &orphan);

            if (cleanup == 0) {
                cleanup = asfs_deletebnode(
                    sb, orphan_bh,
                    found_block);
                asfs_brelse(orphan_bh);
            } else if (cleanup == -ENOENT) {
                cleanup = 0;
            }

            if (cleanup == 0) {
                cleanup = asfs_freespace(
                    sb, found_block,
                    found_blocks);
            }
            return cleanup == 0
                ? result : -EUCLEAN;
        }

        if (object->object.file.data == 0U)
            object->object.file.data =
                cpu_to_be32(new_last);
    }

    *new_space = found_block;
    *added_blocks = found_blocks;
    asfs_bstore(sb, object_bh);
    return 0;
}

int asfs_renameobject(
    struct super_block *sb,
    struct buffer_head *source_bh,
    struct fsObject *source,
    struct buffer_head *parent_bh,
    struct fsObject *parent,
    u8 *new_name)
{
    struct fsObject saved_object;
    u8 old_name[ASFS_MAXFN + 2U];
    const u32 old_parent =
        be32_to_cpu(
            ((struct fsObjectContainer *)
                 source_bh->b_data)->parent);
    const u32 new_parent =
        be32_to_cpu(parent->objectnode);
    size_t old_name_length;
    int result;

    {
        const u8 *const block_start = (const u8 *)source_bh->b_data;
        const u8 *const block_end = block_start + sb->s_blocksize;
        const u8 *source_bytes = (const u8 *)source;
        const u8 *name_start;
        const u8 *terminator;
        size_t object_offset;
        size_t name_offset;
        size_t available;
        size_t limit;

        if (source_bytes < block_start || source_bytes >= block_end)
            return -EUCLEAN;

        object_offset = (size_t)(source_bytes - block_start);
        if (object_offset > sb->s_blocksize ||
            IFS_SFS_OBJECT_FIXED_SIZE > sb->s_blocksize - object_offset)
            return -EUCLEAN;

        name_offset = object_offset + IFS_SFS_OBJECT_FIXED_SIZE;
        if (name_offset >= sb->s_blocksize)
            return -EUCLEAN;

        name_start = block_start + name_offset;
        available = (size_t)(block_end - name_start);
        limit = min_t(size_t, available, ASFS_MAXFN + 1U);
        terminator = memchr(name_start, '\0', limit);
        if (!terminator)
            return -EUCLEAN;
        old_name_length = (size_t)(terminator - name_start);
        if (old_name_length > ASFS_MAXFN)
            return -EUCLEAN;
    }

    memcpy(
        old_name, source->name,
        old_name_length + 1U);
    saved_object = *source;

    result = sfs_remove_object(
        sb, source_bh, source, false);
    if (result != 0)
        return result;

    {
        struct buffer_head *fresh_parent_bh = NULL;
        struct fsObject *fresh_parent = NULL;
        struct buffer_head *destination_bh;
        struct fsObject *destination;

        result = asfs_readobject(
            sb, new_parent,
            &fresh_parent_bh,
            &fresh_parent);
        if (result == 0) {
            destination_bh = fresh_parent_bh;
            destination = fresh_parent;
            result = asfs_createobject(
                sb, &destination_bh,
                &destination,
                &saved_object,
                new_name, TRUE);
            if (result == 0)
                asfs_bstore(
                    sb, destination_bh);

            if (destination_bh !=
                fresh_parent_bh)
                asfs_brelse(
                    destination_bh);
            asfs_brelse(
                fresh_parent_bh);
        }
    }

    if (result == 0) {
        if (new_parent ==
            ASFS_RECYCLEDNODE) {
            const s32 file_blocks =
                (s32)DIV_ROUND_UP(
                    be32_to_cpu(
                        saved_object.
                            object.file.size),
                    sb->s_blocksize);
            result =
                sfs_adjust_recycled_info(
                    sb, 1, file_blocks);
        }
        return result;
    }

    {
        struct buffer_head *restore_parent_bh = NULL;
        struct fsObject *restore_parent = NULL;
        struct buffer_head *restore_bh;
        struct fsObject *restore_object;

        if (asfs_readobject(
                sb, old_parent,
                &restore_parent_bh,
                &restore_parent) == 0) {
            restore_bh =
                restore_parent_bh;
            restore_object =
                restore_parent;
            if (asfs_createobject(
                    sb, &restore_bh,
                    &restore_object,
                    &saved_object,
                    old_name, TRUE) == 0)
                asfs_bstore(
                    sb, restore_bh);

            if (restore_bh !=
                restore_parent_bh)
                asfs_brelse(restore_bh);
            asfs_brelse(
                restore_parent_bh);
        }
    }

    return result;
}

int asfs_truncateblocksinfile(
    struct super_block *sb,
    struct buffer_head *object_bh,
    struct fsObject *object,
    u32 new_size)
{
    struct buffer_head *extent_bh = NULL;
    struct fsExtentBNode *extent = NULL;
    u32 needed_blocks =
        DIV_ROUND_UP(
            new_size, sb->s_blocksize);
    u32 logical = 0U;
    u32 key =
        be32_to_cpu(
            object->object.file.data);
    u32 budget = ASFS_SB(sb)->totalblocks;
    int result;

    if (key == 0U)
        return needed_blocks == 0U
            ? 0 : -EUCLEAN;

    while (key != 0U) {
        const u32 next_key = key;
        u32 extent_blocks;

        if (budget-- == 0U)
            return -EUCLEAN;

        result = asfs_getextent(
            sb, next_key,
            &extent_bh, &extent);
        if (result != 0)
            return result;

        extent_blocks =
            be16_to_cpu(extent->blocks);
        if ((u64)logical + extent_blocks >=
            needed_blocks)
            break;

        logical += extent_blocks;
        key = be32_to_cpu(extent->next);
        asfs_brelse(extent_bh);
        extent_bh = NULL;
    }

    if (!extent_bh || !extent)
        return -EUCLEAN;

    {
        const u32 keep_blocks =
            needed_blocks - logical;
        const u32 current_blocks =
            be16_to_cpu(extent->blocks);
        const u32 extent_key =
            be32_to_cpu(extent->key);
        const u32 next =
            be32_to_cpu(extent->next);
        const u32 previous =
            be32_to_cpu(extent->prev);

        if (keep_blocks > current_blocks) {
            asfs_brelse(extent_bh);
            return -EUCLEAN;
        }

        if (current_blocks > keep_blocks) {
            result = asfs_freespace(
                sb,
                extent_key + keep_blocks,
                current_blocks - keep_blocks);
            if (result != 0) {
                asfs_brelse(extent_bh);
                return result;
            }
        }

        if (next != 0U) {
            result = asfs_deleteextents(
                sb, next);
            if (result != 0) {
                asfs_brelse(extent_bh);
                return result;
            }
        }

        if (keep_blocks != 0U) {
            extent->blocks =
                cpu_to_be16(
                    (u16)keep_blocks);
            extent->next = 0U;
            asfs_bstore(sb, extent_bh);
            asfs_brelse(extent_bh);
            return 0;
        }

        if ((previous & MSB_MASK) != 0U) {
            object->object.file.data = 0U;
            asfs_bstore(sb, object_bh);
        } else {
            struct buffer_head *previous_bh = NULL;
            struct fsExtentBNode *previous_extent = NULL;

            result = asfs_getextent(
                sb, previous & ~MSB_MASK,
                &previous_bh,
                &previous_extent);
            if (result != 0) {
                asfs_brelse(extent_bh);
                return result;
            }

            previous_extent->next = 0U;
            asfs_bstore(sb, previous_bh);
            asfs_brelse(previous_bh);
        }

        result = asfs_deletebnode(
            sb, extent_bh, extent_key);
        asfs_brelse(extent_bh);
        return result;
    }
}

#endif

