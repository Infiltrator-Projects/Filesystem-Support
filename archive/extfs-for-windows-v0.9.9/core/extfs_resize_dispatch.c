// SPDX-License-Identifier: GPL-3.0-or-later
#include "extfs/extfs.h"

/* Internal implementations supplied by core/extfs_classic_resize.c. */
extfs_status extfs_resize_file_ext2_single_indirect_impl(
    extfs_volume *volume,
    extfs_inode *inode,
    extfs_u64 new_size,
    void *scratch,
    extfs_u32 scratch_size);
extfs_status extfs_resize_file_ext3_single_indirect_impl(
    extfs_volume *volume,
    extfs_inode *inode,
    extfs_u64 new_size,
    void *scratch,
    extfs_u32 scratch_size);

static extfs_u64 dispatch_round_up(extfs_u64 value, extfs_u32 divisor)
{
    return value / divisor + ((value % divisor) != 0U ? 1U : 0U);
}

static void dispatch_zero(extfs_u8 *buffer, extfs_u32 count)
{
    while (count != 0U) {
        *buffer++ = 0U;
        --count;
    }
}

/*
 * The bounded ext2 implementation handles allocation-changing operations with
 * the dirty-superblock fence used by the legacy direct writer. For a growth
 * that remains inside an already allocated single-indirect data block, zero
 * the newly exposed bytes and make them durable before the implementation can
 * publish the larger i_size. This preserves the ordered-data invariant even
 * though no allocation metadata changes in that special case.
 */
static extfs_status dispatch_ext2_pre_zero_same_block_growth(
    extfs_volume *volume,
    extfs_inode *inode,
    extfs_u64 new_size,
    void *scratch,
    extfs_u32 scratch_size)
{
    extfs_u64 old_blocks;
    extfs_u64 new_blocks;
    extfs_u64 logical;
    extfs_u64 physical;
    extfs_u64 physical_offset;
    extfs_u32 within;
    extfs_u32 count;
    int hole = 0;
    extfs_status status;

    if (volume == 0 || inode == 0 || scratch == 0 ||
        volume->block_size == 0U || scratch_size < volume->block_size)
        return EXTFS_ERR_INVALID_ARGUMENT;
    if (new_size <= inode->size) return EXTFS_OK;

    old_blocks = dispatch_round_up(inode->size, volume->block_size);
    new_blocks = dispatch_round_up(new_size, volume->block_size);
    if (old_blocks != new_blocks || old_blocks <= 12U)
        return EXTFS_OK;
    if (volume->kind != EXTFS_KIND_EXT2 ||
        extfs_inode_write_assess(volume, inode) != EXTFS_OK)
        return EXTFS_OK; /* implementation performs the fail-closed rejection */
    if (volume->io.write_at == 0 || volume->io.flush == 0)
        return EXTFS_ERR_UNSUPPORTED;

    logical = old_blocks - 1U;
    if (logical > 0xFFFFFFFFULL) return EXTFS_ERR_RANGE;
    status = extfs_map_file_block(volume, inode, (extfs_u32)logical,
                                  &physical, &hole, scratch, scratch_size);
    if (status != EXTFS_OK) return status;
    if (hole != 0 || physical >= volume->total_blocks)
        return EXTFS_ERR_CORRUPT;
    if (physical > 0xFFFFFFFFFFFFFFFFULL / volume->block_size)
        return EXTFS_ERR_RANGE;
    physical_offset = physical * volume->block_size;
    within = (extfs_u32)(inode->size % volume->block_size);
    count = (extfs_u32)(new_size - inode->size);
    if (physical_offset > volume->byte_size ||
        within > volume->byte_size - physical_offset ||
        count > volume->byte_size - (physical_offset + within))
        return EXTFS_ERR_RANGE;

    dispatch_zero((extfs_u8 *)scratch, volume->block_size);
    if (volume->io.write_at(volume->io.user, physical_offset + within,
                            scratch, count) != 0)
        return EXTFS_ERR_IO;
    return volume->io.flush(volume->io.user) == 0 ? EXTFS_OK : EXTFS_ERR_IO;
}

extfs_status extfs_resize_file_ext2_direct(extfs_volume *volume,
                                           extfs_inode *inode,
                                           extfs_u64 new_size,
                                           void *scratch,
                                           extfs_u32 scratch_size)
{
    extfs_status status = dispatch_ext2_pre_zero_same_block_growth(
        volume, inode, new_size, scratch, scratch_size);
    if (status != EXTFS_OK) return status;
    return extfs_resize_file_ext2_single_indirect_impl(
        volume, inode, new_size, scratch, scratch_size);
}

extfs_status extfs_resize_file_ext3_journaled_direct(extfs_volume *volume,
                                                      extfs_inode *inode,
                                                      extfs_u64 new_size,
                                                      void *scratch,
                                                      extfs_u32 scratch_size)
{
    return extfs_resize_file_ext3_single_indirect_impl(
        volume, inode, new_size, scratch, scratch_size);
}
