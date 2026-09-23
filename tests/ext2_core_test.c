/* SPDX-License-Identifier: GPL-2.0-only */
#include "../native/filesystems/ext2/core/ext2_core.h"

#include <stdio.h>
#include <string.h>

static void store_le16(unsigned char *p, unsigned int v)
{
    p[0] = (unsigned char)(v & 0xffU);
    p[1] = (unsigned char)((v >> 8) & 0xffU);
}

static void store_le32(unsigned char *p, unsigned long v)
{
    p[0] = (unsigned char)(v & 0xffU);
    p[1] = (unsigned char)((v >> 8) & 0xffU);
    p[2] = (unsigned char)((v >> 16) & 0xffU);
    p[3] = (unsigned char)((v >> 24) & 0xffU);
}

static int fail(const char *message)
{
    fprintf(stderr, "ext2_core_test: %s\n", message);
    return 1;
}

int main(void)
{
    unsigned char raw[IFS_EXT2_SUPERBLOCK_SIZE] = {0};
    unsigned char gd_raw[32] = {0};
    IfsExt2Superblock sb;
    IfsExt2GroupDescriptor gd;
    IfsExt2BlockPath path;

    store_le32(raw + 0x00U, 8192U);
    store_le32(raw + 0x04U, 8193U);
    store_le32(raw + 0x0CU, 7000U);
    store_le32(raw + 0x10U, 8000U);
    store_le32(raw + 0x14U, 1U);
    store_le32(raw + 0x18U, 0U);
    store_le32(raw + 0x1CU, 0U);
    store_le32(raw + 0x20U, 8192U);
    store_le32(raw + 0x24U, 8192U);
    store_le32(raw + 0x28U, 8192U);
    store_le16(raw + 0x38U, IFS_EXT2_SUPER_MAGIC);
    store_le16(raw + 0x3AU, 1U);
    store_le32(raw + 0x4CU, 1U);
    store_le32(raw + 0x54U, 11U);
    store_le16(raw + 0x58U, 128U);
    store_le32(raw + 0x5CU, IFS_EXT2_FEATURE_COMPAT_EXT_ATTR);
    store_le32(raw + 0x60U, IFS_EXT2_FEATURE_INCOMPAT_FILETYPE);
    store_le32(raw + 0x64U, IFS_EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER);

    if (ifs_ext2_decode_superblock(raw, sizeof(raw), &sb) != IFS_EXT2_OK)
        return fail("valid superblock did not decode");
    if (ifs_ext2_validate_superblock(&sb, 8193U, 1) != IFS_EXT2_OK)
        return fail("valid superblock geometry was rejected");
    if (sb.block_size != 1024U || sb.group_count != 1U ||
        sb.inode_table_blocks_per_group != 1024U)
        return fail("derived superblock geometry is wrong");

    store_le32(gd_raw + 0x00U, 3U);
    store_le32(gd_raw + 0x04U, 4U);
    store_le32(gd_raw + 0x08U, 5U);
    if (ifs_ext2_decode_group_descriptor(gd_raw, sizeof(gd_raw), &gd) != IFS_EXT2_OK ||
        ifs_ext2_validate_group_descriptor(&sb, 0U, &gd) != IFS_EXT2_OK)
        return fail("valid group descriptor was rejected");

    if (ifs_ext2_block_to_path(1024U, 0U, &path) != IFS_EXT2_OK ||
        path.depth != 1U || path.offsets[0] != 0U || path.boundary != 11U)
        return fail("direct block path is wrong");
    if (ifs_ext2_block_to_path(1024U, 12U, &path) != IFS_EXT2_OK ||
        path.depth != 2U || path.offsets[0] != 12U || path.offsets[1] != 0U)
        return fail("single-indirect block path is wrong");
    if (ifs_ext2_block_to_path(1024U, 12U + 256U, &path) != IFS_EXT2_OK ||
        path.depth != 3U || path.offsets[0] != 13U)
        return fail("double-indirect block path is wrong");
    if (ifs_ext2_block_to_path(1024U,
            12ULL + 256ULL + 256ULL * 256ULL, &path) != IFS_EXT2_OK ||
        path.depth != 4U || path.offsets[0] != 14U)
        return fail("triple-indirect block path is wrong");

    store_le32(raw + 0x5CU,
               IFS_EXT2_FEATURE_COMPAT_EXT_ATTR |
               IFS_EXT2_FEATURE_COMPAT_HAS_JOURNAL);
    if (ifs_ext2_decode_superblock(raw, sizeof(raw), &sb) != IFS_EXT2_OK ||
        ifs_ext2_validate_superblock(&sb, 8193U, 0) !=
            IFS_EXT2_ERROR_JOURNALLED)
        return fail("journalled media was accepted as EXT2");

    return 0;
}
