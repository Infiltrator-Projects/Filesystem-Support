/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef INFILTRATR_EXT3_CORE_H
#define INFILTRATR_EXT3_CORE_H

/*
 * Canonical EXT3 filesystem semantics shared by every operating-system
 * adapter.  This interface contains no Linux VFS or Windows IFS types.
 */

#if defined(__KERNEL__)
#include <linux/types.h>
typedef u32 ifs_ext3_u32;
#elif defined(IFS_EXT3_WINDOWS_KERNEL)
typedef unsigned int ifs_ext3_u32;
#else
#include <stdint.h>
typedef uint32_t ifs_ext3_u32;
#endif

#define IFS_EXT3_FEATURE_INCOMPAT_FILETYPE 0x0002U
#define IFS_EXT3_FEATURE_INCOMPAT_RECOVER  0x0004U
#define IFS_EXT3_FEATURE_INCOMPAT_META_BG  0x0010U
#define IFS_EXT3_FEATURE_INCOMPAT_SUPPORTED     (IFS_EXT3_FEATURE_INCOMPAT_FILETYPE |      IFS_EXT3_FEATURE_INCOMPAT_RECOVER |      IFS_EXT3_FEATURE_INCOMPAT_META_BG)

#define IFS_EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001U
#define IFS_EXT3_FEATURE_RO_COMPAT_LARGE_FILE   0x0002U
#define IFS_EXT3_FEATURE_RO_COMPAT_BTREE_DIR    0x0004U
#define IFS_EXT3_FEATURE_RO_COMPAT_SUPPORTED     (IFS_EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER |      IFS_EXT3_FEATURE_RO_COMPAT_LARGE_FILE |      IFS_EXT3_FEATURE_RO_COMPAT_BTREE_DIR)

ifs_ext3_u32 ifs_ext3_unsupported_incompat_features(
    ifs_ext3_u32 feature_incompat);

ifs_ext3_u32 ifs_ext3_unsupported_ro_compat_features(
    ifs_ext3_u32 feature_ro_compat);

typedef enum IfsExt3GeometryStatus {
    IFS_EXT3_GEOMETRY_OK = 0,
    IFS_EXT3_GEOMETRY_INVALID_INODE_SIZE,
    IFS_EXT3_GEOMETRY_FRAGMENT_SIZE_MISMATCH,
    IFS_EXT3_GEOMETRY_ZERO_GROUP_VALUE,
    IFS_EXT3_GEOMETRY_BLOCKS_PER_GROUP_TOO_LARGE,
    IFS_EXT3_GEOMETRY_FRAGMENTS_PER_GROUP_TOO_LARGE,
    IFS_EXT3_GEOMETRY_INODES_PER_GROUP_TOO_LARGE
} IfsExt3GeometryStatus;

IfsExt3GeometryStatus ifs_ext3_validate_geometry(
    ifs_ext3_u32 block_size,
    ifs_ext3_u32 inode_size,
    ifs_ext3_u32 fragment_size,
    ifs_ext3_u32 blocks_per_group,
    ifs_ext3_u32 fragments_per_group,
    ifs_ext3_u32 inodes_per_group);

typedef enum IfsExt3LayoutStatus {
    IFS_EXT3_LAYOUT_OK = 0,
    IFS_EXT3_LAYOUT_INVALID_FIRST_DATA_BLOCK,
    IFS_EXT3_LAYOUT_INVALID_BLOCKS_PER_GROUP
} IfsExt3LayoutStatus;

IfsExt3LayoutStatus ifs_ext3_compute_group_count(
    ifs_ext3_u32 blocks_count,
    ifs_ext3_u32 first_data_block,
    ifs_ext3_u32 blocks_per_group,
    ifs_ext3_u32 *group_count);

#endif
