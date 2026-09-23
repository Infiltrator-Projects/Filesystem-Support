/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ext4_core.h"

#include <stdio.h>

static int fail(const char *message)
{
    fprintf(stderr, "ext4 core test: %s\n", message);
    return 1;
}

int main(void)
{
    ifs_ext4_u32 group_count = 0U;

    if (ifs_ext4_unsupported_incompat_features(
            IFS_EXT4_FEATURE_INCOMPAT_SUPPORTED) != 0U)
        return fail("supported incompat features were rejected");

    if (ifs_ext4_unsupported_incompat_features(
            IFS_EXT4_FEATURE_INCOMPAT_SUPPORTED | 0x80000000U) !=
        0x80000000U)
        return fail("unknown incompat feature was not reported");

    if (ifs_ext4_unsupported_ro_compat_features(
            IFS_EXT4_FEATURE_RO_COMPAT_SUPPORTED |
            IFS_EXT4_FEATURE_RO_COMPAT_READONLY) != 0U)
        return fail("supported read-only-compatible features were rejected");

    if (!ifs_ext4_requires_readonly(IFS_EXT4_FEATURE_RO_COMPAT_READONLY))
        return fail("read-only filesystem feature was not recognised");

    if (ifs_ext4_validate_bigalloc(
            0U, IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC, 0U) !=
        IFS_EXT4_BIGALLOC_REQUIRES_EXTENTS)
        return fail("bigalloc without extents was accepted");

    if (ifs_ext4_validate_bigalloc(
            IFS_EXT4_FEATURE_INCOMPAT_EXTENTS,
            IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC, 1U) !=
        IFS_EXT4_BIGALLOC_INVALID_FIRST_DATA_BLOCK)
        return fail("invalid bigalloc first_data_block was accepted");

    if (ifs_ext4_validate_bigalloc(
            IFS_EXT4_FEATURE_INCOMPAT_EXTENTS,
            IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC, 0U) !=
        IFS_EXT4_BIGALLOC_OK)
        return fail("valid bigalloc feature combination was rejected");

    if (ifs_ext4_validate_inode_geometry(4096U, 256U, 11U) !=
        IFS_EXT4_INODE_GEOMETRY_OK)
        return fail("valid inode geometry was rejected");

    if (ifs_ext4_validate_inode_geometry(4096U, 256U, 10U) !=
        IFS_EXT4_INODE_GEOMETRY_INVALID_FIRST_INODE)
        return fail("invalid first inode was accepted");

    if (ifs_ext4_validate_inode_geometry(4096U, 192U, 11U) !=
        IFS_EXT4_INODE_GEOMETRY_INVALID_INODE_SIZE)
        return fail("invalid inode size was accepted");

    if (ifs_ext4_validate_group_geometry(
            4096U, 256U, 64U, 1, 32768U, 8192U) !=
        IFS_EXT4_GROUP_GEOMETRY_OK)
        return fail("valid block-group geometry was rejected");

    if (ifs_ext4_validate_group_geometry(
            4096U, 256U, 48U, 1, 32768U, 8192U) !=
        IFS_EXT4_GROUP_GEOMETRY_INVALID_DESCRIPTOR_SIZE)
        return fail("invalid 64-bit descriptor size was accepted");

    if (ifs_ext4_validate_group_geometry(
            4096U, 256U, 64U, 1, 0U, 8192U) !=
        IFS_EXT4_GROUP_GEOMETRY_ZERO_VALUE)
        return fail("zero blocks per group was accepted");

    if (ifs_ext4_validate_group_geometry(
            4096U, 256U, 64U, 1, 32768U, 8U) !=
        IFS_EXT4_GROUP_GEOMETRY_INVALID_INODES_PER_GROUP)
        return fail("too few inodes per group was accepted");

    if (ifs_ext4_validate_cluster_geometry(
            4096U, 4096U, 0, 32768U, 32768U) !=
        IFS_EXT4_CLUSTER_GEOMETRY_OK)
        return fail("valid non-bigalloc cluster geometry was rejected");

    if (ifs_ext4_validate_cluster_geometry(
            4096U, 2048U, 1, 32768U, 65536U) !=
        IFS_EXT4_CLUSTER_GEOMETRY_CLUSTER_SMALLER_THAN_BLOCK)
        return fail("bigalloc cluster smaller than block was accepted");

    if (ifs_ext4_validate_cluster_geometry(
            4096U, 8192U, 1, 32768U, 8192U) !=
        IFS_EXT4_CLUSTER_GEOMETRY_GROUP_RATIO_MISMATCH)
        return fail("inconsistent cluster/group ratio was accepted");

    if (ifs_ext4_validate_layout(
            4096U, 0U, 131072U, 1U, 2U, 1U,
            32768U, 128U, 8192U, 32768U, &group_count) !=
        IFS_EXT4_LAYOUT_OK || group_count != 4U)
        return fail("valid EXT4 layout was rejected or miscounted");

    if (ifs_ext4_validate_layout(
            4096U, 1025U, 131072U, 1U, 2U, 1U,
            32768U, 128U, 8192U, 32768U, &group_count) !=
        IFS_EXT4_LAYOUT_RESERVED_GDT_TOO_LARGE)
        return fail("oversized reserved GDT was accepted");

    if (ifs_ext4_validate_layout(
            4096U, 0U, 100U, 100U, 2U, 1U,
            32768U, 128U, 8192U, 8192U, &group_count) !=
        IFS_EXT4_LAYOUT_INVALID_FIRST_DATA_BLOCK)
        return fail("first data block beyond filesystem was accepted");

    if (ifs_ext4_validate_layout(
            1024U, 0U, 32768U, 0U, 0U, 1U,
            32768U, 32U, 8192U, 8192U, &group_count) !=
        IFS_EXT4_LAYOUT_INVALID_1K_FIRST_DATA_BLOCK)
        return fail("invalid 1K first-data-block layout was accepted");

    if (ifs_ext4_validate_layout(
            4096U, 0U, 131072U, 1U, 2U, 1U,
            32768U, 128U, 8192U, 123U, &group_count) !=
        IFS_EXT4_LAYOUT_INVALID_INODE_COUNT)
        return fail("inconsistent inode total was accepted");

    return 0;
}
