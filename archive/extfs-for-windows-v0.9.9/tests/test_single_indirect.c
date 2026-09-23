// SPDX-License-Identifier: GPL-3.0-or-later
#include "extfs/extfs.h"

#include <stdio.h>
#include <string.h>

#define IMAGE_SIZE (2048U * 1024U)
#define BLOCK_SIZE 1024U

typedef struct test_image {
    extfs_u8 bytes[IMAGE_SIZE];
    extfs_u32 flushes;
    extfs_u64 now_seconds;
    extfs_u32 now_nanoseconds;
} test_image;

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

static extfs_u32 load_le32(const extfs_u8 *p)
{
    return (extfs_u32)p[0] |
           ((extfs_u32)p[1] << 8) |
           ((extfs_u32)p[2] << 16) |
           ((extfs_u32)p[3] << 24);
}

static void store_be32(extfs_u8 *p, extfs_u32 value)
{
    p[0] = (extfs_u8)(value >> 24);
    p[1] = (extfs_u8)(value >> 16);
    p[2] = (extfs_u8)(value >> 8);
    p[3] = (extfs_u8)value;
}

static extfs_u32 crc32c(extfs_u32 crc, const extfs_u8 *data, extfs_u32 length)
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

static int image_read(void *user, extfs_u64 offset, void *destination,
                      extfs_u32 count)
{
    test_image *image = (test_image *)user;
    if (offset > sizeof(image->bytes) ||
        (extfs_u64)count > sizeof(image->bytes) - offset)
        return -1;
    memcpy(destination, image->bytes + (size_t)offset, count);
    return 0;
}

static int image_write(void *user, extfs_u64 offset, const void *source,
                       extfs_u32 count)
{
    test_image *image = (test_image *)user;
    if (offset > sizeof(image->bytes) ||
        (extfs_u64)count > sizeof(image->bytes) - offset)
        return -1;
    memcpy(image->bytes + (size_t)offset, source, count);
    return 0;
}

static int image_flush(void *user)
{
    test_image *image = (test_image *)user;
    ++image->flushes;
    return 0;
}

static int image_time(void *user, extfs_u64 *seconds, extfs_u32 *nanoseconds)
{
    test_image *image = (test_image *)user;
    if (seconds == NULL || nanoseconds == NULL) return -1;
    *seconds = image->now_seconds;
    *nanoseconds = image->now_nanoseconds;
    return 0;
}

static void mark_block(extfs_u8 *bitmap, extfs_u32 block)
{
    extfs_u32 bit = block - 1U;
    bitmap[bit >> 3] |= (extfs_u8)(1U << (bit & 7U));
}

static int block_marked(const extfs_u8 *bitmap, extfs_u32 block)
{
    extfs_u32 bit = block - 1U;
    return (bitmap[bit >> 3] & (extfs_u8)(1U << (bit & 7U))) != 0U;
}

static extfs_u8 *prepare_base(test_image *image,
                              extfs_u32 total_blocks,
                              extfs_u32 free_blocks,
                              extfs_u32 blocks_per_group,
                              extfs_u32 inodes_per_group)
{
    extfs_u8 *sb;
    memset(image, 0, sizeof(*image));
    image->now_seconds = 1700000000ULL;
    image->now_nanoseconds = 222333444U;
    sb = image->bytes + 1024U;
    store_le32(sb + 0x00U, inodes_per_group);
    store_le32(sb + 0x04U, total_blocks);
    store_le32(sb + 0x0CU, free_blocks);
    store_le32(sb + 0x14U, 1U);
    store_le32(sb + 0x18U, 0U);
    store_le32(sb + 0x20U, blocks_per_group);
    store_le32(sb + 0x28U, inodes_per_group);
    store_le16(sb + 0x38U, 0xEF53U);
    store_le16(sb + 0x3AU, 0x0001U);
    store_le32(sb + 0x4CU, 1U);
    store_le16(sb + 0x58U, 128U);
    for (extfs_u32 i = 0U; i < 16U; ++i) sb[0x68U + i] = (extfs_u8)(0x21U + i);
    return sb;
}

static void prepare_ext2_12(test_image *image)
{
    extfs_u8 *sb = prepare_base(image, 128U, 110U, 128U, 8U);
    extfs_u8 *descriptor = image->bytes + 2U * BLOCK_SIZE;
    extfs_u8 *bitmap = image->bytes + 3U * BLOCK_SIZE;
    extfs_u8 *inode = image->bytes + 5U * BLOCK_SIZE + 128U;
    extfs_u32 i;

    memcpy(sb + 0x78U, "INDIRECT-EXT2", 13U);
    store_le32(descriptor + 0x00U, 3U);
    store_le32(descriptor + 0x04U, 4U);
    store_le32(descriptor + 0x08U, 5U);
    store_le16(descriptor + 0x0CU, 110U);
    for (i = 1U; i <= 5U; ++i) mark_block(bitmap, i);
    for (i = 20U; i <= 31U; ++i) mark_block(bitmap, i);

    store_le16(inode + 0x00U, 0x8000U);
    store_le32(inode + 0x04U, 12U * BLOCK_SIZE);
    store_le16(inode + 0x1AU, 1U);
    store_le32(inode + 0x1CU, 24U);
    for (i = 0U; i < 12U; ++i) {
        store_le32(inode + 0x28U + i * 4U, 20U + i);
        memset(image->bytes + (20U + i) * BLOCK_SIZE, (int)('A' + (int)(i % 26U)), BLOCK_SIZE);
    }
}

static void prepare_ext3_12(test_image *image)
{
    static const extfs_u8 journal_uuid[16] = {
        'S','I','N','G','L','E','-','I','N','D','I','R','E','C','T','1'
    };
    extfs_u8 *sb = prepare_base(image, 1536U, 1505U, 2048U, 16U);
    extfs_u8 *descriptor = image->bytes + 2U * BLOCK_SIZE;
    extfs_u8 *bitmap = image->bytes + 3U * BLOCK_SIZE;
    extfs_u8 *inode = image->bytes + 5U * BLOCK_SIZE + 128U;
    extfs_u8 *journal_inode = image->bytes + 5U * BLOCK_SIZE + 7U * 128U;
    extfs_u8 *jsb = image->bytes + 20U * BLOCK_SIZE;
    extfs_u32 i;

    memcpy(sb + 0x78U, "INDIRECT-EXT3", 13U);
    store_le32(sb + 0x5CU, 0x00000004U);
    memcpy(sb + 0xD0U, journal_uuid, 16U);
    store_le32(sb + 0xE0U, 8U);

    store_le32(descriptor + 0x00U, 3U);
    store_le32(descriptor + 0x04U, 4U);
    store_le32(descriptor + 0x08U, 5U);
    store_le16(descriptor + 0x0CU, 1505U);
    for (i = 1U; i <= 6U; ++i) mark_block(bitmap, i);
    for (i = 20U; i <= 31U; ++i) mark_block(bitmap, i);
    for (i = 50U; i <= 61U; ++i) mark_block(bitmap, i);

    store_le16(inode + 0x00U, 0x8000U);
    store_le32(inode + 0x04U, 12U * BLOCK_SIZE);
    store_le16(inode + 0x1AU, 1U);
    store_le32(inode + 0x1CU, 24U);
    for (i = 0U; i < 12U; ++i) {
        store_le32(inode + 0x28U + i * 4U, 50U + i);
        memset(image->bytes + (50U + i) * BLOCK_SIZE, (int)('a' + (int)(i % 26U)), BLOCK_SIZE);
    }

    store_le16(journal_inode + 0x00U, 0x8000U);
    store_le32(journal_inode + 0x04U, 1024U * 1024U);
    store_le16(journal_inode + 0x1AU, 1U);
    store_le32(journal_inode + 0x1CU, 2048U);
    for (i = 0U; i < 12U; ++i)
        store_le32(journal_inode + 0x28U + i * 4U, 20U + i);

    store_be32(jsb + 0x00U, 0xC03B3998U);
    store_be32(jsb + 0x04U, 4U);
    store_be32(jsb + 0x0CU, BLOCK_SIZE);
    store_be32(jsb + 0x10U, 1024U);
    store_be32(jsb + 0x14U, 1U);
    store_be32(jsb + 0x18U, 7U);
    store_be32(jsb + 0x1CU, 0U);
    store_be32(jsb + 0x28U, 0x00000010U);
    memcpy(jsb + 0x30U, journal_uuid, 16U);
    store_be32(jsb + 0x40U, 1U);
    jsb[0x50U] = 4U;
    store_be32(jsb + 0x58U, 1U);
    store_be32(jsb + 0xFCU, 0U);
    store_be32(jsb + 0xFCU, crc32c(0xFFFFFFFFU, jsb, BLOCK_SIZE));
}

static void reset_journal_head(test_image *image)
{
    extfs_u8 *jsb = image->bytes + 20U * BLOCK_SIZE;
    store_be32(jsb + 0x58U, 1U);
    store_be32(jsb + 0xFCU, 0U);
    store_be32(jsb + 0xFCU, crc32c(0xFFFFFFFFU, jsb, BLOCK_SIZE));
}

static int expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", message);
        return 1;
    }
    return 0;
}

int main(void)
{
    test_image image;
    extfs_io io;
    extfs_volume volume;
    extfs_inode inode;
    extfs_u8 scratch[8U * BLOCK_SIZE];
    extfs_u8 verify[BLOCK_SIZE];
    extfs_u8 *disk_inode;
    extfs_u8 *bitmap;
    extfs_u32 root;
    extfs_u32 data;
    int failures = 0;

    memset(&io, 0, sizeof(io));
    io.read_at = image_read;
    io.write_at = image_write;
    io.flush = image_flush;
    io.time_now = image_time;
    io.user = &image;

    prepare_ext2_12(&image);
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                       volume.kind == EXTFS_KIND_EXT2 &&
                       extfs_read_inode(&volume, 2U, &inode, verify,
                                        sizeof(verify)) == EXTFS_OK,
                       "12-block ext2 fixture opens");
    failures += expect(extfs_resize_file_ext2_direct(
                           &volume, &inode, 13U * BLOCK_SIZE,
                           scratch, sizeof(scratch)) == EXTFS_OK,
                       "ext2 crosses direct-to-single-indirect boundary");
    root = load_le32(inode.block_map + 12U * 4U);
    data = root != 0U ? load_le32(image.bytes + (extfs_u64)root * BLOCK_SIZE) : 0U;
    bitmap = image.bytes + 3U * BLOCK_SIZE;
    disk_inode = image.bytes + 5U * BLOCK_SIZE + 128U;
    failures += expect(root != 0U && data != 0U &&
                       block_marked(bitmap, root) && block_marked(bitmap, data) &&
                       inode.size == 13U * BLOCK_SIZE &&
                       load_le32(disk_inode + 0x1CU) == 28U &&
                       volume.free_blocks == 108U,
                       "ext2 accounts for indirect metadata and thirteenth data block");
    failures += expect(extfs_resize_file_ext2_direct(
                           &volume, &inode, 12U * BLOCK_SIZE,
                           scratch, sizeof(scratch)) == EXTFS_OK,
                       "ext2 crosses single-indirect back to direct boundary");
    failures += expect(load_le32(inode.block_map + 12U * 4U) == 0U &&
                       !block_marked(bitmap, root) && !block_marked(bitmap, data) &&
                       load_le32(disk_inode + 0x1CU) == 24U &&
                       volume.free_blocks == 110U,
                       "ext2 frees indirect metadata and restores accounting");

    prepare_ext3_12(&image);
    failures += expect(extfs_open(&volume, &io) == EXTFS_OK &&
                       volume.kind == EXTFS_KIND_EXT3 &&
                       extfs_read_inode(&volume, 2U, &inode, verify,
                                        sizeof(verify)) == EXTFS_OK,
                       "12-block ext3 fixture opens");
    failures += expect(extfs_resize_file_ext3_journaled_direct(
                           &volume, &inode, 13U * BLOCK_SIZE,
                           scratch, sizeof(scratch)) == EXTFS_OK,
                       "ext3 crosses direct-to-single-indirect boundary through JBD2");
    root = load_le32(inode.block_map + 12U * 4U);
    data = root != 0U ? load_le32(image.bytes + (extfs_u64)root * BLOCK_SIZE) : 0U;
    bitmap = image.bytes + 3U * BLOCK_SIZE;
    disk_inode = image.bytes + 5U * BLOCK_SIZE + 128U;
    failures += expect(root != 0U && data != 0U &&
                       block_marked(bitmap, root) && block_marked(bitmap, data) &&
                       inode.size == 13U * BLOCK_SIZE &&
                       load_le32(disk_inode + 0x1CU) == 28U &&
                       volume.free_blocks == 1503U &&
                       (volume.feature_incompat & 0x00000004U) == 0U,
                       "ext3 journals indirect block, inode, bitmap and counters");

    reset_journal_head(&image);
    failures += expect(extfs_resize_file_ext3_journaled_direct(
                           &volume, &inode, 12U * BLOCK_SIZE,
                           scratch, sizeof(scratch)) == EXTFS_OK,
                       "ext3 crosses single-indirect back to direct through JBD2");
    failures += expect(load_le32(inode.block_map + 12U * 4U) == 0U &&
                       !block_marked(bitmap, root) && !block_marked(bitmap, data) &&
                       load_le32(disk_inode + 0x1CU) == 24U &&
                       volume.free_blocks == 1505U &&
                       (volume.feature_incompat & 0x00000004U) == 0U,
                       "ext3 frees indirect metadata atomically and restores accounting");

    if (failures == 0) {
        puts("Single-indirect ext2/ext3 resize tests passed.");
        return 0;
    }
    return 1;
}
