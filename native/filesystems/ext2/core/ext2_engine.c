/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ext2_engine.h"

#define EXT2_VALID_FS 0x0001U
#define EXT2_ERROR_FS 0x0002U

#define EXT2_S_IFMT 0xF000U
#define EXT2_S_IFSOCK 0xC000U
#define EXT2_S_IFLNK 0xA000U
#define EXT2_S_IFREG 0x8000U
#define EXT2_S_IFBLK 0x6000U
#define EXT2_S_IFDIR 0x4000U
#define EXT2_S_IFCHR 0x2000U
#define EXT2_S_IFIFO 0x1000U

#define EXT2_IMMUTABLE_FL 0x00000010U
#define EXT2_APPEND_FL 0x00000020U

static ifs_ext2_u16 ifs_ext2_engine_load_le16(const ifs_ext2_u8 *p)
{
    return (ifs_ext2_u16)((ifs_ext2_u16)p[0] |
                          ((ifs_ext2_u16)p[1] << 8));
}

static ifs_ext2_u32 ifs_ext2_engine_load_le32(const ifs_ext2_u8 *p)
{
    return (ifs_ext2_u32)p[0] |
           ((ifs_ext2_u32)p[1] << 8) |
           ((ifs_ext2_u32)p[2] << 16) |
           ((ifs_ext2_u32)p[3] << 24);
}

static void copy_bytes(void *destination, const void *source, ifs_ext2_size_t count)
{
    ifs_ext2_u8 *dst = (ifs_ext2_u8 *)destination;
    const ifs_ext2_u8 *src = (const ifs_ext2_u8 *)source;

    while (count-- != 0U)
        *dst++ = *src++;
}

static void zero_bytes(void *destination, ifs_ext2_size_t count)
{
    ifs_ext2_u8 *dst = (ifs_ext2_u8 *)destination;
    while (count-- != 0U)
        *dst++ = 0U;
}

static int bytes_equal(const void *left, const void *right, ifs_ext2_size_t count)
{
    const ifs_ext2_u8 *a = (const ifs_ext2_u8 *)left;
    const ifs_ext2_u8 *b = (const ifs_ext2_u8 *)right;

    while (count-- != 0U) {
        if (*a++ != *b++)
            return 0;
    }
    return 1;
}

static IfsExt2Status read_exact(
    const IfsExt2Volume *volume,
    ifs_ext2_u64 offset,
    void *destination,
    ifs_ext2_u32 count)
{
    if (volume == IFS_EXT2_NULL || destination == IFS_EXT2_NULL || volume->io.read_at == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (offset > volume->byte_size ||
        (ifs_ext2_u64)count > volume->byte_size - offset)
        return IFS_EXT2_ERROR_RANGE;
    if (count == 0U)
        return IFS_EXT2_OK;
    return volume->io.read_at(volume->io.user, offset, destination, count) == 0
        ? IFS_EXT2_OK : IFS_EXT2_ERROR_IO;
}

static IfsExt2Status write_exact(
    const IfsExt2Volume *volume,
    ifs_ext2_u64 offset,
    const void *source,
    ifs_ext2_u32 count)
{
    if (volume == IFS_EXT2_NULL || source == IFS_EXT2_NULL || volume->io.write_at == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_UNSUPPORTED;
    if (offset > volume->byte_size ||
        (ifs_ext2_u64)count > volume->byte_size - offset)
        return IFS_EXT2_ERROR_RANGE;
    if (count == 0U)
        return IFS_EXT2_OK;
    return volume->io.write_at(volume->io.user, offset, source, count) == 0
        ? IFS_EXT2_OK : IFS_EXT2_ERROR_IO;
}

static IfsExt2Status block_offset(
    const IfsExt2Volume *volume,
    ifs_ext2_u64 block,
    ifs_ext2_u64 *offset)
{
    if (volume == IFS_EXT2_NULL || offset == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (block >= volume->total_blocks ||
        block > (~(ifs_ext2_u64)0) / volume->block_size)
        return IFS_EXT2_ERROR_CORRUPT;
    *offset = block * volume->block_size;
    return IFS_EXT2_OK;
}

static int is_power_of(ifs_ext2_u32 value, const ifs_ext2_u32 base)
{
    if (value < 1U)
        return 0;
    while ((value % base) == 0U)
        value /= base;
    return value == 1U;
}

static int bg_has_super(
    const IfsExt2Volume *volume,
    const ifs_ext2_u32 group)
{
    if ((volume->feature_ro_compat &
         IFS_EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER) == 0U)
        return 1;
    if (group == 0U || group == 1U)
        return 1;
    return is_power_of(group, 3U) ||
           is_power_of(group, 5U) ||
           is_power_of(group, 7U);
}

static IfsExt2Status read_group_descriptor(
    const IfsExt2Volume *volume,
    const ifs_ext2_u32 group,
    IfsExt2GroupDescriptor *descriptor,
    void *scratch,
    const ifs_ext2_u32 scratch_size)
{
    ifs_ext2_u32 desc_per_block;
    ifs_ext2_u32 descriptor_block_index;
    ifs_ext2_u32 descriptor_index;
    ifs_ext2_u64 descriptor_block;
    ifs_ext2_u64 descriptor_offset;
    ifs_ext2_u64 group_first;
    IfsExt2Status status;

    if (volume == IFS_EXT2_NULL || descriptor == IFS_EXT2_NULL || scratch == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (group >= volume->group_count || scratch_size < volume->block_size)
        return IFS_EXT2_ERROR_RANGE;

    desc_per_block = volume->block_size / volume->descriptor_size;
    if (desc_per_block == 0U)
        return IFS_EXT2_ERROR_CORRUPT;

    descriptor_block_index = group / desc_per_block;
    descriptor_index = group % desc_per_block;

    if ((volume->feature_incompat & IFS_EXT2_FEATURE_INCOMPAT_META_BG) == 0U ||
        descriptor_block_index < volume->super.first_meta_bg) {
        descriptor_block = (volume->block_size == 1024U ? 1U : 0U) +
                           descriptor_block_index + 1U;
    } else {
        const ifs_ext2_u32 bg = desc_per_block * descriptor_block_index;
        status = ifs_ext2_group_bounds(
            &volume->super, bg, &group_first, &descriptor_offset);
        if (status != IFS_EXT2_OK)
            return status;
        descriptor_block = group_first +
                           (ifs_ext2_u64)bg_has_super(volume, bg);
    }

    status = block_offset(volume, descriptor_block, &descriptor_offset);
    if (status != IFS_EXT2_OK)
        return status;
    status = read_exact(
        volume, descriptor_offset, scratch, volume->block_size);
    if (status != IFS_EXT2_OK)
        return status;

    descriptor_offset =
        (ifs_ext2_u64)descriptor_index * volume->descriptor_size;
    if (descriptor_offset >
        volume->block_size - volume->descriptor_size)
        return IFS_EXT2_ERROR_CORRUPT;

    status = ifs_ext2_decode_group_descriptor(
        (const ifs_ext2_u8 *)scratch + descriptor_offset,
        volume->descriptor_size, descriptor);
    if (status != IFS_EXT2_OK)
        return status;
    return ifs_ext2_validate_group_descriptor(
        &volume->super, group, descriptor);
}

IfsExt2Status ifs_ext2_open(IfsExt2Volume *volume, const IfsExt2Io *io)
{
    ifs_ext2_u8 raw[IFS_EXT2_SUPERBLOCK_SIZE];
    IfsExt2Status status;
    ifs_ext2_u64 byte_size;
    unsigned int i;

    if (volume == IFS_EXT2_NULL || io == IFS_EXT2_NULL || io->read_at == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;

    zero_bytes(volume, sizeof(*volume));
    volume->io = *io;

    if (io->read_at(io->user, 1024U, raw, sizeof(raw)) != 0)
        return IFS_EXT2_ERROR_IO;

    status = ifs_ext2_decode_superblock(raw, sizeof(raw), &volume->super);
    if (status != IFS_EXT2_OK)
        return status;
    status = ifs_ext2_validate_superblock(
        &volume->super, 0U, io->write_at != IFS_EXT2_NULL);
    if (status != IFS_EXT2_OK)
        return status;

    if ((ifs_ext2_u64)volume->super.blocks_count >
        (~(ifs_ext2_u64)0) / volume->super.block_size)
        return IFS_EXT2_ERROR_RANGE;
    byte_size =
        (ifs_ext2_u64)volume->super.blocks_count * volume->super.block_size;

    volume->block_size = volume->super.block_size;
    volume->blocks_per_group = volume->super.blocks_per_group;
    volume->inodes_per_group = volume->super.inodes_per_group;
    volume->first_data_block = volume->super.first_data_block;
    volume->total_inodes = volume->super.inodes_count;
    volume->total_blocks = volume->super.blocks_count;
    volume->free_blocks = volume->super.free_blocks_count;
    volume->byte_size = byte_size;
    volume->group_count = volume->super.group_count;
    volume->inode_size = volume->super.inode_size;
    volume->descriptor_size = 32U;
    volume->state = volume->super.state;
    volume->revision = volume->super.revision;
    volume->feature_compat = volume->super.feature_compat;
    volume->feature_incompat = volume->super.feature_incompat;
    volume->feature_ro_compat = volume->super.feature_ro_compat;

    for (i = 0U; i < 16U; ++i)
        volume->uuid[i] = raw[0x68U + i];
    for (i = 0U; i < 16U; ++i)
        volume->label[i] = (char)raw[0x78U + i];
    volume->label[16] = '\0';

    return IFS_EXT2_OK;
}

IfsExt2Status ifs_ext2_readonly_assess(
    const IfsExt2Volume *volume, ifs_ext2_u32 *risk_flags)
{
    ifs_ext2_u32 risks = 0U;

    if (volume == IFS_EXT2_NULL || risk_flags == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if ((volume->state & EXT2_VALID_FS) == 0U)
        risks |= IFS_EXT2_READONLY_RISK_DIRTY;
    if ((volume->state & EXT2_ERROR_FS) != 0U)
        risks |= IFS_EXT2_READONLY_RISK_ERROR_STATE;

    *risk_flags = risks;
    return risks == 0U ? IFS_EXT2_OK : IFS_EXT2_ERROR_UNSUPPORTED;
}

IfsExt2Status ifs_ext2_write_assess(
    const IfsExt2Volume *volume, ifs_ext2_u32 *risk_flags)
{
    ifs_ext2_u32 risks = 0U;
    ifs_ext2_u32 readonly_risks = 0U;

    if (volume == IFS_EXT2_NULL || risk_flags == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (volume->io.write_at == IFS_EXT2_NULL)
        risks |= IFS_EXT2_WRITE_RISK_NO_WRITER;
    if (ifs_ext2_readonly_assess(volume, &readonly_risks) != IFS_EXT2_OK)
        risks |= IFS_EXT2_WRITE_RISK_READONLY_POLICY;
    if ((volume->feature_ro_compat &
         ~IFS_EXT2_FEATURE_RO_COMPAT_SUPPORTED) != 0U)
        risks |= IFS_EXT2_WRITE_RISK_UNSUPPORTED_LAYOUT;

    *risk_flags = risks;
    return risks == 0U ? IFS_EXT2_OK : IFS_EXT2_ERROR_UNSUPPORTED;
}

IfsExt2NodeType ifs_ext2_inode_type(const IfsExt2Inode *inode)
{
    ifs_ext2_u16 type;

    if (inode == IFS_EXT2_NULL)
        return IFS_EXT2_NODE_UNKNOWN;
    type = inode->mode & EXT2_S_IFMT;
    switch (type) {
    case EXT2_S_IFREG: return IFS_EXT2_NODE_REGULAR;
    case EXT2_S_IFDIR: return IFS_EXT2_NODE_DIRECTORY;
    case EXT2_S_IFLNK: return IFS_EXT2_NODE_SYMLINK;
    case EXT2_S_IFCHR: return IFS_EXT2_NODE_CHARACTER;
    case EXT2_S_IFBLK: return IFS_EXT2_NODE_BLOCK;
    case EXT2_S_IFIFO: return IFS_EXT2_NODE_FIFO;
    case EXT2_S_IFSOCK: return IFS_EXT2_NODE_SOCKET;
    }
    return IFS_EXT2_NODE_UNKNOWN;
}

IfsExt2Status ifs_ext2_inode_write_assess(
    const IfsExt2Volume *volume, const IfsExt2Inode *inode)
{
    ifs_ext2_u32 risks = 0U;

    if (volume == IFS_EXT2_NULL || inode == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (ifs_ext2_write_assess(volume, &risks) != IFS_EXT2_OK)
        return IFS_EXT2_ERROR_UNSUPPORTED;
    if (ifs_ext2_inode_type(inode) != IFS_EXT2_NODE_REGULAR)
        return IFS_EXT2_ERROR_UNSUPPORTED;
    if ((inode->flags & (EXT2_IMMUTABLE_FL | EXT2_APPEND_FL)) != 0U)
        return IFS_EXT2_ERROR_UNSUPPORTED;
    return IFS_EXT2_OK;
}

IfsExt2Status ifs_ext2_read_inode(
    const IfsExt2Volume *volume,
    const ifs_ext2_u32 inode_number,
    IfsExt2Inode *inode,
    void *scratch,
    const ifs_ext2_u32 scratch_size)
{
    IfsExt2GroupDescriptor descriptor;
    IfsExt2Status status;
    ifs_ext2_u32 group;
    ifs_ext2_u32 index;
    ifs_ext2_u64 inode_offset;
    const ifs_ext2_u8 *raw;
    ifs_ext2_u32 size_high = 0U;

    if (volume == IFS_EXT2_NULL || inode == IFS_EXT2_NULL || scratch == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (inode_number == 0U || inode_number > volume->total_inodes)
        return IFS_EXT2_ERROR_RANGE;
    if (scratch_size < volume->block_size)
        return IFS_EXT2_ERROR_BUFFER_TOO_SMALL;

    group = (inode_number - 1U) / volume->inodes_per_group;
    index = (inode_number - 1U) % volume->inodes_per_group;

    status = read_group_descriptor(
        volume, group, &descriptor, scratch, scratch_size);
    if (status != IFS_EXT2_OK)
        return status;

    inode_offset = (ifs_ext2_u64)descriptor.inode_table * volume->block_size;
    if ((ifs_ext2_u64)index >
        ((~(ifs_ext2_u64)0) - inode_offset) / volume->inode_size)
        return IFS_EXT2_ERROR_RANGE;
    inode_offset += (ifs_ext2_u64)index * volume->inode_size;
    if (inode_offset > volume->byte_size ||
        volume->inode_size > volume->byte_size - inode_offset)
        return IFS_EXT2_ERROR_CORRUPT;

    status = read_exact(
        volume, inode_offset, scratch, volume->inode_size);
    if (status != IFS_EXT2_OK)
        return status;

    raw = (const ifs_ext2_u8 *)scratch;
    zero_bytes(inode, sizeof(*inode));
    inode->number = inode_number;
    inode->mode = ifs_ext2_engine_load_le16(raw + 0x00U);
    inode->uid = ifs_ext2_engine_load_le16(raw + 0x02U);
    inode->size = ifs_ext2_engine_load_le32(raw + 0x04U);
    inode->access_time = ifs_ext2_engine_load_le32(raw + 0x08U);
    inode->change_time = ifs_ext2_engine_load_le32(raw + 0x0CU);
    inode->modification_time = ifs_ext2_engine_load_le32(raw + 0x10U);
    inode->gid = ifs_ext2_engine_load_le16(raw + 0x18U);
    inode->links_count = ifs_ext2_engine_load_le16(raw + 0x1AU);
    inode->flags = ifs_ext2_engine_load_le32(raw + 0x20U);
    copy_bytes(inode->block_map, raw + 0x28U, sizeof(inode->block_map));
    inode->generation = ifs_ext2_engine_load_le32(raw + 0x64U);

    if (volume->inode_size >= 128U) {
        inode->uid |= (ifs_ext2_u32)ifs_ext2_engine_load_le16(raw + 0x78U) << 16;
        inode->gid |= (ifs_ext2_u32)ifs_ext2_engine_load_le16(raw + 0x7AU) << 16;
    }

    if (ifs_ext2_inode_type(inode) == IFS_EXT2_NODE_REGULAR &&
        (volume->feature_ro_compat &
         IFS_EXT2_FEATURE_RO_COMPAT_LARGE_FILE) != 0U) {
        size_high = ifs_ext2_engine_load_le32(raw + 0x6CU);
        inode->size |= (ifs_ext2_u64)size_high << 32;
    }

    return IFS_EXT2_OK;
}

IfsExt2Status ifs_ext2_map_file_block(
    const IfsExt2Volume *volume,
    const IfsExt2Inode *inode,
    const ifs_ext2_u32 logical_block,
    ifs_ext2_u64 *physical_block,
    int *is_hole,
    void *scratch,
    const ifs_ext2_u32 scratch_size)
{
    IfsExt2BlockPath path;
    IfsExt2Status status;
    ifs_ext2_u32 block;
    ifs_ext2_u32 level;
    ifs_ext2_u64 offset;

    if (volume == IFS_EXT2_NULL || inode == IFS_EXT2_NULL || physical_block == IFS_EXT2_NULL ||
        is_hole == IFS_EXT2_NULL || scratch == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (scratch_size < volume->block_size)
        return IFS_EXT2_ERROR_BUFFER_TOO_SMALL;

    status = ifs_ext2_block_to_path(
        volume->block_size, logical_block, &path);
    if (status != IFS_EXT2_OK)
        return status;

    block = ifs_ext2_engine_load_le32(inode->block_map + path.offsets[0] * 4U);
    if (block == 0U) {
        *physical_block = 0U;
        *is_hole = 1;
        return IFS_EXT2_OK;
    }

    for (level = 1U; level < path.depth; ++level) {
        if (block >= volume->total_blocks)
            return IFS_EXT2_ERROR_CORRUPT;
        status = block_offset(volume, block, &offset);
        if (status != IFS_EXT2_OK)
            return status;
        status = read_exact(
            volume, offset, scratch, volume->block_size);
        if (status != IFS_EXT2_OK)
            return status;
        if (path.offsets[level] >= volume->block_size / 4U)
            return IFS_EXT2_ERROR_CORRUPT;
        block = ifs_ext2_engine_load_le32(
            (const ifs_ext2_u8 *)scratch +
            path.offsets[level] * 4U);
        if (block == 0U) {
            *physical_block = 0U;
            *is_hole = 1;
            return IFS_EXT2_OK;
        }
    }

    if (block >= volume->total_blocks)
        return IFS_EXT2_ERROR_CORRUPT;
    *physical_block = block;
    *is_hole = 0;
    return IFS_EXT2_OK;
}

IfsExt2Status ifs_ext2_read_file(
    const IfsExt2Volume *volume,
    const IfsExt2Inode *inode,
    ifs_ext2_u64 byte_offset,
    void *destination,
    ifs_ext2_u32 byte_count,
    void *scratch,
    const ifs_ext2_u32 scratch_size,
    ifs_ext2_u32 *bytes_read)
{
    ifs_ext2_u8 *out = (ifs_ext2_u8 *)destination;
    ifs_ext2_u32 total = 0U;

    if (volume == IFS_EXT2_NULL || inode == IFS_EXT2_NULL || destination == IFS_EXT2_NULL ||
        scratch == IFS_EXT2_NULL || bytes_read == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (scratch_size < volume->block_size)
        return IFS_EXT2_ERROR_BUFFER_TOO_SMALL;
    *bytes_read = 0U;
    if (byte_offset >= inode->size || byte_count == 0U)
        return IFS_EXT2_OK;
    if ((ifs_ext2_u64)byte_count > inode->size - byte_offset)
        byte_count = (ifs_ext2_u32)(inode->size - byte_offset);

    while (total < byte_count) {
        ifs_ext2_u64 current = byte_offset + total;
        ifs_ext2_u32 logical =
            (ifs_ext2_u32)(current / volume->block_size);
        ifs_ext2_u32 within =
            (ifs_ext2_u32)(current % volume->block_size);
        ifs_ext2_u32 chunk = volume->block_size - within;
        ifs_ext2_u64 physical = 0U;
        ifs_ext2_u64 block_byte = 0U;
        int hole = 0;
        IfsExt2Status status;

        if (chunk > byte_count - total)
            chunk = byte_count - total;
        status = ifs_ext2_map_file_block(
            volume, inode, logical, &physical, &hole,
            scratch, scratch_size);
        if (status != IFS_EXT2_OK)
            return status;
        if (hole) {
            zero_bytes(out + total, chunk);
        } else {
            status = block_offset(volume, physical, &block_byte);
            if (status != IFS_EXT2_OK)
                return status;
            status = read_exact(
                volume, block_byte + within, out + total, chunk);
            if (status != IFS_EXT2_OK)
                return status;
        }
        total += chunk;
    }

    *bytes_read = total;
    return IFS_EXT2_OK;
}

IfsExt2Status ifs_ext2_write_file_existing(
    const IfsExt2Volume *volume,
    const IfsExt2Inode *inode,
    const ifs_ext2_u64 byte_offset,
    const void *source,
    const ifs_ext2_u32 byte_count,
    void *scratch,
    const ifs_ext2_u32 scratch_size,
    ifs_ext2_u32 *bytes_written)
{
    const ifs_ext2_u8 *input = (const ifs_ext2_u8 *)source;
    ifs_ext2_u32 total = 0U;
    IfsExt2Status status;

    if (volume == IFS_EXT2_NULL || inode == IFS_EXT2_NULL || source == IFS_EXT2_NULL ||
        scratch == IFS_EXT2_NULL || bytes_written == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    *bytes_written = 0U;
    status = ifs_ext2_inode_write_assess(volume, inode);
    if (status != IFS_EXT2_OK)
        return status;
    if (scratch_size < volume->block_size)
        return IFS_EXT2_ERROR_BUFFER_TOO_SMALL;
    if (byte_offset > inode->size ||
        (ifs_ext2_u64)byte_count > inode->size - byte_offset)
        return IFS_EXT2_ERROR_RANGE;

    while (total < byte_count) {
        ifs_ext2_u64 current = byte_offset + total;
        ifs_ext2_u32 logical =
            (ifs_ext2_u32)(current / volume->block_size);
        ifs_ext2_u32 within =
            (ifs_ext2_u32)(current % volume->block_size);
        ifs_ext2_u32 chunk = volume->block_size - within;
        ifs_ext2_u64 physical = 0U;
        ifs_ext2_u64 block_byte = 0U;
        int hole = 0;

        if (chunk > byte_count - total)
            chunk = byte_count - total;
        status = ifs_ext2_map_file_block(
            volume, inode, logical, &physical, &hole,
            scratch, scratch_size);
        if (status != IFS_EXT2_OK)
            return status;
        if (hole)
            return IFS_EXT2_ERROR_UNSUPPORTED;

        status = block_offset(volume, physical, &block_byte);
        if (status != IFS_EXT2_OK)
            return status;

        if (within == 0U && chunk == volume->block_size) {
            status = write_exact(
                volume, block_byte, input + total, chunk);
        } else {
            status = read_exact(
                volume, block_byte, scratch, volume->block_size);
            if (status == IFS_EXT2_OK) {
                copy_bytes(
                    (ifs_ext2_u8 *)scratch + within,
                    input + total, chunk);
                status = write_exact(
                    volume, block_byte, scratch, volume->block_size);
            }
        }
        if (status != IFS_EXT2_OK)
            return status;
        total += chunk;
    }

    *bytes_written = total;
    return IFS_EXT2_OK;
}

static IfsExt2NodeType dir_type(const ifs_ext2_u8 value)
{
    switch (value) {
    case 1U: return IFS_EXT2_NODE_REGULAR;
    case 2U: return IFS_EXT2_NODE_DIRECTORY;
    case 3U: return IFS_EXT2_NODE_CHARACTER;
    case 4U: return IFS_EXT2_NODE_BLOCK;
    case 5U: return IFS_EXT2_NODE_FIFO;
    case 6U: return IFS_EXT2_NODE_SOCKET;
    case 7U: return IFS_EXT2_NODE_SYMLINK;
    }
    return IFS_EXT2_NODE_UNKNOWN;
}

IfsExt2Status ifs_ext2_iterate_directory(
    const IfsExt2Volume *volume,
    const IfsExt2Inode *directory,
    IfsExt2DirectoryCallback callback,
    void *callback_user,
    void *scratch,
    const ifs_ext2_u32 scratch_size)
{
    ifs_ext2_u64 position = 0U;

    if (volume == IFS_EXT2_NULL || directory == IFS_EXT2_NULL || callback == IFS_EXT2_NULL ||
        scratch == IFS_EXT2_NULL)
        return IFS_EXT2_ERROR_ARGUMENT;
    if (ifs_ext2_inode_type(directory) != IFS_EXT2_NODE_DIRECTORY)
        return IFS_EXT2_ERROR_NOT_DIRECTORY;
    if (scratch_size < volume->block_size)
        return IFS_EXT2_ERROR_BUFFER_TOO_SMALL;

    while (position < directory->size) {
        ifs_ext2_u32 logical =
            (ifs_ext2_u32)(position / volume->block_size);
        ifs_ext2_u64 physical = 0U;
        ifs_ext2_u64 block_byte = 0U;
        ifs_ext2_u32 within = 0U;
        int hole = 0;
        IfsExt2Status status;

        status = ifs_ext2_map_file_block(
            volume, directory, logical, &physical, &hole,
            scratch, scratch_size);
        if (status != IFS_EXT2_OK)
            return status;
        if (hole)
            return IFS_EXT2_ERROR_CORRUPT;
        status = block_offset(volume, physical, &block_byte);
        if (status != IFS_EXT2_OK)
            return status;
        status = read_exact(
            volume, block_byte, scratch, volume->block_size);
        if (status != IFS_EXT2_OK)
            return status;

        while (within < volume->block_size &&
               position < directory->size) {
            const ifs_ext2_u8 *entry;
            ifs_ext2_u32 inode_number;
            ifs_ext2_u32 rec_len;
            ifs_ext2_u32 decoded_name_len;
            ifs_ext2_u8 name_len;
            ifs_ext2_u8 file_type = 0U;
            IfsExt2NodeType type = IFS_EXT2_NODE_UNKNOWN;
            IfsExt2DirectoryRecordStatus record_status;

            if (volume->block_size - within < 8U ||
                directory->size - position < 8U)
                return IFS_EXT2_ERROR_CORRUPT;

            entry = (const ifs_ext2_u8 *)scratch + within;
            inode_number = ifs_ext2_engine_load_le32(entry);
            rec_len = ifs_ext2_directory_record_length_from_disk(
                ifs_ext2_engine_load_le16(entry + 4U), volume->block_size);

            if ((volume->feature_incompat &
                 IFS_EXT2_FEATURE_INCOMPAT_FILETYPE) != 0U) {
                decoded_name_len = entry[6U];
                file_type = entry[7U];
                type = dir_type(file_type);
            } else {
                decoded_name_len = ifs_ext2_engine_load_le16(entry + 6U);
            }

            if (decoded_name_len > IFS_EXT2_MAX_NAME_LENGTH)
                return IFS_EXT2_ERROR_CORRUPT;

            record_status = ifs_ext2_validate_directory_record(
                within, rec_len, decoded_name_len, inode_number,
                volume->block_size, volume->total_inodes);
            if (record_status != IFS_EXT2_DIRECTORY_RECORD_OK ||
                (ifs_ext2_u64)rec_len > directory->size - position)
                return IFS_EXT2_ERROR_CORRUPT;

            name_len = (ifs_ext2_u8)decoded_name_len;
            if (inode_number != 0U &&
                callback(
                    callback_user, inode_number, type,
                    (const char *)(entry + 8U), name_len) != 0)
                return IFS_EXT2_STOP;

            within += rec_len;
            position += rec_len;
        }
    }

    return IFS_EXT2_OK;
}

typedef struct LookupState {
    const char *name;
    ifs_ext2_u8 name_length;
    ifs_ext2_u32 inode_number;
    int found;
} LookupState;

static int lookup_callback(
    void *user,
    ifs_ext2_u32 inode_number,
    IfsExt2NodeType type,
    const char *name,
    ifs_ext2_u8 name_length)
{
    LookupState *state = (LookupState *)user;
    (void)type;

    if (name_length == state->name_length &&
        bytes_equal(name, state->name, name_length)) {
        state->inode_number = inode_number;
        state->found = 1;
        return 1;
    }
    return 0;
}

IfsExt2Status ifs_ext2_lookup(
    const IfsExt2Volume *volume,
    const IfsExt2Inode *directory,
    const char *name,
    const ifs_ext2_u8 name_length,
    ifs_ext2_u32 *inode_number,
    void *scratch,
    const ifs_ext2_u32 scratch_size)
{
    LookupState state;
    IfsExt2Status status;

    if (volume == IFS_EXT2_NULL || directory == IFS_EXT2_NULL || name == IFS_EXT2_NULL ||
        inode_number == IFS_EXT2_NULL || name_length == 0U)
        return IFS_EXT2_ERROR_ARGUMENT;

    state.name = name;
    state.name_length = name_length;
    state.inode_number = 0U;
    state.found = 0;

    status = ifs_ext2_iterate_directory(
        volume, directory, lookup_callback, &state,
        scratch, scratch_size);
    if (status == IFS_EXT2_STOP && state.found) {
        *inode_number = state.inode_number;
        return IFS_EXT2_OK;
    }
    if (status != IFS_EXT2_OK)
        return status;
    return IFS_EXT2_ERROR_NOT_FOUND;
}
