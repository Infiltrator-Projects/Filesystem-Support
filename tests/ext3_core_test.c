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

    {
        ifs_ext3_u16 encoded = 0U;

        if (ifs_ext3_directory_record_length_from_disk(0xFFFFU, 65536U) != 65536U)
            return fail("64K directory record was not decoded");
        if (ifs_ext3_directory_record_length_to_disk(65536U, 65536U, &encoded) != 0 ||
            encoded != 0xFFFFU)
            return fail("64K directory record was not encoded");
        if (ifs_ext3_validate_directory_record(0U, 12U, 1U, 2U, 4096U, 100U) !=
            IFS_EXT3_DIRECTORY_RECORD_OK)
            return fail("valid directory record was rejected");
        if (ifs_ext3_validate_directory_record(4090U, 12U, 1U, 2U, 4096U, 100U) !=
            IFS_EXT3_DIRECTORY_RECORD_CROSSES_BLOCK)
            return fail("cross-block directory record was accepted");
        if (ifs_ext3_validate_directory_record(0U, 12U, 1U, 101U, 4096U, 100U) !=
            IFS_EXT3_DIRECTORY_RECORD_INODE_RANGE)
            return fail("out-of-range directory inode was accepted");
    }


    {
        ifs_ext3_u32 offsets[4] = { 0U, 0U, 0U, 0U };
        ifs_ext3_u32 boundary = 0U;
        const ifs_ext3_u32 ptrs = 1024U;
        const ifs_ext3_u32 bits = 10U;

        if (ifs_ext3_indirect_block_path(0U, ptrs, bits, offsets, &boundary) != 1 ||
            offsets[0] != 0U || boundary != 11U)
            return fail("direct block path is wrong");
        if (ifs_ext3_indirect_block_path(12U, ptrs, bits, offsets, &boundary) != 2 ||
            offsets[0] != IFS_EXT3_IND_BLOCK || offsets[1] != 0U)
            return fail("single-indirect block path is wrong");
        if (ifs_ext3_indirect_block_path(12U + ptrs, ptrs, bits, offsets, &boundary) != 3 ||
            offsets[0] != IFS_EXT3_DIND_BLOCK || offsets[1] != 0U || offsets[2] != 0U)
            return fail("double-indirect block path is wrong");
        if (ifs_ext3_indirect_block_path(
                12ULL + ptrs + (ifs_ext3_u64)ptrs * ptrs,
                ptrs, bits, offsets, &boundary) != 4 ||
            offsets[0] != IFS_EXT3_TIND_BLOCK ||
            offsets[1] != 0U || offsets[2] != 0U || offsets[3] != 0U)
            return fail("triple-indirect block path is wrong");
        if (ifs_ext3_indirect_block_path(
                12ULL + ptrs + (ifs_ext3_u64)ptrs * ptrs +
                    (ifs_ext3_u64)ptrs * ptrs * ptrs,
                ptrs, bits, offsets, &boundary) != 0)
            return fail("out-of-range indirect block was accepted");
    }


    if (ifs_ext3_validate_journal_header(
            IFS_EXT3_JOURNAL_MAGIC,
            IFS_EXT3_JOURNAL_DESCRIPTOR_BLOCK,
            1U) != IFS_EXT3_JOURNAL_OK)
        return fail("journal header validation failed");

    if (ifs_ext3_validate_journal_header(
            0U, IFS_EXT3_JOURNAL_DESCRIPTOR_BLOCK, 1U) !=
        IFS_EXT3_JOURNAL_BAD_MAGIC)
        return fail("bad journal magic was accepted");

    if (ifs_ext3_validate_journal_superblock(
            4096U, 32768U, 1U, 2U,
            IFS_EXT3_JOURNAL_FEATURE_INCOMPAT_REVOKE) !=
        IFS_EXT3_JOURNAL_OK)
        return fail("valid journal superblock was rejected");

    if (ifs_ext3_validate_journal_superblock(
            4096U, 32768U, 1U, 2U, 0x80000000U) !=
        IFS_EXT3_JOURNAL_UNSUPPORTED_FEATURE)
        return fail("unsupported journal feature was accepted");

    return 0;
}
