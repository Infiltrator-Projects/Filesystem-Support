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
        const u32 nodes;
        const u32 base;

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

    memset(root->node, 0,
           sb->s_blocksize -
               sizeof(struct fsNodeContainer));
    root->nodes = cpu_to_be32(new_nodes);

    result = sfs_encode_child_pointer(
        sb, copy_block, true, &encoded);
    if (result != 0) {
        asfs_brelse(root_bh);
        (void)asfs_freeadminspace(sb, copy_block);
        return result;
    }

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

            asfs_brelse(bh);
            if (block != root_block) {
                int result =
                    sfs_propagate_full_state(
                        sb, block,
                        be32_to_cpu(
                            container->nodenumber),
                        true);
                if (result != 0)
                    return result;
                goto restart;
            }

            {
                int result = sfs_add_node_level(sb);
                if (result != 0)
                    return result;
                goto restart;
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
