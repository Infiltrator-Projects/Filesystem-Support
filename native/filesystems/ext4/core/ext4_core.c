/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Canonical EXT4 format policy shared by all host adapters.
 */

#include "ext4_core.h"

ifs_ext4_u32 ifs_ext4_unsupported_incompat_features(
    const ifs_ext4_u32 feature_incompat)
{
    return feature_incompat & ~IFS_EXT4_FEATURE_INCOMPAT_SUPPORTED;
}

ifs_ext4_u32 ifs_ext4_unsupported_ro_compat_features(
    const ifs_ext4_u32 feature_ro_compat)
{
    return feature_ro_compat & ~IFS_EXT4_FEATURE_RO_COMPAT_SUPPORTED &
           ~IFS_EXT4_FEATURE_RO_COMPAT_READONLY;
}

int ifs_ext4_requires_readonly(const ifs_ext4_u32 feature_ro_compat)
{
    return (feature_ro_compat & IFS_EXT4_FEATURE_RO_COMPAT_READONLY) != 0U;
}

IfsExt4BigallocStatus ifs_ext4_validate_bigalloc(
    const ifs_ext4_u32 feature_incompat,
    const ifs_ext4_u32 feature_ro_compat,
    const ifs_ext4_u32 first_data_block)
{
    if ((feature_ro_compat & IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC) == 0U)
        return IFS_EXT4_BIGALLOC_OK;

    if ((feature_incompat & IFS_EXT4_FEATURE_INCOMPAT_EXTENTS) == 0U)
        return IFS_EXT4_BIGALLOC_REQUIRES_EXTENTS;

    if (first_data_block != 0U)
        return IFS_EXT4_BIGALLOC_INVALID_FIRST_DATA_BLOCK;

    return IFS_EXT4_BIGALLOC_OK;
}

IfsExt4InodeGeometryStatus ifs_ext4_validate_inode_geometry(
    const ifs_ext4_u32 block_size,
    const ifs_ext4_u32 inode_size,
    const ifs_ext4_u32 first_inode)
{
    if (first_inode < 11U)
        return IFS_EXT4_INODE_GEOMETRY_INVALID_FIRST_INODE;

    if (inode_size < 128U || inode_size > block_size ||
        (inode_size & (inode_size - 1U)) != 0U)
        return IFS_EXT4_INODE_GEOMETRY_INVALID_INODE_SIZE;

    return IFS_EXT4_INODE_GEOMETRY_OK;
}

IfsExt4GroupGeometryStatus ifs_ext4_validate_group_geometry(
    const ifs_ext4_u32 block_size,
    const ifs_ext4_u32 inode_size,
    const ifs_ext4_u32 descriptor_size,
    const int has_64bit,
    const ifs_ext4_u32 blocks_per_group,
    const ifs_ext4_u32 inodes_per_group)
{
    const ifs_ext4_u32 inodes_per_block =
        inode_size != 0U ? block_size / inode_size : 0U;

    if (has_64bit &&
        (descriptor_size < 64U || descriptor_size > 1024U ||
         (descriptor_size & (descriptor_size - 1U)) != 0U))
        return IFS_EXT4_GROUP_GEOMETRY_INVALID_DESCRIPTOR_SIZE;

    if (inodes_per_block == 0U || blocks_per_group == 0U)
        return IFS_EXT4_GROUP_GEOMETRY_ZERO_VALUE;

    if (inodes_per_group < inodes_per_block ||
        inodes_per_group > block_size * 8U)
        return IFS_EXT4_GROUP_GEOMETRY_INVALID_INODES_PER_GROUP;

    return IFS_EXT4_GROUP_GEOMETRY_OK;
}

IfsExt4ClusterGeometryStatus ifs_ext4_validate_cluster_geometry(
    const ifs_ext4_u32 block_size,
    const ifs_ext4_u32 cluster_size,
    const int has_bigalloc,
    const ifs_ext4_u32 blocks_per_group,
    const ifs_ext4_u32 clusters_per_group)
{
    if (block_size == 0U || cluster_size == 0U)
        return IFS_EXT4_CLUSTER_GEOMETRY_GROUP_RATIO_MISMATCH;

    if (has_bigalloc) {
        if (cluster_size < block_size)
            return IFS_EXT4_CLUSTER_GEOMETRY_CLUSTER_SMALLER_THAN_BLOCK;
    } else {
        if (cluster_size != block_size)
            return IFS_EXT4_CLUSTER_GEOMETRY_CLUSTER_BLOCK_MISMATCH;
        if (blocks_per_group > block_size * 8U)
            return IFS_EXT4_CLUSTER_GEOMETRY_BLOCKS_PER_GROUP_TOO_LARGE;
    }

    if (clusters_per_group > block_size * 8U)
        return IFS_EXT4_CLUSTER_GEOMETRY_CLUSTERS_PER_GROUP_TOO_LARGE;

    if ((ifs_ext4_u64)blocks_per_group !=
        (ifs_ext4_u64)clusters_per_group *
            ((ifs_ext4_u64)cluster_size / block_size))
        return IFS_EXT4_CLUSTER_GEOMETRY_GROUP_RATIO_MISMATCH;

    return IFS_EXT4_CLUSTER_GEOMETRY_OK;
}

IfsExt4LayoutStatus ifs_ext4_validate_layout(
    const ifs_ext4_u32 block_size,
    const ifs_ext4_u32 reserved_gdt_blocks,
    const ifs_ext4_u64 blocks_count,
    const ifs_ext4_u32 first_data_block,
    const ifs_ext4_u32 log_block_size,
    const ifs_ext4_u32 cluster_ratio,
    const ifs_ext4_u32 blocks_per_group,
    const ifs_ext4_u32 descriptors_per_block,
    const ifs_ext4_u32 inodes_per_group,
    const ifs_ext4_u32 inodes_count,
    ifs_ext4_u64 *const group_count)
{
    if (reserved_gdt_blocks > block_size / 4U)
        return IFS_EXT4_LAYOUT_RESERVED_GDT_TOO_LARGE;

    if (blocks_per_group == 0U || group_count == 0)
        return IFS_EXT4_LAYOUT_GROUP_COUNT_TOO_LARGE;

    if ((ifs_ext4_u64)first_data_block >= blocks_count)
        return IFS_EXT4_LAYOUT_INVALID_FIRST_DATA_BLOCK;

    if (first_data_block == 0U && log_block_size == 0U &&
        cluster_ratio == 1U)
        return IFS_EXT4_LAYOUT_INVALID_1K_FIRST_DATA_BLOCK;

    {
        const ifs_ext4_u64 groups =
            (blocks_count - first_data_block + blocks_per_group - 1U) /
            blocks_per_group;
        const ifs_ext4_u64 max_groups =
            0x100000000ULL - descriptors_per_block;

        *group_count = groups;

        if (groups > max_groups)
            return IFS_EXT4_LAYOUT_GROUP_COUNT_TOO_LARGE;

        if (groups * inodes_per_group != inodes_count)
            return IFS_EXT4_LAYOUT_INVALID_INODE_COUNT;
    }

    return IFS_EXT4_LAYOUT_OK;
}

