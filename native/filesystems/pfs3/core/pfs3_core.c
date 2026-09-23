/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pfs3_core.h"

static int ifs_pfs3_is_power_of_two(const ifs_pfs3_u32 value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static ifs_pfs3_u16 ifs_pfs3_read_be16(const unsigned char *data)
{
    return (ifs_pfs3_u16)(((ifs_pfs3_u16)data[0] << 8) |
                          (ifs_pfs3_u16)data[1]);
}

static ifs_pfs3_u32 ifs_pfs3_read_be32(const unsigned char *data)
{
    return ((ifs_pfs3_u32)data[0] << 24) |
           ((ifs_pfs3_u32)data[1] << 16) |
           ((ifs_pfs3_u32)data[2] << 8) |
           (ifs_pfs3_u32)data[3];
}

int ifs_pfs3_decode_root(
    const unsigned char *const bytes,
    const ifs_pfs3_u32 byte_count,
    IfsPfs3RootRecord *const root)
{
    ifs_pfs3_u32 index;

    if (bytes == 0 || root == 0 || byte_count < IFS_PFS3_ROOT_MIN_BYTES)
        return -1;

    root->disk_type = ifs_pfs3_read_be32(bytes + 0U);
    root->options = ifs_pfs3_read_be32(bytes + 4U);
    root->datestamp = ifs_pfs3_read_be32(bytes + 8U);
    root->creation_day = ifs_pfs3_read_be16(bytes + 12U);
    root->creation_minute = ifs_pfs3_read_be16(bytes + 14U);
    root->creation_tick = ifs_pfs3_read_be16(bytes + 16U);
    root->protection = ifs_pfs3_read_be16(bytes + 18U);
    for (index = 0U; index < IFS_PFS3_DISK_NAME_BYTES; index++)
        root->disk_name[index] = bytes[20U + index];
    root->last_reserved = ifs_pfs3_read_be32(bytes + 52U);
    root->first_reserved = ifs_pfs3_read_be32(bytes + 56U);
    root->reserved_free = ifs_pfs3_read_be32(bytes + 60U);
    root->reserved_block_size = ifs_pfs3_read_be16(bytes + 64U);
    root->root_block_cluster = ifs_pfs3_read_be16(bytes + 66U);
    root->blocks_free = ifs_pfs3_read_be32(bytes + 68U);
    root->always_free = ifs_pfs3_read_be32(bytes + 72U);
    root->roving_pointer = ifs_pfs3_read_be32(bytes + 76U);
    root->delete_directory = ifs_pfs3_read_be32(bytes + 80U);
    root->disk_size = ifs_pfs3_read_be32(bytes + 84U);
    root->extension = ifs_pfs3_read_be32(bytes + 88U);
    return 0;
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
    if (root_block_cluster < 1U || root_block_cluster > IFS_PFS3_MAX_ROOT_CLUSTER)
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
    if (reserved_block_count == 0U ||
        reserved_block_count > IFS_PFS3_MAX_RESERVED_BLOCKS)
        return IFS_PFS3_ROOT_RESERVED_COUNT_OUT_OF_RANGE;
    if (root_block_cluster % sectors_per_reserved_block != 0U ||
        root_block_cluster > reserved_sector_count)
        return IFS_PFS3_ROOT_INVALID_ROOT_CLUSTER_ALIGNMENT;
    if (reserved_free > reserved_block_count)
        return IFS_PFS3_ROOT_RESERVED_FREE_OUT_OF_RANGE;

    return IFS_PFS3_ROOT_OK;
}

int ifs_pfs3_validate_allocation_counts(
    const ifs_pfs3_u32 blocks_free,
    const ifs_pfs3_u32 always_free)
{
    return always_free <= blocks_free ? 0 : -1;
}

int ifs_pfs3_validate_disk_name(
    const unsigned char disk_name[IFS_PFS3_DISK_NAME_BYTES])
{
    ifs_pfs3_u32 length, index;
    if (disk_name == 0)
        return -1;
    length = disk_name[0];
    if (length == 0U || length > IFS_PFS3_MAX_DISK_NAME)
        return -1;
    for (index = 0U; index < length; index++) {
        if (disk_name[index + 1U] == ':' || disk_name[index + 1U] == '/')
            return -1;
    }
    return 0;
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
    case IFS_PFS3_ROOT_RESERVED_COUNT_OUT_OF_RANGE:
        return "PFS reserved-block count exceeds implementation limit";
    case IFS_PFS3_ROOT_INVALID_ROOT_CLUSTER_ALIGNMENT:
        return "PFS root-block cluster is not aligned to the reserved area";
    case IFS_PFS3_ROOT_RESERVED_FREE_OUT_OF_RANGE:
        return "PFS reserved-free count exceeds reserved area";
    }
    return "invalid PFS root geometry";
}
