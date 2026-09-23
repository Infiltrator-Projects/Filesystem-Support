/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sfs_core.h"
static int ifs_sfs_is_power_of_two(const ifs_sfs_u32 value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}
IfsSfsRootStatus ifs_sfs_validate_root_layout(
    const ifs_sfs_u32 id, const ifs_sfs_u32 version,
    const ifs_sfs_u32 block_size, const ifs_sfs_u32 total_blocks,
    const ifs_sfs_u32 bitmap_base, const ifs_sfs_u32 adminspace_container,
    const ifs_sfs_u32 root_object_container, const ifs_sfs_u32 extent_bnode_root,
    const ifs_sfs_u32 object_node_root)
{
    if (id != IFS_SFS_ROOT_ID) return IFS_SFS_ROOT_BAD_ID;
    if (version != IFS_SFS_STRUCTURE_VERSION) return IFS_SFS_ROOT_BAD_VERSION;
    if (block_size < IFS_SFS_MIN_BLOCK_SIZE ||
        !ifs_sfs_is_power_of_two(block_size) ||
        block_size <= IFS_SFS_BLOCK_HEADER_SIZE)
        return IFS_SFS_ROOT_INVALID_BLOCK_SIZE;
    if (total_blocks < 3U) return IFS_SFS_ROOT_INVALID_TOTAL_BLOCKS;
    if (bitmap_base == 0U || bitmap_base >= total_blocks ||
        adminspace_container == 0U || adminspace_container >= total_blocks ||
        root_object_container == 0U || root_object_container >= total_blocks ||
        extent_bnode_root == 0U || extent_bnode_root >= total_blocks ||
        object_node_root == 0U || object_node_root >= total_blocks)
        return IFS_SFS_ROOT_BLOCK_REFERENCE_OUT_OF_RANGE;
    if (root_object_container > total_blocks - 3U)
        return IFS_SFS_ROOT_TRANSACTION_BLOCK_OUT_OF_RANGE;
    return IFS_SFS_ROOT_OK;
}
int ifs_sfs_compute_bitmap_layout(
    const ifs_sfs_u32 block_size, const ifs_sfs_u32 total_blocks,
    ifs_sfs_u32 *const blocks_per_bitmap,
    ifs_sfs_u32 *const bitmap_block_count)
{
    ifs_sfs_u64 capacity;
    if (blocks_per_bitmap == 0 || bitmap_block_count == 0 ||
        block_size <= IFS_SFS_BLOCK_HEADER_SIZE || (block_size & 3U) != 0U)
        return -1;
    capacity = ((ifs_sfs_u64)block_size - IFS_SFS_BLOCK_HEADER_SIZE) * 8U;
    if (capacity == 0U || capacity > 0xffffffffULL) return -1;
    *blocks_per_bitmap = (ifs_sfs_u32)capacity;
    *bitmap_block_count = total_blocks / *blocks_per_bitmap +
        (total_blocks % *blocks_per_bitmap != 0U ? 1U : 0U);
    return 0;
}
const char *ifs_sfs_root_status_string(const IfsSfsRootStatus status)
{
    switch (status) {
    case IFS_SFS_ROOT_OK: return "ok";
    case IFS_SFS_ROOT_BAD_ID: return "invalid root block identifier";
    case IFS_SFS_ROOT_BAD_VERSION: return "unsupported SFS structure version";
    case IFS_SFS_ROOT_INVALID_BLOCK_SIZE: return "invalid SFS block size";
    case IFS_SFS_ROOT_INVALID_TOTAL_BLOCKS: return "invalid SFS total block count";
    case IFS_SFS_ROOT_BLOCK_REFERENCE_OUT_OF_RANGE: return "SFS root block reference is out of range";
    case IFS_SFS_ROOT_TRANSACTION_BLOCK_OUT_OF_RANGE: return "SFS transaction-failure block is out of range";
    }
    return "invalid SFS root layout";
}

IfsSfsObjectRecordStatus ifs_sfs_object_record_layout(
    const unsigned char *const name_and_comment,
    const ifs_sfs_u32 available_tail_bytes,
    const ifs_sfs_u32 fixed_prefix_bytes,
    ifs_sfs_u32 *const record_bytes,
    ifs_sfs_u32 *const name_bytes)
{
    ifs_sfs_u32 name_end = 0U;
    ifs_sfs_u32 comment_end;
    ifs_sfs_u64 raw_size;
    ifs_sfs_u64 aligned_size;

    if (name_and_comment == 0 || record_bytes == 0 || name_bytes == 0)
        return IFS_SFS_OBJECT_RECORD_INVALID_ARGUMENT;

    while (name_end < available_tail_bytes &&
           name_and_comment[name_end] != 0U)
        name_end++;

    if (name_end == available_tail_bytes)
        return IFS_SFS_OBJECT_RECORD_NAME_UNTERMINATED;
    if (name_end > IFS_SFS_MAX_FILENAME)
        return IFS_SFS_OBJECT_RECORD_NAME_TOO_LONG;

    comment_end = name_end + 1U;
    while (comment_end < available_tail_bytes &&
           name_and_comment[comment_end] != 0U)
        comment_end++;

    if (comment_end == available_tail_bytes)
        return IFS_SFS_OBJECT_RECORD_COMMENT_UNTERMINATED;

    raw_size =
        (ifs_sfs_u64)fixed_prefix_bytes + (ifs_sfs_u64)comment_end + 1U;
    aligned_size = (raw_size + 1U) & ~1ULL;
    if (aligned_size > 0xffffffffULL ||
        aligned_size >
            (ifs_sfs_u64)fixed_prefix_bytes + available_tail_bytes)
        return IFS_SFS_OBJECT_RECORD_SIZE_OVERFLOW;

    *record_bytes = (ifs_sfs_u32)aligned_size;
    *name_bytes = name_end;
    return IFS_SFS_OBJECT_RECORD_OK;
}

const char *ifs_sfs_object_record_status_string(
    const IfsSfsObjectRecordStatus status)
{
    switch (status) {
    case IFS_SFS_OBJECT_RECORD_OK:
        return "ok";
    case IFS_SFS_OBJECT_RECORD_INVALID_ARGUMENT:
        return "invalid object-record argument";
    case IFS_SFS_OBJECT_RECORD_NAME_UNTERMINATED:
        return "unterminated object name";
    case IFS_SFS_OBJECT_RECORD_NAME_TOO_LONG:
        return "object name exceeds SFS limit";
    case IFS_SFS_OBJECT_RECORD_COMMENT_UNTERMINATED:
        return "unterminated object comment";
    case IFS_SFS_OBJECT_RECORD_SIZE_OVERFLOW:
        return "object record exceeds its containing block";
    }
    return "invalid SFS object record";
}

int ifs_sfs_has_allocation_headroom(
    const ifs_sfs_u32 free_blocks,
    const ifs_sfs_u32 requested_blocks,
    const ifs_sfs_u32 always_free_blocks)
{
    if (free_blocks <= always_free_blocks)
        return 0;

    return requested_blocks <= free_blocks - always_free_blocks;
}

int ifs_sfs_adminspace_block_mask(
    const ifs_sfs_u32 area_start,
    const ifs_sfs_u32 block,
    ifs_sfs_u32 *const mask)
{
    ifs_sfs_u32 offset;

    if (mask == 0 || area_start == 0U || block < area_start)
        return -1;

    offset = block - area_start;
    if (offset >= 32U)
        return -1;

    *mask = 1U << (31U - offset);
    return 0;
}

int ifs_sfs_bitmap_word_find_set(
    const ifs_sfs_u32 word,
    const ifs_sfs_u32 start_bit)
{
    ifs_sfs_u32 bit;

    if (start_bit >= 32U)
        return -1;

    for (bit = start_bit; bit < 32U; ++bit) {
        if ((word & (1U << (31U - bit))) != 0U)
            return (int)bit;
    }

    return -1;
}

int ifs_sfs_bitmap_word_find_zero(
    const ifs_sfs_u32 word,
    const ifs_sfs_u32 start_bit)
{
    return ifs_sfs_bitmap_word_find_set(~word, start_bit);
}

ifs_sfs_u32 ifs_sfs_bitmap_word_set(
    ifs_sfs_u32 word,
    const ifs_sfs_u32 start_bit,
    ifs_sfs_u32 bit_count)
{
    ifs_sfs_u32 bit;

    if (start_bit >= 32U || bit_count == 0U)
        return word;
    if (bit_count > 32U - start_bit)
        bit_count = 32U - start_bit;

    for (bit = 0U; bit < bit_count; ++bit)
        word |= 1U << (31U - start_bit - bit);

    return word;
}

ifs_sfs_u32 ifs_sfs_bitmap_word_clear(
    ifs_sfs_u32 word,
    const ifs_sfs_u32 start_bit,
    ifs_sfs_u32 bit_count)
{
    ifs_sfs_u32 bit;

    if (start_bit >= 32U || bit_count == 0U)
        return word;
    if (bit_count > 32U - start_bit)
        bit_count = 32U - start_bit;

    for (bit = 0U; bit < bit_count; ++bit)
        word &= ~(1U << (31U - start_bit - bit));

    return word;
}
