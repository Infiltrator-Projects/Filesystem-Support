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
                    sizeof(struct fsExtentBNode);
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
