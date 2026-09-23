// SPDX-License-Identifier: GPL-3.0-or-later
#include "extfs/extfs.h"

/*
 * Bounded classic-block resize extension.
 *
 * core/extfs.c still contains the thoroughly-qualified direct-only ext2/ext3
 * implementations.  The build renames those two symbols to *_legacy_direct;
 * this file keeps the public ABI names and delegates operations that remain
 * wholly inside the twelve direct pointers to the legacy implementations.
 * Operations crossing or living above that boundary add exactly one classic
 * single-indirect block.  Double- and triple-indirect mutation stay fail-closed.
 */

#define CLASSIC_DIRECT_COUNT 12U
#define CLASSIC_SINGLE_INDEX 12U
#define CLASSIC_DOUBLE_INDEX 13U
#define CLASSIC_TRIPLE_INDEX 14U
#define CLASSIC_INCOMPAT_FILETYPE 0x00000002U
#define CLASSIC_INODE_FLAG_EXTENTS 0x00080000U
#define CLASSIC_RO_SPARSE_SUPER 0x00000001U
#define CLASSIC_RO_LARGE_FILE 0x00000002U
#define CLASSIC_RO_BTREE_DIR 0x00000004U
#define CLASSIC_STATE_VALID 0x0001U
#define CLASSIC_STATE_VALID_CLEAR_MASK 0xFFFEU
#define CLASSIC_INCOMPAT_RECOVER 0x00000004U
#define CLASSIC_EXT2_SCRATCH_BLOCKS 6U
#define CLASSIC_EXT3_SCRATCH_BLOCKS 8U

extfs_status extfs_resize_file_ext2_legacy_direct(extfs_volume *volume,
                                                   extfs_inode *inode,
                                                   extfs_u64 new_size,
                                                   void *scratch,
                                                   extfs_u32 scratch_size);
extfs_status extfs_resize_file_ext3_journaled_legacy_direct(
    extfs_volume *volume,
    extfs_inode *inode,
    extfs_u64 new_size,
    void *scratch,
    extfs_u32 scratch_size);

static extfs_u16 classic_le16(const extfs_u8 *p)
{
    return (extfs_u16)((extfs_u16)p[0] | ((extfs_u16)p[1] << 8));
}

static extfs_u32 classic_le32(const extfs_u8 *p)
{
    return (extfs_u32)p[0] |
           ((extfs_u32)p[1] << 8) |
           ((extfs_u32)p[2] << 16) |
           ((extfs_u32)p[3] << 24);
}

static void classic_store_le16(extfs_u8 *p, extfs_u16 value)
{
    p[0] = (extfs_u8)value;
    p[1] = (extfs_u8)(value >> 8);
}

static void classic_store_le32(extfs_u8 *p, extfs_u32 value)
{
    p[0] = (extfs_u8)value;
    p[1] = (extfs_u8)(value >> 8);
    p[2] = (extfs_u8)(value >> 16);
    p[3] = (extfs_u8)(value >> 24);
}

static void classic_zero(void *destination, extfs_u32 count)
{
    extfs_u8 *p = (extfs_u8 *)destination;
    while (count != 0U) {
        *p++ = 0U;
        --count;
    }
}

static void classic_copy(void *destination, const void *source, extfs_u32 count)
{
    extfs_u8 *d = (extfs_u8 *)destination;
    const extfs_u8 *s = (const extfs_u8 *)source;
    while (count != 0U) {
        *d++ = *s++;
        --count;
    }
}

static int classic_equal(const extfs_u8 *left,
                         const extfs_u8 *right,
                         extfs_u32 count)
{
    while (count != 0U) {
        if (*left++ != *right++) return 0;
        --count;
    }
    return 1;
}

static extfs_u64 classic_div_round_up(extfs_u64 value, extfs_u64 divisor)
{
    return value / divisor + ((value % divisor) != 0U ? 1U : 0U);
}

static extfs_status classic_read_bytes(const extfs_volume *volume,
                                       extfs_u64 offset,
                                       void *destination,
                                       extfs_u32 count)
{
    if (volume == 0 || volume->io.read_at == 0 || destination == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    if (offset > volume->byte_size ||
        (extfs_u64)count > volume->byte_size - offset)
        return EXTFS_ERR_RANGE;
    return volume->io.read_at(volume->io.user, offset, destination, count) == 0
        ? EXTFS_OK : EXTFS_ERR_IO;
}

static extfs_status classic_write_bytes(const extfs_volume *volume,
                                        extfs_u64 offset,
                                        const void *source,
                                        extfs_u32 count)
{
    if (volume == 0 || volume->io.write_at == 0 || source == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    if (offset > volume->byte_size ||
        (extfs_u64)count > volume->byte_size - offset)
        return EXTFS_ERR_RANGE;
    return volume->io.write_at(volume->io.user, offset, source, count) == 0
        ? EXTFS_OK : EXTFS_ERR_IO;
}

static extfs_status classic_flush(const extfs_volume *volume)
{
    if (volume == 0 || volume->io.flush == 0) return EXTFS_ERR_UNSUPPORTED;
    return volume->io.flush(volume->io.user) == 0 ? EXTFS_OK : EXTFS_ERR_IO;
}

static extfs_status classic_block_offset(const extfs_volume *volume,
                                         extfs_u64 block,
                                         extfs_u64 *offset)
{
    if (volume == 0 || offset == 0 || block >= volume->total_blocks)
        return EXTFS_ERR_CORRUPT;
    if (block > 0xFFFFFFFFFFFFFFFFULL / volume->block_size)
        return EXTFS_ERR_RANGE;
    *offset = block * volume->block_size;
    if (*offset > volume->byte_size ||
        volume->block_size > volume->byte_size - *offset)
        return EXTFS_ERR_RANGE;
    return EXTFS_OK;
}

static extfs_status classic_read_block(const extfs_volume *volume,
                                       extfs_u64 block,
                                       void *destination)
{
    extfs_u64 offset;
    extfs_status status = classic_block_offset(volume, block, &offset);
    if (status != EXTFS_OK) return status;
    return classic_read_bytes(volume, offset, destination, volume->block_size);
}

static extfs_status classic_write_block(const extfs_volume *volume,
                                        extfs_u64 block,
                                        const void *source)
{
    extfs_u64 offset;
    extfs_status status = classic_block_offset(volume, block, &offset);
    if (status != EXTFS_OK) return status;
    return classic_write_bytes(volume, offset, source, volume->block_size);
}

static extfs_status classic_group_for_block(const extfs_volume *volume,
                                            extfs_u64 block,
                                            extfs_u32 *group,
                                            extfs_u32 *bit)
{
    extfs_u64 relative;
    extfs_u64 group64;
    if (volume == 0 || group == 0 || bit == 0 ||
        block < volume->first_data_block || block >= volume->total_blocks)
        return EXTFS_ERR_CORRUPT;
    relative = block - volume->first_data_block;
    group64 = relative / volume->blocks_per_group;
    if (group64 >= volume->group_count) return EXTFS_ERR_CORRUPT;
    *group = (extfs_u32)group64;
    *bit = (extfs_u32)(relative % volume->blocks_per_group);
    return EXTFS_OK;
}

static extfs_status classic_group_bounds(const extfs_volume *volume,
                                         extfs_u32 group,
                                         extfs_u64 *first,
                                         extfs_u32 *count)
{
    extfs_u64 start;
    extfs_u64 remaining;
    if (volume == 0 || first == 0 || count == 0 ||
        group >= volume->group_count)
        return EXTFS_ERR_RANGE;
    start = (extfs_u64)volume->first_data_block +
            (extfs_u64)group * volume->blocks_per_group;
    if (start >= volume->total_blocks) return EXTFS_ERR_CORRUPT;
    remaining = volume->total_blocks - start;
    *first = start;
    *count = remaining < volume->blocks_per_group
        ? (extfs_u32)remaining : volume->blocks_per_group;
    return EXTFS_OK;
}

static extfs_status classic_descriptor_offset(const extfs_volume *volume,
                                              extfs_u32 group,
                                              extfs_u64 *offset)
{
    extfs_u64 table_block;
    extfs_u64 base;
    extfs_u64 delta;
    if (volume == 0 || offset == 0 || group >= volume->group_count)
        return EXTFS_ERR_RANGE;
    table_block = (extfs_u64)volume->first_data_block + 1U;
    if (classic_block_offset(volume, table_block, &base) != EXTFS_OK)
        return EXTFS_ERR_CORRUPT;
    delta = (extfs_u64)group * volume->descriptor_size;
    if (delta > volume->byte_size - base ||
        volume->descriptor_size > volume->byte_size - (base + delta))
        return EXTFS_ERR_CORRUPT;
    *offset = base + delta;
    return EXTFS_OK;
}

static extfs_status classic_read_descriptor(const extfs_volume *volume,
                                            extfs_u32 group,
                                            extfs_u8 descriptor[64])
{
    extfs_u64 offset;
    extfs_status status;
    classic_zero(descriptor, 64U);
    status = classic_descriptor_offset(volume, group, &offset);
    if (status != EXTFS_OK) return status;
    return classic_read_bytes(volume, offset, descriptor,
                              volume->descriptor_size);
}

static extfs_status classic_write_descriptor(const extfs_volume *volume,
                                             extfs_u32 group,
                                             const extfs_u8 descriptor[64])
{
    extfs_u64 offset;
    extfs_status status = classic_descriptor_offset(volume, group, &offset);
    if (status != EXTFS_OK) return status;
    return classic_write_bytes(volume, offset, descriptor,
                               volume->descriptor_size);
}

static int classic_bitmap_test(const extfs_u8 *bitmap, extfs_u32 bit)
{
    return (bitmap[bit >> 3] & (extfs_u8)(1U << (bit & 7U))) != 0U;
}

static void classic_bitmap_set(extfs_u8 *bitmap, extfs_u32 bit)
{
    bitmap[bit >> 3] |= (extfs_u8)(1U << (bit & 7U));
}

static void classic_bitmap_clear(extfs_u8 *bitmap, extfs_u32 bit)
{
    bitmap[bit >> 3] &= (extfs_u8)~(extfs_u8)(1U << (bit & 7U));
}

static int classic_is_power_of(extfs_u32 value, extfs_u32 base)
{
    if (value < 1U || base < 2U) return 0;
    while ((value % base) == 0U) value /= base;
    return value == 1U;
}

static int classic_group_has_super(const extfs_volume *volume,
                                   extfs_u32 group)
{
    if (group == 0U || group == 1U) return 1;
    if ((volume->feature_ro_compat & CLASSIC_RO_SPARSE_SUPER) == 0U) return 1;
    return classic_is_power_of(group, 3U) ||
           classic_is_power_of(group, 5U) ||
           classic_is_power_of(group, 7U);
}

static int classic_known_metadata(const extfs_volume *volume,
                                  extfs_u32 group,
                                  const extfs_u8 descriptor[64],
                                  extfs_u64 block)
{
    extfs_u64 first;
    extfs_u32 count;
    extfs_u64 inode_table = classic_le32(descriptor + 0x08U);
    extfs_u64 inode_table_blocks = classic_div_round_up(
        (extfs_u64)volume->inodes_per_group * volume->inode_size,
        volume->block_size);
    extfs_u64 gdt_blocks = classic_div_round_up(
        (extfs_u64)volume->group_count * volume->descriptor_size,
        volume->block_size);
    if (classic_group_bounds(volume, group, &first, &count) != EXTFS_OK)
        return 1;
    (void)count;
    if (block == classic_le32(descriptor + 0x00U) ||
        block == classic_le32(descriptor + 0x04U))
        return 1;
    if (block >= inode_table && block < inode_table + inode_table_blocks)
        return 1;
    if (classic_group_has_super(volume, group) &&
        block >= first && block <= first + gdt_blocks)
        return 1;
    return 0;
}

static extfs_status classic_inode_offset(const extfs_volume *volume,
                                         extfs_u32 inode_number,
                                         extfs_u64 *offset,
                                         extfs_u32 *inode_group)
{
    extfs_u8 descriptor[64];
    extfs_u32 group;
    extfs_u32 index;
    extfs_u64 inode_table;
    extfs_u64 base;
    extfs_u64 delta;
    extfs_status status;
    if (volume == 0 || offset == 0 || inode_number == 0U ||
        inode_number > volume->total_inodes)
        return EXTFS_ERR_RANGE;
    group = (inode_number - 1U) / volume->inodes_per_group;
    index = (inode_number - 1U) % volume->inodes_per_group;
    status = classic_read_descriptor(volume, group, descriptor);
    if (status != EXTFS_OK) return status;
    inode_table = classic_le32(descriptor + 0x08U);
    status = classic_block_offset(volume, inode_table, &base);
    if (status != EXTFS_OK) return status;
    delta = (extfs_u64)index * volume->inode_size;
    if (delta > volume->byte_size - base ||
        volume->inode_size > volume->byte_size - (base + delta))
        return EXTFS_ERR_CORRUPT;
    *offset = base + delta;
    if (inode_group != 0) *inode_group = group;
    return EXTFS_OK;
}

static extfs_status classic_primary_super_location(const extfs_volume *volume,
                                                   extfs_u64 *block,
                                                   extfs_u32 *within)
{
    if (volume == 0 || block == 0 || within == 0 ||
        volume->block_size < EXTFS_SUPERBLOCK_SIZE)
        return EXTFS_ERR_INVALID_ARGUMENT;
    *block = 1024U / volume->block_size;
    *within = 1024U % volume->block_size;
    if (*block >= volume->total_blocks ||
        *within > volume->block_size - EXTFS_SUPERBLOCK_SIZE)
        return EXTFS_ERR_CORRUPT;
    return EXTFS_OK;
}

static extfs_status classic_blocks_for_size(const extfs_volume *volume,
                                            extfs_u64 size,
                                            extfs_u32 *blocks)
{
    extfs_u64 count;
    if (volume == 0 || blocks == 0 || volume->block_size == 0U)
        return EXTFS_ERR_INVALID_ARGUMENT;
    count = classic_div_round_up(size, volume->block_size);
    if (count > 0xFFFFFFFFULL) return EXTFS_ERR_RANGE;
    *blocks = (extfs_u32)count;
    return EXTFS_OK;
}

static extfs_u32 classic_map_pointer(const extfs_u32 direct[CLASSIC_DIRECT_COUNT],
                                     const extfs_u8 *indirect,
                                     extfs_u32 logical)
{
    if (logical < CLASSIC_DIRECT_COUNT) return direct[logical];
    return classic_le32(indirect + (logical - CLASSIC_DIRECT_COUNT) * 4U);
}

static void classic_set_map_pointer(extfs_u32 direct[CLASSIC_DIRECT_COUNT],
                                    extfs_u8 *indirect,
                                    extfs_u32 logical,
                                    extfs_u32 block)
{
    if (logical < CLASSIC_DIRECT_COUNT) direct[logical] = block;
    else classic_store_le32(indirect + (logical - CLASSIC_DIRECT_COUNT) * 4U,
                            block);
}

static extfs_status classic_validate_allocated_block(const extfs_volume *volume,
                                                     extfs_u64 block,
                                                     extfs_u8 *bitmap)
{
    extfs_u8 descriptor[64];
    extfs_u32 group;
    extfs_u32 bit;
    extfs_u64 bitmap_block;
    extfs_status status = classic_group_for_block(volume, block, &group, &bit);
    if (status != EXTFS_OK) return status;
    status = classic_read_descriptor(volume, group, descriptor);
    if (status != EXTFS_OK) return status;
    if (classic_known_metadata(volume, group, descriptor, block))
        return EXTFS_ERR_CORRUPT;
    bitmap_block = classic_le32(descriptor + 0x00U);
    status = classic_read_block(volume, bitmap_block, bitmap);
    if (status != EXTFS_OK) return status;
    return classic_bitmap_test(bitmap, bit) ? EXTFS_OK : EXTFS_ERR_CORRUPT;
}

static void classic_swap_u32(extfs_u8 *array, extfs_u32 a, extfs_u32 b)
{
    extfs_u32 av = classic_le32(array + a * 4U);
    extfs_u32 bv = classic_le32(array + b * 4U);
    classic_store_le32(array + a * 4U, bv);
    classic_store_le32(array + b * 4U, av);
}

static void classic_heap_sift(extfs_u8 *array, extfs_u32 count, extfs_u32 root)
{
    for (;;) {
        extfs_u32 largest = root;
        extfs_u32 left = root * 2U + 1U;
        extfs_u32 right = left + 1U;
        if (left < count &&
            classic_le32(array + left * 4U) >
            classic_le32(array + largest * 4U))
            largest = left;
        if (right < count &&
            classic_le32(array + right * 4U) >
            classic_le32(array + largest * 4U))
            largest = right;
        if (largest == root) return;
        classic_swap_u32(array, root, largest);
        root = largest;
    }
}

static void classic_heap_sort(extfs_u8 *array, extfs_u32 count)
{
    extfs_u32 i;
    if (count < 2U) return;
    for (i = count / 2U; i != 0U; --i)
        classic_heap_sift(array, count, i - 1U);
    for (i = count; i > 1U; --i) {
        classic_swap_u32(array, 0U, i - 1U);
        classic_heap_sift(array, i - 1U, 0U);
    }
}

static extfs_status classic_load_and_validate_mapping(
    const extfs_volume *volume,
    const extfs_inode *inode,
    extfs_u32 old_blocks,
    extfs_u32 direct[CLASSIC_DIRECT_COUNT],
    extfs_u8 *indirect_old,
    extfs_u8 *bitmap,
    extfs_u8 *sort)
{
    extfs_u32 pointers_per_block = volume->block_size / 4U;
    extfs_u32 indirect_count = old_blocks > CLASSIC_DIRECT_COUNT
        ? old_blocks - CLASSIC_DIRECT_COUNT : 0U;
    extfs_u32 root = classic_le32(inode->block_map + CLASSIC_SINGLE_INDEX * 4U);
    extfs_u32 i;
    extfs_u32 j;
    extfs_status status;

    for (i = 0U; i < CLASSIC_DIRECT_COUNT; ++i)
        direct[i] = classic_le32(inode->block_map + i * 4U);
    if (classic_le32(inode->block_map + CLASSIC_DOUBLE_INDEX * 4U) != 0U ||
        classic_le32(inode->block_map + CLASSIC_TRIPLE_INDEX * 4U) != 0U)
        return EXTFS_ERR_UNSUPPORTED;

    classic_zero(indirect_old, volume->block_size);
    if (old_blocks > CLASSIC_DIRECT_COUNT) {
        if (root == 0U) return EXTFS_ERR_UNSUPPORTED;
        status = classic_validate_allocated_block(volume, root, bitmap);
        if (status != EXTFS_OK) return status;
        status = classic_read_block(volume, root, indirect_old);
        if (status != EXTFS_OK) return status;
        for (i = indirect_count; i < pointers_per_block; ++i) {
            if (classic_le32(indirect_old + i * 4U) != 0U)
                return EXTFS_ERR_UNSUPPORTED;
        }
    } else if (root != 0U) {
        return EXTFS_ERR_UNSUPPORTED;
    }

    for (i = 0U; i < CLASSIC_DIRECT_COUNT; ++i) {
        if (i < old_blocks) {
            if (direct[i] == 0U) return EXTFS_ERR_UNSUPPORTED;
            status = classic_validate_allocated_block(volume, direct[i], bitmap);
            if (status != EXTFS_OK) return status;
            if (root != 0U && direct[i] == root) return EXTFS_ERR_CORRUPT;
            for (j = 0U; j < i; ++j)
                if (direct[j] == direct[i]) return EXTFS_ERR_CORRUPT;
        } else if (direct[i] != 0U) {
            return EXTFS_ERR_UNSUPPORTED;
        }
    }

    if (indirect_count != 0U) {
        classic_copy(sort, indirect_old, indirect_count * 4U);
        for (i = 0U; i < indirect_count; ++i) {
            extfs_u32 block = classic_le32(indirect_old + i * 4U);
            if (block == 0U || block == root) return EXTFS_ERR_CORRUPT;
            status = classic_validate_allocated_block(volume, block, bitmap);
            if (status != EXTFS_OK) return status;
            for (j = 0U; j < CLASSIC_DIRECT_COUNT; ++j)
                if (block == direct[j]) return EXTFS_ERR_CORRUPT;
        }
        classic_heap_sort(sort, indirect_count);
        for (i = 1U; i < indirect_count; ++i)
            if (classic_le32(sort + (i - 1U) * 4U) ==
                classic_le32(sort + i * 4U))
                return EXTFS_ERR_CORRUPT;
    }
    return EXTFS_OK;
}

static extfs_status classic_select_growth_group(
    const extfs_volume *volume,
    extfs_u32 inode_group,
    extfs_u32 old_blocks,
    extfs_u32 new_blocks,
    int needs_root,
    extfs_u32 direct_new[CLASSIC_DIRECT_COUNT],
    extfs_u8 *indirect_new,
    extfs_u32 *root_new,
    extfs_u8 *bitmap,
    extfs_u32 *selected_group)
{
    extfs_u32 needed_data = new_blocks - old_blocks;
    extfs_u32 total_needed = needed_data + (needs_root ? 1U : 0U);
    extfs_u32 attempt;
    if (volume->free_blocks < total_needed) return EXTFS_ERR_NO_SPACE;

    for (attempt = 0U; attempt < volume->group_count; ++attempt) {
        extfs_u32 group = (inode_group + attempt) % volume->group_count;
        extfs_u8 descriptor[64];
        extfs_u64 first;
        extfs_u32 count;
        extfs_u32 available = 0U;
        extfs_u32 bit;
        extfs_u64 bitmap_block;
        extfs_status status = classic_read_descriptor(volume, group, descriptor);
        if (status != EXTFS_OK) return status;
        if (classic_le16(descriptor + 0x0CU) < total_needed) continue;
        status = classic_group_bounds(volume, group, &first, &count);
        if (status != EXTFS_OK) return status;
        bitmap_block = classic_le32(descriptor + 0x00U);
        status = classic_read_block(volume, bitmap_block, bitmap);
        if (status != EXTFS_OK) return status;

        for (bit = 0U; bit < count && available < total_needed; ++bit) {
            extfs_u64 candidate = first + bit;
            if (!classic_bitmap_test(bitmap, bit) &&
                !classic_known_metadata(volume, group, descriptor, candidate))
                ++available;
        }
        if (available < total_needed) continue;

        {
            extfs_u32 logical = old_blocks;
            int root_pending = needs_root;
            for (bit = 0U; bit < count &&
                 (logical < new_blocks || root_pending != 0); ++bit) {
                extfs_u64 candidate64 = first + bit;
                extfs_u32 candidate;
                if (classic_bitmap_test(bitmap, bit) ||
                    classic_known_metadata(volume, group, descriptor, candidate64))
                    continue;
                if (candidate64 > 0xFFFFFFFFULL) return EXTFS_ERR_RANGE;
                candidate = (extfs_u32)candidate64;
                if (root_pending != 0) {
                    *root_new = candidate;
                    root_pending = 0;
                } else {
                    classic_set_map_pointer(direct_new, indirect_new,
                                            logical, candidate);
                    ++logical;
                }
                classic_bitmap_set(bitmap, bit);
            }
            if (logical != new_blocks || root_pending != 0)
                return EXTFS_ERR_CORRUPT;
        }
        *selected_group = group;
        return EXTFS_OK;
    }
    /* Free space exists globally but this bounded transaction refuses to span
     * multiple allocation groups. */
    return EXTFS_ERR_UNSUPPORTED;
}

static extfs_status classic_release_group(const extfs_volume *volume,
                                          extfs_u32 old_blocks,
                                          extfs_u32 new_blocks,
                                          const extfs_u32 direct_old[CLASSIC_DIRECT_COUNT],
                                          const extfs_u8 *indirect_old,
                                          extfs_u32 old_root,
                                          int frees_root,
                                          extfs_u32 *group_out)
{
    extfs_u32 logical;
    extfs_u32 group = 0U;
    int have_group = 0;
    for (logical = new_blocks; logical < old_blocks; ++logical) {
        extfs_u32 block = classic_map_pointer(direct_old, indirect_old, logical);
        extfs_u32 candidate_group;
        extfs_u32 bit;
        extfs_status status = classic_group_for_block(
            volume, block, &candidate_group, &bit);
        if (status != EXTFS_OK) return status;
        if (!have_group) {
            group = candidate_group;
            have_group = 1;
        } else if (group != candidate_group) {
            return EXTFS_ERR_UNSUPPORTED;
        }
        (void)bit;
    }
    if (frees_root != 0) {
        extfs_u32 candidate_group;
        extfs_u32 bit;
        extfs_status status = classic_group_for_block(
            volume, old_root, &candidate_group, &bit);
        if (status != EXTFS_OK) return status;
        if (!have_group) {
            group = candidate_group;
            have_group = 1;
        } else if (group != candidate_group) {
            return EXTFS_ERR_UNSUPPORTED;
        }
    }
    if (!have_group) return EXTFS_ERR_INVALID_ARGUMENT;
    *group_out = group;
    return EXTFS_OK;
}

static extfs_u32 classic_changed_block_count(extfs_u32 old_blocks,
                                             extfs_u32 new_blocks,
                                             int root_change)
{
    extfs_u32 data = old_blocks > new_blocks
        ? old_blocks - new_blocks : new_blocks - old_blocks;
    return data + (root_change != 0 ? 1U : 0U);
}

static extfs_status classic_apply_group_bitmap(
    const extfs_volume *volume,
    extfs_u32 group,
    int allocate,
    extfs_u32 old_blocks,
    extfs_u32 new_blocks,
    const extfs_u32 direct_old[CLASSIC_DIRECT_COUNT],
    const extfs_u8 *indirect_old,
    extfs_u32 old_root,
    const extfs_u32 direct_new[CLASSIC_DIRECT_COUNT],
    const extfs_u8 *indirect_new,
    extfs_u32 new_root,
    int root_change,
    extfs_u8 *bitmap,
    extfs_u8 descriptor[64],
    extfs_u64 *bitmap_block_out,
    extfs_u16 *new_descriptor_free)
{
    extfs_u32 changed = classic_changed_block_count(old_blocks, new_blocks,
                                                    root_change);
    extfs_u16 descriptor_free;
    extfs_u64 bitmap_block;
    extfs_u32 logical;
    extfs_status status = classic_read_descriptor(volume, group, descriptor);
    if (status != EXTFS_OK) return status;
    descriptor_free = classic_le16(descriptor + 0x0CU);
    bitmap_block = classic_le32(descriptor + 0x00U);
    if (bitmap_block == 0U || bitmap_block >= volume->total_blocks)
        return EXTFS_ERR_CORRUPT;
    status = classic_read_block(volume, bitmap_block, bitmap);
    if (status != EXTFS_OK) return status;

    if (allocate != 0) {
        if (descriptor_free < changed) return EXTFS_ERR_CORRUPT;
        if (root_change != 0) {
            extfs_u32 rg;
            extfs_u32 bit;
            status = classic_group_for_block(volume, new_root, &rg, &bit);
            if (status != EXTFS_OK || rg != group ||
                classic_bitmap_test(bitmap, bit) ||
                classic_known_metadata(volume, group, descriptor, new_root))
                return EXTFS_ERR_CORRUPT;
            classic_bitmap_set(bitmap, bit);
        }
        for (logical = old_blocks; logical < new_blocks; ++logical) {
            extfs_u32 block = classic_map_pointer(direct_new, indirect_new,
                                                  logical);
            extfs_u32 bg;
            extfs_u32 bit;
            status = classic_group_for_block(volume, block, &bg, &bit);
            if (status != EXTFS_OK || bg != group ||
                classic_bitmap_test(bitmap, bit) ||
                classic_known_metadata(volume, group, descriptor, block))
                return EXTFS_ERR_CORRUPT;
            classic_bitmap_set(bitmap, bit);
        }
        *new_descriptor_free = (extfs_u16)(descriptor_free - changed);
    } else {
        if ((extfs_u32)descriptor_free + changed > volume->blocks_per_group)
            return EXTFS_ERR_CORRUPT;
        for (logical = new_blocks; logical < old_blocks; ++logical) {
            extfs_u32 block = classic_map_pointer(direct_old, indirect_old,
                                                  logical);
            extfs_u32 bg;
            extfs_u32 bit;
            status = classic_group_for_block(volume, block, &bg, &bit);
            if (status != EXTFS_OK || bg != group ||
                !classic_bitmap_test(bitmap, bit))
                return EXTFS_ERR_CORRUPT;
            classic_bitmap_clear(bitmap, bit);
        }
        if (root_change != 0) {
            extfs_u32 rg;
            extfs_u32 bit;
            status = classic_group_for_block(volume, old_root, &rg, &bit);
            if (status != EXTFS_OK || rg != group ||
                !classic_bitmap_test(bitmap, bit))
                return EXTFS_ERR_CORRUPT;
            classic_bitmap_clear(bitmap, bit);
        }
        *new_descriptor_free = (extfs_u16)(descriptor_free + changed);
    }
    *bitmap_block_out = bitmap_block;
    return EXTFS_OK;
}

static extfs_status classic_validate_inode_raw(const extfs_volume *volume,
                                               const extfs_inode *inode,
                                               const extfs_u8 *raw,
                                               extfs_u32 old_blocks)
{
    extfs_u64 raw_size;
    extfs_u64 allocated_blocks;
    extfs_u64 expected_sectors;
    extfs_u32 i;
    if (classic_le16(raw + 0x00U) != inode->mode ||
        classic_le32(raw + 0x20U) != inode->flags)
        return EXTFS_ERR_CORRUPT;
    raw_size = (extfs_u64)classic_le32(raw + 0x04U) |
               ((extfs_u64)classic_le32(raw + 0x6CU) << 32);
    if (raw_size != inode->size) return EXTFS_ERR_CORRUPT;
    if (classic_le32(raw + 0x68U) != 0U)
        return EXTFS_ERR_UNSUPPORTED; /* external xattr block affects i_blocks */
    for (i = 0U; i < 15U; ++i)
        if (classic_le32(raw + 0x28U + i * 4U) !=
            classic_le32(inode->block_map + i * 4U))
            return EXTFS_ERR_CORRUPT;
    allocated_blocks = old_blocks + (old_blocks > CLASSIC_DIRECT_COUNT ? 1U : 0U);
    expected_sectors = allocated_blocks * (volume->block_size / 512U);
    if (expected_sectors > 0xFFFFFFFFULL ||
        classic_le32(raw + 0x1CU) != (extfs_u32)expected_sectors)
        return EXTFS_ERR_UNSUPPORTED;
    return EXTFS_OK;
}

static extfs_status classic_update_inode_raw(const extfs_volume *volume,
                                             extfs_u8 *raw,
                                             extfs_u64 new_size,
                                             extfs_u32 new_blocks,
                                             const extfs_u32 direct_new[CLASSIC_DIRECT_COUNT],
                                             extfs_u32 new_root)
{
    extfs_u64 allocated_blocks = new_blocks +
        (new_blocks > CLASSIC_DIRECT_COUNT ? 1U : 0U);
    extfs_u64 sectors = allocated_blocks * (volume->block_size / 512U);
    extfs_u32 i;
    if (sectors > 0xFFFFFFFFULL) return EXTFS_ERR_RANGE;
    classic_store_le32(raw + 0x04U, (extfs_u32)new_size);
    classic_store_le32(raw + 0x6CU, (extfs_u32)(new_size >> 32));
    classic_store_le32(raw + 0x1CU, (extfs_u32)sectors);
    for (i = 0U; i < CLASSIC_DIRECT_COUNT; ++i)
        classic_store_le32(raw + 0x28U + i * 4U, direct_new[i]);
    classic_store_le32(raw + 0x28U + CLASSIC_SINGLE_INDEX * 4U, new_root);
    classic_store_le32(raw + 0x28U + CLASSIC_DOUBLE_INDEX * 4U, 0U);
    classic_store_le32(raw + 0x28U + CLASSIC_TRIPLE_INDEX * 4U, 0U);
    return EXTFS_OK;
}

static void classic_publish_inode(extfs_inode *inode,
                                  extfs_u64 new_size,
                                  const extfs_u32 direct_new[CLASSIC_DIRECT_COUNT],
                                  extfs_u32 new_root)
{
    extfs_u32 i;
    for (i = 0U; i < CLASSIC_DIRECT_COUNT; ++i)
        classic_store_le32(inode->block_map + i * 4U, direct_new[i]);
    classic_store_le32(inode->block_map + CLASSIC_SINGLE_INDEX * 4U, new_root);
    classic_store_le32(inode->block_map + CLASSIC_DOUBLE_INDEX * 4U, 0U);
    classic_store_le32(inode->block_map + CLASSIC_TRIPLE_INDEX * 4U, 0U);
    inode->size = new_size;
}

static extfs_status classic_zero_growth_data(
    const extfs_volume *volume,
    const extfs_inode *inode,
    extfs_u32 old_blocks,
    extfs_u32 new_blocks,
    extfs_u64 new_size,
    const extfs_u32 direct_old[CLASSIC_DIRECT_COUNT],
    const extfs_u8 *indirect_old,
    const extfs_u32 direct_new[CLASSIC_DIRECT_COUNT],
    const extfs_u8 *indirect_new,
    extfs_u8 *zero_block)
{
    extfs_status status;
    extfs_u32 logical;
    classic_zero(zero_block, volume->block_size);

    if (new_size > inode->size && old_blocks != 0U &&
        (inode->size % volume->block_size) != 0U) {
        extfs_u64 old_end = (extfs_u64)old_blocks * volume->block_size;
        extfs_u64 zero_end = new_size < old_end ? new_size : old_end;
        if (zero_end > inode->size) {
            extfs_u32 old_last = old_blocks - 1U;
            extfs_u32 block = classic_map_pointer(direct_old, indirect_old,
                                                  old_last);
            extfs_u32 within = (extfs_u32)(inode->size % volume->block_size);
            extfs_u64 offset;
            status = classic_block_offset(volume, block, &offset);
            if (status != EXTFS_OK) return status;
            status = classic_write_bytes(volume, offset + within, zero_block,
                                         (extfs_u32)(zero_end - inode->size));
            if (status != EXTFS_OK) return status;
        }
    }

    for (logical = old_blocks; logical < new_blocks; ++logical) {
        extfs_u32 block = classic_map_pointer(direct_new, indirect_new, logical);
        status = classic_write_block(volume, block, zero_block);
        if (status != EXTFS_OK) return status;
    }
    return EXTFS_OK;
}

static extfs_status classic_zero_ext2_shrink_tail(
    const extfs_volume *volume,
    extfs_u64 new_size,
    extfs_u32 new_blocks,
    const extfs_u32 direct_new[CLASSIC_DIRECT_COUNT],
    const extfs_u8 *indirect_new,
    extfs_u8 *zero_block)
{
    extfs_u32 logical;
    extfs_u32 block;
    extfs_u32 within;
    extfs_u64 offset;
    extfs_status status;
    if (new_size == 0U || (new_size % volume->block_size) == 0U)
        return EXTFS_OK;
    if (new_blocks == 0U) return EXTFS_ERR_CORRUPT;
    logical = new_blocks - 1U;
    block = classic_map_pointer(direct_new, indirect_new, logical);
    within = (extfs_u32)(new_size % volume->block_size);
    classic_zero(zero_block, volume->block_size);
    status = classic_block_offset(volume, block, &offset);
    if (status != EXTFS_OK) return status;
    return classic_write_bytes(volume, offset + within, zero_block,
                               volume->block_size - within);
}

static extfs_status classic_common_preflight(
    const extfs_volume *volume,
    const extfs_inode *inode,
    extfs_u64 new_size,
    extfs_u32 scratch_size,
    extfs_u32 required_scratch_blocks,
    extfs_u32 *old_blocks,
    extfs_u32 *new_blocks)
{
    extfs_u64 max_blocks;
    extfs_status status;
    if (volume == 0 || inode == 0 || old_blocks == 0 || new_blocks == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    if (volume->block_size < 1024U ||
        volume->block_size > EXTFS_MAX_BLOCK_SIZE ||
        (volume->block_size & 3U) != 0U ||
        volume->descriptor_size != 32U || volume->metadata_checksums != 0U ||
        volume->blocks_per_group == 0U || volume->blocks_per_group > 0xFFFFU ||
        scratch_size < volume->block_size * required_scratch_blocks ||
        extfs_inode_write_assess(volume, inode) != EXTFS_OK ||
        (inode->flags & CLASSIC_INODE_FLAG_EXTENTS) != 0U)
        return EXTFS_ERR_UNSUPPORTED;
    status = classic_blocks_for_size(volume, inode->size, old_blocks);
    if (status != EXTFS_OK) return status;
    status = classic_blocks_for_size(volume, new_size, new_blocks);
    if (status != EXTFS_OK) return status;
    max_blocks = CLASSIC_DIRECT_COUNT + volume->block_size / 4U;
    if (*old_blocks > max_blocks || *new_blocks > max_blocks)
        return EXTFS_ERR_UNSUPPORTED;
    return EXTFS_OK;
}

extfs_status extfs_resize_file_ext2_direct(extfs_volume *volume,
                                           extfs_inode *inode,
                                           extfs_u64 new_size,
                                           void *scratch,
                                           extfs_u32 scratch_size)
{
    extfs_u32 old_blocks;
    extfs_u32 new_blocks;
    extfs_status status;
    extfs_u8 *bitmap;
    extfs_u8 *indirect_old;
    extfs_u8 *indirect_new;
    extfs_u8 *sort;
    extfs_u8 *inode_image;
    extfs_u8 *zero_block;
    extfs_u32 direct_old[CLASSIC_DIRECT_COUNT];
    extfs_u32 direct_new[CLASSIC_DIRECT_COUNT];
    extfs_u32 old_root;
    extfs_u32 new_root;
    extfs_u32 inode_group;
    extfs_u64 inode_offset;
    extfs_u32 touched_group = 0U;
    extfs_u32 changed = 0U;
    extfs_u64 new_free_blocks;
    extfs_u16 original_state;
    extfs_u16 dirty_state;
    extfs_u8 descriptor[64];
    extfs_u16 new_descriptor_free = 0U;
    extfs_u64 bitmap_block = 0U;
    int allocation_change = 0;
    int root_change = 0;
    extfs_u32 i;

    if (volume == 0 || inode == 0 || scratch == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    status = classic_blocks_for_size(volume, inode->size, &old_blocks);
    if (status != EXTFS_OK) return status;
    status = classic_blocks_for_size(volume, new_size, &new_blocks);
    if (status != EXTFS_OK) return status;
    if (old_blocks <= CLASSIC_DIRECT_COUNT &&
        new_blocks <= CLASSIC_DIRECT_COUNT)
        return extfs_resize_file_ext2_legacy_direct(
            volume, inode, new_size, scratch, scratch_size);

    status = classic_common_preflight(volume, inode, new_size, scratch_size,
                                      CLASSIC_EXT2_SCRATCH_BLOCKS,
                                      &old_blocks, &new_blocks);
    if (status != EXTFS_OK) return status;
    if (volume->kind != EXTFS_KIND_EXT2 || volume->io.flush == 0)
        return EXTFS_ERR_UNSUPPORTED;
    if (new_size == inode->size) return EXTFS_OK;

    bitmap = (extfs_u8 *)scratch;
    indirect_old = bitmap + volume->block_size;
    indirect_new = indirect_old + volume->block_size;
    sort = indirect_new + volume->block_size;
    inode_image = sort + volume->block_size;
    zero_block = inode_image + volume->block_size;

    status = classic_load_and_validate_mapping(volume, inode, old_blocks,
                                               direct_old, indirect_old,
                                               bitmap, sort);
    if (status != EXTFS_OK) return status;
    for (i = 0U; i < CLASSIC_DIRECT_COUNT; ++i) direct_new[i] = direct_old[i];
    classic_copy(indirect_new, indirect_old, volume->block_size);
    old_root = classic_le32(inode->block_map + CLASSIC_SINGLE_INDEX * 4U);
    new_root = old_root;

    status = classic_inode_offset(volume, inode->number, &inode_offset,
                                  &inode_group);
    if (status != EXTFS_OK) return status;
    if (volume->inode_size > volume->block_size)
        return EXTFS_ERR_UNSUPPORTED;
    status = classic_read_bytes(volume, inode_offset, inode_image,
                                volume->inode_size);
    if (status != EXTFS_OK) return status;
    status = classic_validate_inode_raw(volume, inode, inode_image, old_blocks);
    if (status != EXTFS_OK) return status;

    if (new_blocks > old_blocks) {
        int needs_root = old_blocks <= CLASSIC_DIRECT_COUNT;
        root_change = needs_root;
        allocation_change = 1;
        status = classic_select_growth_group(
            volume, inode_group, old_blocks, new_blocks, needs_root,
            direct_new, indirect_new, &new_root, bitmap, &touched_group);
        if (status != EXTFS_OK) return status;
    } else if (new_blocks < old_blocks) {
        root_change = old_blocks > CLASSIC_DIRECT_COUNT &&
                      new_blocks <= CLASSIC_DIRECT_COUNT;
        allocation_change = 1;
        status = classic_release_group(volume, old_blocks, new_blocks,
                                       direct_old, indirect_old, old_root,
                                       root_change, &touched_group);
        if (status != EXTFS_OK) return status;
        for (i = new_blocks; i < old_blocks; ++i)
            classic_set_map_pointer(direct_new, indirect_new, i, 0U);
        if (root_change != 0) {
            new_root = 0U;
            classic_zero(indirect_new, volume->block_size);
        }
    }

    changed = allocation_change != 0
        ? classic_changed_block_count(old_blocks, new_blocks, root_change) : 0U;
    if (new_blocks > old_blocks) {
        if (volume->free_blocks < changed) return EXTFS_ERR_NO_SPACE;
        new_free_blocks = volume->free_blocks - changed;
    } else {
        if (volume->free_blocks + changed > volume->total_blocks)
            return EXTFS_ERR_CORRUPT;
        new_free_blocks = volume->free_blocks + changed;
    }

    status = classic_read_bytes(volume, 1024U, sort, EXTFS_SUPERBLOCK_SIZE);
    if (status != EXTFS_OK) return status;
    if (classic_le16(sort + 0x38U) != 0xEF53U ||
        classic_le16(sort + 0x3AU) != volume->state ||
        classic_le32(sort + 0x0CU) != (extfs_u32)volume->free_blocks ||
        !classic_equal(sort + 0x68U, volume->uuid, 16U))
        return EXTFS_ERR_CORRUPT;

    status = classic_update_inode_raw(volume, inode_image, new_size, new_blocks,
                                      direct_new, new_root);
    if (status != EXTFS_OK) return status;

    /* The ext2 dirty marker is the crash-consistency fence. */
    original_state = volume->state;
    dirty_state = (extfs_u16)(original_state & CLASSIC_STATE_VALID_CLEAR_MASK);
    classic_store_le16(sort + 0x3AU, dirty_state);
    status = classic_write_bytes(volume, 1024U, sort, EXTFS_SUPERBLOCK_SIZE);
    if (status != EXTFS_OK) return status;
    volume->state = dirty_state;
    status = classic_flush(volume);
    if (status != EXTFS_OK) return status;

    if (new_blocks > old_blocks) {
        status = classic_apply_group_bitmap(
            volume, touched_group, 1, old_blocks, new_blocks,
            direct_old, indirect_old, old_root,
            direct_new, indirect_new, new_root, root_change,
            bitmap, descriptor, &bitmap_block, &new_descriptor_free);
        if (status != EXTFS_OK) return status;
        status = classic_write_block(volume, bitmap_block, bitmap);
        if (status != EXTFS_OK) return status;
        classic_store_le16(descriptor + 0x0CU, new_descriptor_free);
        status = classic_write_descriptor(volume, touched_group, descriptor);
        if (status != EXTFS_OK) return status;
        classic_store_le32(sort + 0x0CU, (extfs_u32)new_free_blocks);
        status = classic_write_bytes(volume, 1024U, sort,
                                     EXTFS_SUPERBLOCK_SIZE);
        if (status != EXTFS_OK) return status;
        status = classic_zero_growth_data(
            volume, inode, old_blocks, new_blocks, new_size,
            direct_old, indirect_old, direct_new, indirect_new, zero_block);
        if (status != EXTFS_OK) return status;
        if (new_blocks > CLASSIC_DIRECT_COUNT) {
            status = classic_write_block(volume, new_root, indirect_new);
            if (status != EXTFS_OK) return status;
        }
        status = classic_write_bytes(volume, inode_offset, inode_image,
                                     volume->inode_size);
        if (status != EXTFS_OK) return status;
    } else {
        /* Publish the smaller size/map before releasing blocks. */
        status = classic_write_bytes(volume, inode_offset, inode_image,
                                     volume->inode_size);
        if (status != EXTFS_OK) return status;
        if (new_blocks > CLASSIC_DIRECT_COUNT) {
            status = classic_write_block(volume, new_root, indirect_new);
            if (status != EXTFS_OK) return status;
        }
        if (allocation_change != 0) {
            status = classic_apply_group_bitmap(
                volume, touched_group, 0, old_blocks, new_blocks,
                direct_old, indirect_old, old_root,
                direct_new, indirect_new, new_root, root_change,
                bitmap, descriptor, &bitmap_block, &new_descriptor_free);
            if (status != EXTFS_OK) return status;
            status = classic_write_block(volume, bitmap_block, bitmap);
            if (status != EXTFS_OK) return status;
            classic_store_le16(descriptor + 0x0CU, new_descriptor_free);
            status = classic_write_descriptor(volume, touched_group, descriptor);
            if (status != EXTFS_OK) return status;
            classic_store_le32(sort + 0x0CU, (extfs_u32)new_free_blocks);
            status = classic_write_bytes(volume, 1024U, sort,
                                         EXTFS_SUPERBLOCK_SIZE);
            if (status != EXTFS_OK) return status;
        }
        status = classic_zero_ext2_shrink_tail(
            volume, new_size, new_blocks, direct_new, indirect_new, zero_block);
        if (status != EXTFS_OK) return status;
    }

    status = classic_flush(volume);
    if (status != EXTFS_OK) return status;
    classic_store_le16(sort + 0x3AU, original_state);
    classic_store_le32(sort + 0x0CU, (extfs_u32)new_free_blocks);
    status = classic_write_bytes(volume, 1024U, sort, EXTFS_SUPERBLOCK_SIZE);
    if (status != EXTFS_OK) return status;
    status = classic_flush(volume);
    if (status != EXTFS_OK) return status;

    classic_publish_inode(inode, new_size, direct_new, new_root);
    volume->free_blocks = new_free_blocks;
    volume->state = original_state;
    return EXTFS_OK;
}

extfs_status extfs_resize_file_ext3_journaled_direct(extfs_volume *volume,
                                                      extfs_inode *inode,
                                                      extfs_u64 new_size,
                                                      void *scratch,
                                                      extfs_u32 scratch_size)
{
    extfs_u32 old_blocks;
    extfs_u32 new_blocks;
    extfs_status status;
    extfs_u8 *bitmap;
    extfs_u8 *gdt_image;
    extfs_u8 *inode_image;
    extfs_u8 *super_image;
    extfs_u8 *indirect_old;
    extfs_u8 *indirect_new;
    extfs_u8 *journal_scratch;
    extfs_u8 *zero_block;
    extfs_u32 direct_old[CLASSIC_DIRECT_COUNT];
    extfs_u32 direct_new[CLASSIC_DIRECT_COUNT];
    extfs_u32 old_root;
    extfs_u32 new_root;
    extfs_u32 inode_group;
    extfs_u64 inode_offset;
    extfs_u64 inode_home;
    extfs_u32 inode_within;
    extfs_u32 touched_group = 0U;
    extfs_u32 changed = 0U;
    extfs_u64 new_free_blocks;
    extfs_u8 descriptor[64];
    extfs_u16 new_descriptor_free = 0U;
    extfs_u64 descriptor_offset;
    extfs_u64 gdt_home = 0U;
    extfs_u32 descriptor_within = 0U;
    extfs_u64 bitmap_block = 0U;
    extfs_u64 super_home = 0U;
    extfs_u32 super_within = 0U;
    extfs_journal journal;
    extfs_journal_metadata items[5];
    extfs_u32 item_count = 0U;
    int allocation_change = 0;
    int root_change = 0;
    int indirect_changed = 0;
    extfs_u32 i;

    if (volume == 0 || inode == 0 || scratch == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    status = classic_blocks_for_size(volume, inode->size, &old_blocks);
    if (status != EXTFS_OK) return status;
    status = classic_blocks_for_size(volume, new_size, &new_blocks);
    if (status != EXTFS_OK) return status;
    if (old_blocks <= CLASSIC_DIRECT_COUNT &&
        new_blocks <= CLASSIC_DIRECT_COUNT)
        return extfs_resize_file_ext3_journaled_legacy_direct(
            volume, inode, new_size, scratch, scratch_size);

    status = classic_common_preflight(volume, inode, new_size, scratch_size,
                                      CLASSIC_EXT3_SCRATCH_BLOCKS,
                                      &old_blocks, &new_blocks);
    if (status != EXTFS_OK) return status;
    if (volume->kind != EXTFS_KIND_EXT3 ||
        (volume->feature_incompat & ~CLASSIC_INCOMPAT_FILETYPE) != 0U ||
        (volume->feature_ro_compat &
         ~(CLASSIC_RO_SPARSE_SUPER | CLASSIC_RO_LARGE_FILE |
           CLASSIC_RO_BTREE_DIR)) != 0U)
        return EXTFS_ERR_UNSUPPORTED;
    if (new_size == inode->size) return EXTFS_OK;

    bitmap = (extfs_u8 *)scratch;
    gdt_image = bitmap + volume->block_size;
    inode_image = gdt_image + volume->block_size;
    super_image = inode_image + volume->block_size;
    indirect_old = super_image + volume->block_size;
    indirect_new = indirect_old + volume->block_size;
    journal_scratch = indirect_new + volume->block_size;
    zero_block = journal_scratch + volume->block_size;

    status = classic_load_and_validate_mapping(volume, inode, old_blocks,
                                               direct_old, indirect_old,
                                               bitmap, zero_block);
    if (status != EXTFS_OK) return status;
    for (i = 0U; i < CLASSIC_DIRECT_COUNT; ++i) direct_new[i] = direct_old[i];
    classic_copy(indirect_new, indirect_old, volume->block_size);
    old_root = classic_le32(inode->block_map + CLASSIC_SINGLE_INDEX * 4U);
    new_root = old_root;

    status = classic_inode_offset(volume, inode->number, &inode_offset,
                                  &inode_group);
    if (status != EXTFS_OK) return status;
    inode_home = inode_offset / volume->block_size;
    inode_within = (extfs_u32)(inode_offset % volume->block_size);
    if (inode_home >= volume->total_blocks ||
        inode_within > volume->block_size - volume->inode_size)
        return EXTFS_ERR_CORRUPT;
    status = classic_read_block(volume, inode_home, inode_image);
    if (status != EXTFS_OK) return status;
    status = classic_validate_inode_raw(volume, inode,
                                        inode_image + inode_within,
                                        old_blocks);
    if (status != EXTFS_OK) return status;

    status = extfs_journal_open(volume, &journal, journal_scratch,
                                volume->block_size);
    if (status != EXTFS_OK) return status;
    {
        extfs_u32 risks = 0U;
        if (extfs_journal_write_assess(volume, &journal, &risks) != EXTFS_OK)
            return EXTFS_ERR_UNSUPPORTED;
    }

    if (new_blocks > old_blocks) {
        int needs_root = old_blocks <= CLASSIC_DIRECT_COUNT;
        root_change = needs_root;
        allocation_change = 1;
        indirect_changed = 1;
        status = classic_select_growth_group(
            volume, inode_group, old_blocks, new_blocks, needs_root,
            direct_new, indirect_new, &new_root, bitmap, &touched_group);
        if (status != EXTFS_OK) return status;
    } else if (new_blocks < old_blocks) {
        root_change = old_blocks > CLASSIC_DIRECT_COUNT &&
                      new_blocks <= CLASSIC_DIRECT_COUNT;
        allocation_change = 1;
        indirect_changed = new_blocks > CLASSIC_DIRECT_COUNT;
        status = classic_release_group(volume, old_blocks, new_blocks,
                                       direct_old, indirect_old, old_root,
                                       root_change, &touched_group);
        if (status != EXTFS_OK) return status;
        for (i = new_blocks; i < old_blocks; ++i)
            classic_set_map_pointer(direct_new, indirect_new, i, 0U);
        if (root_change != 0) {
            new_root = 0U;
            classic_zero(indirect_new, volume->block_size);
        }
    }

    changed = allocation_change != 0
        ? classic_changed_block_count(old_blocks, new_blocks, root_change) : 0U;
    if (new_blocks > old_blocks) {
        if (volume->free_blocks < changed) return EXTFS_ERR_NO_SPACE;
        new_free_blocks = volume->free_blocks - changed;
    } else {
        if (volume->free_blocks + changed > volume->total_blocks)
            return EXTFS_ERR_CORRUPT;
        new_free_blocks = volume->free_blocks + changed;
    }

    status = classic_update_inode_raw(volume, inode_image + inode_within,
                                      new_size, new_blocks,
                                      direct_new, new_root);
    if (status != EXTFS_OK) return status;

    if (allocation_change != 0) {
        status = classic_apply_group_bitmap(
            volume, touched_group, new_blocks > old_blocks,
            old_blocks, new_blocks, direct_old, indirect_old, old_root,
            direct_new, indirect_new, new_root, root_change,
            bitmap, descriptor, &bitmap_block, &new_descriptor_free);
        if (status != EXTFS_OK) return status;

        status = classic_descriptor_offset(volume, touched_group,
                                           &descriptor_offset);
        if (status != EXTFS_OK) return status;
        gdt_home = descriptor_offset / volume->block_size;
        descriptor_within = (extfs_u32)(descriptor_offset % volume->block_size);
        if (gdt_home >= volume->total_blocks ||
            descriptor_within > volume->block_size - volume->descriptor_size)
            return EXTFS_ERR_CORRUPT;
        status = classic_read_block(volume, gdt_home, gdt_image);
        if (status != EXTFS_OK) return status;
        if (!classic_equal(gdt_image + descriptor_within, descriptor,
                           volume->descriptor_size))
            return EXTFS_ERR_CORRUPT;
        classic_store_le16(descriptor + 0x0CU, new_descriptor_free);
        classic_copy(gdt_image + descriptor_within, descriptor,
                     volume->descriptor_size);

        status = classic_primary_super_location(volume, &super_home,
                                                &super_within);
        if (status != EXTFS_OK) return status;
        status = classic_read_block(volume, super_home, super_image);
        if (status != EXTFS_OK) return status;
        {
            extfs_u8 *sb = super_image + super_within;
            if (classic_le16(sb + 0x38U) != 0xEF53U ||
                !classic_equal(sb + 0x68U, volume->uuid, 16U) ||
                classic_le32(sb + 0x0CU) != (extfs_u32)volume->free_blocks ||
                classic_le32(sb + 0x60U) != volume->feature_incompat ||
                new_free_blocks > 0xFFFFFFFFULL)
                return EXTFS_ERR_CORRUPT;
            classic_store_le32(sb + 0x0CU, (extfs_u32)new_free_blocks);
            classic_store_le32(sb + 0x60U,
                               volume->feature_incompat |
                               CLASSIC_INCOMPAT_RECOVER);
        }

        if (bitmap_block == inode_home || bitmap_block == gdt_home ||
            bitmap_block == super_home || inode_home == gdt_home ||
            inode_home == super_home || gdt_home == super_home ||
            (new_blocks > CLASSIC_DIRECT_COUNT &&
             (new_root == bitmap_block || new_root == gdt_home ||
              new_root == inode_home || new_root == super_home)))
            return EXTFS_ERR_CORRUPT;
    }

    /* Ordered data: newly visible bytes are stable before metadata commit. */
    if (new_size > inode->size) {
        status = classic_zero_growth_data(
            volume, inode, old_blocks, new_blocks, new_size,
            direct_old, indirect_old, direct_new, indirect_new, zero_block);
        if (status != EXTFS_OK) return status;
        status = classic_flush(volume);
        if (status != EXTFS_OK) return status;
    }

    if (allocation_change != 0) {
        items[item_count].home_block = bitmap_block;
        items[item_count++].block_data = bitmap;
        items[item_count].home_block = gdt_home;
        items[item_count++].block_data = gdt_image;
    }
    items[item_count].home_block = inode_home;
    items[item_count++].block_data = inode_image;
    if (indirect_changed != 0 && new_blocks > CLASSIC_DIRECT_COUNT) {
        items[item_count].home_block = new_root;
        items[item_count++].block_data = indirect_new;
    }
    if (allocation_change != 0) {
        items[item_count].home_block = super_home;
        items[item_count++].block_data = super_image;
    }

    status = extfs_journal_commit_metadata(volume, &journal, items, item_count,
                                           journal_scratch,
                                           volume->block_size);
    if (status != EXTFS_OK) return status;

    classic_publish_inode(inode, new_size, direct_new, new_root);
    volume->free_blocks = new_free_blocks;
    return EXTFS_OK;
}
