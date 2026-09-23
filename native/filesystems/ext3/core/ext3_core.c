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



ifs_ext3_u32 ifs_ext3_directory_record_length_from_disk(
    const ifs_ext3_u16 encoded_length,
    const ifs_ext3_u32 maximum_record_length)
{
    if (encoded_length == 0xFFFFU &&
        maximum_record_length >= IFS_EXT3_MAX_DIRECTORY_RECORD_LENGTH)
        return IFS_EXT3_MAX_DIRECTORY_RECORD_LENGTH;
    return encoded_length;
}

int ifs_ext3_directory_record_length_to_disk(
    const ifs_ext3_u32 record_length,
    const ifs_ext3_u32 maximum_record_length,
    ifs_ext3_u16 *const encoded_length)
{
    if (encoded_length == 0)
        return -1;
    if (record_length == 0U ||
        record_length > maximum_record_length ||
        (record_length & 3U) != 0U)
        return -1;

    if (record_length == IFS_EXT3_MAX_DIRECTORY_RECORD_LENGTH &&
        maximum_record_length >= IFS_EXT3_MAX_DIRECTORY_RECORD_LENGTH) {
        *encoded_length = 0xFFFFU;
        return 0;
    }
    if (record_length > 0xFFFFU)
        return -1;

    *encoded_length = (ifs_ext3_u16)record_length;
    return 0;
}

IfsExt3DirectoryRecordStatus ifs_ext3_validate_directory_record(
    const ifs_ext3_u32 record_offset,
    const ifs_ext3_u32 record_length,
    const ifs_ext3_u32 name_length,
    const ifs_ext3_u32 inode_number,
    const ifs_ext3_u32 block_size,
    const ifs_ext3_u32 maximum_inode)
{
    const ifs_ext3_u32 minimum_length = (name_length + 11U) & ~3U;

    if (record_length < 12U)
        return IFS_EXT3_DIRECTORY_RECORD_TOO_SHORT;
    if ((record_length & 3U) != 0U)
        return IFS_EXT3_DIRECTORY_RECORD_UNALIGNED;
    if (record_length < minimum_length)
        return IFS_EXT3_DIRECTORY_RECORD_NAME_TOO_LONG;
    if (block_size == 0U ||
        record_offset >= block_size ||
        record_length > block_size - record_offset)
        return IFS_EXT3_DIRECTORY_RECORD_CROSSES_BLOCK;
    if (inode_number > maximum_inode)
        return IFS_EXT3_DIRECTORY_RECORD_INODE_RANGE;
    return IFS_EXT3_DIRECTORY_RECORD_OK;
}

const char *ifs_ext3_directory_record_status_string(
    const IfsExt3DirectoryRecordStatus status)
{
    switch (status) {
    case IFS_EXT3_DIRECTORY_RECORD_OK: return "ok";
    case IFS_EXT3_DIRECTORY_RECORD_TOO_SHORT: return "rec_len is smaller than minimal";
    case IFS_EXT3_DIRECTORY_RECORD_UNALIGNED: return "rec_len % 4 != 0";
    case IFS_EXT3_DIRECTORY_RECORD_NAME_TOO_LONG: return "rec_len is too small for name_len";
    case IFS_EXT3_DIRECTORY_RECORD_CROSSES_BLOCK: return "directory entry across blocks";
    case IFS_EXT3_DIRECTORY_RECORD_INODE_RANGE: return "inode out of bounds";
    }
    return "invalid EXT3 directory record";
}
