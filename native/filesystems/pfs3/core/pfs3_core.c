/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pfs3_core.h"

static int ifs_pfs3_is_power_of_two(const ifs_pfs3_u32 value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

IfsPfs3Format ifs_pfs3_classify_disk_type(const ifs_pfs3_u32 disk_type)
{
    switch (disk_type) {
    case IFS_PFS3_DISK_PFS1:
        return IFS_PFS3_FORMAT_PFS1;
    case IFS_PFS3_DISK_PFS2:
        return IFS_PFS3_FORMAT_PFS2;
    default:
        return IFS_PFS3_FORMAT_INVALID;
    }
}

IfsPfs3RootStatus ifs_pfs3_validate_root_geometry(
    const ifs_pfs3_u32 disk_type,
    const ifs_pfs3_u32 options,
    const ifs_pfs3_u32 logical_block_size,
    const ifs_pfs3_u32 reserved_block_size,
    const ifs_pfs3_u32 root_block_cluster,
    const ifs_pfs3_u32 first_reserved,
    const ifs_pfs3_u32 last_reserved,
    const ifs_pfs3_u32 reserved_free)
{
    ifs_pfs3_u32 sectors_per_reserved_block;
    ifs_pfs3_u32 reserved_sector_count;
    ifs_pfs3_u32 reserved_block_count;

    if (ifs_pfs3_classify_disk_type(disk_type) == IFS_PFS3_FORMAT_INVALID)
        return IFS_PFS3_ROOT_BAD_DISK_TYPE;
    if (options == 0U)
        return IFS_PFS3_ROOT_ZERO_OPTIONS;
    if (logical_block_size < 512U ||
        !ifs_pfs3_is_power_of_two(logical_block_size))
        return IFS_PFS3_ROOT_INVALID_LOGICAL_BLOCK_SIZE;
    if (reserved_block_size < logical_block_size ||
        reserved_block_size > 4096U ||
        !ifs_pfs3_is_power_of_two(reserved_block_size) ||
        reserved_block_size % logical_block_size != 0U)
        return IFS_PFS3_ROOT_INVALID_RESERVED_BLOCK_SIZE;
    if (root_block_cluster < 1U || root_block_cluster > 521U)
        return IFS_PFS3_ROOT_INVALID_ROOT_CLUSTER;

    if (disk_type == IFS_PFS3_DISK_PFS1 &&
        ((options & IFS_PFS3_MODE_LARGEFILE) != 0U ||
         reserved_block_size > 1024U))
        return IFS_PFS3_ROOT_CLASSIC_FEATURE_CONFLICT;

    if (last_reserved < first_reserved)
        return IFS_PFS3_ROOT_INVALID_RESERVED_RANGE;

    sectors_per_reserved_block =
        reserved_block_size / logical_block_size;
    reserved_sector_count = last_reserved - first_reserved + 1U;
    if (reserved_sector_count % sectors_per_reserved_block != 0U)
        return IFS_PFS3_ROOT_INVALID_RESERVED_RANGE;

    reserved_block_count =
        reserved_sector_count / sectors_per_reserved_block;
    if (reserved_free > reserved_block_count)
        return IFS_PFS3_ROOT_RESERVED_FREE_OUT_OF_RANGE;

    return IFS_PFS3_ROOT_OK;
}

const char *ifs_pfs3_root_status_string(const IfsPfs3RootStatus status)
{
    switch (status) {
    case IFS_PFS3_ROOT_OK:
        return "ok";
    case IFS_PFS3_ROOT_BAD_DISK_TYPE:
        return "unsupported PFS disk type";
    case IFS_PFS3_ROOT_ZERO_OPTIONS:
        return "PFS root options are zero";
    case IFS_PFS3_ROOT_INVALID_LOGICAL_BLOCK_SIZE:
        return "invalid PFS logical block size";
    case IFS_PFS3_ROOT_INVALID_RESERVED_BLOCK_SIZE:
        return "invalid PFS reserved block size";
    case IFS_PFS3_ROOT_INVALID_ROOT_CLUSTER:
        return "invalid PFS root-block cluster";
    case IFS_PFS3_ROOT_CLASSIC_FEATURE_CONFLICT:
        return "PFS1 media uses PFS2-only format features";
    case IFS_PFS3_ROOT_INVALID_RESERVED_RANGE:
        return "invalid PFS reserved-block range";
    case IFS_PFS3_ROOT_RESERVED_FREE_OUT_OF_RANGE:
        return "PFS reserved-free count exceeds reserved area";
    }
    return "invalid PFS root geometry";
}
