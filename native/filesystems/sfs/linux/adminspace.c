#include <linux/buffer_head.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include "asfs_fs.h"
#include "bitfuncs.h"

#ifdef CONFIG_ASFS_RW

static int sfs_set_free_block_count(
    struct super_block *sb,
    u32 free_blocks)
{
    struct buffer_head *bh;
    struct fsRootInfo *root_info;

    if (sb->s_blocksize < sizeof(struct fsRootInfo))
        return -EUCLEAN;

    bh = asfs_breadcheck(
        sb, ASFS_SB(sb)->rootobjectcontainer,
        ASFS_OBJECTCONTAINER_ID);
    if (!bh)
        return -EIO;

    root_info = (struct fsRootInfo *)
        ((u8 *)bh->b_data +
         sb->s_blocksize - sizeof(struct fsRootInfo));
    root_info->freeblocks = cpu_to_be32(free_blocks);
    ASFS_SB(sb)->freeblocks = free_blocks;
    asfs_bstore(sb, bh);
    asfs_brelse(bh);
    return 0;
}

static bool sfs_has_space(struct super_block *sb, u32 blocks)
{
    return ifs_sfs_has_allocation_headroom(
        ASFS_SB(sb)->freeblocks,
        blocks,
        ASFS_ALWAYSFREE) != 0;
}

static int sfs_scan_free_range(
    struct super_block *sb,
    u32 start,
    u32 end,
    u32 requested,
    u32 *best_start,
    u32 *best_length)
{
    const u32 bits_per_bitmap = ASFS_SB(sb)->blocks_inbitmap;
    const int words = (int)(bits_per_bitmap >> 5);
    u32 current = start;
    u32 run_start = 0U;
    u32 run_length = 0U;

    if (bits_per_bitmap == 0U || words <= 0 ||
        start > end || end > ASFS_SB(sb)->totalblocks)
        return -EINVAL;

    while (current < end) {
        const u32 bitmap_index = current / bits_per_bitmap;
        const u32 bitmap_base_block = bitmap_index * bits_per_bitmap;
        const u32 local_start = current - bitmap_base_block;
        const u32 local_end = min(
            bits_per_bitmap, end - bitmap_base_block);
        struct buffer_head *bh;
        struct fsBitmap *bitmap;
        u32 cursor = local_start;

        if (bitmap_index >= ASFS_SB(sb)->blocks_bitmap)
            return -EUCLEAN;

        bh = asfs_breadcheck(
            sb, ASFS_SB(sb)->bitmapbase + bitmap_index,
            ASFS_BITMAP_ID);
        if (!bh)
            return -EIO;

        bitmap = (struct fsBitmap *)bh->b_data;

        while (cursor < local_end) {
            int free_start = bmffo(
                bitmap->bitmap, words, (int)cursor);
            int allocated_start;
            u32 free_end;

            if (free_start < 0 ||
                (u32)free_start >= local_end) {
                run_length = 0U;
                break;
            }

            if ((u32)free_start != cursor) {
                run_length = 0U;
                run_start =
                    bitmap_base_block + (u32)free_start;
            } else if (run_length == 0U) {
                run_start =
                    bitmap_base_block + (u32)free_start;
            }

            allocated_start = bmffz(
                bitmap->bitmap, words, free_start);
            free_end = allocated_start < 0
                ? bits_per_bitmap
                : (u32)allocated_start;
            if (free_end > local_end)
                free_end = local_end;

            run_length += free_end - (u32)free_start;
            if (run_length > *best_length) {
                *best_start = run_start;
                *best_length = run_length;
                if (*best_length >= requested) {
                    *best_length = requested;
                    asfs_brelse(bh);
                    return 1;
                }
            }

            if (free_end >= local_end) {
                cursor = local_end;
            } else {
                run_length = 0U;
                cursor = free_end + 1U;
            }
        }

        asfs_brelse(bh);

        if (local_end != bits_per_bitmap)
            run_length = 0U;
        current = bitmap_base_block + local_end;
    }

    return 0;
}

int asfs_findspace(
    struct super_block *sb,
    u32 maxneeded,
    u32 start,
    u32 end,
    u32 *returned_block,
    u32 *returned_blocks)
{
    const u32 total = ASFS_SB(sb)->totalblocks;
    u32 best_start = 0U;
    u32 best_length = 0U;
    int result;

    if (!returned_block || !returned_blocks ||
        maxneeded == 0U || total == 0U)
        return -EINVAL;

    *returned_block = 0U;
    *returned_blocks = 0U;

    if (!sfs_has_space(sb, maxneeded))
        return -ENOSPC;

    start %= total;
    if (end == 0U)
        end = total;
    if (end > total)
        return -EINVAL;

    if (start < end) {
        result = sfs_scan_free_range(
            sb, start, end, maxneeded,
            &best_start, &best_length);
        if (result < 0)
            return result;
    } else {
        result = sfs_scan_free_range(
            sb, start, total, maxneeded,
            &best_start, &best_length);
        if (result < 0)
            return result;

        if (best_length < maxneeded) {
            u32 second_start = 0U;
            u32 second_length = 0U;

            result = sfs_scan_free_range(
                sb, 0U, end, maxneeded,
                &second_start, &second_length);
            if (result < 0)
                return result;
            if (second_length > best_length) {
                best_start = second_start;
                best_length = second_length;
            }
        }
    }

    if (best_length == 0U)
        return -ENOSPC;

    *returned_block = best_start;
    *returned_blocks = best_length;
    return 0;
}

static int sfs_update_bitmap_range(
    struct super_block *sb,
    u32 block,
    u32 blocks,
    bool allocate)
{
    const u32 bits_per_bitmap = ASFS_SB(sb)->blocks_inbitmap;
    struct buffer_head **buffers;
    u32 first_bitmap;
    u32 last_bitmap;
    u32 buffer_count;
    u32 new_free;
    u32 index;
    int result = 0;

    if (blocks == 0U || bits_per_bitmap == 0U ||
        block >= ASFS_SB(sb)->totalblocks ||
        blocks > ASFS_SB(sb)->totalblocks - block)
        return -EINVAL;

    first_bitmap = block / bits_per_bitmap;
    last_bitmap = (block + blocks - 1U) / bits_per_bitmap;
    if (last_bitmap >= ASFS_SB(sb)->blocks_bitmap)
        return -EUCLEAN;

    buffer_count = last_bitmap - first_bitmap + 1U;
    buffers = kcalloc(
        buffer_count, sizeof(*buffers), GFP_NOFS);
    if (!buffers)
        return -ENOMEM;

    for (index = 0U; index < buffer_count; ++index) {
        buffers[index] = asfs_breadcheck(
            sb,
            ASFS_SB(sb)->bitmapbase +
                first_bitmap + index,
            ASFS_BITMAP_ID);
        if (!buffers[index]) {
            result = -EIO;
            goto out;
        }
    }

    for (index = 0U; index < blocks; ++index) {
        const u32 absolute = block + index;
        const u32 relative_bitmap =
            absolute / bits_per_bitmap - first_bitmap;
        const u32 bit = absolute % bits_per_bitmap;
        const u32 word_index = bit >> 5;
        const u32 mask = 1U << (31U - (bit & 31U));
        struct fsBitmap *bitmap =
            (struct fsBitmap *)buffers[relative_bitmap]->b_data;
        const u32 word =
            be32_to_cpu(bitmap->bitmap[word_index]);
        const bool is_free = (word & mask) != 0U;

        if ((allocate && !is_free) ||
            (!allocate && is_free)) {
            result = -EUCLEAN;
            goto out;
        }
    }

    if (allocate) {
        if (ifs_sfs_free_count_after_allocate(
                ASFS_SB(sb)->freeblocks,
                blocks, &new_free) != 0) {
            result = -EUCLEAN;
            goto out;
        }
    } else {
        if (ifs_sfs_free_count_after_release(
                ASFS_SB(sb)->freeblocks,
                blocks,
                ASFS_SB(sb)->totalblocks,
                &new_free) != 0) {
            result = -EUCLEAN;
            goto out;
        }
    }

    for (index = 0U; index < blocks; ++index) {
        const u32 absolute = block + index;
        const u32 relative_bitmap =
            absolute / bits_per_bitmap - first_bitmap;
        const u32 bit = absolute % bits_per_bitmap;
        const u32 word_index = bit >> 5;
        const u32 mask = 1U << (31U - (bit & 31U));
        struct fsBitmap *bitmap =
            (struct fsBitmap *)buffers[relative_bitmap]->b_data;
        u32 word =
            be32_to_cpu(bitmap->bitmap[word_index]);

        word = allocate ? word & ~mask : word | mask;
        bitmap->bitmap[word_index] = cpu_to_be32(word);
    }

    for (index = 0U; index < buffer_count; ++index)
        asfs_bstore(sb, buffers[index]);

    result = sfs_set_free_block_count(sb, new_free);
    if (result != 0) {
        for (index = 0U; index < blocks; ++index) {
            const u32 absolute = block + index;
            const u32 relative_bitmap =
                absolute / bits_per_bitmap - first_bitmap;
            const u32 bit = absolute % bits_per_bitmap;
            const u32 word_index = bit >> 5;
            const u32 mask = 1U << (31U - (bit & 31U));
            struct fsBitmap *bitmap =
                (struct fsBitmap *)
                    buffers[relative_bitmap]->b_data;
            u32 word =
                be32_to_cpu(bitmap->bitmap[word_index]);

            word = allocate ? word | mask : word & ~mask;
            bitmap->bitmap[word_index] =
                cpu_to_be32(word);
        }
        for (index = 0U; index < buffer_count; ++index)
            asfs_bstore(sb, buffers[index]);
    }

out:
    for (index = 0U; index < buffer_count; ++index)
        asfs_brelse(buffers[index]);
    kfree(buffers);
    return result;
}

int asfs_markspace(
    struct super_block *sb,
    u32 block,
    u32 blocks)
{
    if (!sfs_has_space(sb, blocks))
        return -ENOSPC;

    return sfs_update_bitmap_range(
        sb, block, blocks, true);
}

int asfs_freespace(
    struct super_block *sb,
    u32 block,
    u32 blocks)
{
    return sfs_update_bitmap_range(
        sb, block, blocks, false);
}

static int sfs_find_and_mark(
    struct super_block *sb,
    u32 blocks,
    u32 *start)
{
    u32 found = 0U;
    int result;

    if (!start || !sfs_has_space(sb, blocks))
        return -ENOSPC;

    result = asfs_findspace(
        sb, blocks, 0U,
        ASFS_SB(sb)->totalblocks,
        start, &found);
    if (result != 0)
        return result;
    if (found != blocks)
        return -ENOSPC;

    return asfs_markspace(sb, *start, blocks);
}

static u32 sfs_admin_entries_per_container(
    struct super_block *sb)
{
    if (sb->s_blocksize <
        sizeof(struct fsAdminSpaceContainer))
        return 0U;

    return (sb->s_blocksize -
            sizeof(struct fsAdminSpaceContainer)) /
           sizeof(struct fsAdminSpace);
}

static int sfs_find_admin_descriptor_slot(
    struct super_block *sb,
    struct buffer_head **returned_bh,
    struct fsAdminSpace **returned_entry,
    u32 *tail_block)
{
    u32 block = ASFS_SB(sb)->adminspacecontainer;
    u32 budget = ASFS_SB(sb)->totalblocks;
    u32 last = 0U;

    if (!returned_bh || !returned_entry || !tail_block)
        return -EINVAL;

    *returned_bh = NULL;
    *returned_entry = NULL;
    *tail_block = 0U;

    while (block != 0U) {
        struct buffer_head *bh;
        struct fsAdminSpaceContainer *container;
        u32 count;
        u32 index;
        u32 next;

        if (budget-- == 0U)
            return -EUCLEAN;

        bh = asfs_breadcheck(
            sb, block, ASFS_ADMINSPACECONTAINER_ID);
        if (!bh)
            return -EIO;

        container =
            (struct fsAdminSpaceContainer *)bh->b_data;
        count = sfs_admin_entries_per_container(sb);
        for (index = 0U; index < count; ++index) {
            if (container->adminspace[index].space == 0U) {
                *returned_bh = bh;
                *returned_entry =
                    &container->adminspace[index];
                *tail_block = block;
                return 0;
            }
        }

        next = be32_to_cpu(container->next);
        last = block;
        asfs_brelse(bh);
        block = next;
    }

    *tail_block = last;
    return -ENOSPC;
}

int asfs_allocadminspace(
    struct super_block *sb,
    u32 *returned_block)
{
    u32 container_block =
        ASFS_SB(sb)->adminspacecontainer;
    u32 budget = ASFS_SB(sb)->totalblocks;

    if (!returned_block)
        return -EINVAL;
    *returned_block = 0U;

    while (container_block != 0U) {
        struct buffer_head *bh;
        struct fsAdminSpaceContainer *container;
        u32 count;
        u32 index;
        u32 next;

        if (budget-- == 0U)
            return -EUCLEAN;

        bh = asfs_breadcheck(
            sb, container_block,
            ASFS_ADMINSPACECONTAINER_ID);
        if (!bh)
            return -EIO;

        container =
            (struct fsAdminSpaceContainer *)bh->b_data;
        count = sfs_admin_entries_per_container(sb);

        for (index = 0U; index < count; ++index) {
            struct fsAdminSpace *entry =
                &container->adminspace[index];
            const u32 area = be32_to_cpu(entry->space);
            const u32 bits = be32_to_cpu(entry->bits);
            const int bit = area != 0U
                ? ifs_sfs_bitmap_word_find_zero(bits, 0U)
                : -1;

            if (bit >= 0) {
                const u32 mask =
                    1U << (31U - (u32)bit);
                const u32 block = area + (u32)bit;

                if (block >= ASFS_SB(sb)->totalblocks) {
                    asfs_brelse(bh);
                    return -EUCLEAN;
                }

                entry->bits = cpu_to_be32(bits | mask);
                asfs_bstore(sb, bh);
                asfs_brelse(bh);
                *returned_block = block;
                return 0;
            }
        }

        next = be32_to_cpu(container->next);
        asfs_brelse(bh);
        container_block = next;
    }

    {
        struct buffer_head *slot_bh = NULL;
        struct fsAdminSpace *slot = NULL;
        u32 tail_block = 0U;
        u32 area_start = 0U;
        int result;

        result = sfs_find_and_mark(
            sb, 32U, &area_start);
        if (result != 0)
            return result;

        result = sfs_find_admin_descriptor_slot(
            sb, &slot_bh, &slot, &tail_block);
        if (result == 0) {
            slot->space = cpu_to_be32(area_start);
            slot->bits = cpu_to_be32(0x80000000U);
            asfs_bstore(sb, slot_bh);
            asfs_brelse(slot_bh);
            *returned_block = area_start;
            return 0;
        }

        if (result != -ENOSPC || tail_block == 0U) {
            (void)asfs_freespace(sb, area_start, 32U);
            return result;
        }

        {
            struct buffer_head *new_bh;
            struct buffer_head *tail_bh;
            struct fsAdminSpaceContainer *new_container;
            struct fsAdminSpaceContainer *tail;

            new_bh = asfs_getzeroblk(sb, area_start);
            if (!new_bh) {
                (void)asfs_freespace(
                    sb, area_start, 32U);
                return -EIO;
            }

            new_container =
                (struct fsAdminSpaceContainer *)
                    new_bh->b_data;
            new_container->bheader.id =
                cpu_to_be32(
                    ASFS_ADMINSPACECONTAINER_ID);
            new_container->bheader.ownblock =
                cpu_to_be32(area_start);
            new_container->previous =
                cpu_to_be32(tail_block);
            new_container->bits = 32U;
            new_container->adminspace[0].space =
                cpu_to_be32(area_start);
            new_container->adminspace[0].bits =
                cpu_to_be32(0x80000000U);
            asfs_bstore(sb, new_bh);
            asfs_brelse(new_bh);

            tail_bh = asfs_breadcheck(
                sb, tail_block,
                ASFS_ADMINSPACECONTAINER_ID);
            if (!tail_bh) {
                (void)asfs_freespace(
                    sb, area_start, 32U);
                return -EIO;
            }

            tail =
                (struct fsAdminSpaceContainer *)
                    tail_bh->b_data;
            if (tail->next != 0U) {
                asfs_brelse(tail_bh);
                (void)asfs_freespace(
                    sb, area_start, 32U);
                return -EUCLEAN;
            }

            tail->next = cpu_to_be32(area_start);
            asfs_bstore(sb, tail_bh);
            asfs_brelse(tail_bh);
            *returned_block = area_start;
            return 0;
        }
    }
}

int asfs_freeadminspace(
    struct super_block *sb,
    u32 block)
{
    u32 container_block =
        ASFS_SB(sb)->adminspacecontainer;
    u32 budget = ASFS_SB(sb)->totalblocks;

    while (container_block != 0U) {
        struct buffer_head *bh;
        struct fsAdminSpaceContainer *container;
        u32 count;
        u32 index;
        u32 next;

        if (budget-- == 0U)
            return -EUCLEAN;

        bh = asfs_breadcheck(
            sb, container_block,
            ASFS_ADMINSPACECONTAINER_ID);
        if (!bh)
            return -EIO;

        container =
            (struct fsAdminSpaceContainer *)bh->b_data;
        count = sfs_admin_entries_per_container(sb);

        for (index = 0U; index < count; ++index) {
            struct fsAdminSpace *entry =
                &container->adminspace[index];
            u32 mask;

            if (ifs_sfs_adminspace_block_mask(
                    be32_to_cpu(entry->space),
                    block, &mask) == 0) {
                const u32 bits =
                    be32_to_cpu(entry->bits);

                if ((bits & mask) == 0U) {
                    asfs_brelse(bh);
                    return -EUCLEAN;
                }

                entry->bits =
                    cpu_to_be32(bits & ~mask);
                asfs_bstore(sb, bh);
                asfs_brelse(bh);
                return 0;
            }
        }

        next = be32_to_cpu(container->next);
        asfs_brelse(bh);
        container_block = next;
    }

    return -ENOENT;
}

#endif
