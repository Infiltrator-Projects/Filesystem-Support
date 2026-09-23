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
