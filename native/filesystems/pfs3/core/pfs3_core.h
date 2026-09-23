/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFILTRATOR_PFS3_CORE_H
#define INFILTRATOR_PFS3_CORE_H

#if defined(__KERNEL__)
#include <linux/types.h>
typedef u16 ifs_pfs3_u16;
typedef u32 ifs_pfs3_u32;
#else
#include <stdint.h>
typedef uint16_t ifs_pfs3_u16;
typedef uint32_t ifs_pfs3_u32;
#endif

#define IFS_PFS3_DISK_PFS1 0x50465301U
#define IFS_PFS3_DISK_PFS2 0x50465302U
#define IFS_PFS3_MAX_RESERVED_BLOCKS (4096U + 255U * 1024U * 8U)
#define IFS_PFS3_MAX_ROOT_CLUSTER 521U
#define IFS_PFS3_DISK_NAME_BYTES 32U
#define IFS_PFS3_MAX_DISK_NAME 31U

#define IFS_PFS3_MODE_HARDDISK        0x0001U
#define IFS_PFS3_MODE_SPLITTED_ANODES 0x0002U
#define IFS_PFS3_MODE_DIR_EXTENSION   0x0004U
#define IFS_PFS3_MODE_DELDIR          0x0008U
#define IFS_PFS3_MODE_SIZEFIELD       0x0010U
#define IFS_PFS3_MODE_EXTENSION       0x0020U
#define IFS_PFS3_MODE_DATESTAMP       0x0040U
#define IFS_PFS3_MODE_SUPERINDEX      0x0080U
#define IFS_PFS3_MODE_SUPERDELDIR     0x0100U
#define IFS_PFS3_MODE_EXTROVING       0x0200U
#define IFS_PFS3_MODE_LONGFN          0x0400U
#define IFS_PFS3_MODE_LARGEFILE       0x0800U
#define IFS_PFS3_MODE_STORED_GEOM     0x1000U

typedef enum IfsPfs3Format {
    IFS_PFS3_FORMAT_INVALID = 0,
    IFS_PFS3_FORMAT_PFS1,
    IFS_PFS3_FORMAT_PFS2
} IfsPfs3Format;

typedef enum IfsPfs3RootStatus {
    IFS_PFS3_ROOT_OK = 0,
    IFS_PFS3_ROOT_BAD_DISK_TYPE,
    IFS_PFS3_ROOT_ZERO_OPTIONS,
    IFS_PFS3_ROOT_INVALID_LOGICAL_BLOCK_SIZE,
    IFS_PFS3_ROOT_INVALID_RESERVED_BLOCK_SIZE,
    IFS_PFS3_ROOT_INVALID_ROOT_CLUSTER,
    IFS_PFS3_ROOT_CLASSIC_FEATURE_CONFLICT,
    IFS_PFS3_ROOT_INVALID_RESERVED_RANGE,
    IFS_PFS3_ROOT_RESERVED_COUNT_OUT_OF_RANGE,
    IFS_PFS3_ROOT_INVALID_ROOT_CLUSTER_ALIGNMENT,
    IFS_PFS3_ROOT_RESERVED_FREE_OUT_OF_RANGE
} IfsPfs3RootStatus;

IfsPfs3Format ifs_pfs3_classify_disk_type(ifs_pfs3_u32 disk_type);

IfsPfs3RootStatus ifs_pfs3_validate_root_geometry(
    ifs_pfs3_u32 disk_type,
    ifs_pfs3_u32 options,
    ifs_pfs3_u32 logical_block_size,
    ifs_pfs3_u32 reserved_block_size,
    ifs_pfs3_u32 root_block_cluster,
    ifs_pfs3_u32 first_reserved,
    ifs_pfs3_u32 last_reserved,
    ifs_pfs3_u32 reserved_free);

int ifs_pfs3_validate_disk_name(
    const unsigned char disk_name[IFS_PFS3_DISK_NAME_BYTES]);

const char *ifs_pfs3_root_status_string(IfsPfs3RootStatus status);

#endif
