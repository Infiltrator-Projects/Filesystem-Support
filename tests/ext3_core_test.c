/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ext3_core.h"

#include <stdio.h>

static int fail(const char *message)
{
    fprintf(stderr, "ext3 core test: %s\n", message);
    return 1;
}

int main(void)
{
    ifs_ext3_u32 group_count = 0U;

    if (ifs_ext3_unsupported_incompat_features(
            IFS_EXT3_FEATURE_INCOMPAT_SUPPORTED) != 0U)
        return fail("supported incompat features were rejected");

    if (ifs_ext3_unsupported_incompat_features(
            IFS_EXT3_FEATURE_INCOMPAT_SUPPORTED | 0x80000000U) !=
        0x80000000U)
        return fail("unknown incompat feature was not reported");

    if (ifs_ext3_unsupported_ro_compat_features(
            IFS_EXT3_FEATURE_RO_COMPAT_SUPPORTED) != 0U)
        return fail("supported read-only-compatible features were rejected");

    if (ifs_ext3_unsupported_ro_compat_features(
            IFS_EXT3_FEATURE_RO_COMPAT_SUPPORTED | 0x40000000U) !=
        0x40000000U)
        return fail("unknown read-only-compatible feature was not reported");

    if (ifs_ext3_validate_geometry(4096U, 256U, 4096U,
                                   32768U, 32768U, 8192U) !=
        IFS_EXT3_GEOMETRY_OK)
        return fail("valid geometry was rejected");

    if (ifs_ext3_validate_geometry(4096U, 192U, 4096U,
                                   32768U, 32768U, 8192U) !=
        IFS_EXT3_GEOMETRY_INVALID_INODE_SIZE)
        return fail("non-power-of-two inode size was accepted");

    if (ifs_ext3_validate_geometry(4096U, 8192U, 4096U,
                                   32768U, 32768U, 8192U) !=
        IFS_EXT3_GEOMETRY_INVALID_INODE_SIZE)
        return fail("inode larger than block size was accepted");

    if (ifs_ext3_validate_geometry(4096U, 256U, 2048U,
                                   32768U, 32768U, 8192U) !=
        IFS_EXT3_GEOMETRY_FRAGMENT_SIZE_MISMATCH)
        return fail("fragment size mismatch was accepted");

    if (ifs_ext3_validate_geometry(4096U, 256U, 4096U,
                                   0U, 32768U, 8192U) !=
        IFS_EXT3_GEOMETRY_ZERO_GROUP_VALUE)
        return fail("zero blocks per group was accepted");

    if (ifs_ext3_validate_geometry(4096U, 256U, 4096U,
                                   32769U, 32768U, 8192U) !=
        IFS_EXT3_GEOMETRY_BLOCKS_PER_GROUP_TOO_LARGE)
        return fail("oversized block bitmap geometry was accepted");

    if (ifs_ext3_validate_geometry(4096U, 256U, 4096U,
                                   32768U, 32769U, 8192U) !=
        IFS_EXT3_GEOMETRY_FRAGMENTS_PER_GROUP_TOO_LARGE)
        return fail("oversized fragment bitmap geometry was accepted");

    if (ifs_ext3_validate_geometry(4096U, 256U, 4096U,
                                   32768U, 32768U, 32769U) !=
        IFS_EXT3_GEOMETRY_INODES_PER_GROUP_TOO_LARGE)
        return fail("oversized inode bitmap geometry was accepted");

    if (ifs_ext3_compute_group_count(100000U, 1U, 32768U,
                                     &group_count) != IFS_EXT3_LAYOUT_OK ||
        group_count != 4U)
        return fail("valid group count was not computed correctly");

    if (ifs_ext3_compute_group_count(100U, 100U, 32768U,
                                     &group_count) !=
        IFS_EXT3_LAYOUT_INVALID_FIRST_DATA_BLOCK)
        return fail("first data block beyond filesystem was accepted");

    if (ifs_ext3_compute_group_count(100U, 1U, 0U,
                                     &group_count) !=
        IFS_EXT3_LAYOUT_INVALID_BLOCKS_PER_GROUP)
        return fail("zero blocks per group was accepted by layout arithmetic");

    return 0;
}
