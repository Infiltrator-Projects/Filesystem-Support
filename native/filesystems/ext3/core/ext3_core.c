/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Canonical EXT3 format policy shared by all host adapters.
 */

#include "ext3_core.h"

ifs_ext3_u32 ifs_ext3_unsupported_incompat_features(
    const ifs_ext3_u32 feature_incompat)
{
    return feature_incompat & ~IFS_EXT3_FEATURE_INCOMPAT_SUPPORTED;
}

ifs_ext3_u32 ifs_ext3_unsupported_ro_compat_features(
    const ifs_ext3_u32 feature_ro_compat)
{
    return feature_ro_compat & ~IFS_EXT3_FEATURE_RO_COMPAT_SUPPORTED;
}

IfsExt3GeometryStatus ifs_ext3_validate_geometry(
    const ifs_ext3_u32 block_size,
    const ifs_ext3_u32 inode_size,
    const ifs_ext3_u32 fragment_size,
    const ifs_ext3_u32 blocks_per_group,
    const ifs_ext3_u32 fragments_per_group,
    const ifs_ext3_u32 inodes_per_group)
{
    const ifs_ext3_u32 bitmap_capacity = block_size * 8U;

    if (inode_size < 128U || inode_size > block_size ||
        (inode_size & (inode_size - 1U)) != 0U)
        return IFS_EXT3_GEOMETRY_INVALID_INODE_SIZE;

    if (fragment_size != block_size)
        return IFS_EXT3_GEOMETRY_FRAGMENT_SIZE_MISMATCH;

    if (blocks_per_group == 0U || fragments_per_group == 0U ||
        inodes_per_group == 0U)
        return IFS_EXT3_GEOMETRY_ZERO_GROUP_VALUE;

    if (blocks_per_group > bitmap_capacity)
        return IFS_EXT3_GEOMETRY_BLOCKS_PER_GROUP_TOO_LARGE;

    if (fragments_per_group > bitmap_capacity)
        return IFS_EXT3_GEOMETRY_FRAGMENTS_PER_GROUP_TOO_LARGE;

    if (inodes_per_group > bitmap_capacity)
        return IFS_EXT3_GEOMETRY_INODES_PER_GROUP_TOO_LARGE;

    return IFS_EXT3_GEOMETRY_OK;
}

IfsExt3LayoutStatus ifs_ext3_compute_group_count(
    const ifs_ext3_u32 blocks_count,
    const ifs_ext3_u32 first_data_block,
    const ifs_ext3_u32 blocks_per_group,
    ifs_ext3_u32 *const group_count)
{
    if (group_count == 0)
        return IFS_EXT3_LAYOUT_INVALID_BLOCKS_PER_GROUP;

    if (blocks_per_group == 0U)
        return IFS_EXT3_LAYOUT_INVALID_BLOCKS_PER_GROUP;

    if (first_data_block >= blocks_count)
        return IFS_EXT3_LAYOUT_INVALID_FIRST_DATA_BLOCK;

    *group_count =
        ((blocks_count - first_data_block - 1U) / blocks_per_group) + 1U;
    return IFS_EXT3_LAYOUT_OK;
}

