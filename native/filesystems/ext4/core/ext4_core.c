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



ifs_ext4_u32 ifs_ext4_directory_record_min_length(
    const ifs_ext4_u32 name_length,
    const int has_hash)
{
    ifs_ext4_u32 length = name_length + 11U;

    if (has_hash != 0)
        length += 8U;
    return length & ~3U;
}

ifs_ext4_u32 ifs_ext4_directory_record_length_from_disk(
    const ifs_ext4_u16 encoded_length,
    const ifs_ext4_u32 block_size)
{
    const ifs_ext4_u32 length = encoded_length;

    if (block_size >= 65536U) {
        if (length == 0xFFFFU || length == 0U)
            return block_size;
        return (length & 65532U) | ((length & 3U) << 16);
    }
    return length;
}

int ifs_ext4_directory_record_length_to_disk(
    const ifs_ext4_u32 record_length,
    const ifs_ext4_u32 block_size,
    ifs_ext4_u16 *const encoded_length)
{
    if (encoded_length == 0)
        return -1;
    if (record_length == 0U || record_length > block_size ||
        block_size > (1U << 18) || (record_length & 3U) != 0U)
        return -1;

    if (block_size >= 65536U) {
        if (record_length < 65536U) {
            *encoded_length = (ifs_ext4_u16)record_length;
            return 0;
        }
        if (record_length == block_size) {
            *encoded_length = block_size == 65536U ? 0xFFFFU : 0U;
            return 0;
        }
        *encoded_length = (ifs_ext4_u16)(
            (record_length & 65532U) | ((record_length >> 16) & 3U));
        return 0;
    }

    if (record_length > 0xFFFFU)
        return -1;
    *encoded_length = (ifs_ext4_u16)record_length;
    return 0;
}

IfsExt4DirectoryRecordStatus ifs_ext4_validate_directory_record(
    const ifs_ext4_u32 record_offset,
    const ifs_ext4_u32 record_length,
    const ifs_ext4_u32 name_length,
    const ifs_ext4_u32 inode_number,
    const ifs_ext4_u32 buffer_size,
    const ifs_ext4_u32 maximum_inode,
    const int entry_has_hash,
    const int trailing_entry_has_hash,
    const int dot_entry)
{
    const ifs_ext4_u32 minimum =
        ifs_ext4_directory_record_min_length(1U, entry_has_hash);
    const ifs_ext4_u32 minimum_for_name =
        ifs_ext4_directory_record_min_length(name_length, entry_has_hash);
    const ifs_ext4_u32 trailing_minimum =
        ifs_ext4_directory_record_min_length(1U, trailing_entry_has_hash);
    ifs_ext4_u64 next_offset;

    if (record_length < minimum)
        return IFS_EXT4_DIRECTORY_RECORD_TOO_SHORT;
    if ((record_length & 3U) != 0U)
        return IFS_EXT4_DIRECTORY_RECORD_UNALIGNED;
    if (record_length < minimum_for_name)
        return IFS_EXT4_DIRECTORY_RECORD_NAME_TOO_LONG;

    next_offset = (ifs_ext4_u64)record_offset + record_length;
    if (next_offset > buffer_size)
        return IFS_EXT4_DIRECTORY_RECORD_OVERRUN;
    if (next_offset != buffer_size &&
        (ifs_ext4_u64)buffer_size - next_offset < trailing_minimum)
        return IFS_EXT4_DIRECTORY_RECORD_TOO_CLOSE_TO_END;
    if (inode_number > maximum_inode)
        return IFS_EXT4_DIRECTORY_RECORD_INODE_RANGE;
    if (next_offset == buffer_size && dot_entry != 0)
        return IFS_EXT4_DIRECTORY_RECORD_DOT_LAST;
    return IFS_EXT4_DIRECTORY_RECORD_OK;
}

const char *ifs_ext4_directory_record_status_string(
    const IfsExt4DirectoryRecordStatus status)
{
    switch (status) {
    case IFS_EXT4_DIRECTORY_RECORD_OK: return "ok";
    case IFS_EXT4_DIRECTORY_RECORD_TOO_SHORT: return "rec_len is smaller than minimal";
    case IFS_EXT4_DIRECTORY_RECORD_UNALIGNED: return "rec_len % 4 != 0";
    case IFS_EXT4_DIRECTORY_RECORD_NAME_TOO_LONG: return "rec_len is too small for name_len";
    case IFS_EXT4_DIRECTORY_RECORD_OVERRUN: return "directory entry overrun";
    case IFS_EXT4_DIRECTORY_RECORD_TOO_CLOSE_TO_END: return "directory entry too close to block end";
    case IFS_EXT4_DIRECTORY_RECORD_INODE_RANGE: return "inode out of bounds";
    case IFS_EXT4_DIRECTORY_RECORD_DOT_LAST: return "'.' directory cannot be the last in data block";
    }
    return "invalid EXT4 directory record";
}

int ifs_ext4_indirect_block_path(
    ifs_ext4_u64 logical_block,
    const ifs_ext4_u32 pointers_per_block,
    const ifs_ext4_u32 pointer_bits,
    ifs_ext4_u32 offsets[4],
    ifs_ext4_u32 *const boundary)
{
    ifs_ext4_u64 remaining = logical_block;
    const ifs_ext4_u64 indirect_blocks = pointers_per_block;
    ifs_ext4_u64 double_blocks;
    ifs_ext4_u64 triple_blocks;
    ifs_ext4_u32 final = 0U;
    int depth = 0;

    if (offsets == 0 || pointers_per_block == 0U ||
        pointer_bits >= 32U ||
        ((ifs_ext4_u64)1U << pointer_bits) != pointers_per_block) {
        if (boundary != 0)
            *boundary = 0U;
        return 0;
    }

    double_blocks = indirect_blocks * indirect_blocks;
    triple_blocks = double_blocks * indirect_blocks;

    if (remaining < IFS_EXT4_NDIR_BLOCKS) {
        offsets[depth++] = (ifs_ext4_u32)remaining;
        final = IFS_EXT4_NDIR_BLOCKS;
    } else {
        remaining -= IFS_EXT4_NDIR_BLOCKS;
        if (remaining < indirect_blocks) {
            offsets[depth++] = IFS_EXT4_IND_BLOCK;
            offsets[depth++] = (ifs_ext4_u32)remaining;
            final = pointers_per_block;
        } else {
            remaining -= indirect_blocks;
            if (remaining < double_blocks) {
                offsets[depth++] = IFS_EXT4_DIND_BLOCK;
                offsets[depth++] = (ifs_ext4_u32)(remaining >> pointer_bits);
                offsets[depth++] = (ifs_ext4_u32)(remaining & (pointers_per_block - 1U));
                final = pointers_per_block;
            } else {
                remaining -= double_blocks;
                if (remaining < triple_blocks) {
                    offsets[depth++] = IFS_EXT4_TIND_BLOCK;
                    offsets[depth++] = (ifs_ext4_u32)(remaining >> (pointer_bits * 2U));
                    offsets[depth++] = (ifs_ext4_u32)((remaining >> pointer_bits) &
                                                      (pointers_per_block - 1U));
                    offsets[depth++] = (ifs_ext4_u32)(remaining & (pointers_per_block - 1U));
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
                ((ifs_ext4_u32)remaining & (pointers_per_block - 1U));
    }
    return depth;
}
