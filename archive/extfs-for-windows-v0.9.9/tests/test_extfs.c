// SPDX-License-Identifier: GPL-3.0-or-later
#include "extfs/extfs.h"

#include <stdio.h>
#include <string.h>

#define TEST_IMAGE_SIZE (2048U * 1024U)
#define EXTFS_TEST_EXTENTS_FLAG 0x00080000U

typedef struct memory_image {
    extfs_u8 bytes[TEST_IMAGE_SIZE];
    extfs_u32 write_attempts;
    extfs_u32 fail_write_number;
    extfs_u32 flush_attempts;
    extfs_u32 fail_flush_number;
    extfs_u32 time_attempts;
    extfs_u32 fail_time;
    extfs_u64 time_seconds;
    extfs_u32 time_nanoseconds;
} memory_image;

static void store_le16(extfs_u8 *p, extfs_u16 value)
{
    p[0] = (extfs_u8)value;
    p[1] = (extfs_u8)(value >> 8);
}

static void store_le32(extfs_u8 *p, extfs_u32 value)
{
    p[0] = (extfs_u8)value;
    p[1] = (extfs_u8)(value >> 8);
    p[2] = (extfs_u8)(value >> 16);
    p[3] = (extfs_u8)(value >> 24);
}

static void store_be32(extfs_u8 *p, extfs_u32 value)
{
    p[0] = (extfs_u8)(value >> 24);
    p[1] = (extfs_u8)(value >> 16);
    p[2] = (extfs_u8)(value >> 8);
    p[3] = (extfs_u8)value;
}

static extfs_u16 load_be16(const extfs_u8 *p)
{
    return (extfs_u16)(((extfs_u16)p[0] << 8) | (extfs_u16)p[1]);
}

static extfs_u32 load_be32(const extfs_u8 *p)
{
    return ((extfs_u32)p[0] << 24) |
           ((extfs_u32)p[1] << 16) |
           ((extfs_u32)p[2] << 8) |
           (extfs_u32)p[3];
}

static extfs_u64 load_be64(const extfs_u8 *p)
{
    return ((extfs_u64)load_be32(p) << 32) |
           (extfs_u64)load_be32(p + 4U);
}

static extfs_u32 test_crc32c(extfs_u32 crc,
                             const extfs_u8 *data,
                             extfs_u32 length)
{
    extfs_u32 i;
    while (length != 0U) {
        crc ^= (extfs_u32)*data++;
        for (i = 0U; i < 8U; ++i) {
            extfs_u32 mask = (extfs_u32)(0U - (crc & 1U));
            crc = (crc >> 1) ^ (0x82F63B78U & mask);
        }
        --length;
    }
    return crc;
}

static extfs_u32 test_ext4_seed(const extfs_u8 *sb)
{
    return test_crc32c(0xFFFFFFFFU, sb + 0x68U, 16U);
}

static void test_ext4_inode_checksum(const extfs_u8 *sb,
                                     extfs_u32 inode_number,
                                     extfs_u8 *inode,
                                     extfs_u32 inode_size)
{
    extfs_u8 inum[4];
    extfs_u8 zeros[2] = {0U, 0U};
    extfs_u32 crc;
    store_le32(inum, inode_number);
    store_le16(inode + 0x7CU, 0U);
    crc = test_crc32c(test_ext4_seed(sb), inum, 4U);
    crc = test_crc32c(crc, inode + 0x64U, 4U);
    crc = test_crc32c(crc, inode, 0x7CU);
    crc = test_crc32c(crc, zeros, 2U);
    if (inode_size > 0x7EU)
        crc = test_crc32c(crc, inode + 0x7EU, inode_size - 0x7EU);
    store_le16(inode + 0x7CU, (extfs_u16)crc);
}

static void test_ext4_group_checksums(const extfs_u8 *sb,
                                      extfs_u32 group,
                                      extfs_u8 *descriptor,
                                      const extfs_u8 *block_bitmap,
                                      const extfs_u8 *inode_bitmap)
{
    extfs_u8 group_le[4];
    extfs_u8 zeros[2] = {0U, 0U};
    extfs_u32 seed = test_ext4_seed(sb);
    extfs_u32 bpg = (extfs_u32)sb[0x20U] |
                    ((extfs_u32)sb[0x21U] << 8) |
                    ((extfs_u32)sb[0x22U] << 16) |
                    ((extfs_u32)sb[0x23U] << 24);
    extfs_u32 crc;
    store_le32(group_le, group);
    crc = test_crc32c(seed, block_bitmap, bpg / 8U);
    store_le16(descriptor + 0x18U, (extfs_u16)crc);
    crc = test_crc32c(seed, inode_bitmap,
                      (((extfs_u32)sb[0x28U] |
                        ((extfs_u32)sb[0x29U] << 8) |
                        ((extfs_u32)sb[0x2AU] << 16) |
                        ((extfs_u32)sb[0x2BU] << 24)) / 8U));
    store_le16(descriptor + 0x1AU, (extfs_u16)crc);
    store_le16(descriptor + 0x1EU, 0U);
    crc = test_crc32c(seed, group_le, 4U);
    crc = test_crc32c(crc, descriptor, 0x1EU);
    crc = test_crc32c(crc, zeros, 2U);
    store_le16(descriptor + 0x1EU, (extfs_u16)crc);
}

static void test_ext4_super_checksum(extfs_u8 *sb)
{
    store_le32(sb + 0x3FCU, test_crc32c(0xFFFFFFFFU, sb, 0x3FCU));
}

static void test_ext4_extent_block_checksum(const extfs_u8 *sb,
                                             extfs_u32 inode_number,
                                             extfs_u32 inode_generation,
                                             extfs_u8 *block,
                                             extfs_u32 block_size)
{
    extfs_u8 inum[4];
    extfs_u8 generation[4];
    extfs_u32 maximum = (extfs_u32)block[4] |
                        ((extfs_u32)block[5] << 8);
    extfs_u32 tail_offset = 12U + maximum * 12U;
    extfs_u32 crc;
    store_le32(inum, inode_number);
    store_le32(generation, inode_generation);
    if (tail_offset > block_size - 4U) return;
    store_le32(block + tail_offset, 0U);
    crc = test_crc32c(test_ext4_seed(sb), inum, 4U);
    crc = test_crc32c(crc, generation, 4U);
    crc = test_crc32c(crc, block, tail_offset);
    store_le32(block + tail_offset, crc);
}

static int memory_read(void *user,
                       extfs_u64 offset,
                       void *destination,
                       extfs_u32 count)
{
    memory_image *image = (memory_image *)user;
    if (offset > sizeof(image->bytes) ||
        (extfs_u64)count > sizeof(image->bytes) - offset) {
        return -1;
    }
    memcpy(destination, image->bytes + (size_t)offset, count);
    return 0;
}

static int memory_write(void *user,
                        extfs_u64 offset,
                        const void *source,
                        extfs_u32 count)
{
    memory_image *image = (memory_image *)user;
    if (offset > sizeof(image->bytes) ||
        (extfs_u64)count > sizeof(image->bytes) - offset) {
        return -1;
    }
    ++image->write_attempts;
    if (image->fail_write_number != 0U &&
        image->write_attempts == image->fail_write_number) {
        return -1;
    }
    memcpy(image->bytes + (size_t)offset, source, count);
    return 0;
}

static int memory_flush(void *user)
{
    memory_image *image = (memory_image *)user;
    ++image->flush_attempts;
    if (image->fail_flush_number != 0U &&
        image->flush_attempts == image->fail_flush_number) {
        return -1;
    }
    return 0;
}

static int memory_time(void *user, extfs_u64 *seconds,
                       extfs_u32 *nanoseconds)
{
    memory_image *image = (memory_image *)user;
    ++image->time_attempts;
    if (image->fail_time != 0U || seconds == NULL || nanoseconds == NULL)
        return -1;
    *seconds = image->time_seconds;
    *nanoseconds = image->time_nanoseconds;
    return 0;
}

static int expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", message);
        return 1;
    }
    return 0;
}

static int count_directory_entry(void *user,
                                 extfs_u32 inode_number,
                                 extfs_node_type type,
                                 const char *name,
                                 extfs_u8 name_length)
{
    int *count = (int *)user;
    (void)inode_number;
    (void)type;
    (void)name;
    (void)name_length;
    ++*count;
    return 0;
}

static extfs_u8 *prepare_ext2(memory_image *image)
{
    extfs_u8 *sb;
    memset(image, 0, sizeof(*image));
    image->time_seconds = 1700000000ULL;
    image->time_nanoseconds = 123456700U;
    sb = image->bytes + 1024U;
    store_le32(sb + 0x00U, 128U);
    store_le32(sb + 0x04U, 1024U);
    store_le32(sb + 0x0CU, 100U);
    store_le32(sb + 0x14U, 1U);
    store_le32(sb + 0x18U, 0U);
    store_le32(sb + 0x20U, 8192U);
    store_le32(sb + 0x28U, 128U);
    store_le16(sb + 0x38U, 0xEF53U);
    store_le16(sb + 0x3AU, 0x0001U);
    store_le32(sb + 0x4CU, 0U);
    memcpy(sb + 0x78U, "UNIT-EXT2", 9U);
    return sb;
}

static void prepare_ext2_direct_resize_image(memory_image *image)
{
    extfs_u8 *sb = prepare_ext2(image);
    extfs_u8 *descriptor;
    extfs_u8 *bitmap;
    extfs_u8 *inode;

    /* One 15-block group: blocks 1..15.  Metadata occupies 1..5 and the
     * initial four-byte regular file uses block 8. */
    store_le32(sb + 0x00U, 8U);
    store_le32(sb + 0x04U, 16U);
    store_le32(sb + 0x0CU, 9U);
    store_le32(sb + 0x20U, 16U);
    store_le32(sb + 0x28U, 8U);

    descriptor = image->bytes + 2U * 1024U;
    store_le32(descriptor + 0x00U, 3U); /* block bitmap */
    store_le32(descriptor + 0x04U, 4U); /* inode bitmap */
    store_le32(descriptor + 0x08U, 5U); /* inode table */
    store_le16(descriptor + 0x0CU, 9U);

    bitmap = image->bytes + 3U * 1024U;
    bitmap[0] = 0x9FU; /* blocks 1..5 and 8 allocated */

    inode = image->bytes + 5U * 1024U + 128U; /* inode 2 */
    store_le16(inode + 0x00U, 0x8000U);
    store_le32(inode + 0x04U, 4U);
    store_le16(inode + 0x1AU, 1U);
    store_le32(inode + 0x1CU, 2U); /* one 1 KiB block = two sectors */
    store_le32(inode + 0x28U, 8U);
    memset(image->bytes + 8U * 1024U, 'Q', 1024U);
    memcpy(image->bytes + 8U * 1024U, "DATA", 4U);
}

static void prepare_ext3_journal_image(memory_image *image)
{
    static const extfs_u8 journal_uuid[16] = {
        'J','B','D','2','-','U','N','I','T','-','0','0','0','0','0','1'
    };
    extfs_u8 *sb = prepare_ext2(image);
    extfs_u8 *descriptor;
    extfs_u8 *bitmap;
    extfs_u8 *inode;
    extfs_u8 *jsb;
    extfs_u32 i;

    /* One ordinary group. The synthetic journal declares the real JBD2
     * minimum length (1024 blocks); only the first few logical journal blocks
     * need physical mappings for these single-transaction tests. */
    store_le32(sb + 0x00U, 16U);
    store_le32(sb + 0x04U, 1536U);
    store_le32(sb + 0x0CU, 1516U);
    store_le32(sb + 0x20U, 2048U);
    store_le32(sb + 0x28U, 16U);
    store_le32(sb + 0x4CU, 1U);
    store_le16(sb + 0x58U, 128U);
    store_le32(sb + 0x5CU, 0x00000004U); /* HAS_JOURNAL */
    for (i = 0U; i < 16U; ++i) sb[0x68U + i] = (extfs_u8)(0x10U + i);
    memcpy(sb + 0xD0U, journal_uuid, 16U);
    store_le32(sb + 0xE0U, 8U);          /* internal journal inode */
    memcpy(sb + 0x78U, "UNIT-EXT3", 9U);

    descriptor = image->bytes + 2U * 1024U;
    store_le32(descriptor + 0x00U, 3U);
    store_le32(descriptor + 0x04U, 4U);
    store_le32(descriptor + 0x08U, 5U);  /* inode table */
    store_le16(descriptor + 0x0CU, 1516U);

    /* Mark primary metadata, the first twelve journal blocks and one ordinary
     * four-byte test file as allocated. */
    bitmap = image->bytes + 3U * 1024U;
    for (i = 1U; i <= 6U; ++i)
        bitmap[(i - 1U) >> 3] |= (extfs_u8)(1U << ((i - 1U) & 7U));
    for (i = 20U; i <= 31U; ++i)
        bitmap[(i - 1U) >> 3] |= (extfs_u8)(1U << ((i - 1U) & 7U));
    bitmap[(50U - 1U) >> 3] |=
        (extfs_u8)(1U << ((50U - 1U) & 7U));

    inode = image->bytes + 5U * 1024U + 128U; /* inode 2 */
    store_le16(inode + 0x00U, 0x8000U);
    store_le32(inode + 0x04U, 4U);
    store_le16(inode + 0x1AU, 1U);
    store_le32(inode + 0x1CU, 2U);
    store_le32(inode + 0x28U, 50U);
    memset(image->bytes + 50U * 1024U, 'Q', 1024U);
    memcpy(image->bytes + 50U * 1024U, "DATA", 4U);

    inode = image->bytes + 5U * 1024U + 7U * 128U; /* inode 8 */
    store_le16(inode + 0x00U, 0x8000U);
    store_le32(inode + 0x04U, 1024U * 1024U);
    store_le16(inode + 0x1AU, 1U);
    store_le32(inode + 0x1CU, 2048U);
    for (i = 0U; i < 12U; ++i) {
        store_le32(inode + 0x28U + i * 4U, 20U + i);
    }

    jsb = image->bytes + 20U * 1024U;
    store_be32(jsb + 0x00U, 0xC03B3998U);
    store_be32(jsb + 0x04U, 4U);         /* JBD2 superblock v2 */
    store_be32(jsb + 0x08U, 0U);
    store_be32(jsb + 0x0CU, 1024U);
    store_be32(jsb + 0x10U, 1024U);
    store_be32(jsb + 0x14U, 1U);
    store_be32(jsb + 0x18U, 7U);
    store_be32(jsb + 0x1CU, 0U);         /* clean journal */
    store_be32(jsb + 0x24U, 0U);
    store_be32(jsb + 0x28U, 0x00000010U); /* checksum v3 */
    store_be32(jsb + 0x2CU, 0U);
    memcpy(jsb + 0x30U, journal_uuid, 16U);
    store_be32(jsb + 0x40U, 1U);
    jsb[0x50U] = 4U;                     /* CRC32C */
    store_be32(jsb + 0x58U, 1U);         /* clean head */
    store_be32(jsb + 0xFCU, 0U);
    store_be32(jsb + 0xFCU, test_crc32c(0xFFFFFFFFU, jsb, 1024U));

    memset(image->bytes + 40U * 1024U, 0xA5, 1024U);
}

static void prepare_ext4_journal_extent_image(memory_image *image)
{
    extfs_u8 *sb;
    extfs_u8 *descriptor;
    extfs_u8 *bitmap;
    extfs_u8 *inode_bitmap;
    extfs_u8 *inode;

    prepare_ext3_journal_image(image);
    sb = image->bytes + 1024U;
    descriptor = image->bytes + 2U * 1024U;
    bitmap = image->bytes + 3U * 1024U;
    inode_bitmap = image->bytes + 4U * 1024U;

    store_le32(sb + 0x60U, 0x00000040U); /* EXTENTS */
    store_le32(sb + 0x64U, 0x00000400U); /* METADATA_CSUM */
    sb[0x175U] = 1U;                    /* CRC32C */
    memcpy(sb + 0x78U, "UNIT-EXT4", 9U);

    inode = image->bytes + 5U * 1024U + 128U; /* inode 2 */
    store_le32(inode + 0x20U, EXTFS_TEST_EXTENTS_FLAG);
    memset(inode + 0x28U, 0, 60U);
    store_le16(inode + 0x28U, 0xF30AU);
    store_le16(inode + 0x2AU, 1U);
    store_le16(inode + 0x2CU, 4U);
    store_le16(inode + 0x2EU, 0U);
    store_le32(inode + 0x34U, 0U); /* ee_block */
    store_le16(inode + 0x38U, 1U); /* ee_len */
    store_le16(inode + 0x3AU, 0U); /* ee_start_hi */
    store_le32(inode + 0x3CU, 50U); /* ee_start_lo */

    test_ext4_inode_checksum(sb, 2U, inode, 128U);
    inode = image->bytes + 5U * 1024U + 7U * 128U; /* journal inode 8 */
    test_ext4_inode_checksum(sb, 8U, inode, 128U);
    test_ext4_group_checksums(sb, 0U, descriptor, bitmap, inode_bitmap);
    test_ext4_super_checksum(sb);
}

static void prepare_ext4_fragmented_extent_image(memory_image *image)
{
    extfs_u8 *sb;
    extfs_u8 *descriptor;
    extfs_u8 *bitmap;
    extfs_u8 *inode_bitmap;

    prepare_ext4_journal_extent_image(image);
    sb = image->bytes + 1024U;
    descriptor = image->bytes + 2U * 1024U;
    bitmap = image->bytes + 3U * 1024U;
    inode_bitmap = image->bytes + 4U * 1024U;

    /* Occupy the block immediately after the file so 0.8 must allocate a
     * second inline extent rather than extending physical block 50. */
    bitmap[(51U - 1U) >> 3] |=
        (extfs_u8)(1U << ((51U - 1U) & 7U));
    store_le16(descriptor + 0x0CU, 1515U);
    store_le32(sb + 0x0CU, 1515U);
    memset(image->bytes + 51U * 1024U, 0x5AU, 1024U);

    test_ext4_group_checksums(sb, 0U, descriptor, bitmap, inode_bitmap);
    test_ext4_super_checksum(sb);
}

static void prepare_ext4_four_extent_image(memory_image *image)
{
    extfs_u8 *sb;
    extfs_u8 *descriptor;
    extfs_u8 *bitmap;
    extfs_u8 *inode_bitmap;
    extfs_u8 *inode;
    extfs_u32 physical[4] = {50U, 52U, 54U, 56U};
    extfs_u32 blockers[4] = {51U, 53U, 55U, 57U};
    extfs_u32 i;

    prepare_ext4_journal_extent_image(image);
    sb = image->bytes + 1024U;
    descriptor = image->bytes + 2U * 1024U;
    bitmap = image->bytes + 3U * 1024U;
    inode_bitmap = image->bytes + 4U * 1024U;
    inode = image->bytes + 5U * 1024U + 128U;

    /* Four one-block file extents with occupied neighbours. Growing by one
     * block cannot merge the last extent and therefore forces 0.9 to allocate
     * an external extent leaf plus a fifth data extent. */
    for (i = 0U; i < 4U; ++i) {
        bitmap[(physical[i] - 1U) >> 3] |=
            (extfs_u8)(1U << ((physical[i] - 1U) & 7U));
        bitmap[(blockers[i] - 1U) >> 3] |=
            (extfs_u8)(1U << ((blockers[i] - 1U) & 7U));
        memset(image->bytes + physical[i] * 1024U,
               (int)('A' + (int)i), 1024U);
        memset(image->bytes + blockers[i] * 1024U, 0x5AU, 1024U);
    }
    store_le16(descriptor + 0x0CU, 1509U);
    store_le32(sb + 0x0CU, 1509U);
    store_le32(inode + 0x04U, 4096U);
    store_le32(inode + 0x1CU, 8U);
    memset(inode + 0x28U, 0, 60U);
    store_le16(inode + 0x28U, 0xF30AU);
    store_le16(inode + 0x2AU, 4U);
    store_le16(inode + 0x2CU, 4U);
    store_le16(inode + 0x2EU, 0U);
    for (i = 0U; i < 4U; ++i) {
        extfs_u8 *ex = inode + 0x34U + i * 12U;
        store_le32(ex + 0x00U, i);
        store_le16(ex + 0x04U, 1U);
        store_le16(ex + 0x06U, 0U);
        store_le32(ex + 0x08U, physical[i]);
    }

    test_ext4_inode_checksum(sb, 2U, inode, 128U);
    test_ext4_group_checksums(sb, 0U, descriptor, bitmap, inode_bitmap);
    test_ext4_super_checksum(sb);
}

static void prepare_ext4_empty_extent_image(memory_image *image)
{
    extfs_u8 *sb;
    extfs_u8 *descriptor;
    extfs_u8 *bitmap;
    extfs_u8 *inode_bitmap;
    extfs_u8 *inode;

    prepare_ext4_journal_extent_image(image);
    sb = image->bytes + 1024U;
    descriptor = image->bytes + 2U * 1024U;
    bitmap = image->bytes + 3U * 1024U;
    inode_bitmap = image->bytes + 4U * 1024U;
    inode = image->bytes + 5U * 1024U + 128U;

    bitmap[(50U - 1U) >> 3] &=
        (extfs_u8)~(extfs_u8)(1U << ((50U - 1U) & 7U));
    store_le16(descriptor + 0x0CU, 1517U);
    store_le32(sb + 0x0CU, 1517U);
    store_le32(inode + 0x04U, 0U);
    store_le32(inode + 0x1CU, 0U);
    store_le16(inode + 0x2AU, 0U);
    memset(inode + 0x34U, 0, 48U);

    test_ext4_inode_checksum(sb, 2U, inode, 128U);
    test_ext4_group_checksums(sb, 0U, descriptor, bitmap, inode_bitmap);
    test_ext4_super_checksum(sb);
}

static void prepare_ext4_2k_external_leaf_image(memory_image *image)
{
    extfs_u8 *sb;
    extfs_u8 *descriptor;
    extfs_u8 *bitmap;
    extfs_u8 *inode_bitmap;
    extfs_u8 *inode;
    extfs_u8 *leaf;
    extfs_u32 i;

    memset(image, 0, sizeof(*image));
    image->time_seconds = 1700000000ULL;
    image->time_nanoseconds = 123456700U;
    sb = image->bytes + 1024U;
    store_le32(sb + 0x00U, 16U);
    store_le32(sb + 0x04U, 512U);
    store_le32(sb + 0x0CU, 505U);
    store_le32(sb + 0x14U, 0U);
    store_le32(sb + 0x18U, 1U); /* 2 KiB block size */
    store_le32(sb + 0x20U, 1024U);
    store_le32(sb + 0x28U, 16U);
    store_le16(sb + 0x38U, 0xEF53U);
    store_le16(sb + 0x3AU, 0x0001U);
    store_le32(sb + 0x4CU, 1U);
    store_le16(sb + 0x58U, 128U);
    store_le32(sb + 0x60U, 0x00000040U); /* EXTENTS */
    store_le32(sb + 0x64U, 0x00000400U); /* METADATA_CSUM */
    for (i = 0U; i < 16U; ++i) sb[0x68U + i] = (extfs_u8)(0x40U + i);
    memcpy(sb + 0x78U, "UNIT-2K-EXT4", 12U);
    sb[0x175U] = 1U;

    descriptor = image->bytes + 1U * 2048U;
    bitmap = image->bytes + 2U * 2048U;
    inode_bitmap = image->bytes + 3U * 2048U;
    store_le32(descriptor + 0x00U, 2U);
    store_le32(descriptor + 0x04U, 3U);
    store_le32(descriptor + 0x08U, 4U);
    store_le16(descriptor + 0x0CU, 505U);
    for (i = 0U; i <= 4U; ++i)
        bitmap[i >> 3] |= (extfs_u8)(1U << (i & 7U));
    bitmap[6U >> 3] |= (extfs_u8)(1U << (6U & 7U));
    bitmap[7U >> 3] |= (extfs_u8)(1U << (7U & 7U));
    inode_bitmap[0] = 0x02U; /* inode 2 */

    inode = image->bytes + 4U * 2048U + 128U;
    store_le16(inode + 0x00U, 0x8000U);
    store_le32(inode + 0x04U, 4U);
    store_le16(inode + 0x1AU, 1U);
    store_le32(inode + 0x1CU, 8U); /* data + one extent block */
    store_le32(inode + 0x20U, EXTFS_TEST_EXTENTS_FLAG);
    memset(inode + 0x28U, 0, 60U);
    store_le16(inode + 0x28U, 0xF30AU);
    store_le16(inode + 0x2AU, 1U);
    store_le16(inode + 0x2CU, 4U);
    store_le16(inode + 0x2EU, 1U);
    store_le32(inode + 0x34U, 0U);
    store_le32(inode + 0x38U, 6U);

    leaf = image->bytes + 6U * 2048U;
    store_le16(leaf + 0x00U, 0xF30AU);
    store_le16(leaf + 0x02U, 1U);
    store_le16(leaf + 0x04U, 169U);
    store_le16(leaf + 0x06U, 0U);
    store_le32(leaf + 0x0CU, 0U);
    store_le16(leaf + 0x10U, 1U);
    store_le16(leaf + 0x12U, 0U);
    store_le32(leaf + 0x14U, 7U);
    test_ext4_extent_block_checksum(sb, 2U, 0U, leaf, 2048U);
    memcpy(image->bytes + 7U * 2048U, "2KOK", 4U);

    test_ext4_inode_checksum(sb, 2U, inode, 128U);
    test_ext4_group_checksums(sb, 0U, descriptor, bitmap, inode_bitmap);
    test_ext4_super_checksum(sb);
}

static void prepare_ext4_timestamp_image(memory_image *image)
{
    extfs_u8 *sb = prepare_ext2(image);
    extfs_u8 *descriptor;
    extfs_u8 *inode;

    store_le32(sb + 0x00U, 8U);
    store_le32(sb + 0x04U, 16U);
    store_le32(sb + 0x0CU, 8U);
    store_le32(sb + 0x28U, 8U);
    store_le32(sb + 0x4CU, 1U);
    store_le16(sb + 0x58U, 256U);
    store_le32(sb + 0x60U, 0x00000040U); /* extents */

    descriptor = image->bytes + 2U * 1024U;
    store_le32(descriptor + 0x08U, 3U);

    inode = image->bytes + 3U * 1024U + 256U; /* inode 2 */
    store_le16(inode + 0x00U, 0x8000U);       /* regular file */
    store_le16(inode + 0x80U, 24U);           /* through i_crtime_extra */

    store_le32(inode + 0x08U, 0xFFFFFFFFU);   /* -1, epoch 0 */
    store_le32(inode + 0x8CU, 0U);

    store_le32(inode + 0x0CU, 0x80000000U);   /* +2147483648, epoch 1 */
    store_le32(inode + 0x84U, (123456789U << 2) | 1U);

    store_le32(inode + 0x10U, 0x80000000U);   /* -2147483648, epoch 0 */
    store_le32(inode + 0x88U, 0U);

    store_le32(inode + 0x90U, 0U);            /* +8589934592, epoch 2 */
    store_le32(inode + 0x94U, (987654321U << 2) | 2U);
}

int main(void)
{
    memory_image image;
    extfs_io io;
    extfs_volume volume;
    extfs_inode inode;
    extfs_u8 *sb;
    extfs_u8 scratch[1024];
    extfs_u8 resize_scratch[8U * 1024U];
    extfs_u8 byte = 0U;
    extfs_u32 bytes_read = 0U;
    extfs_u32 risks;
    extfs_u32 write_risks;
    extfs_journal journal;
    extfs_journal_metadata journal_item;
    extfs_journal_metadata multi_items[2];
    extfs_u8 journal_metadata[1024];
    extfs_u8 journal_metadata_second[1024];
    extfs_u32 journal_risks;
    extfs_u32 commit_checksum;
    extfs_u64 physical;
    int hole;
    int failures = 0;

    io.read_at = memory_read;
    io.write_at = memory_write;
    io.flush = memory_flush;
    io.time_now = memory_time;
    io.user = &image;

    (void)prepare_ext2(&image);
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK,
                       "minimal ext2 superblock opens");
    failures += expect(volume.kind == EXTFS_KIND_EXT2,
                       "minimal image is classified as ext2");
    failures += expect(volume.block_size == 1024U,
                       "1 KiB block size is decoded");
    failures += expect(volume.byte_size == 1024ULL * 1024ULL,
                       "declared filesystem byte size is checked and retained");
    failures += expect(volume.free_blocks == 100U,
                       "free block count is decoded");
    failures += expect(strcmp(volume.label, "UNIT-EXT2") == 0,
                       "volume label is decoded");
    failures += expect(extfs_readonly_assess(&volume, &risks) == EXTFS_OK &&
                       risks == 0U, "clean ext2 passes read-only policy");
    failures += expect(extfs_write_assess(&volume, &write_risks) == EXTFS_OK &&
                       write_risks == 0U,
                       "clean ext2 with writer passes in-place write policy");

    volume.io.write_at = NULL;
    failures += expect(extfs_write_assess(&volume, &write_risks) ==
                       EXTFS_ERR_UNSUPPORTED &&
                       (write_risks & EXTFS_WRITE_RISK_NO_WRITER) != 0U,
                       "write policy requires a host writer");
    volume.io.write_at = memory_write;

    volume.feature_ro_compat = 0x00001000U; /* RO_COMPAT_READONLY */
    failures += expect(extfs_write_assess(&volume, &write_risks) ==
                       EXTFS_ERR_UNSUPPORTED &&
                       (write_risks & EXTFS_WRITE_RISK_UNSUPPORTED_RO_COMPAT) != 0U,
                       "filesystem read-only feature disables writes");
    volume.feature_ro_compat = 0U;

    volume.feature_ro_compat = 0x40000000U; /* deliberately unknown */
    failures += expect(extfs_write_assess(&volume, &write_risks) ==
                       EXTFS_ERR_UNSUPPORTED &&
                       (write_risks & EXTFS_WRITE_RISK_UNSUPPORTED_RO_COMPAT) != 0U,
                       "unknown RO_COMPAT features disable writes");
    volume.feature_ro_compat = 0U;

    volume.feature_incompat = 0x00000100U; /* MMP */
    failures += expect(extfs_write_assess(&volume, &write_risks) ==
                       EXTFS_ERR_UNSUPPORTED &&
                       (write_risks & EXTFS_WRITE_RISK_MMP) != 0U,
                       "MMP volumes remain read-only until ownership is implemented");
    volume.feature_incompat = 0U;

    volume.state = 0U;
    failures += expect(extfs_readonly_assess(&volume, &risks) ==
                       EXTFS_ERR_UNSUPPORTED &&
                       (risks & EXTFS_READONLY_RISK_DIRTY) != 0U,
                       "dirty volume is refused by read-only policy");

    sb = prepare_ext2(&image);
    store_le16(sb + 0x38U, 0U);
    failures += expect(extfs_open(&volume, &io) == EXTFS_ERR_NOT_EXT,
                       "wrong magic is rejected");

    sb = prepare_ext2(&image);
    store_le32(sb + 0x00U, 100U);
    store_le32(sb + 0x28U, 1U);
    failures += expect(extfs_open(&volume, &io) == EXTFS_ERR_CORRUPT,
                       "inode count cannot exceed group inode capacity");

    sb = prepare_ext2(&image);
    store_le32(sb + 0x04U, 3U);
    store_le32(sb + 0x150U, 1U);
    store_le32(sb + 0x20U, 1U);
    store_le32(sb + 0x60U, 0x00000080U); /* 64-bit */
    store_le16(sb + 0xFEU, 64U);
    failures += expect(extfs_open(&volume, &io) == EXTFS_ERR_RANGE,
                       "block-group count cannot truncate to 32 bits");

    sb = prepare_ext2(&image);
    store_le32(sb + 0x04U, 0xFFFFFFFFU);
    store_le32(sb + 0x150U, 0xFFFFFFFFU);
    store_le32(sb + 0x60U, 0x00000080U); /* 64-bit */
    store_le16(sb + 0xFEU, 64U);
    failures += expect(extfs_open(&volume, &io) == EXTFS_ERR_RANGE,
                       "filesystem byte address space cannot overflow u64");

    sb = prepare_ext2(&image);
    store_le32(sb + 0x18U, 2U); /* 4 KiB blocks */
    store_le32(sb + 0x14U, 1U); /* must be zero for >1 KiB blocks */
    failures += expect(extfs_open(&volume, &io) == EXTFS_ERR_CORRUPT,
                       "invalid first-data-block geometry is rejected");

    sb = prepare_ext2(&image);
    store_le32(sb + 0x60U, 0x00000010U); /* meta_bg */
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK,
                       "unsupported meta_bg layout remains inspectable");
    failures += expect(extfs_readonly_assess(&volume, &risks) ==
                       EXTFS_ERR_UNSUPPORTED &&
                       (risks & EXTFS_READONLY_RISK_UNSUPPORTED_LAYOUT) != 0U,
                       "meta_bg traversal is refused by read-only policy");
    failures += expect(extfs_read_inode(&volume, EXTFS_ROOT_INODE, &inode,
                                        scratch, sizeof(scratch)) ==
                       EXTFS_ERR_UNSUPPORTED,
                       "core traversal APIs refuse unsupported metadata layouts");

    (void)prepare_ext2(&image);
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK,
                       "base image reopens before inode API tests");
    memset(&inode, 0, sizeof(inode));
    inode.mode = 0x2000U; /* character device */
    inode.size = 1U;
    failures += expect(extfs_read_file(&volume, &inode, 0U, &byte, 1U,
                                       scratch, sizeof(scratch), &bytes_read) ==
                       EXTFS_ERR_UNSUPPORTED,
                       "special inodes are not interpreted as file block maps");

    memset(&inode, 0, sizeof(inode));
    inode.mode = 0x8000U;
    inode.flags = EXTFS_TEST_EXTENTS_FLAG;
    store_le16(inode.block_map + 0x00U, 0xF30AU);
    store_le16(inode.block_map + 0x02U, 2U);
    store_le16(inode.block_map + 0x04U, 4U);
    store_le16(inode.block_map + 0x06U, 0U);
    store_le32(inode.block_map + 12U, 10U);
    store_le16(inode.block_map + 16U, 2U);
    store_le32(inode.block_map + 20U, 20U);
    store_le32(inode.block_map + 24U, 11U); /* overlaps logical 10..11 */
    store_le16(inode.block_map + 28U, 1U);
    store_le32(inode.block_map + 32U, 30U);
    failures += expect(extfs_map_file_block(&volume, &inode, 10U, &physical,
                                            &hole, scratch, sizeof(scratch)) ==
                       EXTFS_ERR_CORRUPT,
                       "overlapping extent records are rejected");

    {
        static const extfs_u8 original[] = "abcdefghij";
        static const extfs_u8 replacement[] = "XYZ";
        extfs_u32 bytes_written = 0U;

        (void)prepare_ext2(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK,
                           "base image reopens before in-place write tests");
        memset(&inode, 0, sizeof(inode));
        inode.mode = 0x8000U;
        inode.size = sizeof(original) - 1U;
        store_le32(inode.block_map, 4U);
        memcpy(image.bytes + 4U * 1024U, original, sizeof(original) - 1U);
        failures += expect(extfs_write_file_existing(
                               &volume, &inode, 3U, replacement,
                               sizeof(replacement) - 1U, scratch,
                               sizeof(scratch), &bytes_written) == EXTFS_OK &&
                           bytes_written == sizeof(replacement) - 1U,
                           "existing allocated regular-file bytes can be overwritten");
        failures += expect(memcmp(image.bytes + 4U * 1024U,
                                  "abcXYZghij", 10U) == 0,
                           "in-place write changes only requested data bytes");

        bytes_written = 0U;
        failures += expect(extfs_write_file_existing(
                               &volume, &inode, 9U, replacement, 3U, scratch,
                               sizeof(scratch), &bytes_written) ==
                           EXTFS_ERR_RANGE && bytes_written == 0U,
                           "write beyond EOF is refused instead of extending file");

        store_le32(inode.block_map, 0U);
        bytes_written = 0U;
        failures += expect(extfs_write_file_existing(
                               &volume, &inode, 0U, replacement, 3U, scratch,
                               sizeof(scratch), &bytes_written) ==
                           EXTFS_ERR_UNSUPPORTED && bytes_written == 0U,
                           "writes into sparse holes are refused without allocation");

        /* Policy failures are preflighted across the complete request.  The
         * first block is allocated and the second is a hole; crossing the
         * boundary must leave the first block untouched. */
        memset(&inode, 0, sizeof(inode));
        inode.mode = 0x8000U;
        inode.size = 2048U;
        store_le32(inode.block_map + 0U, 4U);
        store_le32(inode.block_map + 4U, 0U);
        memset(image.bytes + 4U * 1024U, 'A', 1024U);
        {
            extfs_u8 cross[4] = {'W', 'X', 'Y', 'Z'};
            bytes_written = 99U;
            failures += expect(extfs_write_file_existing(
                                   &volume, &inode, 1022U, cross,
                                   sizeof(cross), scratch, sizeof(scratch),
                                   &bytes_written) == EXTFS_ERR_UNSUPPORTED &&
                               bytes_written == 0U,
                               "hole-crossing writes fail before modifying allocated data");
            failures += expect(image.bytes[4U * 1024U + 1022U] == 'A' &&
                               image.bytes[4U * 1024U + 1023U] == 'A',
                               "preflight prevents partial data change on unsupported range");
        }

        store_le32(inode.block_map, 4U);
        inode.size = sizeof(original) - 1U;
        inode.flags = 0x00000010U; /* immutable */
        failures += expect(extfs_write_file_existing(
                               &volume, &inode, 0U, replacement, 3U, scratch,
                               sizeof(scratch), &bytes_written) ==
                           EXTFS_ERR_UNSUPPORTED,
                           "immutable inode data is never overwritten");

        /* The same bounded writer must work through an initialized ext4 extent
         * and refuse an unwritten extent without converting its metadata. */
        inode.flags = EXTFS_TEST_EXTENTS_FLAG;
        inode.size = 4U;
        memset(inode.block_map, 0, sizeof(inode.block_map));
        store_le16(inode.block_map + 0U, 0xF30AU);
        store_le16(inode.block_map + 2U, 1U);
        store_le16(inode.block_map + 4U, 4U);
        store_le16(inode.block_map + 6U, 0U);
        store_le32(inode.block_map + 12U, 0U);
        store_le16(inode.block_map + 16U, 1U);
        store_le16(inode.block_map + 18U, 0U);
        store_le32(inode.block_map + 20U, 5U);
        memcpy(image.bytes + 5U * 1024U, "abcd", 4U);
        volume.feature_incompat |= 0x00000040U; /* extents */
        bytes_written = 0U;
        failures += expect(extfs_write_file_existing(
                               &volume, &inode, 1U, replacement, 2U, scratch,
                               sizeof(scratch), &bytes_written) == EXTFS_OK &&
                           bytes_written == 2U &&
                           memcmp(image.bytes + 5U * 1024U, "aXYd", 4U) == 0,
                           "initialized extent data can be overwritten");

        store_le16(inode.block_map + 16U, 0x8001U); /* unwritten extent */
        bytes_written = 0U;
        failures += expect(extfs_write_file_existing(
                               &volume, &inode, 0U, replacement, 1U, scratch,
                               sizeof(scratch), &bytes_written) ==
                           EXTFS_ERR_UNSUPPORTED && bytes_written == 0U,
                           "unwritten extents are refused without conversion");
    }

    {
        extfs_inode resized;
        extfs_u8 verify_scratch[1024];
        extfs_u8 *disk_inode;
        extfs_u8 *disk_bitmap;
        extfs_u8 *disk_descriptor;

        {
            extfs_io no_flush = io;
            no_flush.flush = NULL;
            prepare_ext2_direct_resize_image(&image);
            failures += expect(extfs_open(&volume, &no_flush) == EXTFS_OK &&
                               extfs_read_inode(&volume, 2U, &resized,
                                                verify_scratch,
                                                sizeof(verify_scratch)) == EXTFS_OK,
                               "no-flush ext2 resize image opens read-only");
            image.write_attempts = 0U;
            failures += expect(extfs_resize_file_ext2_direct(
                                   &volume, &resized, 500U, resize_scratch,
                                   sizeof(resize_scratch)) == EXTFS_ERR_UNSUPPORTED &&
                               image.write_attempts == 0U &&
                               (image.bytes[1024U + 0x3AU] & 1U) != 0U,
                               "ext2 resize refuses missing durability support before any write");
        }

        prepare_ext2_direct_resize_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "same-block growth ext2 image opens");
        failures += expect(extfs_resize_file_ext2_direct(
                               &volume, &resized, 500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK &&
                           resized.size == 500U && volume.free_blocks == 9U,
                           "ext2 can grow within an existing direct block without allocation");
        failures += expect(image.bytes[8U * 1024U + 4U] == 0U &&
                           image.bytes[8U * 1024U + 499U] == 0U &&
                           image.bytes[8U * 1024U + 500U] == (extfs_u8)'Q',
                           "same-block growth zeros only newly exposed bytes");
        failures += expect(image.flush_attempts == 3U,
                           "successful ext2 resize uses dirty, commit and clean durability barriers");

        prepare_ext2_direct_resize_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK,
                           "direct-resize ext2 image opens");
        failures += expect(extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "direct-resize test inode reads");
        failures += expect(extfs_resize_file_ext2_direct(
                               &volume, &resized, 2500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK,
                           "ext2 direct file can grow with block allocation");
        failures += expect(resized.size == 2500U &&
                           resized.block_map[0] == 8U &&
                           resized.block_map[4] == 6U &&
                           resized.block_map[8] == 7U,
                           "growth retains old block and installs allocated direct blocks");
        failures += expect(volume.free_blocks == 7U,
                           "growth updates in-memory free-block count");
        disk_bitmap = image.bytes + 3U * 1024U;
        disk_descriptor = image.bytes + 2U * 1024U;
        disk_inode = image.bytes + 5U * 1024U + 128U;
        failures += expect(disk_bitmap[0] == 0xFFU &&
                           disk_descriptor[0x0CU] == 7U &&
                           image.bytes[1024U + 0x0CU] == 7U,
                           "growth commits bitmap and free-block counters");
        failures += expect(disk_inode[0x04U] == (extfs_u8)(2500U & 0xFFU) &&
                           disk_inode[0x1CU] == 6U,
                           "growth commits inode size and i_blocks");
        failures += expect(image.bytes[8U * 1024U + 4U] == 0U &&
                           image.bytes[6U * 1024U] == 0U &&
                           image.bytes[7U * 1024U] == 0U,
                           "growth zero-fills every newly exposed byte");
        failures += expect((image.bytes[1024U + 0x3AU] & 1U) != 0U,
                           "successful ext2 metadata transaction restores clean state");

        failures += expect(extfs_resize_file_ext2_direct(
                               &volume, &resized, 2U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK,
                           "ext2 direct file can shrink and free blocks");
        failures += expect(resized.size == 2U &&
                           resized.block_map[4] == 0U &&
                           resized.block_map[8] == 0U &&
                           volume.free_blocks == 9U,
                           "shrink clears direct pointers and restores free count");
        failures += expect(disk_bitmap[0] == 0x9FU &&
                           disk_descriptor[0x0CU] == 9U &&
                           image.bytes[1024U + 0x0CU] == 9U,
                           "shrink releases blocks in bitmap and counters");
        failures += expect(image.bytes[8U * 1024U + 2U] == 0U &&
                           image.bytes[8U * 1024U + 1023U] == 0U,
                           "shrink zeros retained partial-block tail");

        volume.kind = EXTFS_KIND_EXT3;
        failures += expect(extfs_resize_file_ext2_direct(
                               &volume, &resized, 3U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_ERR_UNSUPPORTED,
                           "journaled filesystems refuse metadata resize before JBD2");

        prepare_ext2_direct_resize_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "dirty-barrier failure-injection image opens");
        image.fail_flush_number = 1U;
        failures += expect(extfs_resize_file_ext2_direct(
                               &volume, &resized, 2500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_ERR_IO,
                           "ext2 resize stops when the dirty marker cannot be made durable");
        failures += expect(image.write_attempts == 1U &&
                           (volume.state & 1U) == 0U &&
                           (image.bytes[1024U + 0x3AU] & 1U) == 0U,
                           "dirty-barrier failure occurs before any allocation or inode mutation");

        prepare_ext2_direct_resize_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "pre-clean barrier failure-injection image opens");
        image.fail_flush_number = 2U;
        failures += expect(extfs_resize_file_ext2_direct(
                               &volume, &resized, 2500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_ERR_IO &&
                           (volume.state & 1U) == 0U &&
                           (image.bytes[1024U + 0x3AU] & 1U) == 0U,
                           "ext2 resize never writes the clean marker when mutation durability is uncertain");
        failures += expect(extfs_write_assess(&volume, &write_risks) ==
                               EXTFS_ERR_UNSUPPORTED,
                           "pre-clean flush failure leaves the mounted volume fail-closed");

        prepare_ext2_direct_resize_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "metadata-write failure-injection resize image opens");
        image.fail_write_number = 2U;
        failures += expect(extfs_resize_file_ext2_direct(
                               &volume, &resized, 2500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_ERR_IO,
                           "metadata I/O failure is reported during ext2 growth");
        failures += expect((volume.state & 1U) == 0U &&
                           (image.bytes[1024U + 0x3AU] & 1U) == 0U,
                           "interrupted ext2 metadata transaction remains dirty");
        failures += expect(extfs_write_assess(&volume, &write_risks) ==
                               EXTFS_ERR_UNSUPPORTED,
                           "dirty-after-failure volume refuses subsequent writes");
    }

    {
        extfs_inode resized;
        extfs_u8 verify_scratch[1024];
        extfs_u8 *disk_inode;
        extfs_u8 *disk_bitmap;
        extfs_u8 *disk_descriptor;
        extfs_u8 *disk_super;

        prepare_ext3_journal_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           volume.kind == EXTFS_KIND_EXT3 &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "journaled direct-resize ext3 image opens");
        failures += expect(extfs_resize_file_ext3_journaled_direct(
                               &volume, &resized, 500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK &&
                           resized.size == 500U &&
                           volume.free_blocks == 1516U,
                           "ext3 same-block growth commits inode size through JBD2");
        failures += expect(image.bytes[50U * 1024U + 4U] == 0U &&
                           image.bytes[50U * 1024U + 499U] == 0U &&
                           image.bytes[50U * 1024U + 500U] ==
                               (extfs_u8)'Q',
                           "ext3 same-block growth zeroes newly exposed data before commit");
        failures += expect((volume.feature_incompat & 0x00000004U) == 0U &&
                           load_be32(image.bytes + 20U * 1024U + 0x1CU) == 0U,
                           "successful ext3 resize leaves filesystem and journal clean");

        /* A shrink does not overwrite bytes that are still part of the same
         * allocated block: doing so before the size transaction committed
         * would damage the old file if the transaction failed.  A later
         * growth must therefore zero that newly exposed range before making
         * the larger size durable. */
        prepare_ext3_journal_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "ext3 partial-tail resize image opens");
        failures += expect(extfs_resize_file_ext3_journaled_direct(
                               &volume, &resized, 2U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK &&
                           image.bytes[50U * 1024U + 2U] ==
                               (extfs_u8)'T' &&
                           image.bytes[50U * 1024U + 3U] ==
                               (extfs_u8)'A',
                           "ext3 shrink commits size without pre-commit tail destruction");
        failures += expect(extfs_resize_file_ext3_journaled_direct(
                               &volume, &resized, 4U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK &&
                           image.bytes[50U * 1024U + 2U] == 0U &&
                           image.bytes[50U * 1024U + 3U] == 0U,
                           "ext3 regrowth zeroes stale partial-block tail before commit");

        prepare_ext3_journal_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "ext3 allocation-resize image opens");
        failures += expect(extfs_resize_file_ext3_journaled_direct(
                               &volume, &resized, 2500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK,
                           "ext3 direct file grows with journaled block allocation");
        failures += expect(resized.size == 2500U &&
                           load_be32(image.bytes + 20U * 1024U + 0x1CU) == 0U &&
                           volume.free_blocks == 1514U,
                           "ext3 growth checkpoints metadata and empties journal");
        failures += expect(
                           (extfs_u32)resized.block_map[0] == 50U &&
                           (extfs_u32)resized.block_map[4] == 7U &&
                           (extfs_u32)resized.block_map[8] == 8U,
                           "ext3 growth retains old block and installs two direct blocks");
        disk_bitmap = image.bytes + 3U * 1024U;
        disk_descriptor = image.bytes + 2U * 1024U;
        disk_inode = image.bytes + 5U * 1024U + 128U;
        disk_super = image.bytes + 1024U;
        failures += expect((disk_bitmap[(7U - 1U) >> 3] &
                            (extfs_u8)(1U << ((7U - 1U) & 7U))) != 0U &&
                           (disk_bitmap[(8U - 1U) >> 3] &
                            (extfs_u8)(1U << ((8U - 1U) & 7U))) != 0U &&
                           disk_descriptor[0x0CU] == (extfs_u8)(1514U & 0xFFU) &&
                           disk_descriptor[0x0DU] == (extfs_u8)(1514U >> 8) &&
                           disk_super[0x0CU] == (extfs_u8)(1514U & 0xFFU),
                           "ext3 growth journals bitmap and both free-block counters");
        failures += expect(disk_inode[0x1CU] == 6U &&
                           image.bytes[7U * 1024U] == 0U &&
                           image.bytes[8U * 1024U] == 0U,
                           "ext3 growth journals i_blocks after zeroing new data blocks");

        /* The compact synthetic journal maps only its first twelve logical
         * blocks directly. A real ext3 journal continues through indirect
         * blocks; wrap this clean test journal to its first log block before
         * exercising a second transaction in the same image. */
        store_be32(image.bytes + 20U * 1024U + 0x58U, 1U);
        store_be32(image.bytes + 20U * 1024U + 0xFCU, 0U);
        store_be32(image.bytes + 20U * 1024U + 0xFCU,
                   test_crc32c(0xFFFFFFFFU,
                               image.bytes + 20U * 1024U, 1024U));

        failures += expect(extfs_resize_file_ext3_journaled_direct(
                               &volume, &resized, 2U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK,
                           "ext3 direct file shrinks through JBD2");
        failures += expect(resized.size == 2U &&
                           resized.block_map[4] == 0U &&
                           resized.block_map[8] == 0U &&
                           volume.free_blocks == 1516U,
                           "ext3 shrink clears direct pointers and restores counters");
        failures += expect((disk_bitmap[(7U - 1U) >> 3] &
                            (extfs_u8)(1U << ((7U - 1U) & 7U))) == 0U &&
                           (disk_bitmap[(8U - 1U) >> 3] &
                            (extfs_u8)(1U << ((8U - 1U) & 7U))) == 0U,
                           "ext3 shrink releases direct blocks atomically");

        prepare_ext3_journal_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "ext3 journal failure-injection image opens");
        image.fail_write_number = 3U;
        failures += expect(extfs_resize_file_ext3_journaled_direct(
                               &volume, &resized, 500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_ERR_IO,
                           "ext3 resize reports an I/O failure after recovery is armed");
        failures += expect((volume.feature_incompat & 0x00000004U) != 0U &&
                           (load_be32(image.bytes + 20U * 1024U + 0x1CU) != 0U ||
                            (image.bytes[1024U + 0x60U] & 0x04U) != 0U),
                           "failed journal transaction leaves recovery required");
        failures += expect(extfs_write_assess(&volume, &write_risks) ==
                               EXTFS_ERR_UNSUPPORTED,
                           "recovery-required ext3 volume refuses subsequent writes");
    }

    {
        extfs_inode resized;
        extfs_inode reopened;
        extfs_u8 verify_scratch[1024];
        extfs_u8 *disk_inode;
        extfs_u8 *disk_bitmap;
        extfs_u8 *disk_descriptor;
        extfs_u8 *disk_super;

        prepare_ext4_journal_extent_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           volume.kind == EXTFS_KIND_EXT4 &&
                           volume.metadata_checksums != 0U &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "checksummed ext4 single-extent resize image opens");
        failures += expect(extfs_resize_file_ext4_journaled_extent_tree(
                               &volume, &resized, 2500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK &&
                           resized.size == 2500U &&
                           volume.free_blocks == 1514U,
                           "ext4 inline extent grows through checksummed JBD2 metadata");
        failures += expect(resized.block_map[0x10U] == 3U &&
                           image.bytes[51U * 1024U] == 0U &&
                           image.bytes[52U * 1024U] == 0U,
                           "ext4 growth extends contiguous extent after zeroing new blocks");

        disk_bitmap = image.bytes + 3U * 1024U;
        disk_descriptor = image.bytes + 2U * 1024U;
        disk_inode = image.bytes + 5U * 1024U + 128U;
        disk_super = image.bytes + 1024U;
        failures += expect((disk_bitmap[(51U - 1U) >> 3] &
                            (extfs_u8)(1U << ((51U - 1U) & 7U))) != 0U &&
                           (disk_bitmap[(52U - 1U) >> 3] &
                            (extfs_u8)(1U << ((52U - 1U) & 7U))) != 0U &&
                           disk_inode[0x38U] == 3U &&
                           disk_descriptor[0x0CU] ==
                               (extfs_u8)(1514U & 0xFFU) &&
                           disk_super[0x0CU] ==
                               (extfs_u8)(1514U & 0xFFU),
                           "ext4 growth checkpoints extent, bitmap and counters");
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &reopened,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK &&
                           reopened.size == 2500U,
                           "ext4 growth leaves superblock/group/inode checksums readable");

        /* Wrap the compact synthetic journal before a second metadata
         * transaction, just as in the ext3 transaction tests. */
        store_be32(image.bytes + 20U * 1024U + 0x58U, 1U);
        store_be32(image.bytes + 20U * 1024U + 0xFCU, 0U);
        store_be32(image.bytes + 20U * 1024U + 0xFCU,
                   test_crc32c(0xFFFFFFFFU,
                               image.bytes + 20U * 1024U, 1024U));
        failures += expect(extfs_resize_file_ext4_journaled_extent_tree(
                               &volume, &reopened, 2U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK &&
                           reopened.size == 2U &&
                           reopened.block_map[0x10U] == 1U &&
                           volume.free_blocks == 1516U,
                           "ext4 inline extent shrink frees its trailing blocks");
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &reopened,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK &&
                           reopened.size == 2U,
                           "ext4 shrink rebuilds metadata checksums correctly");

        prepare_ext4_journal_extent_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK &&
                           extfs_resize_file_ext4_journaled_extent_tree(
                               &volume, &resized, 0U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK &&
                           resized.size == 0U &&
                           resized.block_map[0x02U] == 0U &&
                           volume.free_blocks == 1517U,
                           "ext4 inline extent can truncate completely to an empty root");

        prepare_ext4_empty_extent_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &reopened,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK &&
                           reopened.size == 0U &&
                           reopened.block_map[0x02U] == 0U,
                           "ext4 empty inline-root image opens");
        failures += expect(extfs_resize_file_ext4_journaled_extent_tree(
                               &volume, &reopened, 1500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK &&
                           reopened.size == 1500U &&
                           reopened.block_map[0x02U] == 1U &&
                           reopened.block_map[0x10U] == 2U &&
                           volume.free_blocks == 1515U,
                           "0.8 creates the first initialized inline extent");
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &reopened,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK &&
                           reopened.size == 1500U &&
                           reopened.block_map[0x02U] == 1U,
                           "first-extent insertion leaves all ext4 checksums readable");

        prepare_ext4_fragmented_extent_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "ext4 fragmented-growth image opens");
        failures += expect(extfs_resize_file_ext4_journaled_extent_tree(
                               &volume, &resized, 2500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK &&
                           resized.size == 2500U &&
                           resized.block_map[0x02U] == 2U &&
                           volume.free_blocks == 1513U,
                           "0.8 appends a second inline extent when physical continuation is busy");
        failures += expect(resized.block_map[0x10U] == 1U &&
                           resized.block_map[0x18U] == 1U &&
                           resized.block_map[0x1CU] == 2U &&
                           resized.block_map[0x20U] == 7U &&
                           image.bytes[7U * 1024U] == 0U &&
                           image.bytes[8U * 1024U] == 0U &&
                           image.bytes[51U * 1024U] == 0x5AU,
                           "fragmented growth preserves the occupied neighbour and zeroes the new run");
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &reopened,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK &&
                           reopened.block_map[0x02U] == 2U,
                           "multi-extent root reopens after checksum rebuild");

        {
            extfs_u8 marker = (extfs_u8)'X';
            extfs_u32 bytes_written = 0U;
            failures += expect(extfs_write_file_existing(
                                   &volume, &reopened, 1024U, &marker, 1U,
                                   verify_scratch, sizeof(verify_scratch),
                                   &bytes_written) == EXTFS_OK &&
                               bytes_written == 1U &&
                               image.bytes[7U * 1024U] == marker,
                               "existing-data writer follows the new second inline extent");
        }

        store_be32(image.bytes + 20U * 1024U + 0x58U, 1U);
        store_be32(image.bytes + 20U * 1024U + 0xFCU, 0U);
        store_be32(image.bytes + 20U * 1024U + 0xFCU,
                   test_crc32c(0xFFFFFFFFU,
                               image.bytes + 20U * 1024U, 1024U));
        failures += expect(extfs_resize_file_ext4_journaled_extent_tree(
                               &volume, &reopened, 2U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK &&
                           reopened.size == 2U &&
                           reopened.block_map[0x02U] == 1U &&
                           reopened.block_map[0x10U] == 1U &&
                           volume.free_blocks == 1515U,
                           "0.8 shrink removes trailing inline extents atomically");

        prepare_ext4_four_extent_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "four-inline-extent ext4 image opens");
        failures += expect(extfs_resize_file_ext4_journaled_extent_tree(
                               &volume, &resized, 5120U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK &&
                           resized.size == 5120U &&
                           ((extfs_u32)resized.block_map[0x06U] |
                            ((extfs_u32)resized.block_map[0x07U] << 8)) == 1U &&
                           volume.free_blocks == 1507U,
                           "0.9 converts a full inline root to one external depth-1 leaf");
        failures += expect(((extfs_u32)resized.block_map[0x02U] |
                            ((extfs_u32)resized.block_map[0x03U] << 8)) == 1U &&
                           image.bytes[7U * 1024U] == 0x0AU &&
                           image.bytes[7U * 1024U + 1U] == 0xF3U &&
                           image.bytes[8U * 1024U] == 0U &&
                           image.bytes[57U * 1024U] == 0x5AU,
                           "external leaf is checksummed metadata and new data is separately zeroed");
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &reopened,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK &&
                           reopened.size == 5120U,
                           "external depth-1 extent tree reopens with valid checksums");
        {
            extfs_u8 marker = (extfs_u8)'Z';
            extfs_u32 bytes_written = 0U;
            failures += expect(extfs_write_file_existing(
                                   &volume, &reopened, 4096U, &marker, 1U,
                                   verify_scratch, sizeof(verify_scratch),
                                   &bytes_written) == EXTFS_OK &&
                               bytes_written == 1U &&
                               image.bytes[8U * 1024U] == marker,
                               "existing-data writer follows the new external leaf");
        }

        /* A corrupt leaf checksum must be caught before resize metadata writes. */
        image.bytes[7U * 1024U + 24U] ^= 0x01U;
        image.write_attempts = 0U;
        failures += expect(extfs_resize_file_ext4_journaled_extent_tree(
                               &volume, &reopened, 6144U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_ERR_CHECKSUM &&
                           image.write_attempts == 0U,
                           "external extent-leaf checksum corruption fails closed before writes");
        image.bytes[7U * 1024U + 24U] ^= 0x01U;

        store_be32(image.bytes + 20U * 1024U + 0x58U, 1U);
        store_be32(image.bytes + 20U * 1024U + 0xFCU, 0U);
        store_be32(image.bytes + 20U * 1024U + 0xFCU,
                   test_crc32c(0xFFFFFFFFU,
                               image.bytes + 20U * 1024U, 1024U));
        failures += expect(extfs_resize_file_ext4_journaled_extent_tree(
                               &volume, &reopened, 4096U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_OK &&
                           reopened.size == 4096U &&
                           ((extfs_u32)reopened.block_map[0x06U] |
                            ((extfs_u32)reopened.block_map[0x07U] << 8)) == 0U &&
                           ((extfs_u32)reopened.block_map[0x02U] |
                            ((extfs_u32)reopened.block_map[0x03U] << 8)) == 4U &&
                           volume.free_blocks == 1509U,
                           "0.9 shrink collapses the one-leaf tree back into the inode root");

        prepare_ext4_journal_extent_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK,
                           "ext4 bitmap-corruption image opens before corruption");
        image.bytes[3U * 1024U + 7U] ^= 0x01U;
        image.write_attempts = 0U;
        failures += expect(extfs_resize_file_ext4_journaled_extent_tree(
                               &volume, &resized, 2500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_ERR_CHECKSUM &&
                           image.write_attempts == 0U,
                           "ext4 allocator refuses a bad block-bitmap checksum before writes");

        prepare_ext4_journal_extent_image(&image);
        disk_inode = image.bytes + 5U * 1024U + 128U;
        store_le16(disk_inode + 0x2AU, 2U);
        test_ext4_inode_checksum(image.bytes + 1024U, 2U, disk_inode, 128U);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK &&
                           extfs_resize_file_ext4_journaled_extent_tree(
                               &volume, &resized, 2500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_ERR_UNSUPPORTED,
                           "0.8 ext4 writer fails closed on malformed inline extent roots");

        prepare_ext4_journal_extent_image(&image);
        disk_inode = image.bytes + 5U * 1024U + 128U;
        store_le16(disk_inode + 0x76U, 1U); /* i_file_acl_high */
        test_ext4_inode_checksum(image.bytes + 1024U, 2U, disk_inode, 128U);
        image.write_attempts = 0U;
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           extfs_read_inode(&volume, 2U, &resized,
                                            verify_scratch,
                                            sizeof(verify_scratch)) == EXTFS_OK &&
                           extfs_resize_file_ext4_journaled_extent_tree(
                               &volume, &resized, 2500U, resize_scratch,
                               sizeof(resize_scratch)) == EXTFS_ERR_UNSUPPORTED &&
                           image.write_attempts == 0U,
                           "0.9 ext4 resizer refuses high external-xattr block bits before writes");
    }

    {
        extfs_u8 *entry;
        int entry_count = 0;

        (void)prepare_ext2(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK,
                           "base image reopens before directory corruption tests");
        memset(&inode, 0, sizeof(inode));
        inode.mode = 0x4000U;
        inode.size = 1024U;
        store_le32(inode.block_map, 4U);
        entry = image.bytes + 4U * 1024U;
        memset(entry, 0, 1024U);
        store_le32(entry + 0U, volume.total_inodes + 1U);
        store_le16(entry + 4U, 1024U);
        entry[6] = 1U;
        entry[8] = 'x';
        failures += expect(extfs_iterate_directory(
                               &volume, &inode, count_directory_entry,
                               &entry_count, scratch, sizeof(scratch)) ==
                           EXTFS_ERR_CORRUPT,
                           "directory entries cannot reference impossible inodes");

        memset(entry, 0, 1024U);
        store_le32(entry + 0U, EXTFS_ROOT_INODE);
        store_le16(entry + 4U, 10U); /* not 4-byte aligned */
        entry[6] = 1U;
        entry[8] = 'x';
        failures += expect(extfs_iterate_directory(
                               &volume, &inode, count_directory_entry,
                               &entry_count, scratch, sizeof(scratch)) ==
                           EXTFS_ERR_CORRUPT,
                           "misaligned directory record lengths are rejected");
    }

    {
        extfs_u8 scratch2k[2048U];
        extfs_u8 data2k[4U] = {0U, 0U, 0U, 0U};
        extfs_u32 got2k = 0U;
        prepare_ext4_2k_external_leaf_image(&image);
        failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                           volume.block_size == 2048U &&
                           extfs_read_inode(&volume, 2U, &inode,
                                            scratch2k, sizeof(scratch2k)) == EXTFS_OK &&
                           extfs_read_file(&volume, &inode, 0U, data2k,
                                           sizeof(data2k), scratch2k,
                                           sizeof(scratch2k), &got2k) == EXTFS_OK &&
                           got2k == 4U && memcmp(data2k, "2KOK", 4U) == 0,
                           "2 KiB ext4 extent-tail checksum is read from eh_max offset");
    }

    prepare_ext4_timestamp_image(&image);
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK,
                       "timestamp test ext4 image opens");
    failures += expect(extfs_read_inode(&volume, 2U, &inode, scratch,
                                        sizeof(scratch)) == EXTFS_OK,
                       "extended timestamp inode reads");
    failures += expect(inode.access_time == -1LL,
                       "pre-1970 signed timestamp is preserved");
    failures += expect(inode.change_time == 2147483648LL &&
                       inode.change_time_nanoseconds == 123456789U,
                       "epoch extension and nanoseconds decode for ctime");
    failures += expect(inode.modification_time == -2147483648LL,
                       "signed 2038 boundary decodes correctly");
    failures += expect(inode.creation_time == 8589934592LL &&
                       inode.creation_time_nanoseconds == 987654321U,
                       "birth time uses its actual extra-field boundary");


    /* JBD2 foundation: parse a clean internal checksum-v3 journal, commit one
     * metadata block through descriptor/data/commit/checkpoint ordering, and
     * ensure recovery remains armed when durability fails mid-transaction. */
    prepare_ext3_journal_image(&image);
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                       volume.kind == EXTFS_KIND_EXT3 &&
                       volume.journal_inode == 8U,
                       "ext3 internal journal identity is parsed");
    failures += expect(extfs_journal_open(&volume, &journal, scratch,
                                          sizeof(scratch)) == EXTFS_OK &&
                       journal.clean != 0U && journal.sequence == 7U &&
                       journal.head == 1U && journal.checksum_v3 != 0U,
                       "clean JBD2 checksum-v3 journal opens");
    failures += expect(extfs_journal_write_assess(&volume, &journal,
                                                  &journal_risks) == EXTFS_OK &&
                       journal_risks == 0U,
                       "clean internal JBD2 journal passes write assessment");

    /* If the primary filesystem superblock is itself part of a transaction,
     * its checkpoint image must retain RECOVER until the journal is empty.
     * Refuse a caller that would create an apparently clean filesystem while
     * committed metadata still depended on replay. */
    memcpy(journal_metadata, image.bytes + 1024U, 1024U);
    journal_item.home_block = 1U;
    journal_item.block_data = journal_metadata;
    image.write_attempts = 0U;
    failures += expect(extfs_journal_commit_metadata(&volume, &journal,
                                                      &journal_item, 1U,
                                                      scratch,
                                                      sizeof(scratch)) ==
                                                      EXTFS_ERR_INVALID_ARGUMENT &&
                       image.write_attempts == 0U,
                       "JBD2 rejects primary-superblock checkpoint image without RECOVER");

    memset(journal_metadata, 0x5A, sizeof(journal_metadata));
    store_be32(journal_metadata, 0xC03B3998U); /* force JBD2 escape handling */
    journal_item.home_block = 40U;
    journal_item.block_data = journal_metadata;
    image.write_attempts = 0U;
    image.flush_attempts = 0U;
    failures += expect(extfs_journal_commit_metadata(&volume, &journal,
                                                      &journal_item, 1U,
                                                      scratch,
                                                      sizeof(scratch)) == EXTFS_OK,
                       "single-block JBD2 metadata transaction commits");
    failures += expect(memcmp(image.bytes + 40U * 1024U, journal_metadata,
                              1024U) == 0,
                       "committed metadata is checkpointed to home block");
    failures += expect(load_be32(image.bytes + 21U * 1024U) == 0xC03B3998U &&
                       load_be32(image.bytes + 21U * 1024U + 4U) == 1U &&
                       load_be32(image.bytes + 21U * 1024U + 8U) == 8U,
                       "descriptor block records JBD2 magic/type/transaction id");
    failures += expect(load_be32(image.bytes + 22U * 1024U) == 0U,
                       "journal copy escapes metadata beginning with JBD2 magic");
    failures += expect(load_be32(image.bytes + 23U * 1024U) == 0xC03B3998U &&
                       load_be32(image.bytes + 23U * 1024U + 4U) == 2U &&
                       load_be32(image.bytes + 23U * 1024U + 8U) == 8U,
                       "commit block is written for the same transaction");
    failures += expect(image.bytes[23U * 1024U + 12U] == 0U &&
                       image.bytes[23U * 1024U + 13U] == 0U &&
                       load_be32(image.bytes + 23U * 1024U + 16U) != 0U,
                       "checksum-v3 commit keeps type/size zero and stores CRC32C");
    failures += expect(load_be64(image.bytes + 23U * 1024U + 48U) ==
                           1700000000ULL &&
                       load_be32(image.bytes + 23U * 1024U + 56U) ==
                           123456700U,
                       "JBD2 commit stores host wall-clock timestamp");
    commit_checksum = load_be32(image.bytes + 23U * 1024U + 16U);
    memcpy(scratch, image.bytes + 23U * 1024U, 1024U);
    store_be32(scratch + 16U, 0U);
    failures += expect(commit_checksum ==
                       test_crc32c(journal.checksum_seed, scratch, 1024U),
                       "checksum-v3 commit CRC32C covers the complete zeroed-checksum block");
    failures += expect(load_be32(image.bytes + 20U * 1024U + 0x1CU) == 0U &&
                       load_be32(image.bytes + 20U * 1024U + 0x18U) == 8U &&
                       load_be32(image.bytes + 20U * 1024U + 0x58U) == 4U,
                       "successful checkpoint marks journal empty and advances head");
    failures += expect((image.bytes[1024U + 0x60U] & 0x04U) == 0U &&
                       (volume.feature_incompat & 0x00000004U) == 0U &&
                       image.flush_attempts >= 7U,
                       "successful transaction clears RECOVER only after durability barriers");

    prepare_ext3_journal_image(&image);
    store_be32(image.bytes + 20U * 1024U + 0x28U, 0x00000008U);
    store_be32(image.bytes + 20U * 1024U + 0xFCU, 0U);
    store_be32(image.bytes + 20U * 1024U + 0xFCU,
               test_crc32c(0xFFFFFFFFU, image.bytes + 20U * 1024U, 1024U));
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                       extfs_journal_open(&volume, &journal, scratch,
                                          sizeof(scratch)) == EXTFS_OK &&
                       journal.checksum_v2 != 0U &&
                       journal.checksum_v3 == 0U,
                       "clean JBD2 checksum-v2 journal opens");
    memset(journal_metadata, 0x44, sizeof(journal_metadata));
    journal_item.home_block = 40U;
    journal_item.block_data = journal_metadata;
    failures += expect(extfs_journal_commit_metadata(&volume, &journal,
                                                      &journal_item, 1U,
                                                      scratch,
                                                      sizeof(scratch)) == EXTFS_OK,
                       "checksum-v2 metadata transaction commits");
    failures += expect(load_be32(image.bytes + 21U * 1024U + 12U) == 40U &&
                       load_be16(image.bytes + 21U * 1024U + 18U) == 8U &&
                       load_be16(image.bytes + 21U * 1024U + 16U) != 0U &&
                       load_be32(image.bytes + 21U * 1024U + 1020U) != 0U,
                       "checksum-v2 descriptor uses truncated tag CRC and descriptor tail CRC");

    prepare_ext3_journal_image(&image);
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                       extfs_journal_open(&volume, &journal, scratch,
                                          sizeof(scratch)) == EXTFS_OK,
                       "fresh JBD2 multi-block image opens");
    memset(journal_metadata, 0x11, sizeof(journal_metadata));
    memset(journal_metadata_second, 0x22, sizeof(journal_metadata_second));
    multi_items[0].home_block = 40U;
    multi_items[0].block_data = journal_metadata;
    multi_items[1].home_block = 41U;
    multi_items[1].block_data = journal_metadata_second;
    failures += expect(extfs_journal_commit_metadata(&volume, &journal,
                                                      multi_items, 2U,
                                                      scratch,
                                                      sizeof(scratch)) == EXTFS_OK,
                       "multi-block JBD2 metadata transaction commits");
    failures += expect(load_be32(image.bytes + 21U * 1024U + 12U) == 40U &&
                       load_be32(image.bytes + 21U * 1024U + 16U) == 0U &&
                       load_be32(image.bytes + 21U * 1024U + 44U) == 41U &&
                       load_be32(image.bytes + 21U * 1024U + 48U) ==
                           (0x00000002U | 0x00000008U),
                       "descriptor emits first UUID then SAME_UUID/LAST_TAG for later metadata");
    failures += expect(memcmp(image.bytes + 22U * 1024U, journal_metadata,
                              1024U) == 0 &&
                       memcmp(image.bytes + 23U * 1024U, journal_metadata_second,
                              1024U) == 0 &&
                       load_be32(image.bytes + 24U * 1024U + 4U) == 2U &&
                       load_be32(image.bytes + 20U * 1024U + 0x58U) == 5U,
                       "multi-block transaction logs both blocks, commits, and advances head");

    prepare_ext3_journal_image(&image);
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                       extfs_journal_open(&volume, &journal, scratch,
                                          sizeof(scratch)) == EXTFS_OK,
                       "fresh JBD2 no-flush image opens");
    volume.io.flush = NULL;
    failures += expect(extfs_journal_write_assess(&volume, &journal,
                                                  &journal_risks) ==
                                                  EXTFS_ERR_UNSUPPORTED &&
                       (journal_risks & EXTFS_JOURNAL_RISK_NO_FLUSH) != 0U,
                       "journal transaction writer requires a durability flush callback");
    volume.io.flush = memory_flush;

    prepare_ext3_journal_image(&image);
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                       extfs_journal_open(&volume, &journal, scratch,
                                          sizeof(scratch)) == EXTFS_OK,
                       "fresh JBD2 no-clock image opens");
    volume.io.time_now = NULL;
    failures += expect(extfs_journal_write_assess(&volume, &journal,
                                                  &journal_risks) ==
                                                  EXTFS_ERR_UNSUPPORTED &&
                       (journal_risks & EXTFS_JOURNAL_RISK_NO_CLOCK) != 0U,
                       "journal transaction writer requires a wall-clock callback");
    volume.io.time_now = memory_time;

    prepare_ext3_journal_image(&image);
    image.fail_time = 1U;
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                       extfs_journal_open(&volume, &journal, scratch,
                                          sizeof(scratch)) == EXTFS_OK,
                       "fresh JBD2 clock-failure image opens");
    memset(journal_metadata, 0x77, sizeof(journal_metadata));
    journal_item.home_block = 40U;
    journal_item.block_data = journal_metadata;
    failures += expect(extfs_journal_commit_metadata(&volume, &journal,
                                                      &journal_item, 1U,
                                                      scratch, sizeof(scratch)) ==
                                                      EXTFS_ERR_IO &&
                       (volume.feature_incompat & 0x00000004U) == 0U &&
                       (image.bytes[1024U + 0x60U] & 0x04U) == 0U &&
                       image.write_attempts == 0U,
                       "clock failure occurs before recovery is armed or disk is changed");
    image.fail_time = 0U;

    prepare_ext3_journal_image(&image);
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                       extfs_journal_open(&volume, &journal, scratch,
                                          sizeof(scratch)) == EXTFS_OK,
                       "fresh JBD2 failure-injection image opens");
    memset(journal_metadata, 0x33, sizeof(journal_metadata));
    journal_item.home_block = 40U;
    journal_item.block_data = journal_metadata;
    image.flush_attempts = 0U;
    image.fail_flush_number = 4U; /* commit record was issued; flush fails */
    failures += expect(extfs_journal_commit_metadata(&volume, &journal,
                                                      &journal_item, 1U,
                                                      scratch,
                                                      sizeof(scratch)) == EXTFS_ERR_IO,
                       "JBD2 durability failure is surfaced as I/O error");
    failures += expect((volume.feature_incompat & 0x00000004U) != 0U &&
                       (image.bytes[1024U + 0x60U] & 0x04U) != 0U,
                       "failed transaction remains recovery-required on disk and in memory");
    failures += expect(extfs_write_assess(&volume, &write_risks) ==
                       EXTFS_ERR_UNSUPPORTED,
                       "ordinary writes fail closed after journal transaction failure");
    image.fail_flush_number = 0U;

    prepare_ext3_journal_image(&image);
    store_be32(image.bytes + 20U * 1024U + 0x48U, 2U);
    store_be32(image.bytes + 20U * 1024U + 0xFCU, 0U);
    store_be32(image.bytes + 20U * 1024U + 0xFCU,
               test_crc32c(0xFFFFFFFFU, image.bytes + 20U * 1024U, 1024U));
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                       extfs_journal_open(&volume, &journal, scratch,
                                          sizeof(scratch)) == EXTFS_OK &&
                       journal.max_transaction == 2U,
                       "JBD2 maximum transaction size is parsed");
    memset(journal_metadata, 0x11, sizeof(journal_metadata));
    memset(journal_metadata_second, 0x22, sizeof(journal_metadata_second));
    multi_items[0].home_block = 40U;
    multi_items[0].block_data = journal_metadata;
    multi_items[1].home_block = 41U;
    multi_items[1].block_data = journal_metadata_second;
    failures += expect(extfs_journal_commit_metadata(&volume, &journal,
                                                      multi_items, 2U, scratch,
                                                      sizeof(scratch)) ==
                                                      EXTFS_ERR_NO_SPACE &&
                       image.write_attempts == 0U,
                       "JBD2 on-disk transaction-size limit is enforced before writes");

    prepare_ext3_journal_image(&image);
    image.bytes[20U * 1024U + 0xFCU] ^= 0x01U;
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                       extfs_journal_open(&volume, &journal, scratch,
                                          sizeof(scratch)) == EXTFS_ERR_CHECKSUM,
                       "corrupt JBD2 superblock checksum is refused");

    prepare_ext3_journal_image(&image);
    store_be32(image.bytes + 20U * 1024U + 0x28U, 0x00000030U);
    store_be32(image.bytes + 20U * 1024U + 0xFCU, 0U);
    store_be32(image.bytes + 20U * 1024U + 0xFCU,
               test_crc32c(0xFFFFFFFFU, image.bytes + 20U * 1024U, 1024U));
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                       extfs_journal_open(&volume, &journal, scratch,
                                          sizeof(scratch)) == EXTFS_OK &&
                       extfs_journal_write_assess(&volume, &journal,
                                                  &journal_risks) ==
                                                  EXTFS_ERR_UNSUPPORTED &&
                       (journal_risks & EXTFS_JOURNAL_RISK_UNSUPPORTED_FEATURE) != 0U,
                       "fast-commit journals are parsed but refused by transaction writer");

    if (failures == 0) {
        puts("All ExtFS unit tests passed.");
        return 0;
    }
    return 1;
}
