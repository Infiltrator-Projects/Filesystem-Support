/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sfs_core.h"
static int ifs_sfs_is_power_of_two(const ifs_sfs_u32 value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}
IfsSfsRootStatus ifs_sfs_validate_root_probe(
    const ifs_sfs_u32 id,
    const ifs_sfs_u32 version,
    const ifs_sfs_u32 block_size,
    const ifs_sfs_u32 total_blocks)
{
    if (id != IFS_SFS_ROOT_ID)
        return IFS_SFS_ROOT_BAD_ID;
    if (version != IFS_SFS_STRUCTURE_VERSION)
        return IFS_SFS_ROOT_BAD_VERSION;
    if (block_size < IFS_SFS_MIN_BLOCK_SIZE ||
        !ifs_sfs_is_power_of_two(block_size) ||
        block_size <= IFS_SFS_BLOCK_HEADER_SIZE)
        return IFS_SFS_ROOT_INVALID_BLOCK_SIZE;
    if (total_blocks < 3U)
        return IFS_SFS_ROOT_INVALID_TOTAL_BLOCKS;
    return IFS_SFS_ROOT_OK;
}

IfsSfsRootStatus ifs_sfs_validate_root_layout(
    const ifs_sfs_u32 id, const ifs_sfs_u32 version,
    const ifs_sfs_u32 block_size, const ifs_sfs_u32 total_blocks,
    const ifs_sfs_u32 bitmap_base, const ifs_sfs_u32 adminspace_container,
    const ifs_sfs_u32 root_object_container, const ifs_sfs_u32 extent_bnode_root,
    const ifs_sfs_u32 object_node_root)
{
    const IfsSfsRootStatus probe_status =
        ifs_sfs_validate_root_probe(id, version, block_size, total_blocks);

    if (probe_status != IFS_SFS_ROOT_OK)
        return probe_status;
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

int ifs_sfs_free_count_after_allocate(
    const ifs_sfs_u32 current_free,
    const ifs_sfs_u32 allocated_blocks,
    ifs_sfs_u32 *const new_free)
{
    if (new_free == 0 || allocated_blocks > current_free)
        return -1;

    *new_free = current_free - allocated_blocks;
    return 0;
}

int ifs_sfs_free_count_after_release(
    const ifs_sfs_u32 current_free,
    const ifs_sfs_u32 released_blocks,
    const ifs_sfs_u32 total_blocks,
    ifs_sfs_u32 *const new_free)
{
    if (new_free == 0 || current_free > total_blocks ||
        released_blocks > total_blocks - current_free)
        return -1;

    *new_free = current_free + released_blocks;
    return 0;
}

static ifs_sfs_u32 ifs_sfs_read_be32(const unsigned char *const data)
{
    return ((ifs_sfs_u32)data[0] << 24) |
           ((ifs_sfs_u32)data[1] << 16) |
           ((ifs_sfs_u32)data[2] << 8) |
           (ifs_sfs_u32)data[3];
}

ifs_sfs_u32 ifs_sfs_calculate_block_checksum(
    const unsigned char *const block,
    const ifs_sfs_u32 block_size)
{
    ifs_sfs_u32 checksum = 1U;
    ifs_sfs_u32 offset;

    if (block == 0 || block_size < IFS_SFS_BLOCK_HEADER_SIZE ||
        (block_size & 3U) != 0U)
        return 0U;

    for (offset = 0U; offset < block_size; offset += 4U)
        checksum += ifs_sfs_read_be32(block + offset);

    checksum -= ifs_sfs_read_be32(block + 4U);
    return 0U - checksum;
}

int ifs_sfs_validate_block_header(
    const unsigned char *const block,
    const ifs_sfs_u32 block_size,
    const ifs_sfs_u32 expected_block_number,
    const ifs_sfs_u32 expected_block_id)
{
    if (block == 0 || block_size < IFS_SFS_BLOCK_HEADER_SIZE ||
        (block_size & 3U) != 0U)
        return 0;

    return ifs_sfs_read_be32(block) == expected_block_id &&
           ifs_sfs_read_be32(block + 8U) == expected_block_number &&
           ifs_sfs_read_be32(block + 4U) ==
               ifs_sfs_calculate_block_checksum(block, block_size);
}

int ifs_sfs_node_leaf_slot(
    const ifs_sfs_u32 block_size,
    const ifs_sfs_u32 base_node,
    const ifs_sfs_u32 target_node,
    ifs_sfs_u32 *const slot)
{
    ifs_sfs_u32 capacity;
    ifs_sfs_u32 offset;

    if (slot == 0 || block_size <= IFS_SFS_NODE_CONTAINER_FIXED_SIZE ||
        target_node < base_node)
        return -1;

    capacity =
        (block_size - IFS_SFS_NODE_CONTAINER_FIXED_SIZE) /
        IFS_SFS_OBJECT_NODE_SIZE;
    offset = target_node - base_node;
    if (offset >= capacity)
        return -1;

    *slot = offset;
    return 0;
}

int ifs_sfs_node_index_slot(
    const ifs_sfs_u32 block_size,
    const ifs_sfs_u32 base_node,
    const ifs_sfs_u32 nodes_per_entry,
    const ifs_sfs_u32 target_node,
    ifs_sfs_u32 *const slot)
{
    ifs_sfs_u32 capacity;
    ifs_sfs_u32 offset;
    ifs_sfs_u32 index;

    if (slot == 0 || block_size <= IFS_SFS_NODE_CONTAINER_FIXED_SIZE ||
        nodes_per_entry <= 1U || target_node < base_node)
        return -1;

    capacity =
        (block_size - IFS_SFS_NODE_CONTAINER_FIXED_SIZE) /
        IFS_SFS_NODE_INDEX_ENTRY_SIZE;
    offset = target_node - base_node;
    index = offset / nodes_per_entry;
    if (index >= capacity)
        return -1;

    *slot = index;
    return 0;
}

int ifs_sfs_validate_btree_layout(
    const ifs_sfs_u32 block_size,
    const ifs_sfs_u32 node_count,
    const ifs_sfs_u32 node_size,
    const int is_leaf,
    ifs_sfs_u32 *const capacity)
{
    ifs_sfs_u32 minimum_node_size;
    ifs_sfs_u32 available_nodes;

    if (capacity == 0 ||
        block_size <= IFS_SFS_BNODE_CONTAINER_FIXED_SIZE ||
        (is_leaf != 0 && is_leaf != 1) ||
        node_size == 0U || (node_size & 1U) != 0U)
        return -1;

    minimum_node_size = is_leaf != 0 ?
        IFS_SFS_BTREE_EXTENT_NODE_MIN_SIZE :
        IFS_SFS_BTREE_INTERNAL_NODE_MIN_SIZE;
    if (node_size < minimum_node_size)
        return -1;

    available_nodes =
        (block_size - IFS_SFS_BNODE_CONTAINER_FIXED_SIZE) / node_size;
    if (available_nodes == 0U || node_count > available_nodes)
        return -1;

    *capacity = available_nodes;
    return 0;
}

int ifs_sfs_validate_extent(
    const ifs_sfs_u32 key,
    const ifs_sfs_u32 next,
    const ifs_sfs_u32 block_count,
    const ifs_sfs_u32 total_blocks)
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

int ifs_sfs_adjust_counter(
    const ifs_sfs_u32 current_value,
    const ifs_sfs_i32 delta,
    ifs_sfs_u32 *const result)
{
    ifs_sfs_u64 amount;

    if (result == 0)
        return -1;

    if (delta < 0) {
        amount = (ifs_sfs_u64)(-(ifs_sfs_i64)delta);
        if (amount > current_value)
            return -1;
        *result = current_value - (ifs_sfs_u32)amount;
        return 0;
    }

    amount = (ifs_sfs_u64)delta;
    if (amount > 0xffffffffULL - current_value)
        return -1;
    *result = current_value + (ifs_sfs_u32)amount;
    return 0;
}

int ifs_sfs_select_root_copy(
    const int primary_valid,
    const ifs_sfs_u32 primary_sequence,
    const int backup_valid,
    const ifs_sfs_u32 backup_sequence)
{
    if (primary_valid == 0 && backup_valid == 0)
        return -1;
    if (primary_valid == 0)
        return 1;
    if (backup_valid == 0)
        return 0;
    return backup_sequence > primary_sequence ? 1 : 0;
}
