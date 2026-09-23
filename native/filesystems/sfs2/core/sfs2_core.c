/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sfs2_core.h"

static int ifs_sfs2_is_power_of_two(const ifs_sfs2_u32 value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

IfsSfs2RootStatus ifs_sfs2_validate_root_layout(
    const ifs_sfs2_u32 id,
    const ifs_sfs2_u32 version,
    const ifs_sfs2_u32 block_size,
    const ifs_sfs2_u32 total_blocks,
    const ifs_sfs2_u32 bitmap_base,
    const ifs_sfs2_u32 adminspace_container,
    const ifs_sfs2_u32 root_object_container,
    const ifs_sfs2_u32 extent_bnode_root,
    const ifs_sfs2_u32 object_node_root)
{
    if (id != IFS_SFS2_ROOT_ID)
        return IFS_SFS2_ROOT_BAD_ID;
    if (version != IFS_SFS2_STRUCTURE_VERSION)
        return IFS_SFS2_ROOT_BAD_VERSION;
    if (block_size < 512U || !ifs_sfs2_is_power_of_two(block_size))
        return IFS_SFS2_ROOT_INVALID_BLOCK_SIZE;
    if (total_blocks < 2U)
        return IFS_SFS2_ROOT_INVALID_TOTAL_BLOCKS;
    if (bitmap_base == 0U || bitmap_base >= total_blocks ||
        adminspace_container == 0U || adminspace_container >= total_blocks ||
        root_object_container == 0U || root_object_container >= total_blocks ||
        extent_bnode_root == 0U || extent_bnode_root >= total_blocks ||
        object_node_root == 0U || object_node_root >= total_blocks)
        return IFS_SFS2_ROOT_BLOCK_REFERENCE_OUT_OF_RANGE;
    return IFS_SFS2_ROOT_OK;
}

ifs_sfs2_u64 ifs_sfs2_decode_file_size(
    const ifs_sfs2_u32 high_32,
    const ifs_sfs2_u16 low_16)
{
    return ((ifs_sfs2_u64)high_32 << 16) | low_16;
}

int ifs_sfs2_encode_file_size(
    const ifs_sfs2_u64 file_size,
    ifs_sfs2_u32 *const high_32,
    ifs_sfs2_u16 *const low_16)
{
    if (high_32 == 0 || low_16 == 0 || file_size > IFS_SFS2_MAX_FILE_SIZE)
        return -1;

    *high_32 = (ifs_sfs2_u32)(file_size >> 16);
    *low_16 = (ifs_sfs2_u16)(file_size & 0xffffU);
    return 0;
}

const char *ifs_sfs2_root_status_string(const IfsSfs2RootStatus status)
{
    switch (status) {
    case IFS_SFS2_ROOT_OK:
        return "ok";
    case IFS_SFS2_ROOT_BAD_ID:
        return "invalid SFS2 root identifier";
    case IFS_SFS2_ROOT_BAD_VERSION:
        return "unsupported SFS2 structure version";
    case IFS_SFS2_ROOT_INVALID_BLOCK_SIZE:
        return "invalid SFS2 block size";
    case IFS_SFS2_ROOT_INVALID_TOTAL_BLOCKS:
        return "invalid SFS2 total block count";
    case IFS_SFS2_ROOT_BLOCK_REFERENCE_OUT_OF_RANGE:
        return "SFS2 root block reference is out of range";
    }
    return "invalid SFS2 root layout";
}

int ifs_sfs2_select_root_copy(
    const int primary_valid,
    const ifs_sfs2_u32 primary_sequence,
    const int backup_valid,
    const ifs_sfs2_u32 backup_sequence)
{
    if (primary_valid == 0 && backup_valid == 0)
        return -1;
    if (primary_valid == 0)
        return 1;
    if (backup_valid == 0)
        return 0;
    return backup_sequence > primary_sequence ? 1 : 0;
}

static ifs_sfs2_u32 ifs_sfs2_read_be32(const unsigned char *const data)
{
    return ((ifs_sfs2_u32)data[0] << 24) |
           ((ifs_sfs2_u32)data[1] << 16) |
           ((ifs_sfs2_u32)data[2] << 8) |
           (ifs_sfs2_u32)data[3];
}

ifs_sfs2_u32 ifs_sfs2_calculate_block_checksum(
    const unsigned char *const block,
    const ifs_sfs2_u32 block_size)
{
    ifs_sfs2_u32 checksum = 1U;
    ifs_sfs2_u32 offset;

    if (block == 0 || block_size < IFS_SFS2_BLOCK_HEADER_SIZE ||
        (block_size & 3U) != 0U)
        return 0U;

    for (offset = 0U; offset < block_size; offset += 4U)
        checksum += ifs_sfs2_read_be32(block + offset);

    checksum -= ifs_sfs2_read_be32(block + 4U);
    return 0U - checksum;
}

int ifs_sfs2_validate_block_header(
    const unsigned char *const block,
    const ifs_sfs2_u32 block_size,
    const ifs_sfs2_u32 expected_block_number,
    const ifs_sfs2_u32 expected_block_id)
{
    if (block == 0 || block_size < IFS_SFS2_BLOCK_HEADER_SIZE ||
        (block_size & 3U) != 0U)
        return 0;

    return ifs_sfs2_read_be32(block) == expected_block_id &&
           ifs_sfs2_read_be32(block + 8U) == expected_block_number &&
           ifs_sfs2_read_be32(block + 4U) ==
               ifs_sfs2_calculate_block_checksum(block, block_size);
}

IfsSfs2ObjectRecordStatus ifs_sfs2_object_record_layout(
    const unsigned char *const name_and_comment,
    const ifs_sfs2_u32 available_tail_bytes,
    ifs_sfs2_u32 *const record_bytes,
    ifs_sfs2_u32 *const name_bytes)
{
    ifs_sfs2_u32 name_end = 0U;
    ifs_sfs2_u32 comment_end;
    ifs_sfs2_u64 raw_size;
    ifs_sfs2_u64 aligned_size;

    if (name_and_comment == 0 || record_bytes == 0 || name_bytes == 0)
        return IFS_SFS2_OBJECT_RECORD_INVALID_ARGUMENT;

    while (name_end < available_tail_bytes &&
           name_and_comment[name_end] != 0U)
        name_end++;

    if (name_end == available_tail_bytes)
        return IFS_SFS2_OBJECT_RECORD_NAME_UNTERMINATED;
    if (name_end > IFS_SFS2_MAX_FILENAME)
        return IFS_SFS2_OBJECT_RECORD_NAME_TOO_LONG;

    comment_end = name_end + 1U;
    while (comment_end < available_tail_bytes &&
           name_and_comment[comment_end] != 0U)
        comment_end++;

    if (comment_end == available_tail_bytes)
        return IFS_SFS2_OBJECT_RECORD_COMMENT_UNTERMINATED;

    raw_size = (ifs_sfs2_u64)IFS_SFS2_OBJECT_FIXED_SIZE +
               (ifs_sfs2_u64)comment_end + 1U;
    aligned_size = (raw_size + 1U) & ~1ULL;
    if (aligned_size > 0xffffffffULL ||
        aligned_size >
            (ifs_sfs2_u64)IFS_SFS2_OBJECT_FIXED_SIZE +
            available_tail_bytes)
        return IFS_SFS2_OBJECT_RECORD_SIZE_OVERFLOW;

    *record_bytes = (ifs_sfs2_u32)aligned_size;
    *name_bytes = name_end;
    return IFS_SFS2_OBJECT_RECORD_OK;
}

int ifs_sfs2_validate_extent(
    const ifs_sfs2_u32 key,
    const ifs_sfs2_u32 next,
    const ifs_sfs2_u32 block_count,
    const ifs_sfs2_u32 total_blocks)
{
    if (total_blocks == 0U || block_count == 0U ||
        key == 0U || key >= total_blocks)
        return -1;
    if (block_count > total_blocks - key)
        return -1;
    if (next != 0U && (next >= total_blocks || next == key))
        return -1;
    return 0;
}
