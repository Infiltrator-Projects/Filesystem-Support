/* SPDX-License-Identifier: GPL-2.0-only */
#include "../native/filesystems/ext2/core/ext2_engine.h"

#include <stdio.h>
#include <string.h>

#define BLOCK_SIZE 1024U
#define BLOCK_COUNT 33U

typedef struct MemoryDisk {
    unsigned char bytes[BLOCK_SIZE * BLOCK_COUNT];
    unsigned int writes;
} MemoryDisk;

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

static int memory_read(
    void *user, ifs_ext2_u64 offset, void *destination, ifs_ext2_u32 count)
{
    MemoryDisk *disk = (MemoryDisk *)user;
    if (offset > sizeof(disk->bytes) ||
        count > sizeof(disk->bytes) - (size_t)offset)
        return -1;
    memcpy(destination, disk->bytes + (size_t)offset, count);
    return 0;
}

static int memory_write(
    void *user, ifs_ext2_u64 offset, const void *source, ifs_ext2_u32 count)
{
    MemoryDisk *disk = (MemoryDisk *)user;
    if (offset > sizeof(disk->bytes) ||
        count > sizeof(disk->bytes) - (size_t)offset)
        return -1;
    memcpy(disk->bytes + (size_t)offset, source, count);
    ++disk->writes;
    return 0;
}

static int fail(const char *message)
{
    fprintf(stderr, "ext2_engine_test: %s\n", message);
    return 1;
}

static void make_inode(
    unsigned char *raw, unsigned int mode, unsigned long size,
    unsigned int first_block)
{
    memset(raw, 0, 128U);
    store_le16(raw + 0x00U, mode);
    store_le16(raw + 0x02U, 1000U);
    store_le32(raw + 0x04U, size);
    store_le16(raw + 0x18U, 1000U);
    store_le16(raw + 0x1AU, 1U);
    store_le32(raw + 0x28U, first_block);
}

int main(void)
{
    MemoryDisk disk;
    IfsExt2Io io;
    IfsExt2Volume volume;
    IfsExt2Inode root;
    IfsExt2Inode file;
    unsigned char scratch[BLOCK_SIZE];
    unsigned char output[16] = {0};
    unsigned int read_count = 0U;
    unsigned int written = 0U;
    unsigned int found = 0U;
    unsigned int risks = 0U;
    unsigned char *sb;
    unsigned char *gd;
    unsigned char *inode_table;
    unsigned char *dir;

    memset(&disk, 0, sizeof(disk));
    sb = disk.bytes + BLOCK_SIZE;
    gd = disk.bytes + BLOCK_SIZE * 2U;
    inode_table = disk.bytes + BLOCK_SIZE * 5U;
    dir = disk.bytes + BLOCK_SIZE * 6U;

    store_le32(sb + 0x00U, 8U);
    store_le32(sb + 0x04U, BLOCK_COUNT);
    store_le32(sb + 0x0CU, 25U);
    store_le32(sb + 0x10U, 5U);
    store_le32(sb + 0x14U, 1U);
    store_le32(sb + 0x18U, 0U);
    store_le32(sb + 0x1CU, 0U);
    store_le32(sb + 0x20U, 32U);
    store_le32(sb + 0x24U, 32U);
    store_le32(sb + 0x28U, 8U);
    store_le16(sb + 0x38U, IFS_EXT2_SUPER_MAGIC);
    store_le16(sb + 0x3AU, 1U);
    store_le32(sb + 0x4CU, 1U);
    store_le32(sb + 0x54U, 11U);
    store_le16(sb + 0x58U, 128U);
    store_le32(sb + 0x60U, IFS_EXT2_FEATURE_INCOMPAT_FILETYPE);
    memcpy(sb + 0x68U, "0123456789abcdef", 16U);
    memcpy(sb + 0x78U, "engine-test", 11U);

    store_le32(gd + 0x00U, 3U);
    store_le32(gd + 0x04U, 4U);
    store_le32(gd + 0x08U, 5U);

    make_inode(inode_table + 128U, 0x41EDU, BLOCK_SIZE, 6U);
    make_inode(inode_table + 256U, 0x81A4U, 5U, 7U);

    store_le32(dir + 0U, 2U);
    store_le16(dir + 4U, 12U);
    dir[6U] = 1U;
    dir[7U] = 2U;
    dir[8U] = '.';

    store_le32(dir + 12U, 2U);
    store_le16(dir + 16U, 12U);
    dir[18U] = 2U;
    dir[19U] = 2U;
    dir[20U] = '.';
    dir[21U] = '.';

    store_le32(dir + 24U, 3U);
    store_le16(dir + 28U, 1000U);
    dir[30U] = 5U;
    dir[31U] = 1U;
    memcpy(dir + 32U, "hello", 5U);
    memcpy(disk.bytes + BLOCK_SIZE * 7U, "hello", 5U);

    io.read_at = memory_read;
    io.write_at = memory_write;
    io.flush = NULL;
    io.user = &disk;

    if (ifs_ext2_open(&volume, &io) != IFS_EXT2_OK)
        return fail("canonical engine did not open valid EXT2 image");
    if (volume.block_size != BLOCK_SIZE || volume.total_blocks != BLOCK_COUNT)
        return fail("opened volume geometry is wrong");
    if (ifs_ext2_readonly_assess(&volume, &risks) != IFS_EXT2_OK)
        return fail("clean EXT2 image was marked unsafe");

    if (ifs_ext2_read_inode(&volume, 2U, &root, scratch, sizeof(scratch)) != IFS_EXT2_OK ||
        ifs_ext2_inode_type(&root) != IFS_EXT2_NODE_DIRECTORY)
        return fail("root inode decode failed");
    if (ifs_ext2_lookup(&volume, &root, "hello", 5U, &found,
                        scratch, sizeof(scratch)) != IFS_EXT2_OK ||
        found != 3U)
        return fail("directory lookup failed");
    if (ifs_ext2_read_inode(&volume, found, &file, scratch, sizeof(scratch)) != IFS_EXT2_OK)
        return fail("file inode decode failed");
    if (ifs_ext2_read_file(&volume, &file, 0U, output, 5U,
                           scratch, sizeof(scratch), &read_count) != IFS_EXT2_OK ||
        read_count != 5U || memcmp(output, "hello", 5U) != 0)
        return fail("file read failed");

    if (ifs_ext2_write_file_existing(&volume, &file, 1U, "A", 1U,
                                     scratch, sizeof(scratch), &written) != IFS_EXT2_OK ||
        written != 1U || disk.writes != 1U)
        return fail("existing-block write failed");
    memset(output, 0, sizeof(output));
    if (ifs_ext2_read_file(&volume, &file, 0U, output, 5U,
                           scratch, sizeof(scratch), &read_count) != IFS_EXT2_OK ||
        memcmp(output, "hAllo", 5U) != 0)
        return fail("written file did not read back");

    return 0;
}
