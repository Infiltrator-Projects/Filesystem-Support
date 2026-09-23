/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ext2_core.h"

static ifs_ext2_u16 load_le16(const ifs_ext2_u8 *p)
{
    return (ifs_ext2_u16)((ifs_ext2_u16)p[0] |
                          ((ifs_ext2_u16)p[1] << 8));
}

static ifs_ext2_u32 load_le32(const ifs_ext2_u8 *p)
{
    return (ifs_ext2_u32)p[0] |
           ((ifs_ext2_u32)p[1] << 8) |
           ((ifs_ext2_u32)p[2] << 16) |
           ((ifs_ext2_u32)p[3] << 24);
}

static int is_power_of_two(const ifs_ext2_u32 value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

IfsExt2Status ifs_ext2_decode_superblock(
    const void *raw_superblock,
    const ifs_ext2_size_t raw_size,
    IfsExt2Superblock *superblock)
{
    const ifs_ext2_u8 *raw = (const ifs_ext2_u8 *)raw_superblock;

    if (raw == IFS_EXT2_NULL || superblock == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (raw_size < IFS_EXT2_SUPERBLOCK_SIZE)
        return IFS_EXT2_ERROR_RANGE;
    if (load_le16(raw + 0x38U) != IFS_EXT2_SUPER_MAGIC)
        return IFS_EXT2_ERROR_MAGIC;

    superblock->inodes_count = load_le32(raw + 0x00U);
    superblock->blocks_count = load_le32(raw + 0x04U);
    superblock->reserved_blocks_count = load_le32(raw + 0x08U);
    superblock->free_blocks_count = load_le32(raw + 0x0CU);
    superblock->free_inodes_count = load_le32(raw + 0x10U);
    superblock->first_data_block = load_le32(raw + 0x14U);
    superblock->log_block_size = load_le32(raw + 0x18U);
    superblock->log_fragment_size = load_le32(raw + 0x1CU);
    superblock->blocks_per_group = load_le32(raw + 0x20U);
    superblock->fragments_per_group = load_le32(raw + 0x24U);
    superblock->inodes_per_group = load_le32(raw + 0x28U);
    superblock->state = load_le16(raw + 0x3AU);
    superblock->errors = load_le16(raw + 0x3CU);
    superblock->revision = load_le32(raw + 0x4CU);
    superblock->first_inode = load_le32(raw + 0x54U);
    superblock->inode_size = load_le16(raw + 0x58U);
    superblock->feature_compat = load_le32(raw + 0x5CU);
    superblock->feature_incompat = load_le32(raw + 0x60U);
    superblock->feature_ro_compat = load_le32(raw + 0x64U);
    superblock->first_meta_bg = load_le32(raw + 0x104U);
    superblock->block_size = 0U;
    superblock->inodes_per_block = 0U;
    superblock->inode_table_blocks_per_group = 0U;
    superblock->group_count = 0U;
    return IFS_EXT2_OK;
}

IfsExt2Status ifs_ext2_validate_superblock(
    IfsExt2Superblock *superblock,
    const ifs_ext2_u64 device_blocks,
    const int writable)
{
    ifs_ext2_u64 data_blocks;
    ifs_ext2_u64 groups;
    ifs_ext2_u64 computed_inodes;
    ifs_ext2_u32 inode_size;

    if (superblock == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (superblock->revision > IFS_EXT2_MAX_REVISION)
        return IFS_EXT2_ERROR_REVISION;
    if ((superblock->feature_compat &
         IFS_EXT2_FEATURE_COMPAT_HAS_JOURNAL) != 0U)
        return IFS_EXT2_ERROR_JOURNALLED;
    if ((superblock->feature_incompat &
         ~IFS_EXT2_FEATURE_INCOMPAT_SUPPORTED) != 0U)
        return IFS_EXT2_ERROR_FEATURES;
    if (writable != 0 &&
        (superblock->feature_ro_compat &
         ~IFS_EXT2_FEATURE_RO_COMPAT_SUPPORTED) != 0U)
        return IFS_EXT2_ERROR_FEATURES;

    if (superblock->log_block_size > 6U)
        return IFS_EXT2_ERROR_BLOCK_SIZE;
    superblock->block_size =
        IFS_EXT2_MIN_BLOCK_SIZE << superblock->log_block_size;
    if (superblock->block_size < IFS_EXT2_MIN_BLOCK_SIZE ||
        superblock->block_size > IFS_EXT2_MAX_BLOCK_SIZE)
        return IFS_EXT2_ERROR_BLOCK_SIZE;
    if (superblock->log_fragment_size != superblock->log_block_size)
        return IFS_EXT2_ERROR_FRAGMENT_SIZE;

    if ((superblock->block_size == 1024U &&
         superblock->first_data_block != 1U) ||
        (superblock->block_size != 1024U &&
         superblock->first_data_block != 0U))
        return IFS_EXT2_ERROR_GEOMETRY;
    if (superblock->blocks_count <= superblock->first_data_block ||
        superblock->inodes_count == 0U ||
        superblock->blocks_per_group == 0U ||
        superblock->inodes_per_group == 0U)
        return IFS_EXT2_ERROR_GEOMETRY;
    if (superblock->blocks_per_group >
        superblock->block_size * 8U)
        return IFS_EXT2_ERROR_GEOMETRY;
    if (superblock->inodes_per_group >
        superblock->block_size * 8U)
        return IFS_EXT2_ERROR_GEOMETRY;

    inode_size = superblock->revision == 0U
        ? IFS_EXT2_GOOD_OLD_INODE_SIZE
        : superblock->inode_size;
    if (inode_size < IFS_EXT2_GOOD_OLD_INODE_SIZE ||
        inode_size > superblock->block_size ||
        !is_power_of_two(inode_size))
        return IFS_EXT2_ERROR_INODE_SIZE;
    superblock->inode_size = (ifs_ext2_u16)inode_size;
    if (superblock->revision == 0U)
        superblock->first_inode = IFS_EXT2_GOOD_OLD_FIRST_INO;

    superblock->inodes_per_block = superblock->block_size / inode_size;
    if (superblock->inodes_per_block == 0U ||
        superblock->inodes_per_group < superblock->inodes_per_block)
        return IFS_EXT2_ERROR_GEOMETRY;
    superblock->inode_table_blocks_per_group =
        superblock->inodes_per_group / superblock->inodes_per_block;
    if (superblock->inode_table_blocks_per_group == 0U ||
        superblock->blocks_per_group <=
        superblock->inode_table_blocks_per_group + 3U)
        return IFS_EXT2_ERROR_GEOMETRY;

    data_blocks = (ifs_ext2_u64)superblock->blocks_count -
                  superblock->first_data_block;
    groups = (data_blocks + superblock->blocks_per_group - 1U) /
             superblock->blocks_per_group;
    if (groups == 0U || groups > 0xFFFFFFFFULL)
        return IFS_EXT2_ERROR_GEOMETRY;
    superblock->group_count = (ifs_ext2_u32)groups;

    computed_inodes = groups * superblock->inodes_per_group;
    if (computed_inodes != superblock->inodes_count)
        return IFS_EXT2_ERROR_GEOMETRY;
    if (superblock->free_blocks_count > superblock->blocks_count ||
        superblock->free_inodes_count > superblock->inodes_count ||
        superblock->reserved_blocks_count > superblock->blocks_count)
        return IFS_EXT2_ERROR_CORRUPT;
    if (device_blocks != 0U &&
        (ifs_ext2_u64)superblock->blocks_count > device_blocks)
        return IFS_EXT2_ERROR_GEOMETRY;

    return IFS_EXT2_OK;
}

IfsExt2Status ifs_ext2_decode_group_descriptor(
    const void *raw_descriptor,
    const ifs_ext2_size_t raw_size,
    IfsExt2GroupDescriptor *descriptor)
{
    const ifs_ext2_u8 *raw = (const ifs_ext2_u8 *)raw_descriptor;

    if (raw == IFS_EXT2_NULL || descriptor == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (raw_size < 32U)
        return IFS_EXT2_ERROR_RANGE;

    descriptor->block_bitmap = load_le32(raw + 0x00U);
    descriptor->inode_bitmap = load_le32(raw + 0x04U);
    descriptor->inode_table = load_le32(raw + 0x08U);
    descriptor->free_blocks_count = load_le16(raw + 0x0CU);
    descriptor->free_inodes_count = load_le16(raw + 0x0EU);
    descriptor->used_dirs_count = load_le16(raw + 0x10U);
    return IFS_EXT2_OK;
}

IfsExt2Status ifs_ext2_group_bounds(
    const IfsExt2Superblock *superblock,
    const ifs_ext2_u32 group,
    ifs_ext2_u64 *first_block,
    ifs_ext2_u64 *last_block)
{
    ifs_ext2_u64 first;
    ifs_ext2_u64 last;

    if (superblock == IFS_EXT2_NULL || first_block == IFS_EXT2_NULL || last_block == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (group >= superblock->group_count ||
        superblock->blocks_per_group == 0U)
        return IFS_EXT2_ERROR_RANGE;

    first = (ifs_ext2_u64)superblock->first_data_block +
            (ifs_ext2_u64)group * superblock->blocks_per_group;
    if (first >= superblock->blocks_count)
        return IFS_EXT2_ERROR_CORRUPT;

    last = first + superblock->blocks_per_group - 1U;
    if (last >= superblock->blocks_count)
        last = (ifs_ext2_u64)superblock->blocks_count - 1U;

    *first_block = first;
    *last_block = last;
    return IFS_EXT2_OK;
}

IfsExt2Status ifs_ext2_validate_group_descriptor(
    const IfsExt2Superblock *superblock,
    const ifs_ext2_u32 group,
    const IfsExt2GroupDescriptor *descriptor)
{
    ifs_ext2_u64 first;
    ifs_ext2_u64 last;
    IfsExt2Status status;

    if (descriptor == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;

    status = ifs_ext2_group_bounds(superblock, group, &first, &last);
    if (status != IFS_EXT2_OK)
        return status;

    if ((ifs_ext2_u64)descriptor->block_bitmap < first ||
        (ifs_ext2_u64)descriptor->block_bitmap > last ||
        (ifs_ext2_u64)descriptor->inode_bitmap < first ||
        (ifs_ext2_u64)descriptor->inode_bitmap > last ||
        (ifs_ext2_u64)descriptor->inode_table < first ||
        (ifs_ext2_u64)descriptor->inode_table > last)
        return IFS_EXT2_ERROR_CORRUPT;

    if (superblock->inode_table_blocks_per_group == 0U ||
        (ifs_ext2_u64)(superblock->inode_table_blocks_per_group - 1U) >
        last - descriptor->inode_table)
        return IFS_EXT2_ERROR_CORRUPT;

    return IFS_EXT2_OK;
}

IfsExt2Status ifs_ext2_block_to_path(
    const ifs_ext2_u32 block_size,
    ifs_ext2_u64 logical_block,
    IfsExt2BlockPath *path)
{
    ifs_ext2_u64 ptrs;
    ifs_ext2_u64 single;
    ifs_ext2_u64 double_capacity;
    ifs_ext2_u64 triple_capacity;
    ifs_ext2_u64 relative;
    ifs_ext2_u64 final_capacity;

    if (path == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (block_size < IFS_EXT2_MIN_BLOCK_SIZE ||
        block_size > IFS_EXT2_MAX_BLOCK_SIZE ||
        (block_size & (block_size - 1U)) != 0U ||
        (block_size & 3U) != 0U)
        return IFS_EXT2_ERROR_BLOCK_SIZE;

    path->offsets[0] = 0U;
    path->offsets[1] = 0U;
    path->offsets[2] = 0U;
    path->offsets[3] = 0U;
    path->depth = 0U;
    path->boundary = 0U;

    ptrs = block_size / 4U;
    single = ptrs;
    double_capacity = ptrs * ptrs;
    triple_capacity = double_capacity * ptrs;

    if (logical_block < IFS_EXT2_NDIR_BLOCKS) {
        path->offsets[0] = (ifs_ext2_u32)logical_block;
        path->depth = 1U;
        final_capacity = IFS_EXT2_NDIR_BLOCKS;
        relative = logical_block;
    } else {
        relative = logical_block - IFS_EXT2_NDIR_BLOCKS;
        if (relative < single) {
            path->offsets[0] = IFS_EXT2_IND_BLOCK;
            path->offsets[1] = (ifs_ext2_u32)relative;
            path->depth = 2U;
            final_capacity = ptrs;
        } else {
            relative -= single;
            if (relative < double_capacity) {
                path->offsets[0] = IFS_EXT2_DIND_BLOCK;
                path->offsets[1] = (ifs_ext2_u32)(relative / ptrs);
                path->offsets[2] = (ifs_ext2_u32)(relative % ptrs);
                path->depth = 3U;
                final_capacity = ptrs;
            } else {
                relative -= double_capacity;
                if (relative >= triple_capacity)
                    return IFS_EXT2_ERROR_RANGE;
                path->offsets[0] = IFS_EXT2_TIND_BLOCK;
                path->offsets[1] =
                    (ifs_ext2_u32)(relative / double_capacity);
                path->offsets[2] =
                    (ifs_ext2_u32)((relative / ptrs) % ptrs);
                path->offsets[3] = (ifs_ext2_u32)(relative % ptrs);
                path->depth = 4U;
                final_capacity = ptrs;
            }
        }
    }

    path->boundary =
        (ifs_ext2_u32)(final_capacity - 1U - (relative % ptrs));
    return IFS_EXT2_OK;
}

ifs_ext2_u32 ifs_ext2_directory_record_required_length(
    const ifs_ext2_u32 name_length)
{
    if (name_length > 255U)
        return 0U;

    return (name_length + 8U + 3U) & ~3U;
}

int ifs_ext2_directory_record_can_insert(
    const ifs_ext2_u32 record_length,
    const ifs_ext2_u32 existing_name_length,
    const ifs_ext2_u32 existing_inode_number,
    const ifs_ext2_u32 requested_name_length,
    ifs_ext2_u32 *const occupied_length)
{
    const ifs_ext2_u32 requested =
        ifs_ext2_directory_record_required_length(requested_name_length);
    const ifs_ext2_u32 occupied =
        ifs_ext2_directory_record_required_length(existing_name_length);

    if (occupied_length == IFS_EXT2_NULL || requested == 0U)
        return 0;

    *occupied_length = existing_inode_number == 0U ? 0U : occupied;

    if (existing_inode_number == 0U)
        return record_length >= requested;

    if (occupied == 0U || occupied > record_length)
        return 0;

    return record_length - occupied >= requested;
}

IfsExt2Status ifs_ext2_directory_initial_layout(
    const ifs_ext2_u32 block_size,
    ifs_ext2_u32 *const dot_record_length,
    ifs_ext2_u32 *const dotdot_record_length)
{
    const ifs_ext2_u32 dot_length =
        ifs_ext2_directory_record_required_length(1U);
    const ifs_ext2_u32 dotdot_minimum =
        ifs_ext2_directory_record_required_length(2U);

    if (dot_record_length == IFS_EXT2_NULL ||
        dotdot_record_length == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (block_size < IFS_EXT2_MIN_BLOCK_SIZE ||
        block_size > IFS_EXT2_MAX_BLOCK_SIZE ||
        (block_size & (block_size - 1U)) != 0U)
        return IFS_EXT2_ERROR_BLOCK_SIZE;
    if (dot_length == 0U || dotdot_minimum == 0U ||
        block_size - dot_length < dotdot_minimum)
        return IFS_EXT2_ERROR_GEOMETRY;

    *dot_record_length = dot_length;
    *dotdot_record_length = block_size - dot_length;
    return IFS_EXT2_OK;
}

ifs_ext2_u32 ifs_ext2_directory_record_length_from_disk(
    const ifs_ext2_u16 encoded_length,
    const ifs_ext2_u32 maximum_record_length)
{
    if (encoded_length == 0xFFFFU &&
        maximum_record_length >= IFS_EXT2_MAX_BLOCK_SIZE)
        return IFS_EXT2_MAX_BLOCK_SIZE;
    return encoded_length;
}

IfsExt2Status ifs_ext2_directory_record_length_to_disk(
    const ifs_ext2_u32 record_length,
    const ifs_ext2_u32 maximum_record_length,
    ifs_ext2_u16 *encoded_length)
{
    if (encoded_length == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (record_length == IFS_EXT2_MAX_BLOCK_SIZE &&
        maximum_record_length >= IFS_EXT2_MAX_BLOCK_SIZE) {
        *encoded_length = 0xFFFFU;
        return IFS_EXT2_OK;
    }
    if (record_length > 0xFFFFU)
        return IFS_EXT2_ERROR_RANGE;
    *encoded_length = (ifs_ext2_u16)record_length;
    return IFS_EXT2_OK;
}

IfsExt2DirectoryRecordStatus ifs_ext2_validate_directory_record(
    const ifs_ext2_u32 record_offset,
    const ifs_ext2_u32 record_length,
    const ifs_ext2_u32 name_length,
    const ifs_ext2_u32 inode_number,
    const ifs_ext2_u32 block_size,
    const ifs_ext2_u32 maximum_inode)
{
    ifs_ext2_u32 minimum_length;
    ifs_ext2_u32 offset_in_block;

    /*
     * EXT2 directory entries have an eight-byte fixed header. A live entry
     * needs at least one name byte, rounded to a four-byte record boundary;
     * Linux has historically treated shorter records as corruption as well.
     */
    if (record_length < 12U)
        return IFS_EXT2_DIRECTORY_RECORD_TOO_SHORT;
    if ((record_length & 3U) != 0U)
        return IFS_EXT2_DIRECTORY_RECORD_UNALIGNED;

    minimum_length = (name_length + 8U + 3U) & ~3U;
    if (record_length < minimum_length)
        return IFS_EXT2_DIRECTORY_RECORD_NAME_TOO_LONG;

    if (block_size == 0U)
        return IFS_EXT2_DIRECTORY_RECORD_CROSSES_BLOCK;
    offset_in_block = record_offset % block_size;
    if (record_length > block_size - offset_in_block)
        return IFS_EXT2_DIRECTORY_RECORD_CROSSES_BLOCK;

    if (inode_number > maximum_inode)
        return IFS_EXT2_DIRECTORY_RECORD_INODE_RANGE;

    return IFS_EXT2_DIRECTORY_RECORD_OK;
}

const char *ifs_ext2_directory_record_status_string(
    const IfsExt2DirectoryRecordStatus status)
{
    switch (status) {
    case IFS_EXT2_DIRECTORY_RECORD_OK:
        return "ok";
    case IFS_EXT2_DIRECTORY_RECORD_TOO_SHORT:
        return "rec_len is smaller than minimal";
    case IFS_EXT2_DIRECTORY_RECORD_UNALIGNED:
        return "unaligned directory entry";
    case IFS_EXT2_DIRECTORY_RECORD_NAME_TOO_LONG:
        return "rec_len is too small for name_len";
    case IFS_EXT2_DIRECTORY_RECORD_CROSSES_BLOCK:
        return "directory entry across blocks";
    case IFS_EXT2_DIRECTORY_RECORD_INODE_RANGE:
        return "inode out of bounds";
    }
    return "invalid EXT2 directory record";
}

const char *ifs_ext2_status_string(const IfsExt2Status status)
{
    switch (status) {
    case IFS_EXT2_OK: return "ok";
    case IFS_EXT2_ERROR_ARGUMENT: return "invalid argument";
    case IFS_EXT2_ERROR_MAGIC: return "not an EXT2 superblock";
    case IFS_EXT2_ERROR_REVISION: return "unsupported EXT2 revision";
    case IFS_EXT2_ERROR_JOURNALLED: return "journalled filesystem is not EXT2";
    case IFS_EXT2_ERROR_FEATURES: return "unsupported EXT2 feature set";
    case IFS_EXT2_ERROR_BLOCK_SIZE: return "invalid EXT2 block size";
    case IFS_EXT2_ERROR_FRAGMENT_SIZE: return "fragment size differs from block size";
    case IFS_EXT2_ERROR_INODE_SIZE: return "invalid EXT2 inode size";
    case IFS_EXT2_ERROR_GEOMETRY: return "invalid EXT2 geometry";
    case IFS_EXT2_ERROR_RANGE: return "EXT2 value is out of range";
    case IFS_EXT2_ERROR_CORRUPT: return "corrupt EXT2 metadata";
    case IFS_EXT2_ERROR_IO: return "EXT2 I/O error";
    case IFS_EXT2_ERROR_UNSUPPORTED: return "unsupported EXT2 operation";
    case IFS_EXT2_ERROR_BUFFER_TOO_SMALL: return "EXT2 buffer too small";
    case IFS_EXT2_ERROR_NOT_FOUND: return "EXT2 object not found";
    case IFS_EXT2_ERROR_NOT_DIRECTORY: return "EXT2 object is not a directory";
    case IFS_EXT2_ERROR_IS_DIRECTORY: return "EXT2 object is a directory";
    case IFS_EXT2_ERROR_NO_SPACE: return "EXT2 has no free space";
    case IFS_EXT2_STOP: return "EXT2 enumeration stopped";
    }
    return "unknown EXT2 error";
}
