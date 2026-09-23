/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef INFILTRATR_EXT4_CORE_H
#define INFILTRATR_EXT4_CORE_H

/*
 * Canonical EXT4 filesystem semantics shared by every operating-system
 * adapter.  This interface contains no Linux VFS or Windows IFS types.
 */

#if defined(__KERNEL__)
#include <linux/types.h>
typedef u32 ifs_ext4_u32;
typedef u64 ifs_ext4_u64;
#elif defined(IFS_EXT4_WINDOWS_KERNEL)
typedef unsigned int ifs_ext4_u32;
typedef unsigned long long ifs_ext4_u64;
#else
#include <stdint.h>
typedef uint32_t ifs_ext4_u32;
typedef uint64_t ifs_ext4_u64;
#endif

#define IFS_EXT4_FEATURE_INCOMPAT_FILETYPE    0x0002U
#define IFS_EXT4_FEATURE_INCOMPAT_RECOVER     0x0004U
#define IFS_EXT4_FEATURE_INCOMPAT_META_BG     0x0010U
#define IFS_EXT4_FEATURE_INCOMPAT_EXTENTS     0x0040U
#define IFS_EXT4_FEATURE_INCOMPAT_64BIT       0x0080U
#define IFS_EXT4_FEATURE_INCOMPAT_MMP         0x0100U
#define IFS_EXT4_FEATURE_INCOMPAT_FLEX_BG     0x0200U
#define IFS_EXT4_FEATURE_INCOMPAT_EA_INODE    0x0400U
#define IFS_EXT4_FEATURE_INCOMPAT_CSUM_SEED   0x2000U
#define IFS_EXT4_FEATURE_INCOMPAT_LARGEDIR    0x4000U
#define IFS_EXT4_FEATURE_INCOMPAT_INLINE_DATA 0x8000U
#define IFS_EXT4_FEATURE_INCOMPAT_ENCRYPT     0x10000U
#define IFS_EXT4_FEATURE_INCOMPAT_CASEFOLD    0x20000U
#define IFS_EXT4_FEATURE_INCOMPAT_SUPPORTED     (IFS_EXT4_FEATURE_INCOMPAT_FILETYPE |      IFS_EXT4_FEATURE_INCOMPAT_RECOVER |      IFS_EXT4_FEATURE_INCOMPAT_META_BG |      IFS_EXT4_FEATURE_INCOMPAT_EXTENTS |      IFS_EXT4_FEATURE_INCOMPAT_64BIT |      IFS_EXT4_FEATURE_INCOMPAT_MMP |      IFS_EXT4_FEATURE_INCOMPAT_FLEX_BG |      IFS_EXT4_FEATURE_INCOMPAT_EA_INODE |      IFS_EXT4_FEATURE_INCOMPAT_CSUM_SEED |      IFS_EXT4_FEATURE_INCOMPAT_LARGEDIR |      IFS_EXT4_FEATURE_INCOMPAT_INLINE_DATA |      IFS_EXT4_FEATURE_INCOMPAT_ENCRYPT |      IFS_EXT4_FEATURE_INCOMPAT_CASEFOLD)

#define IFS_EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER   0x0001U
#define IFS_EXT4_FEATURE_RO_COMPAT_LARGE_FILE     0x0002U
#define IFS_EXT4_FEATURE_RO_COMPAT_BTREE_DIR      0x0004U
#define IFS_EXT4_FEATURE_RO_COMPAT_HUGE_FILE      0x0008U
#define IFS_EXT4_FEATURE_RO_COMPAT_GDT_CSUM       0x0010U
#define IFS_EXT4_FEATURE_RO_COMPAT_DIR_NLINK      0x0020U
#define IFS_EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE    0x0040U
#define IFS_EXT4_FEATURE_RO_COMPAT_QUOTA          0x0100U
#define IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC       0x0200U
#define IFS_EXT4_FEATURE_RO_COMPAT_METADATA_CSUM  0x0400U
#define IFS_EXT4_FEATURE_RO_COMPAT_READONLY       0x1000U
#define IFS_EXT4_FEATURE_RO_COMPAT_PROJECT        0x2000U
#define IFS_EXT4_FEATURE_RO_COMPAT_VERITY         0x8000U
#define IFS_EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT 0x10000U
#define IFS_EXT4_FEATURE_RO_COMPAT_SUPPORTED     (IFS_EXT4_FEATURE_RO_COMPAT_SPARSE_SUPER |      IFS_EXT4_FEATURE_RO_COMPAT_LARGE_FILE |      IFS_EXT4_FEATURE_RO_COMPAT_BTREE_DIR |      IFS_EXT4_FEATURE_RO_COMPAT_HUGE_FILE |      IFS_EXT4_FEATURE_RO_COMPAT_GDT_CSUM |      IFS_EXT4_FEATURE_RO_COMPAT_DIR_NLINK |      IFS_EXT4_FEATURE_RO_COMPAT_EXTRA_ISIZE |      IFS_EXT4_FEATURE_RO_COMPAT_QUOTA |      IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC |      IFS_EXT4_FEATURE_RO_COMPAT_METADATA_CSUM |      IFS_EXT4_FEATURE_RO_COMPAT_PROJECT |      IFS_EXT4_FEATURE_RO_COMPAT_VERITY |      IFS_EXT4_FEATURE_RO_COMPAT_ORPHAN_PRESENT)

typedef enum IfsExt4BigallocStatus {
    IFS_EXT4_BIGALLOC_OK = 0,
    IFS_EXT4_BIGALLOC_REQUIRES_EXTENTS,
    IFS_EXT4_BIGALLOC_INVALID_FIRST_DATA_BLOCK
} IfsExt4BigallocStatus;

ifs_ext4_u32 ifs_ext4_unsupported_incompat_features(
    ifs_ext4_u32 feature_incompat);

ifs_ext4_u32 ifs_ext4_unsupported_ro_compat_features(
    ifs_ext4_u32 feature_ro_compat);

int ifs_ext4_requires_readonly(ifs_ext4_u32 feature_ro_compat);

IfsExt4BigallocStatus ifs_ext4_validate_bigalloc(
    ifs_ext4_u32 feature_incompat,
    ifs_ext4_u32 feature_ro_compat,
    ifs_ext4_u32 first_data_block);

typedef enum IfsExt4InodeGeometryStatus {
    IFS_EXT4_INODE_GEOMETRY_OK = 0,
    IFS_EXT4_INODE_GEOMETRY_INVALID_FIRST_INODE,
    IFS_EXT4_INODE_GEOMETRY_INVALID_INODE_SIZE
} IfsExt4InodeGeometryStatus;

typedef enum IfsExt4GroupGeometryStatus {
    IFS_EXT4_GROUP_GEOMETRY_OK = 0,
    IFS_EXT4_GROUP_GEOMETRY_INVALID_DESCRIPTOR_SIZE,
    IFS_EXT4_GROUP_GEOMETRY_ZERO_VALUE,
    IFS_EXT4_GROUP_GEOMETRY_INVALID_INODES_PER_GROUP
} IfsExt4GroupGeometryStatus;

typedef enum IfsExt4ClusterGeometryStatus {
    IFS_EXT4_CLUSTER_GEOMETRY_OK = 0,
    IFS_EXT4_CLUSTER_GEOMETRY_CLUSTER_SMALLER_THAN_BLOCK,
    IFS_EXT4_CLUSTER_GEOMETRY_CLUSTER_BLOCK_MISMATCH,
    IFS_EXT4_CLUSTER_GEOMETRY_BLOCKS_PER_GROUP_TOO_LARGE,
    IFS_EXT4_CLUSTER_GEOMETRY_CLUSTERS_PER_GROUP_TOO_LARGE,
    IFS_EXT4_CLUSTER_GEOMETRY_GROUP_RATIO_MISMATCH
} IfsExt4ClusterGeometryStatus;

IfsExt4InodeGeometryStatus ifs_ext4_validate_inode_geometry(
    ifs_ext4_u32 block_size,
    ifs_ext4_u32 inode_size,
    ifs_ext4_u32 first_inode);

IfsExt4GroupGeometryStatus ifs_ext4_validate_group_geometry(
    ifs_ext4_u32 block_size,
    ifs_ext4_u32 inode_size,
    ifs_ext4_u32 descriptor_size,
    int has_64bit,
    ifs_ext4_u32 blocks_per_group,
    ifs_ext4_u32 inodes_per_group);

IfsExt4ClusterGeometryStatus ifs_ext4_validate_cluster_geometry(
    ifs_ext4_u32 block_size,
    ifs_ext4_u32 cluster_size,
    int has_bigalloc,
    ifs_ext4_u32 blocks_per_group,
    ifs_ext4_u32 clusters_per_group);

typedef enum IfsExt4LayoutStatus {
    IFS_EXT4_LAYOUT_OK = 0,
    IFS_EXT4_LAYOUT_RESERVED_GDT_TOO_LARGE,
    IFS_EXT4_LAYOUT_INVALID_FIRST_DATA_BLOCK,
    IFS_EXT4_LAYOUT_INVALID_1K_FIRST_DATA_BLOCK,
    IFS_EXT4_LAYOUT_GROUP_COUNT_TOO_LARGE,
    IFS_EXT4_LAYOUT_INVALID_INODE_COUNT
} IfsExt4LayoutStatus;

IfsExt4LayoutStatus ifs_ext4_validate_layout(
    ifs_ext4_u32 block_size,
    ifs_ext4_u32 reserved_gdt_blocks,
    ifs_ext4_u64 blocks_count,
    ifs_ext4_u32 first_data_block,
    ifs_ext4_u32 log_block_size,
    ifs_ext4_u32 cluster_ratio,
    ifs_ext4_u32 blocks_per_group,
    ifs_ext4_u32 descriptors_per_block,
    ifs_ext4_u32 inodes_per_group,
    ifs_ext4_u32 inodes_count,
    ifs_ext4_u64 *group_count);

#endif
