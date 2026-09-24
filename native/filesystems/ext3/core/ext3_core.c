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

int ifs_ext3_indirect_block_path(
    ifs_ext3_u64 logical_block,
    const ifs_ext3_u32 pointers_per_block,
    const ifs_ext3_u32 pointer_bits,
    ifs_ext3_u32 offsets[4],
    ifs_ext3_u32 *const boundary)
{
    ifs_ext3_u64 remaining = logical_block;
    const ifs_ext3_u64 indirect_blocks = pointers_per_block;
    ifs_ext3_u64 double_blocks;
    ifs_ext3_u64 triple_blocks;
    ifs_ext3_u32 final = 0U;
    int depth = 0;

    if (offsets == 0 || pointers_per_block == 0U ||
        pointer_bits >= 32U ||
        ((ifs_ext3_u64)1U << pointer_bits) != pointers_per_block) {
        if (boundary != 0)
            *boundary = 0U;
        return 0;
    }

    double_blocks = indirect_blocks * indirect_blocks;
    triple_blocks = double_blocks * indirect_blocks;

    if (remaining < IFS_EXT3_NDIR_BLOCKS) {
        offsets[depth++] = (ifs_ext3_u32)remaining;
        final = IFS_EXT3_NDIR_BLOCKS;
    } else {
        remaining -= IFS_EXT3_NDIR_BLOCKS;
        if (remaining < indirect_blocks) {
            offsets[depth++] = IFS_EXT3_IND_BLOCK;
            offsets[depth++] = (ifs_ext3_u32)remaining;
            final = pointers_per_block;
        } else {
            remaining -= indirect_blocks;
            if (remaining < double_blocks) {
                offsets[depth++] = IFS_EXT3_DIND_BLOCK;
                offsets[depth++] = (ifs_ext3_u32)(remaining >> pointer_bits);
                offsets[depth++] = (ifs_ext3_u32)(remaining & (pointers_per_block - 1U));
                final = pointers_per_block;
            } else {
                remaining -= double_blocks;
                if (remaining < triple_blocks) {
                    offsets[depth++] = IFS_EXT3_TIND_BLOCK;
                    offsets[depth++] = (ifs_ext3_u32)(remaining >> (pointer_bits * 2U));
                    offsets[depth++] = (ifs_ext3_u32)((remaining >> pointer_bits) &
                                                      (pointers_per_block - 1U));
                    offsets[depth++] = (ifs_ext3_u32)(remaining & (pointers_per_block - 1U));
                    final = pointers_per_block;
                }
            }
        }
    }

    if (boundary != 0) {
        if (depth == 0)
            *boundary = 0U;
        else
            *boundary = final - 1U -
                ((ifs_ext3_u32)remaining & (pointers_per_block - 1U));
    }
    return depth;
}


IfsExt3JournalStatus ifs_ext3_validate_journal_header(
    const ifs_ext3_u32 magic,
    const ifs_ext3_u32 block_type,
    const ifs_ext3_u32 sequence)
{
    if (magic != IFS_EXT3_JOURNAL_MAGIC)
        return IFS_EXT3_JOURNAL_BAD_MAGIC;

    switch (block_type) {
    case IFS_EXT3_JOURNAL_DESCRIPTOR_BLOCK:
    case IFS_EXT3_JOURNAL_COMMIT_BLOCK:
    case IFS_EXT3_JOURNAL_SUPERBLOCK_V1:
    case IFS_EXT3_JOURNAL_SUPERBLOCK_V2:
    case IFS_EXT3_JOURNAL_REVOKE_BLOCK:
        break;
    default:
        return IFS_EXT3_JOURNAL_BAD_TYPE;
    }

    if (sequence == 0U &&
        block_type != IFS_EXT3_JOURNAL_SUPERBLOCK_V1 &&
        block_type != IFS_EXT3_JOURNAL_SUPERBLOCK_V2)
        return IFS_EXT3_JOURNAL_BAD_ARGUMENT;

    return IFS_EXT3_JOURNAL_OK;
}

IfsExt3JournalStatus ifs_ext3_validate_journal_superblock(
    const ifs_ext3_u32 block_size,
    const ifs_ext3_u32 max_length,
    const ifs_ext3_u32 first_block,
    const ifs_ext3_u32 start_block,
    const ifs_ext3_u32 incompat_features)
{
    if (block_size < 1024U || block_size > 65536U ||
        (block_size & (block_size - 1U)) != 0U)
        return IFS_EXT3_JOURNAL_BAD_BLOCK_SIZE;

    if (max_length < 2U || first_block == 0U ||
        first_block >= max_length ||
        (start_block != 0U &&
         (start_block < first_block || start_block >= max_length)))
        return IFS_EXT3_JOURNAL_BAD_GEOMETRY;

    if ((incompat_features &
         ~IFS_EXT3_JOURNAL_FEATURE_INCOMPAT_REVOKE) != 0U)
        return IFS_EXT3_JOURNAL_UNSUPPORTED_FEATURE;

    return IFS_EXT3_JOURNAL_OK;
}

