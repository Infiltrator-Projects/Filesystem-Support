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
