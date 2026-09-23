// SPDX-License-Identifier: GPL-3.0-or-later
#include "extfs/extfs.h"

/* ext feature flags understood by the portable core. */
#define EXTFS_COMPAT_HAS_JOURNAL       0x00000004U

#define EXTFS_INCOMPAT_COMPRESSION     0x00000001U
#define EXTFS_INCOMPAT_FILETYPE        0x00000002U
#define EXTFS_INCOMPAT_RECOVER         0x00000004U
#define EXTFS_INCOMPAT_JOURNAL_DEV     0x00000008U
#define EXTFS_INCOMPAT_META_BG         0x00000010U
#define EXTFS_INCOMPAT_EXTENTS         0x00000040U
#define EXTFS_INCOMPAT_64BIT           0x00000080U
#define EXTFS_INCOMPAT_MMP             0x00000100U
#define EXTFS_INCOMPAT_FLEX_BG         0x00000200U
#define EXTFS_INCOMPAT_EA_INODE        0x00000400U
#define EXTFS_INCOMPAT_DIRDATA         0x00001000U
#define EXTFS_INCOMPAT_CSUM_SEED       0x00002000U
#define EXTFS_INCOMPAT_LARGEDIR        0x00004000U
#define EXTFS_INCOMPAT_INLINE_DATA     0x00008000U
#define EXTFS_INCOMPAT_ENCRYPT         0x00010000U
#define EXTFS_INCOMPAT_CASEFOLD        0x00020000U

#define EXTFS_RO_COMPAT_SPARSE_SUPER    0x00000001U
#define EXTFS_RO_COMPAT_LARGE_FILE      0x00000002U
#define EXTFS_RO_COMPAT_BTREE_DIR       0x00000004U
#define EXTFS_RO_COMPAT_HUGE_FILE       0x00000008U
#define EXTFS_RO_COMPAT_GDT_CSUM        0x00000010U
#define EXTFS_RO_COMPAT_DIR_NLINK       0x00000020U
#define EXTFS_RO_COMPAT_EXTRA_ISIZE     0x00000040U
#define EXTFS_RO_COMPAT_QUOTA           0x00000100U
#define EXTFS_RO_COMPAT_BIGALLOC       0x00000200U
#define EXTFS_RO_COMPAT_METADATA_CSUM  0x00000400U
#define EXTFS_RO_COMPAT_READONLY        0x00001000U
#define EXTFS_RO_COMPAT_PROJECT         0x00002000U
#define EXTFS_RO_COMPAT_VERITY          0x00008000U
#define EXTFS_RO_COMPAT_ORPHAN_PRESENT 0x00010000U

#define EXTFS_STATE_VALID              0x0001U
#define EXTFS_STATE_ERROR              0x0002U
#define EXTFS_STATE_VALID_CLEAR_MASK   0xFFFEU

#define EXTFS_INODE_FLAG_IMMUTABLE     0x00000010U
#define EXTFS_INODE_FLAG_APPEND        0x00000020U
#define EXTFS_INODE_FLAG_ENCRYPT       0x00000800U
#define EXTFS_INODE_FLAG_JOURNAL_DATA  0x00004000U
#define EXTFS_INODE_FLAG_EXTENTS       0x00080000U
#define EXTFS_INODE_FLAG_VERITY        0x00100000U
#define EXTFS_INODE_FLAG_EOFBLOCKS     0x00400000U
#define EXTFS_INODE_FLAG_INLINE_DATA   0x10000000U
#define EXTFS_INODE_FLAG_INDEX         0x00001000U
#define EXTFS_EXTENT_MAGIC             0xF30AU
#define EXTFS_BG_BLOCK_UNINIT           0x0002U
#define EXTFS_DIRECT_BLOCK_COUNT       12U

/* JBD2 on-disk constants. JBD2 integers are big-endian even though ext
 * filesystem metadata itself is little-endian. */
#define EXTFS_JBD2_MAGIC                    0xC03B3998U
#define EXTFS_JBD2_DESCRIPTOR_BLOCK         1U
#define EXTFS_JBD2_COMMIT_BLOCK             2U
#define EXTFS_JBD2_SUPERBLOCK_V1            3U
#define EXTFS_JBD2_SUPERBLOCK_V2            4U
#define EXTFS_JBD2_FLAG_ESCAPE              0x00000001U
#define EXTFS_JBD2_FLAG_SAME_UUID           0x00000002U
#define EXTFS_JBD2_FLAG_LAST_TAG            0x00000008U
#define EXTFS_JBD2_COMPAT_CHECKSUM          0x00000001U
#define EXTFS_JBD2_INCOMPAT_REVOKE          0x00000001U
#define EXTFS_JBD2_INCOMPAT_64BIT           0x00000002U
#define EXTFS_JBD2_INCOMPAT_ASYNC_COMMIT    0x00000004U
#define EXTFS_JBD2_INCOMPAT_CSUM_V2         0x00000008U
#define EXTFS_JBD2_INCOMPAT_CSUM_V3         0x00000010U
#define EXTFS_JBD2_INCOMPAT_FAST_COMMIT     0x00000020U
#define EXTFS_JBD2_CRC32C_CHKSUM            4U
#define EXTFS_JBD2_MIN_JOURNAL_BLOCKS       1024U
#define EXTFS_JBD2_SUPPORTED_INCOMPAT ( \
    EXTFS_JBD2_INCOMPAT_REVOKE | EXTFS_JBD2_INCOMPAT_64BIT | \
    EXTFS_JBD2_INCOMPAT_CSUM_V2 | EXTFS_JBD2_INCOMPAT_CSUM_V3)

#define EXTFS_MODE_TYPE_MASK           0xF000U
#define EXTFS_MODE_FIFO                0x1000U
#define EXTFS_MODE_CHARACTER           0x2000U
#define EXTFS_MODE_DIRECTORY           0x4000U
#define EXTFS_MODE_BLOCK               0x6000U
#define EXTFS_MODE_REGULAR             0x8000U
#define EXTFS_MODE_SYMLINK              0xA000U
#define EXTFS_MODE_SOCKET               0xC000U

#define EXTFS_SUPPORTED_INCOMPAT ( \
    EXTFS_INCOMPAT_FILETYPE | EXTFS_INCOMPAT_EXTENTS | \
    EXTFS_INCOMPAT_64BIT | EXTFS_INCOMPAT_MMP | \
    EXTFS_INCOMPAT_FLEX_BG | EXTFS_INCOMPAT_EA_INODE | \
    EXTFS_INCOMPAT_CSUM_SEED | EXTFS_INCOMPAT_LARGEDIR)

/* RO_COMPAT bits that do not require metadata changes for the deliberately
 * narrow same-size/in-place data writer.  Unknown RO_COMPAT bits must keep a
 * volume read-only by definition of the ext feature model. */
#define EXTFS_WRITE_SAFE_RO_COMPAT ( \
    EXTFS_RO_COMPAT_SPARSE_SUPER | EXTFS_RO_COMPAT_LARGE_FILE | \
    EXTFS_RO_COMPAT_BTREE_DIR | EXTFS_RO_COMPAT_HUGE_FILE | \
    EXTFS_RO_COMPAT_DIR_NLINK | EXTFS_RO_COMPAT_EXTRA_ISIZE | \
    EXTFS_RO_COMPAT_QUOTA | EXTFS_RO_COMPAT_METADATA_CSUM | \
    EXTFS_RO_COMPAT_PROJECT)

static extfs_u16 extfs_le16(const extfs_u8 *p)
{
    return (extfs_u16)((extfs_u16)p[0] | ((extfs_u16)p[1] << 8));
}

static extfs_u32 extfs_le32(const extfs_u8 *p)
{
    return (extfs_u32)p[0] |
           ((extfs_u32)p[1] << 8) |
           ((extfs_u32)p[2] << 16) |
           ((extfs_u32)p[3] << 24);
}

static void extfs_store_le16(extfs_u8 *p, extfs_u16 value)
{
    p[0] = (extfs_u8)value;
    p[1] = (extfs_u8)(value >> 8);
}

static void extfs_store_le32(extfs_u8 *p, extfs_u32 value)
{
    p[0] = (extfs_u8)value;
    p[1] = (extfs_u8)(value >> 8);
    p[2] = (extfs_u8)(value >> 16);
    p[3] = (extfs_u8)(value >> 24);
}

static extfs_u32 extfs_be32(const extfs_u8 *p)
{
    return ((extfs_u32)p[0] << 24) |
           ((extfs_u32)p[1] << 16) |
           ((extfs_u32)p[2] << 8) |
           (extfs_u32)p[3];
}

static void extfs_store_be16(extfs_u8 *p, extfs_u16 value)
{
    p[0] = (extfs_u8)(value >> 8);
    p[1] = (extfs_u8)value;
}

static void extfs_store_be32(extfs_u8 *p, extfs_u32 value)
{
    p[0] = (extfs_u8)(value >> 24);
    p[1] = (extfs_u8)(value >> 16);
    p[2] = (extfs_u8)(value >> 8);
    p[3] = (extfs_u8)value;
}

static void extfs_store_be64(extfs_u8 *p, extfs_u64 value)
{
    p[0] = (extfs_u8)(value >> 56);
    p[1] = (extfs_u8)(value >> 48);
    p[2] = (extfs_u8)(value >> 40);
    p[3] = (extfs_u8)(value >> 32);
    p[4] = (extfs_u8)(value >> 24);
    p[5] = (extfs_u8)(value >> 16);
    p[6] = (extfs_u8)(value >> 8);
    p[7] = (extfs_u8)value;
}

static void extfs_zero(void *destination, extfs_u32 count)
{
    extfs_u8 *p = (extfs_u8 *)destination;
    while (count != 0U) {
        *p++ = 0U;
        --count;
    }
}

static void extfs_copy(void *destination, const void *source, extfs_u32 count)
{
    extfs_u8 *d = (extfs_u8 *)destination;
    const extfs_u8 *s = (const extfs_u8 *)source;
    while (count != 0U) {
        *d++ = *s++;
        --count;
    }
}

static int extfs_bytes_equal(const char *left, const char *right, extfs_u32 count)
{
    while (count != 0U) {
        if ((extfs_u8)*left++ != (extfs_u8)*right++) {
            return 0;
        }
        --count;
    }
    return 1;
}

#define EXTFS_U32_MAX_VALUE 0xFFFFFFFFU
#define EXTFS_U64_MAX_VALUE 0xFFFFFFFFFFFFFFFFULL

static int extfs_add_u64(extfs_u64 left, extfs_u64 right, extfs_u64 *result)
{
    if (result == 0 || right > EXTFS_U64_MAX_VALUE - left) {
        return 0;
    }
    *result = left + right;
    return 1;
}

static int extfs_mul_u64(extfs_u64 left, extfs_u64 right, extfs_u64 *result)
{
    if (result == 0 || (left != 0U && right > EXTFS_U64_MAX_VALUE / left)) {
        return 0;
    }
    *result = left * right;
    return 1;
}

/* Convert a physical filesystem block plus an in-block byte offset to an
 * absolute byte offset without permitting arithmetic wrap or escape beyond
 * the geometry established by extfs_open(). */
static extfs_status extfs_block_byte_offset(const extfs_volume *volume,
                                            extfs_u64 block,
                                            extfs_u32 within,
                                            extfs_u64 *offset)
{
    extfs_u64 base;
    if (volume == 0 || offset == 0 || block >= volume->total_blocks ||
        within >= volume->block_size) {
        return EXTFS_ERR_CORRUPT;
    }
    if (extfs_mul_u64(block, volume->block_size, &base) == 0 ||
        extfs_add_u64(base, within, offset) == 0 ||
        *offset >= volume->byte_size) {
        return EXTFS_ERR_RANGE;
    }
    return EXTFS_OK;
}

static extfs_u64 extfs_div_round_up_u64(extfs_u64 value, extfs_u64 divisor)
{
    return value / divisor + ((value % divisor) != 0U ? 1U : 0U);
}

/* Locate the filesystem block containing the primary ext superblock.  For
 * 1 KiB filesystems this is block 1; for larger block sizes the superblock
 * occupies bytes 1024..2047 inside block 0. */
static extfs_status extfs_primary_superblock_block_location(
    const extfs_volume *volume,
    extfs_u64 *home_block,
    extfs_u32 *within_block)
{
    extfs_u64 block;
    extfs_u32 within;
    if (volume == 0 || home_block == 0 || within_block == 0 ||
        volume->block_size < EXTFS_SUPERBLOCK_SIZE) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    block = 1024U / volume->block_size;
    within = 1024U % volume->block_size;
    if (block >= volume->total_blocks ||
        within > volume->block_size - EXTFS_SUPERBLOCK_SIZE) {
        return EXTFS_ERR_CORRUPT;
    }
    *home_block = block;
    *within_block = within;
    return EXTFS_OK;
}

/* Linux's crc32c convention: reflected Castagnoli polynomial, no final xor. */
static extfs_u32 extfs_crc32c(extfs_u32 crc,
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

static extfs_status extfs_read_bytes(const extfs_volume *volume,
                                     extfs_u64 offset,
                                     void *destination,
                                     extfs_u32 count)
{
    if (count == 0U) {
        return EXTFS_OK;
    }
    if (volume == 0 || volume->io.read_at == 0 || destination == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    if (offset + (extfs_u64)count < offset) {
        return EXTFS_ERR_RANGE;
    }
    if (volume->byte_size != 0U &&
        offset + (extfs_u64)count > volume->byte_size) {
        return EXTFS_ERR_RANGE;
    }
    return volume->io.read_at(volume->io.user, offset, destination, count) == 0
        ? EXTFS_OK : EXTFS_ERR_IO;
}

static extfs_status extfs_write_bytes(const extfs_volume *volume,
                                      extfs_u64 offset,
                                      const void *source,
                                      extfs_u32 count)
{
    if (count == 0U) {
        return EXTFS_OK;
    }
    if (volume == 0 || volume->io.write_at == 0 || source == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    if (offset + (extfs_u64)count < offset) {
        return EXTFS_ERR_RANGE;
    }
    if (volume->byte_size != 0U &&
        offset + (extfs_u64)count > volume->byte_size) {
        return EXTFS_ERR_RANGE;
    }
    return volume->io.write_at(volume->io.user, offset, source, count) == 0
        ? EXTFS_OK : EXTFS_ERR_IO;
}

static extfs_status extfs_flush(const extfs_volume *volume)
{
    if (volume == 0 || volume->io.flush == 0) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    return volume->io.flush(volume->io.user) == 0 ? EXTFS_OK : EXTFS_ERR_IO;
}

static extfs_status extfs_read_block(const extfs_volume *volume,
                                      extfs_u64 block,
                                      void *destination)
{
    extfs_u64 offset;
    extfs_status status = extfs_block_byte_offset(volume, block, 0U, &offset);
    if (status != EXTFS_OK) return status;
    return extfs_read_bytes(volume, offset, destination, volume->block_size);
}

static extfs_status extfs_write_block(const extfs_volume *volume,
                                       extfs_u64 block,
                                       const void *source)
{
    extfs_u64 offset;
    extfs_status status = extfs_block_byte_offset(volume, block, 0U, &offset);
    if (status != EXTFS_OK) return status;
    return extfs_write_bytes(volume, offset, source, volume->block_size);
}

static extfs_status extfs_validate_superblock_checksum(const extfs_u8 *sb)
{
    extfs_u32 stored = extfs_le32(sb + 0x3FCU);
    extfs_u32 computed = extfs_crc32c(0xFFFFFFFFU, sb, 0x3FCU);
    return stored == computed ? EXTFS_OK : EXTFS_ERR_CHECKSUM;
}

extfs_status extfs_open(extfs_volume *volume, const extfs_io *io)
{
    extfs_u8 sb[EXTFS_SUPERBLOCK_SIZE];
    extfs_u32 log_block_size;
    extfs_u64 data_blocks;
    extfs_u64 group_count64;
    extfs_u64 inode_capacity;
    extfs_u64 descriptor_table_offset;
    extfs_u64 descriptor_table_bytes;
    extfs_u64 descriptor_table_end;
    extfs_u32 label_index;
    extfs_status status;

    if (volume == 0 || io == 0 || io->read_at == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }

    extfs_zero(volume, (extfs_u32)sizeof(*volume));
    volume->io = *io;
    status = extfs_read_bytes(volume, 1024U, sb, EXTFS_SUPERBLOCK_SIZE);
    if (status != EXTFS_OK) {
        return status;
    }
    if (extfs_le16(sb + 0x38U) != 0xEF53U) {
        return EXTFS_ERR_NOT_EXT;
    }

    log_block_size = extfs_le32(sb + 0x18U);
    if (log_block_size > 6U) {
        return EXTFS_ERR_CORRUPT;
    }
    volume->block_size = 1024U << log_block_size;
    volume->total_inodes = extfs_le32(sb + 0x00U);
    volume->first_data_block = extfs_le32(sb + 0x14U);
    volume->blocks_per_group = extfs_le32(sb + 0x20U);
    volume->inodes_per_group = extfs_le32(sb + 0x28U);
    volume->state = extfs_le16(sb + 0x3AU);
    volume->revision = extfs_le32(sb + 0x4CU);
    volume->feature_compat = extfs_le32(sb + 0x5CU);
    volume->feature_incompat = extfs_le32(sb + 0x60U);
    volume->feature_ro_compat = extfs_le32(sb + 0x64U);
    volume->unsupported_incompat =
        volume->feature_incompat & ~EXTFS_SUPPORTED_INCOMPAT;
    volume->total_blocks = (extfs_u64)extfs_le32(sb + 0x04U);
    volume->free_blocks = (extfs_u64)extfs_le32(sb + 0x0CU);
    if ((volume->feature_incompat & EXTFS_INCOMPAT_64BIT) != 0U) {
        volume->total_blocks |= (extfs_u64)extfs_le32(sb + 0x150U) << 32;
        volume->free_blocks |= (extfs_u64)extfs_le32(sb + 0x158U) << 32;
    }

    if (volume->revision == 0U) {
        volume->inode_size = 128U;
    } else {
        volume->inode_size = extfs_le16(sb + 0x58U);
    }
    if ((volume->feature_incompat & EXTFS_INCOMPAT_64BIT) != 0U) {
        volume->descriptor_size = extfs_le16(sb + 0xFEU);
    } else {
        volume->descriptor_size = 32U;
    }

    if (volume->total_inodes < EXTFS_ROOT_INODE ||
        volume->total_blocks == 0U ||
        volume->free_blocks > volume->total_blocks ||
        volume->blocks_per_group == 0U || volume->inodes_per_group == 0U ||
        volume->first_data_block >= volume->total_blocks ||
        volume->inode_size < 128U || volume->inode_size > volume->block_size ||
        (volume->inode_size & 3U) != 0U ||
        volume->descriptor_size < 32U || volume->descriptor_size > 64U ||
        (volume->descriptor_size & 7U) != 0U ||
        ((volume->feature_incompat & EXTFS_INCOMPAT_64BIT) != 0U &&
         volume->descriptor_size < 64U) ||
        (volume->block_size == 1024U && volume->first_data_block != 1U) ||
        (volume->block_size != 1024U && volume->first_data_block != 0U) ||
        volume->inodes_per_group > volume->block_size * 8U ||
        (((volume->feature_ro_compat & EXTFS_RO_COMPAT_BIGALLOC) == 0U) &&
         volume->blocks_per_group > volume->block_size * 8U)) {
        return EXTFS_ERR_CORRUPT;
    }

    /* All later core I/O uses 64-bit byte offsets.  Reject a superblock whose
     * declared address space cannot be represented before any block-to-byte
     * multiplication is attempted. */
    if (extfs_mul_u64(volume->total_blocks, volume->block_size,
                      &volume->byte_size) == 0) {
        return EXTFS_ERR_RANGE;
    }

    data_blocks = volume->total_blocks - volume->first_data_block;
    group_count64 = extfs_div_round_up_u64(data_blocks,
                                           volume->blocks_per_group);
    if (group_count64 == 0U || group_count64 > EXTFS_U32_MAX_VALUE) {
        return EXTFS_ERR_RANGE;
    }
    volume->group_count = (extfs_u32)group_count64;

    inode_capacity = (extfs_u64)volume->group_count *
                     volume->inodes_per_group;
    if ((extfs_u64)volume->total_inodes > inode_capacity) {
        return EXTFS_ERR_CORRUPT;
    }

    /* For layouts understood by the reader, the primary group descriptor
     * table begins in the block following the primary superblock.  Unknown
     * incompat features may deliberately change metadata placement, so keep
     * extfs_open() useful for inspection and defer traversal refusal to the
     * data APIs rather than applying an ordinary-layout assumption here. */
    if (volume->unsupported_incompat == 0U &&
        (extfs_mul_u64((extfs_u64)volume->first_data_block + 1U,
                       volume->block_size, &descriptor_table_offset) == 0 ||
         extfs_mul_u64(volume->group_count, volume->descriptor_size,
                       &descriptor_table_bytes) == 0 ||
         extfs_add_u64(descriptor_table_offset, descriptor_table_bytes,
                       &descriptor_table_end) == 0 ||
         descriptor_table_end > volume->byte_size)) {
        return EXTFS_ERR_CORRUPT;
    }

    extfs_copy(volume->uuid, sb + 0x68U, 16U);
    extfs_copy(volume->journal_uuid, sb + 0xD0U, 16U);
    volume->journal_inode = extfs_le32(sb + 0xE0U);
    for (label_index = 0U; label_index < 16U; ++label_index) {
        char c = (char)sb[0x78U + label_index];
        volume->label[label_index] = c;
        if (c == '\0') {
            break;
        }
    }
    volume->label[16] = '\0';
    if (label_index == 16U) {
        volume->label[16] = '\0';
    }

    volume->metadata_checksums =
        (volume->feature_ro_compat & EXTFS_RO_COMPAT_METADATA_CSUM) != 0U;
    if (volume->metadata_checksums != 0U) {
        if (sb[0x175U] != 1U) {
            return EXTFS_ERR_UNSUPPORTED;
        }
        status = extfs_validate_superblock_checksum(sb);
        if (status != EXTFS_OK) {
            return status;
        }
        volume->superblock_checksum_valid = 1U;
        if ((volume->feature_incompat & EXTFS_INCOMPAT_CSUM_SEED) != 0U) {
            volume->checksum_seed = extfs_le32(sb + 0x270U);
        } else {
            volume->checksum_seed = extfs_crc32c(
                0xFFFFFFFFU, volume->uuid, 16U);
        }
    }

    if ((volume->feature_incompat &
         (EXTFS_INCOMPAT_EXTENTS | EXTFS_INCOMPAT_64BIT |
          EXTFS_INCOMPAT_FLEX_BG | EXTFS_INCOMPAT_CSUM_SEED)) != 0U ||
        volume->metadata_checksums != 0U) {
        volume->kind = EXTFS_KIND_EXT4;
    } else if ((volume->feature_compat & EXTFS_COMPAT_HAS_JOURNAL) != 0U) {
        volume->kind = EXTFS_KIND_EXT3;
    } else {
        volume->kind = EXTFS_KIND_EXT2;
    }

    return EXTFS_OK;
}

extfs_status extfs_readonly_assess(const extfs_volume *volume,
                                   extfs_u32 *risk_flags)
{
    extfs_u32 risks = 0U;
    if (volume == 0 || risk_flags == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    if ((volume->state & EXTFS_STATE_VALID) == 0U) {
        risks |= EXTFS_READONLY_RISK_DIRTY;
    }
    if ((volume->state & EXTFS_STATE_ERROR) != 0U) {
        risks |= EXTFS_READONLY_RISK_ERROR_STATE;
    }
    if ((volume->feature_incompat & EXTFS_INCOMPAT_RECOVER) != 0U ||
        (volume->feature_ro_compat & EXTFS_RO_COMPAT_ORPHAN_PRESENT) != 0U) {
        risks |= EXTFS_READONLY_RISK_NEEDS_RECOVERY;
    }
    if (volume->unsupported_incompat != 0U) {
        risks |= EXTFS_READONLY_RISK_UNSUPPORTED_INCOMPAT;
    }
    if ((volume->feature_incompat &
         (EXTFS_INCOMPAT_COMPRESSION | EXTFS_INCOMPAT_JOURNAL_DEV |
          EXTFS_INCOMPAT_META_BG | EXTFS_INCOMPAT_DIRDATA |
          EXTFS_INCOMPAT_INLINE_DATA | EXTFS_INCOMPAT_ENCRYPT |
          EXTFS_INCOMPAT_CASEFOLD)) != 0U ||
        (volume->feature_ro_compat & EXTFS_RO_COMPAT_BIGALLOC) != 0U) {
        risks |= EXTFS_READONLY_RISK_UNSUPPORTED_LAYOUT;
    }
    if ((volume->feature_ro_compat & EXTFS_RO_COMPAT_GDT_CSUM) != 0U &&
        volume->metadata_checksums == 0U) {
        risks |= EXTFS_READONLY_RISK_UNVERIFIED_CHECKSUMS;
    }
    *risk_flags = risks;
    return risks == 0U ? EXTFS_OK : EXTFS_ERR_UNSUPPORTED;
}

extfs_status extfs_write_assess(const extfs_volume *volume,
                                extfs_u32 *risk_flags)
{
    extfs_u32 risks = 0U;
    extfs_u32 readonly_risks = 0U;
    if (volume == 0 || risk_flags == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    if (volume->io.write_at == 0) {
        risks |= EXTFS_WRITE_RISK_NO_WRITER;
    }
    if (extfs_readonly_assess(volume, &readonly_risks) != EXTFS_OK) {
        risks |= EXTFS_WRITE_RISK_READONLY_POLICY;
    }
    if ((volume->feature_ro_compat & ~EXTFS_WRITE_SAFE_RO_COMPAT) != 0U ||
        (volume->feature_ro_compat &
         (EXTFS_RO_COMPAT_READONLY | EXTFS_RO_COMPAT_VERITY)) != 0U) {
        risks |= EXTFS_WRITE_RISK_UNSUPPORTED_RO_COMPAT;
    }
    if ((volume->feature_incompat & EXTFS_INCOMPAT_MMP) != 0U) {
        risks |= EXTFS_WRITE_RISK_MMP;
    }
    *risk_flags = risks;
    return risks == 0U ? EXTFS_OK : EXTFS_ERR_UNSUPPORTED;
}

extfs_status extfs_inode_write_assess(const extfs_volume *volume,
                                      const extfs_inode *inode)
{
    extfs_u32 risks = 0U;
    if (volume == 0 || inode == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    if (extfs_write_assess(volume, &risks) != EXTFS_OK) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    if (extfs_inode_type(inode) != EXTFS_NODE_REGULAR) {
        return extfs_inode_type(inode) == EXTFS_NODE_DIRECTORY
            ? EXTFS_ERR_IS_DIRECTORY : EXTFS_ERR_UNSUPPORTED;
    }
    if ((inode->flags & (EXTFS_INODE_FLAG_IMMUTABLE |
                         EXTFS_INODE_FLAG_APPEND |
                         EXTFS_INODE_FLAG_ENCRYPT |
                         EXTFS_INODE_FLAG_JOURNAL_DATA |
                         EXTFS_INODE_FLAG_VERITY |
                         EXTFS_INODE_FLAG_INLINE_DATA)) != 0U) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    return EXTFS_OK;
}

/* Traversal is stricter than superblock inspection.  Incompat features can
 * redefine metadata placement or data interpretation; bigalloc changes block
 * mapping units, and legacy gdt_csum descriptors are not yet authenticated by
 * this core.  Refuse these before following any on-disk pointer. */
static int extfs_traversal_supported(const extfs_volume *volume)
{
    return volume != 0 &&
           volume->unsupported_incompat == 0U &&
           (volume->feature_ro_compat & EXTFS_RO_COMPAT_BIGALLOC) == 0U &&
           !((volume->feature_ro_compat & EXTFS_RO_COMPAT_GDT_CSUM) != 0U &&
             volume->metadata_checksums == 0U);
}

static extfs_status extfs_group_descriptor_byte_offset(
    const extfs_volume *volume, extfs_u32 group, extfs_u64 *offset)
{
    extfs_u64 table_block;
    extfs_u64 descriptor_delta;
    extfs_status status;
    if (volume == 0 || offset == 0 || group >= volume->group_count) {
        return EXTFS_ERR_RANGE;
    }
    table_block = (extfs_u64)volume->first_data_block + 1U;
    status = extfs_block_byte_offset(volume, table_block, 0U, offset);
    if (status != EXTFS_OK) {
        return status;
    }
    descriptor_delta = (extfs_u64)group * volume->descriptor_size;
    if (extfs_add_u64(*offset, descriptor_delta, offset) == 0 ||
        *offset > volume->byte_size - volume->descriptor_size) {
        return EXTFS_ERR_CORRUPT;
    }
    return EXTFS_OK;
}

/*
 * Group descriptors live immediately after the primary superblock block.
 * ext4 metadata_csum covers the descriptor with bg_checksum itself zeroed and
 * prefixes the group number, so validate before trusting inode-table offsets.
 */
static extfs_status extfs_read_group_descriptor(const extfs_volume *volume,
                                                 extfs_u32 group,
                                                 extfs_u8 descriptor[64])
{
    extfs_u64 offset;
    extfs_status status;
    status = extfs_group_descriptor_byte_offset(volume, group, &offset);
    if (status != EXTFS_OK) {
        return status;
    }
    status = extfs_read_bytes(volume, offset, descriptor,
                              volume->descriptor_size);
    if (status != EXTFS_OK) {
        return status;
    }
    if (volume->metadata_checksums != 0U) {
        extfs_u8 group_le[4];
        extfs_u8 zeros[2] = {0U, 0U};
        extfs_u32 crc = volume->checksum_seed;
        extfs_u16 stored = extfs_le16(descriptor + 0x1EU);
        extfs_store_le32(group_le, group);
        crc = extfs_crc32c(crc, group_le, 4U);
        crc = extfs_crc32c(crc, descriptor, 0x1EU);
        crc = extfs_crc32c(crc, zeros, 2U);
        if (volume->descriptor_size > 0x20U) {
            crc = extfs_crc32c(crc, descriptor + 0x20U,
                               volume->descriptor_size - 0x20U);
        }
        if ((extfs_u16)crc != stored) {
            return EXTFS_ERR_CHECKSUM;
        }
    }
    return EXTFS_OK;
}

static extfs_status extfs_inode_byte_offset(const extfs_volume *volume,
                                              extfs_u32 inode_number,
                                              extfs_u64 *byte_offset,
                                              extfs_u32 *inode_group)
{
    extfs_u8 descriptor[64] = {0U};
    extfs_u32 group;
    extfs_u32 index;
    extfs_u64 inode_table;
    extfs_u64 inode_delta;
    extfs_status status;

    if (volume == 0 || byte_offset == 0 || inode_number == 0U ||
        inode_number > volume->total_inodes) {
        return EXTFS_ERR_RANGE;
    }
    group = (inode_number - 1U) / volume->inodes_per_group;
    index = (inode_number - 1U) % volume->inodes_per_group;
    status = extfs_read_group_descriptor(volume, group, descriptor);
    if (status != EXTFS_OK) {
        return status;
    }
    inode_table = extfs_le32(descriptor + 0x08U);
    if (volume->descriptor_size >= 64U) {
        inode_table |= (extfs_u64)extfs_le32(descriptor + 0x28U) << 32;
    }
    if (inode_table == 0U || inode_table >= volume->total_blocks) {
        return EXTFS_ERR_CORRUPT;
    }
    status = extfs_block_byte_offset(volume, inode_table, 0U, byte_offset);
    if (status != EXTFS_OK) {
        return status;
    }
    inode_delta = (extfs_u64)index * volume->inode_size;
    if (extfs_add_u64(*byte_offset, inode_delta, byte_offset) == 0 ||
        *byte_offset > volume->byte_size - volume->inode_size) {
        return EXTFS_ERR_CORRUPT;
    }
    if (inode_group != 0) {
        *inode_group = group;
    }
    return EXTFS_OK;
}

/*
 * ext4 inode checksums cover inode number, generation and the complete on-disk
 * inode while treating the checksum fields as zero.  Old 128-byte/short-extra
 * inodes carry only the low 16 bits; larger inodes may carry all 32 bits.
 */
static extfs_status extfs_validate_inode_checksum(const extfs_volume *volume,
                                                  extfs_u32 inode_number,
                                                  const extfs_u8 *raw)
{
    extfs_u8 number_le[4];
    extfs_u8 zeros[2] = {0U, 0U};
    extfs_u32 expected;
    extfs_u32 crc;
    extfs_u16 extra_size = 0U;
    int has_high = 0;

    if (volume->metadata_checksums == 0U) {
        return EXTFS_OK;
    }
    expected = extfs_le16(raw + 0x7CU);
    if (volume->inode_size >= 0x84U) {
        extra_size = extfs_le16(raw + 0x80U);
        if (extra_size >= 4U) {
            expected |= (extfs_u32)extfs_le16(raw + 0x82U) << 16;
            has_high = 1;
        }
    }

    extfs_store_le32(number_le, inode_number);
    crc = extfs_crc32c(volume->checksum_seed, number_le, 4U);
    crc = extfs_crc32c(crc, raw + 0x64U, 4U);
    crc = extfs_crc32c(crc, raw, 0x7CU);
    crc = extfs_crc32c(crc, zeros, 2U);
    if (has_high != 0) {
        crc = extfs_crc32c(crc, raw + 0x7EU, 4U);
        crc = extfs_crc32c(crc, zeros, 2U);
        if (volume->inode_size > 0x84U) {
            crc = extfs_crc32c(crc, raw + 0x84U,
                               volume->inode_size - 0x84U);
        }
    } else if (volume->inode_size > 0x7EU) {
        crc = extfs_crc32c(crc, raw + 0x7EU,
                           volume->inode_size - 0x7EU);
    }

    if (has_high != 0) {
        return crc == expected ? EXTFS_OK : EXTFS_ERR_CHECKSUM;
    }
    return (extfs_u16)crc == (extfs_u16)expected
        ? EXTFS_OK : EXTFS_ERR_CHECKSUM;
}

/* Rebuild an ext4 metadata_csum inode checksum after mutating the raw
 * inode image.  The checksum seed is the filesystem seed followed by the
 * inode number and generation, matching ext4's on-disk definition. */
static extfs_status extfs_store_inode_checksum(const extfs_volume *volume,
                                                extfs_u32 inode_number,
                                                extfs_u8 *raw)
{
    extfs_u8 number_le[4];
    extfs_u8 zeros[2] = {0U, 0U};
    extfs_u32 crc;
    extfs_u16 extra_size = 0U;
    int has_high = 0;

    if (volume == 0 || raw == 0 || volume->metadata_checksums == 0U)
        return EXTFS_ERR_INVALID_ARGUMENT;
    if (volume->inode_size < 128U) return EXTFS_ERR_CORRUPT;

    if (volume->inode_size >= 0x84U) {
        extra_size = extfs_le16(raw + 0x80U);
        if (extra_size >= 4U) has_high = 1;
    }
    extfs_store_le16(raw + 0x7CU, 0U);
    if (has_high != 0) extfs_store_le16(raw + 0x82U, 0U);

    extfs_store_le32(number_le, inode_number);
    crc = extfs_crc32c(volume->checksum_seed, number_le, 4U);
    crc = extfs_crc32c(crc, raw + 0x64U, 4U);
    crc = extfs_crc32c(crc, raw, 0x7CU);
    crc = extfs_crc32c(crc, zeros, 2U);
    if (has_high != 0) {
        crc = extfs_crc32c(crc, raw + 0x7EU, 4U);
        crc = extfs_crc32c(crc, zeros, 2U);
        if (volume->inode_size > 0x84U)
            crc = extfs_crc32c(crc, raw + 0x84U,
                               volume->inode_size - 0x84U);
    } else if (volume->inode_size > 0x7EU) {
        crc = extfs_crc32c(crc, raw + 0x7EU,
                           volume->inode_size - 0x7EU);
    }
    extfs_store_le16(raw + 0x7CU, (extfs_u16)crc);
    if (has_high != 0) extfs_store_le16(raw + 0x82U, (extfs_u16)(crc >> 16));
    return extfs_validate_inode_checksum(volume, inode_number, raw);
}

static extfs_status extfs_ext4_validate_block_bitmap_checksum(
    const extfs_volume *volume,
    const extfs_u8 *bitmap,
    const extfs_u8 *descriptor)
{
    extfs_u32 bitmap_bytes;
    extfs_u32 crc;
    if (volume == 0 || bitmap == 0 || descriptor == 0 ||
        volume->metadata_checksums == 0U ||
        (volume->blocks_per_group & 7U) != 0U ||
        volume->blocks_per_group / 8U > volume->block_size) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    bitmap_bytes = volume->blocks_per_group / 8U;
    crc = extfs_crc32c(volume->checksum_seed, bitmap, bitmap_bytes);
    return (extfs_u16)crc == extfs_le16(descriptor + 0x18U)
        ? EXTFS_OK : EXTFS_ERR_CHECKSUM;
}

/* Rebuild the metadata_csum fields associated with an ext4 block bitmap.
 * 0.9.1 only mutates non-bigalloc filesystems, so the checksum length is the
 * ordinary blocks-per-group bitmap length.  With a 32-byte descriptor only
 * the low 16 bits of the bitmap checksum are stored. */
static extfs_status extfs_ext4_store_bitmap_group_checksums(
    const extfs_volume *volume,
    extfs_u32 group,
    const extfs_u8 *bitmap,
    extfs_u8 *descriptor)
{
    extfs_u8 group_le[4];
    extfs_u8 zeros[2] = {0U, 0U};
    extfs_u32 bitmap_bytes;
    extfs_u32 crc;

    if (volume == 0 || bitmap == 0 || descriptor == 0 ||
        volume->metadata_checksums == 0U ||
        (volume->feature_ro_compat & EXTFS_RO_COMPAT_BIGALLOC) != 0U ||
        (volume->blocks_per_group & 7U) != 0U ||
        volume->blocks_per_group / 8U > volume->block_size ||
        volume->descriptor_size != 32U) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    bitmap_bytes = volume->blocks_per_group / 8U;
    crc = extfs_crc32c(volume->checksum_seed, bitmap, bitmap_bytes);
    extfs_store_le16(descriptor + 0x18U, (extfs_u16)crc);

    extfs_store_le16(descriptor + 0x1EU, 0U);
    extfs_store_le32(group_le, group);
    crc = extfs_crc32c(volume->checksum_seed, group_le, 4U);
    crc = extfs_crc32c(crc, descriptor, 0x1EU);
    crc = extfs_crc32c(crc, zeros, 2U);
    extfs_store_le16(descriptor + 0x1EU, (extfs_u16)crc);
    return EXTFS_OK;
}

static void extfs_ext4_store_superblock_checksum(extfs_u8 *superblock)
{
    extfs_store_le32(superblock + 0x3FCU,
                     extfs_crc32c(0xFFFFFFFFU, superblock, 0x3FCU));
}

/* Extended inode fields are valid only when both the physical inode record and
 * i_extra_isize encompass the complete field.  Old inodes can legitimately
 * have a large record size but no usable extended fields. */
static int extfs_inode_extra_field_fits(const extfs_volume *volume,
                                        const extfs_u8 *raw,
                                        extfs_u32 field_offset,
                                        extfs_u32 field_size)
{
    extfs_u32 extra_size;
    extfs_u32 field_end;
    if (volume->inode_size <= 128U || field_offset < 128U ||
        field_size > EXTFS_U32_MAX_VALUE - field_offset) {
        return 0;
    }
    field_end = field_offset + field_size;
    if (field_end > volume->inode_size) {
        return 0;
    }
    extra_size = extfs_le16(raw + 0x80U);
    return field_end <= 128U + extra_size;
}

/* ext4 widens a signed 32-bit Unix timestamp with two epoch bits stored in
 * the low bits of the corresponding *_extra word; the remaining 30 bits are
 * nanoseconds.  This mirrors the kernel encoding, including pre-1970 times. */
static extfs_status extfs_decode_inode_time(const extfs_u8 *raw,
                                            extfs_u32 base_offset,
                                            int has_extra,
                                            extfs_u32 extra_offset,
                                            extfs_s64 *seconds,
                                            extfs_u32 *nanoseconds)
{
    extfs_u32 base;
    extfs_s64 decoded;
    extfs_u32 extra = 0U;
    extfs_u32 nsec = 0U;
    if (raw == 0 || seconds == 0 || nanoseconds == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    base = extfs_le32(raw + base_offset);
    decoded = (base & 0x80000000U) != 0U
        ? (extfs_s64)(extfs_u64)base - 0x100000000LL
        : (extfs_s64)base;
    if (has_extra != 0) {
        extra = extfs_le32(raw + extra_offset);
        nsec = extra >> 2;
        if (nsec >= 1000000000U) {
            return EXTFS_ERR_CORRUPT;
        }
        decoded += (extfs_s64)(extra & 3U) * 0x100000000LL;
    }
    *seconds = decoded;
    *nanoseconds = nsec;
    return EXTFS_OK;
}

/* Resolve inode -> group -> inode-table byte offset, then decode only after
 * the group descriptor and inode checksum have passed validation. */
extfs_status extfs_read_inode(const extfs_volume *volume,
                              extfs_u32 inode_number,
                              extfs_inode *inode,
                              void *scratch,
                              extfs_u32 scratch_size)
{
    extfs_u8 descriptor[64] = {0U};
    extfs_u8 *raw = (extfs_u8 *)scratch;
    extfs_u32 group;
    extfs_u32 index;
    extfs_u64 inode_table;
    extfs_u64 inode_delta;
    extfs_u64 byte_offset;
    extfs_status status;

    if (volume == 0 || inode == 0 || scratch == 0 || inode_number == 0U) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    if (extfs_traversal_supported(volume) == 0) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    if (inode_number > volume->total_inodes) {
        return EXTFS_ERR_RANGE;
    }
    if (scratch_size < volume->inode_size) {
        return EXTFS_ERR_BUFFER_TOO_SMALL;
    }

    group = (inode_number - 1U) / volume->inodes_per_group;
    index = (inode_number - 1U) % volume->inodes_per_group;
    status = extfs_read_group_descriptor(volume, group, descriptor);
    if (status != EXTFS_OK) {
        return status;
    }
    inode_table = extfs_le32(descriptor + 0x08U);
    if (volume->descriptor_size >= 64U) {
        inode_table |= (extfs_u64)extfs_le32(descriptor + 0x28U) << 32;
    }
    if (inode_table == 0U || inode_table >= volume->total_blocks) {
        return EXTFS_ERR_CORRUPT;
    }
    status = extfs_block_byte_offset(volume, inode_table, 0U, &byte_offset);
    if (status != EXTFS_OK) {
        return status;
    }
    inode_delta = (extfs_u64)index * volume->inode_size;
    if (extfs_add_u64(byte_offset, inode_delta, &byte_offset) == 0 ||
        byte_offset > volume->byte_size - volume->inode_size) {
        return EXTFS_ERR_CORRUPT;
    }
    status = extfs_read_bytes(volume, byte_offset, raw, volume->inode_size);
    if (status != EXTFS_OK) {
        return status;
    }
    if (volume->inode_size > 128U &&
        extfs_le16(raw + 0x80U) > volume->inode_size - 128U) {
        return EXTFS_ERR_CORRUPT;
    }
    status = extfs_validate_inode_checksum(volume, inode_number, raw);
    if (status != EXTFS_OK) {
        return status;
    }

    extfs_zero(inode, (extfs_u32)sizeof(*inode));
    inode->number = inode_number;
    inode->mode = extfs_le16(raw + 0x00U);
    inode->uid = extfs_le16(raw + 0x02U);
    inode->size = extfs_le32(raw + 0x04U);
    inode->gid = extfs_le16(raw + 0x18U);
    inode->links_count = extfs_le16(raw + 0x1AU);
    inode->flags = extfs_le32(raw + 0x20U);
    inode->generation = extfs_le32(raw + 0x64U);
    status = extfs_decode_inode_time(
        raw, 0x08U, extfs_inode_extra_field_fits(volume, raw, 0x8CU, 4U),
        0x8CU, &inode->access_time, &inode->access_time_nanoseconds);
    if (status != EXTFS_OK) return status;
    status = extfs_decode_inode_time(
        raw, 0x0CU, extfs_inode_extra_field_fits(volume, raw, 0x84U, 4U),
        0x84U, &inode->change_time, &inode->change_time_nanoseconds);
    if (status != EXTFS_OK) return status;
    status = extfs_decode_inode_time(
        raw, 0x10U, extfs_inode_extra_field_fits(volume, raw, 0x88U, 4U),
        0x88U, &inode->modification_time,
        &inode->modification_time_nanoseconds);
    if (status != EXTFS_OK) return status;
    inode->creation_time = inode->change_time;
    inode->creation_time_nanoseconds = inode->change_time_nanoseconds;
    if (volume->inode_size >= 128U) {
        inode->uid |= (extfs_u32)extfs_le16(raw + 0x78U) << 16;
        inode->gid |= (extfs_u32)extfs_le16(raw + 0x7AU) << 16;
    }
    if ((inode->mode & EXTFS_MODE_TYPE_MASK) == EXTFS_MODE_REGULAR ||
        (inode->mode & EXTFS_MODE_TYPE_MASK) == EXTFS_MODE_DIRECTORY ||
        (inode->mode & EXTFS_MODE_TYPE_MASK) == EXTFS_MODE_SYMLINK) {
        inode->size |= (extfs_u64)extfs_le32(raw + 0x6CU) << 32;
    }
    /* Birth time begins at 0x90; its nanosecond/epoch extension at 0x94 is
     * optional and must independently fit inside i_extra_isize. */
    if (extfs_inode_extra_field_fits(volume, raw, 0x90U, 4U) != 0) {
        status = extfs_decode_inode_time(
            raw, 0x90U,
            extfs_inode_extra_field_fits(volume, raw, 0x94U, 4U), 0x94U,
            &inode->creation_time, &inode->creation_time_nanoseconds);
        if (status != EXTFS_OK) return status;
    }
    if ((extfs_inode_type(inode) == EXTFS_NODE_REGULAR ||
         extfs_inode_type(inode) == EXTFS_NODE_DIRECTORY ||
         extfs_inode_type(inode) == EXTFS_NODE_SYMLINK) &&
        inode->size > 0x100000000ULL * volume->block_size) {
        return EXTFS_ERR_RANGE;
    }
    extfs_copy(inode->block_map, raw + 0x28U, 60U);
    return EXTFS_OK;
}

/* External extent-tree nodes append an ext4 metadata checksum tail. */
static extfs_status extfs_validate_extent_block(const extfs_volume *volume,
                                                const extfs_inode *inode,
                                                const extfs_u8 *block)
{
    extfs_u8 number_le[4];
    extfs_u8 generation_le[4];
    extfs_u32 maximum;
    extfs_u32 tail_offset;
    extfs_u32 crc;
    extfs_u32 stored;
    if (volume == 0 || inode == 0 || block == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    if (volume->metadata_checksums == 0U) return EXTFS_OK;
    if (volume->block_size < 16U ||
        extfs_le16(block + 0x00U) != EXTFS_EXTENT_MAGIC)
        return EXTFS_ERR_CORRUPT;

    /* Linux locates ext4_extent_tail immediately after eh_max records, not
     * blindly at block_size-4. They coincide for common 1K/4K blocks but not
     * for every valid ext4 block size (notably 2K/8K). */
    maximum = extfs_le16(block + 0x04U);
    if (maximum > (volume->block_size - 12U) / 12U)
        return EXTFS_ERR_CORRUPT;
    tail_offset = 12U + maximum * 12U;
    if (tail_offset > volume->block_size - 4U)
        return EXTFS_ERR_CORRUPT;

    extfs_store_le32(number_le, inode->number);
    extfs_store_le32(generation_le, inode->generation);
    crc = extfs_crc32c(volume->checksum_seed, number_le, 4U);
    crc = extfs_crc32c(crc, generation_le, 4U);
    crc = extfs_crc32c(crc, block, tail_offset);
    stored = extfs_le32(block + tail_offset);
    return crc == stored ? EXTFS_OK : EXTFS_ERR_CHECKSUM;
}

/*
 * Walk an ext4 extent B-tree without recursion.  The inode's 60-byte i_block
 * area is the root; index entries select the last child whose logical start is
 * <= the requested block until a leaf extent is reached.  Unwritten extents
 * are exposed as holes to readers; the bounded writer refuses them rather than
 * performing the metadata conversion needed to initialise them.
 */
static extfs_status extfs_map_extent(const extfs_volume *volume,
                                     const extfs_inode *inode,
                                     extfs_u32 logical_block,
                                     extfs_u64 *physical_block,
                                     int *is_hole,
                                     extfs_u8 *scratch)
{
    const extfs_u8 *node = inode->block_map;
    extfs_u32 node_size = 60U;
    extfs_u16 expected_depth = 0U;
    int root = 1;

    for (;;) {
        extfs_u16 entries;
        extfs_u16 maximum;
        extfs_u16 depth;
        extfs_u32 i;
        if (node_size < 12U || extfs_le16(node) != EXTFS_EXTENT_MAGIC) {
            return EXTFS_ERR_CORRUPT;
        }
        entries = extfs_le16(node + 0x02U);
        maximum = extfs_le16(node + 0x04U);
        depth = extfs_le16(node + 0x06U);
        if (depth > 5U || entries > maximum ||
            maximum > (node_size - 12U) / 12U ||
            12U + (extfs_u32)entries * 12U > node_size) {
            return EXTFS_ERR_CORRUPT;
        }
        if (root == 0 && depth != expected_depth) {
            return EXTFS_ERR_CORRUPT;
        }
        if (depth == 0U) {
            extfs_u64 previous_end = 0U;
            extfs_u64 matched_physical = 0U;
            int matched = 0;
            int matched_hole = 1;
            for (i = 0U; i < entries; ++i) {
                const extfs_u8 *extent = node + 12U + i * 12U;
                extfs_u32 first = extfs_le32(extent + 0x00U);
                extfs_u16 encoded_length = extfs_le16(extent + 0x04U);
                extfs_u32 length = encoded_length > 32768U
                    ? (extfs_u32)encoded_length - 32768U
                    : encoded_length;
                extfs_u64 logical_end;
                extfs_u64 start = extfs_le32(extent + 0x08U);
                start |= (extfs_u64)extfs_le16(extent + 0x06U) << 32;
                if (length == 0U) {
                    return EXTFS_ERR_CORRUPT;
                }
                logical_end = (extfs_u64)first + length;
                if (logical_end > 0x100000000ULL ||
                    (i != 0U && (extfs_u64)first < previous_end) ||
                    start == 0U || start >= volume->total_blocks ||
                    (extfs_u64)length > volume->total_blocks - start) {
                    return EXTFS_ERR_CORRUPT;
                }
                previous_end = logical_end;
                if (logical_block >= first &&
                    (extfs_u64)logical_block < logical_end) {
                    matched = 1;
                    matched_hole = encoded_length > 32768U;
                    if (matched_hole == 0) {
                        matched_physical = start + logical_block - first;
                    }
                }
            }
            *physical_block = matched_hole != 0 ? 0U : matched_physical;
            *is_hole = matched == 0 || matched_hole != 0;
            return EXTFS_OK;
        } else {
            const extfs_u8 *selected = 0;
            extfs_u64 child;
            extfs_status status;
            if (entries == 0U) {
                return EXTFS_ERR_CORRUPT;
            }
            {
                extfs_u32 previous_first = 0U;
                for (i = 0U; i < entries; ++i) {
                    const extfs_u8 *index = node + 12U + i * 12U;
                    extfs_u32 first = extfs_le32(index);
                    extfs_u64 candidate = extfs_le32(index + 0x04U);
                    candidate |= (extfs_u64)extfs_le16(index + 0x08U) << 32;
                    if ((i != 0U && first <= previous_first) ||
                        candidate == 0U || candidate >= volume->total_blocks) {
                        return EXTFS_ERR_CORRUPT;
                    }
                    previous_first = first;
                    if (first > logical_block) {
                        continue;
                    }
                    selected = index;
                }
            }
            if (selected == 0) {
                *physical_block = 0U;
                *is_hole = 1;
                return EXTFS_OK;
            }
            child = extfs_le32(selected + 0x04U);
            child |= (extfs_u64)extfs_le16(selected + 0x08U) << 32;
            if (child == 0U || child >= volume->total_blocks) {
                return EXTFS_ERR_CORRUPT;
            }
            {
                extfs_u64 child_offset;
                status = extfs_block_byte_offset(volume, child, 0U,
                                                 &child_offset);
                if (status != EXTFS_OK) {
                    return status;
                }
                status = extfs_read_bytes(volume, child_offset, scratch,
                                          volume->block_size);
            }
            if (status != EXTFS_OK) {
                return status;
            }
            status = extfs_validate_extent_block(volume, inode, scratch);
            if (status != EXTFS_OK) {
                return status;
            }
            expected_depth = (extfs_u16)(depth - 1U);
            node = scratch;
            node_size = volume->block_size -
                        (volume->metadata_checksums != 0U ? 4U : 0U);
            root = 0;
        }
    }
}

/*
 * ext2/ext3 classic mapping uses 12 direct pointers followed by single, double
 * and triple indirection.  At each level the remaining logical-block number is
 * decomposed in base (block_size / 4) to select 32-bit block pointers.
 */
static extfs_status extfs_map_classic(const extfs_volume *volume,
                                      const extfs_inode *inode,
                                      extfs_u32 logical_block,
                                      extfs_u64 *physical_block,
                                      int *is_hole,
                                      extfs_u8 *scratch)
{
    extfs_u64 relative = logical_block;
    extfs_u64 pointers_per_block = volume->block_size / 4U;
    extfs_u64 single_capacity = pointers_per_block;
    extfs_u64 double_capacity = pointers_per_block * pointers_per_block;
    extfs_u64 triple_capacity = double_capacity * pointers_per_block;
    extfs_u32 indices[3] = {0U, 0U, 0U};
    extfs_u32 root_index;
    extfs_u32 levels;
    extfs_u32 level;
    extfs_u64 block;
    extfs_status status;

    if (relative < 12U) {
        block = extfs_le32(inode->block_map + (extfs_u32)relative * 4U);
        if (block == 0U) {
            *physical_block = 0U;
            *is_hole = 1;
            return EXTFS_OK;
        }
        if (block >= volume->total_blocks) {
            return EXTFS_ERR_CORRUPT;
        }
        *physical_block = block;
        *is_hole = 0;
        return EXTFS_OK;
    }

    relative -= 12U;
    if (relative < single_capacity) {
        root_index = 12U;
        levels = 1U;
        indices[0] = (extfs_u32)relative;
    } else if ((relative -= single_capacity) < double_capacity) {
        root_index = 13U;
        levels = 2U;
        indices[0] = (extfs_u32)(relative / pointers_per_block);
        indices[1] = (extfs_u32)(relative % pointers_per_block);
    } else if ((relative -= double_capacity) < triple_capacity) {
        root_index = 14U;
        levels = 3U;
        indices[0] = (extfs_u32)(relative / double_capacity);
        indices[1] = (extfs_u32)((relative / pointers_per_block) %
                                 pointers_per_block);
        indices[2] = (extfs_u32)(relative % pointers_per_block);
    } else {
        return EXTFS_ERR_RANGE;
    }

    block = extfs_le32(inode->block_map + root_index * 4U);
    if (block == 0U) {
        *physical_block = 0U;
        *is_hole = 1;
        return EXTFS_OK;
    }
    for (level = 0U; level < levels; ++level) {
        if (block >= volume->total_blocks) {
            return EXTFS_ERR_CORRUPT;
        }
        {
            extfs_u64 block_offset;
            status = extfs_block_byte_offset(volume, block, 0U,
                                             &block_offset);
            if (status != EXTFS_OK) {
                return status;
            }
            status = extfs_read_bytes(volume, block_offset, scratch,
                                      volume->block_size);
        }
        if (status != EXTFS_OK) {
            return status;
        }
        block = extfs_le32(scratch + indices[level] * 4U);
        if (block == 0U) {
            *physical_block = 0U;
            *is_hole = 1;
            return EXTFS_OK;
        }
    }
    if (block >= volume->total_blocks) {
        return EXTFS_ERR_CORRUPT;
    }
    *physical_block = block;
    *is_hole = 0;
    return EXTFS_OK;
}

extfs_status extfs_map_file_block(const extfs_volume *volume,
                                  const extfs_inode *inode,
                                  extfs_u32 logical_block,
                                  extfs_u64 *physical_block,
                                  int *is_hole,
                                  void *scratch,
                                  extfs_u32 scratch_size)
{
    if (volume == 0 || inode == 0 || physical_block == 0 || is_hole == 0 ||
        scratch == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    if (extfs_traversal_supported(volume) == 0) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    if (scratch_size < volume->block_size) {
        return EXTFS_ERR_BUFFER_TOO_SMALL;
    }
    if (extfs_inode_type(inode) != EXTFS_NODE_REGULAR &&
        extfs_inode_type(inode) != EXTFS_NODE_DIRECTORY &&
        extfs_inode_type(inode) != EXTFS_NODE_SYMLINK) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    if ((inode->flags & EXTFS_INODE_FLAG_EXTENTS) != 0U) {
        return extfs_map_extent(volume, inode, logical_block, physical_block,
                                is_hole, (extfs_u8 *)scratch);
    }
    return extfs_map_classic(volume, inode, logical_block, physical_block,
                             is_hole, (extfs_u8 *)scratch);
}

/*
 * File reads are assembled a block at a time.  This keeps the host I/O contract
 * simple, permits sparse ranges to be zero-filled, and never requires a caller
 * buffer larger than one filesystem-block scratch area.
 */
extfs_status extfs_read_file(const extfs_volume *volume,
                             const extfs_inode *inode,
                             extfs_u64 byte_offset,
                             void *destination,
                             extfs_u32 byte_count,
                             void *scratch,
                             extfs_u32 scratch_size,
                             extfs_u32 *bytes_read)
{
    extfs_u8 *output = (extfs_u8 *)destination;
    extfs_u32 remaining;
    extfs_u32 completed = 0U;
    if (volume == 0 || inode == 0 || destination == 0 || scratch == 0 ||
        bytes_read == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    *bytes_read = 0U;
    if (extfs_inode_type(inode) == EXTFS_NODE_DIRECTORY) {
        return EXTFS_ERR_IS_DIRECTORY;
    }
    if (extfs_inode_type(inode) != EXTFS_NODE_REGULAR &&
        extfs_inode_type(inode) != EXTFS_NODE_SYMLINK) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    if (byte_offset >= inode->size || byte_count == 0U) {
        return EXTFS_OK;
    }
    remaining = byte_count;
    if ((extfs_u64)remaining > inode->size - byte_offset) {
        remaining = (extfs_u32)(inode->size - byte_offset);
    }

    if (extfs_inode_type(inode) == EXTFS_NODE_SYMLINK && inode->size <= 60U) {
        extfs_copy(output, inode->block_map + (extfs_u32)byte_offset, remaining);
        *bytes_read = remaining;
        return EXTFS_OK;
    }

    while (remaining != 0U) {
        extfs_u64 logical64 = byte_offset / volume->block_size;
        extfs_u32 within = (extfs_u32)(byte_offset % volume->block_size);
        extfs_u32 chunk = volume->block_size - within;
        extfs_u64 physical;
        int hole;
        extfs_status status;
        if (logical64 > 0xFFFFFFFFULL) {
            return EXTFS_ERR_RANGE;
        }
        if (chunk > remaining) {
            chunk = remaining;
        }
        status = extfs_map_file_block(volume, inode, (extfs_u32)logical64,
                                      &physical, &hole, scratch, scratch_size);
        if (status != EXTFS_OK) {
            return status;
        }
        if (hole != 0) {
            extfs_zero(output + completed, chunk);
        } else {
            extfs_u64 physical_offset;
            status = extfs_block_byte_offset(volume, physical, within,
                                             &physical_offset);
            if (status == EXTFS_OK) {
                status = extfs_read_bytes(volume, physical_offset,
                                          output + completed, chunk);
            }
            if (status != EXTFS_OK) {
                return status;
            }
        }
        completed += chunk;
        remaining -= chunk;
        byte_offset += chunk;
    }
    *bytes_read = completed;
    return EXTFS_OK;
}

/*
 * The first write primitive deliberately changes data blocks only.  Keeping
 * i_size and the block map fixed means no allocation bitmap, extent tree,
 * inode checksum, directory record or journal metadata needs to be modified.
 * This is useful functionality, but it is intentionally not yet a general
 * read/write filesystem implementation.
 */
extfs_status extfs_write_file_existing(const extfs_volume *volume,
                                          const extfs_inode *inode,
                                          extfs_u64 byte_offset,
                                          const void *source,
                                          extfs_u32 byte_count,
                                          void *scratch,
                                          extfs_u32 scratch_size,
                                          extfs_u32 *bytes_written)
{
    const extfs_u8 *input = (const extfs_u8 *)source;
    extfs_u64 cursor;
    extfs_u32 remaining;
    extfs_u32 completed = 0U;

    if (volume == 0 || inode == 0 || source == 0 || scratch == 0 ||
        bytes_written == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    *bytes_written = 0U;
    if (scratch_size < volume->block_size) {
        return EXTFS_ERR_BUFFER_TOO_SMALL;
    }
    if (extfs_inode_write_assess(volume, inode) != EXTFS_OK) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    if (byte_count == 0U) {
        return EXTFS_OK;
    }
    if (byte_offset > inode->size ||
        (extfs_u64)byte_count > inode->size - byte_offset) {
        return EXTFS_ERR_RANGE;
    }

    /*
     * Preflight the complete range before changing a byte.  In particular, a
     * request that starts in an allocated block but later crosses a sparse or
     * unwritten block must fail at the filesystem-policy level before any
     * earlier allocated block is modified.
     */
    cursor = byte_offset;
    remaining = byte_count;
    while (remaining != 0U) {
        extfs_u64 logical64 = cursor / volume->block_size;
        extfs_u32 within = (extfs_u32)(cursor % volume->block_size);
        extfs_u32 chunk = volume->block_size - within;
        extfs_u64 physical;
        extfs_u64 physical_offset;
        int hole;
        extfs_status status;

        if (logical64 > 0xFFFFFFFFULL) {
            return EXTFS_ERR_RANGE;
        }
        if (chunk > remaining) {
            chunk = remaining;
        }
        status = extfs_map_file_block(volume, inode, (extfs_u32)logical64,
                                      &physical, &hole, scratch, scratch_size);
        if (status != EXTFS_OK) {
            return status;
        }
        if (hole != 0) {
            return EXTFS_ERR_UNSUPPORTED;
        }
        status = extfs_block_byte_offset(volume, physical, within,
                                         &physical_offset);
        if (status != EXTFS_OK) {
            return status;
        }
        remaining -= chunk;
        cursor += chunk;
    }

    cursor = byte_offset;
    remaining = byte_count;
    while (remaining != 0U) {
        extfs_u64 logical64 = cursor / volume->block_size;
        extfs_u32 within = (extfs_u32)(cursor % volume->block_size);
        extfs_u32 chunk = volume->block_size - within;
        extfs_u64 physical;
        extfs_u64 physical_offset;
        int hole;
        extfs_status status;

        if (chunk > remaining) {
            chunk = remaining;
        }
        status = extfs_map_file_block(volume, inode, (extfs_u32)logical64,
                                      &physical, &hole, scratch, scratch_size);
        if (status != EXTFS_OK || hole != 0) {
            /* Preflight already established this mapping.  A different result
             * now indicates an I/O/corruption condition rather than a normal
             * unsupported-range rejection. */
            *bytes_written = completed;
            return status != EXTFS_OK ? status : EXTFS_ERR_CORRUPT;
        }
        status = extfs_block_byte_offset(volume, physical, within,
                                         &physical_offset);
        if (status != EXTFS_OK) {
            *bytes_written = completed;
            return status;
        }
        status = extfs_write_bytes(volume, physical_offset,
                                   input + completed, chunk);
        if (status != EXTFS_OK) {
            *bytes_written = completed;
            return status;
        }
        completed += chunk;
        remaining -= chunk;
        cursor += chunk;
    }
    *bytes_written = completed;
    return EXTFS_OK;
}

static extfs_status extfs_ext2_group_for_block(const extfs_volume *volume,
                                                  extfs_u64 block,
                                                  extfs_u32 *group,
                                                  extfs_u32 *bit)
{
    extfs_u64 relative;
    extfs_u64 group64;
    if (volume == 0 || group == 0 || bit == 0 ||
        block < volume->first_data_block || block >= volume->total_blocks) {
        return EXTFS_ERR_CORRUPT;
    }
    relative = block - volume->first_data_block;
    group64 = relative / volume->blocks_per_group;
    if (group64 >= volume->group_count) {
        return EXTFS_ERR_CORRUPT;
    }
    *group = (extfs_u32)group64;
    *bit = (extfs_u32)(relative % volume->blocks_per_group);
    return EXTFS_OK;
}

static extfs_status extfs_ext2_group_bounds(const extfs_volume *volume,
                                             extfs_u32 group,
                                             extfs_u64 *first,
                                             extfs_u32 *count)
{
    extfs_u64 start;
    extfs_u64 remaining;
    if (volume == 0 || first == 0 || count == 0 ||
        group >= volume->group_count) {
        return EXTFS_ERR_RANGE;
    }
    start = (extfs_u64)volume->first_data_block +
            (extfs_u64)group * volume->blocks_per_group;
    if (start >= volume->total_blocks) {
        return EXTFS_ERR_CORRUPT;
    }
    remaining = volume->total_blocks - start;
    *first = start;
    *count = remaining < volume->blocks_per_group
        ? (extfs_u32)remaining : volume->blocks_per_group;
    return EXTFS_OK;
}

static int extfs_bitmap_test(const extfs_u8 *bitmap, extfs_u32 bit)
{
    return (bitmap[bit >> 3] & (extfs_u8)(1U << (bit & 7U))) != 0U;
}

static void extfs_bitmap_set(extfs_u8 *bitmap, extfs_u32 bit)
{
    bitmap[bit >> 3] |= (extfs_u8)(1U << (bit & 7U));
}

static void extfs_bitmap_clear(extfs_u8 *bitmap, extfs_u32 bit)
{
    bitmap[bit >> 3] &= (extfs_u8)~(extfs_u8)(1U << (bit & 7U));
}

static extfs_status extfs_ext2_write_group_descriptor(extfs_volume *volume,
                                                       extfs_u32 group,
                                                       const extfs_u8 *descriptor)
{
    extfs_u64 offset;
    extfs_status status;
    if (volume == 0 || descriptor == 0 || volume->kind != EXTFS_KIND_EXT2 ||
        volume->descriptor_size != 32U) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    status = extfs_group_descriptor_byte_offset(volume, group, &offset);
    if (status != EXTFS_OK) {
        return status;
    }
    return extfs_write_bytes(volume, offset, descriptor,
                             volume->descriptor_size);
}

static extfs_status extfs_ext2_write_superblock(extfs_volume *volume,
                                                 extfs_u8 *superblock,
                                                 extfs_u16 state,
                                                 extfs_u64 free_blocks)
{
    if (volume == 0 || superblock == 0 ||
        volume->kind != EXTFS_KIND_EXT2 || free_blocks > 0xFFFFFFFFULL) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    extfs_store_le16(superblock + 0x3AU, state);
    extfs_store_le32(superblock + 0x0CU, (extfs_u32)free_blocks);
    return extfs_write_bytes(volume, 1024U, superblock,
                             EXTFS_SUPERBLOCK_SIZE);
}

static int extfs_is_power_of(extfs_u32 value, extfs_u32 base)
{
    if (value < 1U || base < 2U) return 0;
    while ((value % base) == 0U) value /= base;
    return value == 1U;
}

static int extfs_ext2_group_has_super(const extfs_volume *volume,
                                      extfs_u32 group)
{
    if (group == 0U || group == 1U) return 1;
    if ((volume->feature_ro_compat & EXTFS_RO_COMPAT_SPARSE_SUPER) == 0U)
        return 1;
    return extfs_is_power_of(group, 3U) ||
           extfs_is_power_of(group, 5U) ||
           extfs_is_power_of(group, 7U);
}

static int extfs_ext2_block_is_known_metadata(const extfs_volume *volume,
                                               extfs_u32 group,
                                               const extfs_u8 descriptor[64],
                                               extfs_u64 block)
{
    extfs_u64 first;
    extfs_u32 count;
    extfs_u64 inode_table = extfs_le32(descriptor + 0x08U);
    extfs_u64 inode_table_blocks = extfs_div_round_up_u64(
        (extfs_u64)volume->inodes_per_group * volume->inode_size,
        volume->block_size);
    extfs_u64 gdt_blocks = extfs_div_round_up_u64(
        (extfs_u64)volume->group_count * volume->descriptor_size,
        volume->block_size);
    if (extfs_ext2_group_bounds(volume, group, &first, &count) != EXTFS_OK)
        return 1;
    (void)count;
    if (block == extfs_le32(descriptor + 0x00U) ||
        block == extfs_le32(descriptor + 0x04U)) {
        return 1;
    }
    if (block >= inode_table && block < inode_table + inode_table_blocks)
        return 1;
    if (extfs_ext2_group_has_super(volume, group) &&
        block >= first && block <= first + gdt_blocks) {
        return 1;
    }
    return 0;
}

/*
 * ext2 is the first metadata-writing target because it has no journal whose
 * transaction protocol must be reproduced.  This deliberately stops at the
 * twelve direct i_block entries: no indirect-block allocation is attempted.
 * The primary superblock is marked dirty before metadata mutation and restored
 * to its original clean state only after all metadata writes complete.  If a
 * lower-device failure interrupts the operation, the volume remains dirty and
 * subsequent writes are refused until an external fsck repairs it.
 */
extfs_status extfs_resize_file_ext2_direct(extfs_volume *volume,
                                              extfs_inode *inode,
                                              extfs_u64 new_size,
                                              void *scratch,
                                              extfs_u32 scratch_size)
{
    extfs_u8 *bitmap = (extfs_u8 *)scratch;
    extfs_u8 *work;
    extfs_u8 descriptor[64] = {0U};
    extfs_u64 allocated[EXTFS_DIRECT_BLOCK_COUNT] = {0U};
    extfs_u64 freed[EXTFS_DIRECT_BLOCK_COUNT] = {0U};
    extfs_u64 inode_offset;
    extfs_u64 maximum_size;
    extfs_u64 new_free_blocks;
    extfs_u32 old_blocks;
    extfs_u32 new_blocks;
    extfs_u32 allocate_count = 0U;
    extfs_u32 free_count = 0U;
    extfs_u32 inode_group;
    extfs_u32 i;
    extfs_u16 original_state;
    extfs_u16 dirty_state;
    extfs_status status;

    if (volume == 0 || inode == 0 || scratch == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    if (scratch_size < volume->block_size * 2U) {
        return EXTFS_ERR_BUFFER_TOO_SMALL;
    }
    if (volume->kind != EXTFS_KIND_EXT2 || volume->descriptor_size != 32U ||
        volume->metadata_checksums != 0U ||
        volume->blocks_per_group > 0xFFFFU ||
        extfs_inode_write_assess(volume, inode) != EXTFS_OK ||
        (inode->flags & EXTFS_INODE_FLAG_EXTENTS) != 0U) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    maximum_size = (extfs_u64)EXTFS_DIRECT_BLOCK_COUNT * volume->block_size;
    if (inode->size > maximum_size || new_size > maximum_size) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    if (new_size == inode->size) {
        return EXTFS_OK;
    }

    /* A metadata resize cannot safely begin unless the host can provide a
     * durability barrier. Refuse this capability gap before even the dirty
     * superblock is written; otherwise a caller lacking flush support would
     * be left with an avoidable persistent dirty-state mutation. */
    if (volume->io.flush == 0) {
        return EXTFS_ERR_UNSUPPORTED;
    }

    old_blocks = (extfs_u32)extfs_div_round_up_u64(inode->size,
                                                    volume->block_size);
    new_blocks = (extfs_u32)extfs_div_round_up_u64(new_size,
                                                    volume->block_size);

    /* Direct-only mutation requires the current mapping to be ordinary,
     * contiguous in the inode representation, and free of indirect pointers.
     * Sparse direct files stay on the conservative data-only path for now. */
    for (i = 0U; i < old_blocks; ++i) {
        extfs_u64 block = extfs_le32(inode->block_map + i * 4U);
        extfs_u32 group;
        extfs_u32 bit;
        extfs_u32 j;
        extfs_u64 bitmap_block;
        if (block == 0U || block >= volume->total_blocks) {
            return EXTFS_ERR_UNSUPPORTED;
        }
        for (j = 0U; j < i; ++j) {
            if (block == extfs_le32(inode->block_map + j * 4U)) {
                return EXTFS_ERR_CORRUPT;
            }
        }
        status = extfs_ext2_group_for_block(volume, block, &group, &bit);
        if (status != EXTFS_OK) return status;
        status = extfs_read_group_descriptor(volume, group, descriptor);
        if (status != EXTFS_OK) return status;
        if (extfs_ext2_block_is_known_metadata(volume, group, descriptor,
                                               block)) {
            return EXTFS_ERR_CORRUPT;
        }
        bitmap_block = extfs_le32(descriptor + 0x00U);
        status = extfs_read_block(volume, bitmap_block, bitmap);
        if (status != EXTFS_OK) return status;
        if (!extfs_bitmap_test(bitmap, bit)) return EXTFS_ERR_CORRUPT;
    }
    for (i = old_blocks; i < 15U; ++i) {
        if (extfs_le32(inode->block_map + i * 4U) != 0U) {
            return EXTFS_ERR_UNSUPPORTED;
        }
    }

    status = extfs_inode_byte_offset(volume, inode->number, &inode_offset,
                                     &inode_group);
    if (status != EXTFS_OK) {
        return status;
    }
    work = bitmap + volume->block_size;

    if (new_blocks > old_blocks) {
        extfs_u32 needed = new_blocks - old_blocks;
        extfs_u32 pass;
        if (volume->free_blocks < needed) {
            return EXTFS_ERR_NO_SPACE;
        }
        /* Preflight allocation without changing a bitmap.  Search the inode's
         * group first, then wrap through the remaining groups. */
        for (pass = 0U; pass < volume->group_count && allocate_count < needed;
             ++pass) {
            extfs_u32 group = (inode_group + pass) % volume->group_count;
            extfs_u64 first;
            extfs_u32 group_blocks;
            extfs_u64 bitmap_block;
            extfs_u16 descriptor_free;
            extfs_u32 bit;
            status = extfs_read_group_descriptor(volume, group, descriptor);
            if (status != EXTFS_OK) return status;
            descriptor_free = extfs_le16(descriptor + 0x0CU);
            if (descriptor_free == 0U) continue;
            bitmap_block = extfs_le32(descriptor + 0x00U);
            if (bitmap_block == 0U || bitmap_block >= volume->total_blocks) {
                return EXTFS_ERR_CORRUPT;
            }
            status = extfs_ext2_group_bounds(volume, group, &first,
                                              &group_blocks);
            if (status != EXTFS_OK) return status;
            status = extfs_read_block(volume, bitmap_block, bitmap);
            if (status != EXTFS_OK) return status;
            for (bit = 0U; bit < group_blocks && allocate_count < needed; ++bit) {
                extfs_u64 candidate = first + bit;
                if (!extfs_bitmap_test(bitmap, bit) &&
                    !extfs_ext2_block_is_known_metadata(volume, group, descriptor,
                                                        candidate)) {
                    allocated[allocate_count++] = candidate;
                }
            }
        }
        if (allocate_count != needed) {
            return EXTFS_ERR_NO_SPACE;
        }
    } else if (new_blocks < old_blocks) {
        for (i = new_blocks; i < old_blocks; ++i) {
            extfs_u64 block = extfs_le32(inode->block_map + i * 4U);
            extfs_u32 group;
            extfs_u32 bit;
            extfs_u64 bitmap_block;
            status = extfs_ext2_group_for_block(volume, block, &group, &bit);
            if (status != EXTFS_OK) return status;
            status = extfs_read_group_descriptor(volume, group, descriptor);
            if (status != EXTFS_OK) return status;
            bitmap_block = extfs_le32(descriptor + 0x00U);
            status = extfs_read_block(volume, bitmap_block, bitmap);
            if (status != EXTFS_OK) return status;
            if (!extfs_bitmap_test(bitmap, bit)) {
                return EXTFS_ERR_CORRUPT;
            }
            freed[free_count++] = block;
        }
    }

    /* Read and validate the primary superblock one final time before the first
     * mutation.  Its free count must agree with the opened volume snapshot. */
    status = extfs_read_bytes(volume, 1024U, work, EXTFS_SUPERBLOCK_SIZE);
    if (status != EXTFS_OK) return status;
    if (extfs_le16(work + 0x38U) != 0xEF53U ||
        extfs_le32(work + 0x0CU) != (extfs_u32)volume->free_blocks) {
        return EXTFS_ERR_CORRUPT;
    }
    original_state = volume->state;
    /* Use an explicit 16-bit clear mask rather than ~EXTFS_STATE_VALID.
     * The latter is promoted to a wider unsigned constant first and MSVC
     * correctly diagnoses the narrowing cast under /W4 /WX (C4310). */
    dirty_state = (extfs_u16)(original_state & EXTFS_STATE_VALID_CLEAR_MASK);
    status = extfs_ext2_write_superblock(volume, work, dirty_state,
                                         volume->free_blocks);
    if (status != EXTFS_OK) return status;
    volume->state = dirty_state;

    /* The dirty marker is the ext2 crash-consistency fence.  It must reach
     * stable storage before any allocation, inode or data mutation can become
     * durable; otherwise a controller may reorder later metadata ahead of the
     * dirty superblock and a crash can leave a partially changed filesystem
     * appearing clean. */
    status = extfs_flush(volume);
    if (status != EXTFS_OK) return status;

    if (new_blocks > old_blocks) {
        extfs_u32 pass;
        extfs_u32 needed = new_blocks - old_blocks;
        /* Commit each touched group bitmap and count while the filesystem is
         * dirty.  Re-read and re-check selected bits to catch external races. */
        for (pass = 0U; pass < volume->group_count; ++pass) {
            extfs_u32 selected = 0U;
            extfs_u32 k;
            extfs_u64 bitmap_block;
            extfs_u64 descriptor_offset;
            extfs_u16 descriptor_free;
            for (k = 0U; k < allocate_count; ++k) {
                extfs_u32 group;
                extfs_u32 bit;
                status = extfs_ext2_group_for_block(volume, allocated[k],
                                                     &group, &bit);
                if (status != EXTFS_OK) return status;
                if (group == pass) ++selected;
            }
            if (selected == 0U) continue;
            status = extfs_read_group_descriptor(volume, pass, descriptor);
            if (status != EXTFS_OK) return status;
            descriptor_free = extfs_le16(descriptor + 0x0CU);
            if (descriptor_free < selected) return EXTFS_ERR_CORRUPT;
            bitmap_block = extfs_le32(descriptor + 0x00U);
            status = extfs_read_block(volume, bitmap_block, bitmap);
            if (status != EXTFS_OK) return status;
            for (k = 0U; k < allocate_count; ++k) {
                extfs_u32 group;
                extfs_u32 bit;
                status = extfs_ext2_group_for_block(volume, allocated[k],
                                                     &group, &bit);
                if (status != EXTFS_OK) return status;
                if (group == pass) {
                    if (extfs_bitmap_test(bitmap, bit)) return EXTFS_ERR_CORRUPT;
                    extfs_bitmap_set(bitmap, bit);
                }
            }
            status = extfs_write_block(volume, bitmap_block, bitmap);
            if (status != EXTFS_OK) return status;
            extfs_store_le16(descriptor + 0x0CU,
                             (extfs_u16)(descriptor_free - selected));
            status = extfs_group_descriptor_byte_offset(volume, pass,
                                                         &descriptor_offset);
            if (status != EXTFS_OK) return status;
            status = extfs_write_bytes(volume, descriptor_offset, descriptor,
                                       volume->descriptor_size);
            if (status != EXTFS_OK) return status;
        }
        new_free_blocks = volume->free_blocks - needed;
        status = extfs_ext2_write_superblock(volume, work, dirty_state,
                                             new_free_blocks);
        if (status != EXTFS_OK) return status;

        /* New blocks are zeroed before the inode can point at them. */
        extfs_zero(bitmap, volume->block_size);
        for (i = 0U; i < allocate_count; ++i) {
            status = extfs_write_block(volume, allocated[i], bitmap);
            if (status != EXTFS_OK) return status;
        }

        /* Zero bytes newly exposed inside an already allocated final block. */
        if (inode->size != 0U && (inode->size % volume->block_size) != 0U) {
            extfs_u64 old_last_end = extfs_div_round_up_u64(
                inode->size, volume->block_size) * volume->block_size;
            extfs_u64 zero_end = new_size < old_last_end ? new_size : old_last_end;
            if (zero_end > inode->size) {
                extfs_u32 index = (extfs_u32)(inode->size / volume->block_size);
                extfs_u64 block = extfs_le32(inode->block_map + index * 4U);
                extfs_u32 within = (extfs_u32)(inode->size % volume->block_size);
                extfs_u32 count = (extfs_u32)(zero_end - inode->size);
                {
                    extfs_u64 zero_offset;
                    status = extfs_block_byte_offset(volume, block, within,
                                                     &zero_offset);
                    if (status == EXTFS_OK) {
                        status = extfs_write_bytes(volume, zero_offset, bitmap,
                                                   count);
                    }
                }
                if (status != EXTFS_OK) return status;
            }
        }

        status = extfs_read_bytes(volume, inode_offset, bitmap,
                                  volume->inode_size);
        if (status != EXTFS_OK) return status;
        {
            extfs_u32 sectors = extfs_le32(bitmap + 0x1CU);
            extfs_u32 sectors_per_block = volume->block_size / 512U;
            extfs_u64 add = (extfs_u64)allocate_count * sectors_per_block;
            if (add > 0xFFFFFFFFULL - sectors) return EXTFS_ERR_RANGE;
            for (i = 0U; i < allocate_count; ++i) {
                extfs_store_le32(bitmap + 0x28U + (old_blocks + i) * 4U,
                                 (extfs_u32)allocated[i]);
            }
            extfs_store_le32(bitmap + 0x1CU, sectors + (extfs_u32)add);
        }
        extfs_store_le32(bitmap + 0x04U, (extfs_u32)new_size);
        extfs_store_le32(bitmap + 0x6CU, (extfs_u32)(new_size >> 32));
        status = extfs_write_bytes(volume, inode_offset, bitmap,
                                   volume->inode_size);
        if (status != EXTFS_OK) return status;

        for (i = 0U; i < allocate_count; ++i) {
            extfs_store_le32(inode->block_map + (old_blocks + i) * 4U,
                             (extfs_u32)allocated[i]);
        }
        inode->size = new_size;
        volume->free_blocks = new_free_blocks;
    } else if (new_size > inode->size) {
        /* Growth that stays inside the already allocated final block changes no
         * allocation metadata. Zero the newly exposed range before publishing
         * the larger i_size so stale bytes can never become visible. */
        extfs_u32 index = (extfs_u32)(inode->size / volume->block_size);
        extfs_u64 block = extfs_le32(inode->block_map + index * 4U);
        extfs_u32 within = (extfs_u32)(inode->size % volume->block_size);
        extfs_u32 count = (extfs_u32)(new_size - inode->size);
        extfs_u64 zero_offset;
        extfs_zero(bitmap, volume->block_size);
        status = extfs_block_byte_offset(volume, block, within, &zero_offset);
        if (status == EXTFS_OK) {
            status = extfs_write_bytes(volume, zero_offset, bitmap, count);
        }
        if (status != EXTFS_OK) return status;
        status = extfs_read_bytes(volume, inode_offset, bitmap,
                                  volume->inode_size);
        if (status != EXTFS_OK) return status;
        extfs_store_le32(bitmap + 0x04U, (extfs_u32)new_size);
        extfs_store_le32(bitmap + 0x6CU, (extfs_u32)(new_size >> 32));
        status = extfs_write_bytes(volume, inode_offset, bitmap,
                                   volume->inode_size);
        if (status != EXTFS_OK) return status;
        inode->size = new_size;
    } else {
        /* Shrink changes the inode before freeing blocks. A crash can therefore
         * leak blocks, but cannot expose a freed block as file data; fsck can
         * safely recover the leak on an unjournaled ext2. */
        status = extfs_read_bytes(volume, inode_offset, bitmap,
                                  volume->inode_size);
        if (status != EXTFS_OK) return status;
        {
            extfs_u32 sectors = extfs_le32(bitmap + 0x1CU);
            extfs_u32 sectors_per_block = volume->block_size / 512U;
            extfs_u64 remove = (extfs_u64)free_count * sectors_per_block;
            if (remove > sectors) return EXTFS_ERR_CORRUPT;
            for (i = new_blocks; i < old_blocks; ++i) {
                extfs_store_le32(bitmap + 0x28U + i * 4U, 0U);
            }
            extfs_store_le32(bitmap + 0x1CU, sectors - (extfs_u32)remove);
        }
        extfs_store_le32(bitmap + 0x04U, (extfs_u32)new_size);
        extfs_store_le32(bitmap + 0x6CU, (extfs_u32)(new_size >> 32));
        status = extfs_write_bytes(volume, inode_offset, bitmap,
                                   volume->inode_size);
        if (status != EXTFS_OK) return status;

        for (i = new_blocks; i < old_blocks; ++i) {
            extfs_store_le32(inode->block_map + i * 4U, 0U);
        }
        inode->size = new_size;

        /* Zero a retained partial block tail so a later extension cannot reveal
         * bytes that were logically truncated. */
        if (new_size != 0U && (new_size % volume->block_size) != 0U) {
            extfs_u32 index = (extfs_u32)(new_size / volume->block_size);
            extfs_u64 block = extfs_le32(inode->block_map + index * 4U);
            extfs_u32 within = (extfs_u32)(new_size % volume->block_size);
            extfs_zero(bitmap, volume->block_size);
            {
                extfs_u64 zero_offset;
                status = extfs_block_byte_offset(volume, block, within,
                                                 &zero_offset);
                if (status == EXTFS_OK) {
                    status = extfs_write_bytes(volume, zero_offset, bitmap,
                                               volume->block_size - within);
                }
            }
            if (status != EXTFS_OK) return status;
        }

        {
            extfs_u32 group;
            for (group = 0U; group < volume->group_count; ++group) {
                extfs_u32 selected = 0U;
                extfs_u32 k;
                extfs_u64 bitmap_block;
                extfs_u16 descriptor_free;
                for (k = 0U; k < free_count; ++k) {
                    extfs_u32 kg;
                    extfs_u32 kb;
                    status = extfs_ext2_group_for_block(volume, freed[k],
                                                         &kg, &kb);
                    if (status != EXTFS_OK) return status;
                    if (kg == group) ++selected;
                }
                if (selected == 0U) continue;
                status = extfs_read_group_descriptor(volume, group, descriptor);
                if (status != EXTFS_OK) return status;
                descriptor_free = extfs_le16(descriptor + 0x0CU);
                bitmap_block = extfs_le32(descriptor + 0x00U);
                status = extfs_read_block(volume, bitmap_block, bitmap);
                if (status != EXTFS_OK) return status;
                for (k = 0U; k < free_count; ++k) {
                    extfs_u32 kg;
                    extfs_u32 kb;
                    status = extfs_ext2_group_for_block(volume, freed[k],
                                                         &kg, &kb);
                    if (status != EXTFS_OK) return status;
                    if (kg == group) {
                        if (!extfs_bitmap_test(bitmap, kb))
                            return EXTFS_ERR_CORRUPT;
                        extfs_bitmap_clear(bitmap, kb);
                    }
                }
                if ((extfs_u32)descriptor_free + selected >
                    volume->blocks_per_group) {
                    return EXTFS_ERR_CORRUPT;
                }
                status = extfs_write_block(volume, bitmap_block, bitmap);
                if (status != EXTFS_OK) return status;
                extfs_store_le16(descriptor + 0x0CU,
                                 (extfs_u16)(descriptor_free + selected));
                status = extfs_ext2_write_group_descriptor(volume, group,
                                                            descriptor);
                if (status != EXTFS_OK) return status;
            }
        }
        new_free_blocks = volume->free_blocks + free_count;
        if (new_free_blocks > volume->total_blocks) return EXTFS_ERR_CORRUPT;
        status = extfs_ext2_write_superblock(volume, work, dirty_state,
                                             new_free_blocks);
        if (status != EXTFS_OK) return status;
        volume->free_blocks = new_free_blocks;
    }

    /* All ext2 mutation must be durable before the clean marker is allowed to
     * reach stable storage.  This second barrier pairs with the dirty-marker
     * barrier above and prevents write-cache reordering from advertising a
     * partially committed resize as clean after a crash. */
    status = extfs_flush(volume);
    if (status != EXTFS_OK) {
        volume->state = dirty_state;
        return status;
    }

    status = extfs_ext2_write_superblock(volume, work, original_state,
                                         volume->free_blocks);
    if (status != EXTFS_OK) {
        volume->state = dirty_state;
        return status;
    }

    /* Make the clean transition durable before reporting success.  If this
     * final flush fails, all preceding filesystem mutation is already durable;
     * keep the in-memory volume dirty and fail closed for the rest of the mount. */
    status = extfs_flush(volume);
    if (status != EXTFS_OK) {
        volume->state = dirty_state;
        return status;
    }
    volume->state = original_state;
    return EXTFS_OK;
}

/*
 * JBD2 transaction foundation.
 *
 * The writer intentionally handles only a clean internal journal and one
 * descriptor worth of complete metadata-block images. It does not replay a
 * dirty journal and does not emit revoke or fast-commit records. The critical
 * durability order is explicit: arm ext recovery, publish a non-empty journal,
 * write descriptor/data, flush, write+flush commit, checkpoint+flush, mark the
 * journal empty, then clear ext recovery.
 */
static int extfs_uuid_is_zero(const extfs_u8 *uuid)
{
    extfs_u32 i;
    for (i = 0U; i < 16U; ++i) {
        if (uuid[i] != 0U) return 0;
    }
    return 1;
}

static extfs_u32 extfs_jbd2_next_block(const extfs_journal *journal,
                                       extfs_u32 block)
{
    ++block;
    return block >= journal->maxlen ? journal->first : block;
}

static extfs_status extfs_journal_map_block(const extfs_volume *volume,
                                            const extfs_journal *journal,
                                            extfs_u32 journal_block,
                                            extfs_u64 *physical_block,
                                            void *scratch,
                                            extfs_u32 scratch_size)
{
    int hole = 0;
    extfs_status status;
    if (journal_block >= journal->maxlen) return EXTFS_ERR_RANGE;
    status = extfs_map_file_block(volume, &journal->inode, journal_block,
                                  physical_block, &hole, scratch, scratch_size);
    if (status != EXTFS_OK) return status;
    if (hole != 0 || *physical_block >= volume->total_blocks) {
        return EXTFS_ERR_CORRUPT;
    }
    return EXTFS_OK;
}

static extfs_u32 extfs_jbd2_superblock_checksum(extfs_u8 *superblock)
{
    extfs_u32 stored = extfs_be32(superblock + 0xFCU);
    extfs_u32 checksum;
    extfs_store_be32(superblock + 0xFCU, 0U);
    checksum = extfs_crc32c(0xFFFFFFFFU, superblock, 1024U);
    extfs_store_be32(superblock + 0xFCU, stored);
    return checksum;
}

static extfs_status extfs_journal_store_superblock(extfs_volume *volume,
                                                   extfs_journal *journal,
                                                   extfs_u32 sequence,
                                                   extfs_u32 start,
                                                   extfs_u32 head,
                                                   extfs_u8 *scratch,
                                                   extfs_u32 scratch_size)
{
    extfs_u64 physical;
    extfs_status status;
    if (scratch_size < volume->block_size) return EXTFS_ERR_BUFFER_TOO_SMALL;
    status = extfs_journal_map_block(volume, journal, 0U, &physical,
                                     scratch, scratch_size);
    if (status != EXTFS_OK) return status;
    status = extfs_read_block(volume, physical, scratch);
    if (status != EXTFS_OK) return status;
    if (extfs_be32(scratch + 0x00U) != EXTFS_JBD2_MAGIC ||
        extfs_be32(scratch + 0x04U) != EXTFS_JBD2_SUPERBLOCK_V2) {
        return EXTFS_ERR_CORRUPT;
    }
    extfs_store_be32(scratch + 0x18U, sequence);
    extfs_store_be32(scratch + 0x1CU, start);
    extfs_store_be32(scratch + 0x58U, head);
    if (journal->checksum_v2 != 0U || journal->checksum_v3 != 0U) {
        extfs_store_be32(scratch + 0xFCU, 0U);
        extfs_store_be32(scratch + 0xFCU,
                         extfs_crc32c(0xFFFFFFFFU, scratch, 1024U));
    }
    return extfs_write_block(volume, physical, scratch);
}

/* Make or clear the ext needs-recovery incompat bit in the primary
 * superblock. With metadata_csum the superblock CRC is updated before write. */
static extfs_status extfs_set_recovery_required(extfs_volume *volume,
                                                int required,
                                                extfs_u8 *scratch,
                                                extfs_u32 scratch_size)
{
    extfs_u32 incompat;
    extfs_status status;
    if (scratch_size < EXTFS_SUPERBLOCK_SIZE) return EXTFS_ERR_BUFFER_TOO_SMALL;
    status = extfs_read_bytes(volume, 1024U, scratch, EXTFS_SUPERBLOCK_SIZE);
    if (status != EXTFS_OK) return status;
    if (extfs_le16(scratch + 0x38U) != 0xEF53U ||
        !extfs_bytes_equal((const char *)(scratch + 0x68U),
                           (const char *)volume->uuid, 16U)) {
        return EXTFS_ERR_CORRUPT;
    }
    if (volume->metadata_checksums != 0U &&
        extfs_validate_superblock_checksum(scratch) != EXTFS_OK) {
        return EXTFS_ERR_CHECKSUM;
    }
    incompat = extfs_le32(scratch + 0x60U);
    if (required != 0) incompat |= EXTFS_INCOMPAT_RECOVER;
    else incompat &= ~EXTFS_INCOMPAT_RECOVER;
    extfs_store_le32(scratch + 0x60U, incompat);
    if (volume->metadata_checksums != 0U) {
        extfs_store_le32(scratch + 0x3FCU,
                         extfs_crc32c(0xFFFFFFFFU, scratch, 0x3FCU));
    }
    status = extfs_write_bytes(volume, 1024U, scratch,
                               EXTFS_SUPERBLOCK_SIZE);
    if (status != EXTFS_OK) return status;
    status = extfs_flush(volume);
    if (status != EXTFS_OK) return status;
    return EXTFS_OK;
}

extfs_status extfs_journal_open(const extfs_volume *volume,
                                extfs_journal *journal,
                                void *scratch,
                                extfs_u32 scratch_size)
{
    extfs_u8 *block = (extfs_u8 *)scratch;
    extfs_u64 physical;
    extfs_u64 required_bytes;
    extfs_u32 stored_checksum;
    extfs_status status;

    if (volume == 0 || journal == 0 || scratch == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    if (scratch_size < volume->block_size) return EXTFS_ERR_BUFFER_TOO_SMALL;
    if ((volume->feature_compat & EXTFS_COMPAT_HAS_JOURNAL) == 0U ||
        volume->journal_inode == 0U) {
        return EXTFS_ERR_NOT_FOUND;
    }
    if ((volume->feature_incompat & EXTFS_INCOMPAT_JOURNAL_DEV) != 0U) {
        return EXTFS_ERR_UNSUPPORTED;
    }

    extfs_zero(journal, (extfs_u32)sizeof(*journal));
    status = extfs_read_inode(volume, volume->journal_inode, &journal->inode,
                              scratch, scratch_size);
    if (status != EXTFS_OK) return status;
    if ((journal->inode.mode & EXTFS_MODE_TYPE_MASK) != EXTFS_MODE_REGULAR) {
        return EXTFS_ERR_CORRUPT;
    }
    {
        int hole = 0;
        status = extfs_map_file_block(volume, &journal->inode, 0U, &physical,
                                      &hole, scratch, scratch_size);
        if (status == EXTFS_OK && hole != 0) status = EXTFS_ERR_CORRUPT;
    }
    if (status != EXTFS_OK) return status;
    status = extfs_read_block(volume, physical, block);
    if (status != EXTFS_OK) return status;

    if (extfs_be32(block + 0x00U) != EXTFS_JBD2_MAGIC) return EXTFS_ERR_CORRUPT;
    if (extfs_be32(block + 0x04U) != EXTFS_JBD2_SUPERBLOCK_V2) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    journal->block_size = extfs_be32(block + 0x0CU);
    journal->maxlen = extfs_be32(block + 0x10U);
    journal->first = extfs_be32(block + 0x14U);
    journal->sequence = extfs_be32(block + 0x18U);
    journal->start = extfs_be32(block + 0x1CU);
    journal->errno_value = extfs_be32(block + 0x20U);
    journal->feature_compat = extfs_be32(block + 0x24U);
    journal->feature_incompat = extfs_be32(block + 0x28U);
    journal->feature_ro_compat = extfs_be32(block + 0x2CU);
    extfs_copy(journal->uuid, block + 0x30U, 16U);
    journal->max_transaction = extfs_be32(block + 0x48U);
    journal->max_trans_data = extfs_be32(block + 0x4CU);
    journal->checksum_type = block[0x50U];
    journal->head = extfs_be32(block + 0x58U);
    journal->checksum_v2 =
        (journal->feature_incompat & EXTFS_JBD2_INCOMPAT_CSUM_V2) != 0U;
    journal->checksum_v3 =
        (journal->feature_incompat & EXTFS_JBD2_INCOMPAT_CSUM_V3) != 0U;
    journal->has_64bit =
        (journal->feature_incompat & EXTFS_JBD2_INCOMPAT_64BIT) != 0U;
    journal->clean = journal->start == 0U ? 1U : 0U;

    if (journal->block_size != volume->block_size ||
        journal->maxlen < EXTFS_JBD2_MIN_JOURNAL_BLOCKS ||
        journal->first == 0U || journal->first >= journal->maxlen ||
        extfs_be32(block + 0x40U) != 1U ||
        (journal->head != 0U &&
         (journal->head < journal->first || journal->head >= journal->maxlen))) {
        return EXTFS_ERR_CORRUPT;
    }
    if (extfs_mul_u64(journal->maxlen, volume->block_size,
                      &required_bytes) == 0 ||
        journal->inode.size < required_bytes) {
        return EXTFS_ERR_CORRUPT;
    }
    if (!extfs_uuid_is_zero(volume->journal_uuid) &&
        !extfs_bytes_equal((const char *)volume->journal_uuid,
                           (const char *)journal->uuid, 16U)) {
        return EXTFS_ERR_CORRUPT;
    }
    if (journal->checksum_v2 != 0U && journal->checksum_v3 != 0U) {
        return EXTFS_ERR_CORRUPT;
    }
    if ((journal->checksum_v2 != 0U || journal->checksum_v3 != 0U)) {
        if (journal->checksum_type != EXTFS_JBD2_CRC32C_CHKSUM) {
            return EXTFS_ERR_UNSUPPORTED;
        }
        stored_checksum = extfs_be32(block + 0xFCU);
        if (stored_checksum != extfs_jbd2_superblock_checksum(block)) {
            return EXTFS_ERR_CHECKSUM;
        }
        journal->checksum_seed = extfs_crc32c(0xFFFFFFFFU,
                                               journal->uuid, 16U);
    }
    if (journal->head == 0U) journal->head = journal->first;
    return EXTFS_OK;
}

extfs_status extfs_journal_write_assess(const extfs_volume *volume,
                                        const extfs_journal *journal,
                                        extfs_u32 *risk_flags)
{
    extfs_u32 risks = 0U;
    extfs_u32 write_risks = 0U;
    if (volume == 0 || journal == 0 || risk_flags == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    if ((volume->feature_compat & EXTFS_COMPAT_HAS_JOURNAL) == 0U ||
        volume->journal_inode == 0U) risks |= EXTFS_JOURNAL_RISK_NO_JOURNAL;
    if ((volume->feature_incompat & EXTFS_INCOMPAT_JOURNAL_DEV) != 0U)
        risks |= EXTFS_JOURNAL_RISK_EXTERNAL_JOURNAL;
    if (volume->io.write_at == 0) risks |= EXTFS_JOURNAL_RISK_NO_WRITER;
    if (volume->io.flush == 0) risks |= EXTFS_JOURNAL_RISK_NO_FLUSH;
    if (volume->io.time_now == 0) risks |= EXTFS_JOURNAL_RISK_NO_CLOCK;
    if (journal->clean == 0U || journal->start != 0U ||
        journal->errno_value != 0U ||
        (volume->state & EXTFS_STATE_VALID) == 0U ||
        (volume->state & EXTFS_STATE_ERROR) != 0U ||
        (volume->feature_incompat & EXTFS_INCOMPAT_RECOVER) != 0U ||
        (volume->feature_ro_compat & EXTFS_RO_COMPAT_ORPHAN_PRESENT) != 0U)
        risks |= EXTFS_JOURNAL_RISK_DIRTY;
    if (extfs_write_assess(volume, &write_risks) != EXTFS_OK &&
        (risks & EXTFS_JOURNAL_RISK_DIRTY) == 0U)
        risks |= EXTFS_JOURNAL_RISK_UNSUPPORTED_FEATURE;
    if (journal->feature_ro_compat != 0U ||
        journal->feature_compat != 0U ||
        (journal->feature_incompat & ~EXTFS_JBD2_SUPPORTED_INCOMPAT) != 0U ||
        (journal->feature_incompat &
         (EXTFS_JBD2_INCOMPAT_ASYNC_COMMIT |
          EXTFS_JBD2_INCOMPAT_FAST_COMMIT)) != 0U ||
        (journal->checksum_v2 != 0U && journal->checksum_v3 != 0U)) {
        risks |= EXTFS_JOURNAL_RISK_UNSUPPORTED_FEATURE;
    }
    if ((journal->checksum_v2 != 0U || journal->checksum_v3 != 0U) &&
        journal->checksum_type != EXTFS_JBD2_CRC32C_CHKSUM) {
        risks |= EXTFS_JOURNAL_RISK_CHECKSUM;
    }
    *risk_flags = risks;
    return risks == 0U ? EXTFS_OK : EXTFS_ERR_UNSUPPORTED;
}

static extfs_u32 extfs_jbd2_data_checksum(const extfs_journal *journal,
                                          extfs_u32 tid,
                                          const extfs_u8 *data,
                                          extfs_u32 block_size,
                                          int escaped)
{
    extfs_u8 sequence[4];
    extfs_u8 zero_magic[4] = {0U, 0U, 0U, 0U};
    extfs_u32 crc;
    extfs_store_be32(sequence, tid);
    crc = extfs_crc32c(journal->checksum_seed, sequence, 4U);
    if (escaped != 0) {
        crc = extfs_crc32c(crc, zero_magic, 4U);
        if (block_size > 4U) crc = extfs_crc32c(crc, data + 4U,
                                                block_size - 4U);
    } else {
        crc = extfs_crc32c(crc, data, block_size);
    }
    return crc;
}

static extfs_status extfs_jbd2_build_descriptor(const extfs_volume *volume,
                                                const extfs_journal *journal,
                                                extfs_u32 tid,
                                                const extfs_journal_metadata *items,
                                                extfs_u32 item_count,
                                                extfs_u8 *block)
{
    extfs_u32 pos = 12U;
    extfs_u32 tail_bytes =
        (journal->checksum_v2 != 0U || journal->checksum_v3 != 0U) ? 4U : 0U;
    extfs_u32 i;
    extfs_zero(block, volume->block_size);
    extfs_store_be32(block + 0U, EXTFS_JBD2_MAGIC);
    extfs_store_be32(block + 4U, EXTFS_JBD2_DESCRIPTOR_BLOCK);
    extfs_store_be32(block + 8U, tid);

    for (i = 0U; i < item_count; ++i) {
        const extfs_u8 *data = (const extfs_u8 *)items[i].block_data;
        extfs_u32 flags = 0U;
        extfs_u32 tag_bytes = journal->checksum_v3 != 0U ? 16U :
                              (journal->has_64bit != 0U ? 12U : 8U);
        int escaped = extfs_be32(data) == EXTFS_JBD2_MAGIC;
        extfs_u32 checksum = 0U;
        if (i != 0U) flags |= EXTFS_JBD2_FLAG_SAME_UUID;
        if (i + 1U == item_count) flags |= EXTFS_JBD2_FLAG_LAST_TAG;
        if (escaped != 0) flags |= EXTFS_JBD2_FLAG_ESCAPE;
        if (items[i].home_block > 0xFFFFFFFFULL && journal->has_64bit == 0U)
            return EXTFS_ERR_RANGE;
        if (pos > volume->block_size ||
            tag_bytes + (i == 0U ? 16U : 0U) + tail_bytes >
                volume->block_size - pos) return EXTFS_ERR_NO_SPACE;
        if (journal->checksum_v2 != 0U || journal->checksum_v3 != 0U) {
            checksum = extfs_jbd2_data_checksum(journal, tid, data,
                                                volume->block_size, escaped);
        }
        extfs_store_be32(block + pos, (extfs_u32)items[i].home_block);
        if (journal->checksum_v3 != 0U) {
            extfs_store_be32(block + pos + 4U, flags);
            extfs_store_be32(block + pos + 8U,
                             (extfs_u32)(items[i].home_block >> 32));
            extfs_store_be32(block + pos + 12U, checksum);
        } else {
            extfs_store_be16(block + pos + 4U, (extfs_u16)checksum);
            extfs_store_be16(block + pos + 6U, (extfs_u16)flags);
            if (journal->has_64bit != 0U)
                extfs_store_be32(block + pos + 8U,
                                 (extfs_u32)(items[i].home_block >> 32));
        }
        pos += tag_bytes;
        if (i == 0U) {
            extfs_copy(block + pos, journal->uuid, 16U);
            pos += 16U;
        }
    }
    if (tail_bytes != 0U) {
        extfs_store_be32(block + volume->block_size - 4U, 0U);
        extfs_store_be32(block + volume->block_size - 4U,
                         extfs_crc32c(journal->checksum_seed, block,
                                      volume->block_size));
    }
    return EXTFS_OK;
}

static void extfs_jbd2_build_commit(const extfs_volume *volume,
                                    const extfs_journal *journal,
                                    extfs_u32 tid,
                                    extfs_u64 commit_seconds,
                                    extfs_u32 commit_nanoseconds,
                                    extfs_u8 *block)
{
    extfs_zero(block, volume->block_size);
    extfs_store_be32(block + 0U, EXTFS_JBD2_MAGIC);
    extfs_store_be32(block + 4U, EXTFS_JBD2_COMMIT_BLOCK);
    extfs_store_be32(block + 8U, tid);
    /* commit_header.h_commit_sec/h_commit_nsec are part of the standard JBD2
     * commit record. Populate them before calculating checksum-v2/v3 so the
     * timestamp is covered by the commit-block CRC exactly like Linux JBD2. */
    extfs_store_be64(block + 48U, commit_seconds);
    extfs_store_be32(block + 56U, commit_nanoseconds);
    if (journal->checksum_v2 != 0U || journal->checksum_v3 != 0U) {
        /* JBD2 checksum-v2/v3 commit blocks leave h_chksum_type and
         * h_chksum_size zero. The CRC32C is stored in h_chksum[0] after
         * hashing the complete commit block with h_chksum[0] zero. */
        extfs_store_be32(block + 16U, 0U);
        extfs_store_be32(block + 16U,
                         extfs_crc32c(journal->checksum_seed, block,
                                      volume->block_size));
    }
}

static extfs_status extfs_journal_fail_armed(extfs_volume *volume,
                                               extfs_status status)
{
    volume->feature_incompat |= EXTFS_INCOMPAT_RECOVER;
    volume->unsupported_incompat =
        volume->feature_incompat & ~EXTFS_SUPPORTED_INCOMPAT;
    return status;
}

extfs_status extfs_journal_commit_metadata(extfs_volume *volume,
                                            extfs_journal *journal,
                                            const extfs_journal_metadata *items,
                                            extfs_u32 item_count,
                                            void *scratch,
                                            extfs_u32 scratch_size)
{
    extfs_u8 *block = (extfs_u8 *)scratch;
    extfs_u32 risks = 0U;
    extfs_u32 descriptor_block;
    extfs_u32 log_block;
    extfs_u32 commit_block;
    extfs_u32 next_head;
    extfs_u32 tid;
    extfs_u32 usable_blocks;
    extfs_u32 transaction_blocks;
    extfs_u32 commit_nanoseconds;
    extfs_u32 i;
    extfs_u64 physical;
    extfs_u64 commit_seconds;
    extfs_status status;

    if (volume == 0 || journal == 0 || items == 0 || item_count == 0U ||
        scratch == 0) return EXTFS_ERR_INVALID_ARGUMENT;
    if (scratch_size < volume->block_size) return EXTFS_ERR_BUFFER_TOO_SMALL;
    if (extfs_journal_write_assess(volume, journal, &risks) != EXTFS_OK)
        return EXTFS_ERR_UNSUPPORTED;

    usable_blocks = journal->maxlen - journal->first;
    if (usable_blocks < 4U || item_count > usable_blocks - 3U)
        return EXTFS_ERR_NO_SPACE;
    transaction_blocks = item_count + 2U; /* descriptor + metadata + commit */
    if (journal->max_transaction != 0U &&
        transaction_blocks > journal->max_transaction)
        return EXTFS_ERR_NO_SPACE;
    for (i = 0U; i < item_count; ++i) {
        extfs_u32 j;
        if (items[i].block_data == 0 ||
            items[i].home_block >= volume->total_blocks)
            return EXTFS_ERR_INVALID_ARGUMENT;
        for (j = 0U; j < i; ++j) {
            if (items[i].home_block == items[j].home_block)
                return EXTFS_ERR_INVALID_ARGUMENT;
        }
    }
    /*
     * A caller may journal the primary filesystem superblock itself (for
     * example when updating free-block counters).  That checkpoint image must
     * keep NEEDS_RECOVERY set until the journal has been marked empty.  If it
     * cleared RECOVER during checkpoint, a crash in the tiny window before the
     * journal superblock is emptied could make Linux trust half-checkpointed
     * metadata instead of replaying the committed transaction.
     */
    {
        extfs_u64 super_home;
        extfs_u32 super_within;
        status = extfs_primary_superblock_block_location(
            volume, &super_home, &super_within);
        if (status != EXTFS_OK) return status;
        for (i = 0U; i < item_count; ++i) {
            if (items[i].home_block == super_home) {
                const extfs_u8 *image =
                    (const extfs_u8 *)items[i].block_data + super_within;
                if (extfs_le16(image + 0x38U) != 0xEF53U ||
                    !extfs_bytes_equal((const char *)(image + 0x68U),
                                       (const char *)volume->uuid, 16U) ||
                    (extfs_le32(image + 0x60U) &
                     EXTFS_INCOMPAT_RECOVER) == 0U) {
                    return EXTFS_ERR_INVALID_ARGUMENT;
                }
                if (volume->metadata_checksums != 0U &&
                    extfs_validate_superblock_checksum(image) != EXTFS_OK) {
                    return EXTFS_ERR_CHECKSUM;
                }
            }
        }
    }

    /* Obtain and validate the commit timestamp before arming filesystem
     * recovery. A host clock failure must not leave any persistent journal
     * state behind. */
    if (volume->io.time_now(volume->io.user, &commit_seconds,
                            &commit_nanoseconds) != 0)
        return EXTFS_ERR_IO;
    if (commit_nanoseconds >= 1000000000U)
        return EXTFS_ERR_RANGE;

    tid = journal->sequence + 1U;
    if (tid == 0U) return EXTFS_ERR_RANGE;
    descriptor_block = journal->head;
    if (descriptor_block < journal->first || descriptor_block >= journal->maxlen)
        descriptor_block = journal->first;
    log_block = descriptor_block;
    for (i = 0U; i < item_count + 1U; ++i)
        log_block = extfs_jbd2_next_block(journal, log_block);
    commit_block = log_block;
    next_head = extfs_jbd2_next_block(journal, commit_block);
    if (next_head == descriptor_block) return EXTFS_ERR_NO_SPACE;

    /* Descriptor capacity and tag-format validation happen before the first
     * persistent state change. */
    status = extfs_jbd2_build_descriptor(volume, journal, tid, items,
                                         item_count, block);
    if (status != EXTFS_OK) return status;

    /* A crash after this point must cause normal ext mounting to invoke JBD2
     * recovery rather than trusting potentially half-checkpointed metadata. */
    status = extfs_set_recovery_required(volume, 1, block, scratch_size);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);

    status = extfs_journal_store_superblock(volume, journal, tid,
                                            descriptor_block, journal->head,
                                            block, scratch_size);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);
    status = extfs_flush(volume);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);

    /* Map before building into scratch: extent/indirect mapping is allowed to
     * use scratch and therefore must not run after scratch contains payload. */
    status = extfs_journal_map_block(volume, journal, descriptor_block,
                                     &physical, block, scratch_size);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);
    status = extfs_jbd2_build_descriptor(volume, journal, tid, items,
                                         item_count, block);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);
    status = extfs_write_block(volume, physical, block);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);

    log_block = extfs_jbd2_next_block(journal, descriptor_block);
    for (i = 0U; i < item_count; ++i) {
        const extfs_u8 *source = (const extfs_u8 *)items[i].block_data;
        status = extfs_journal_map_block(volume, journal, log_block,
                                         &physical, block, scratch_size);
        if (status != EXTFS_OK)
            return extfs_journal_fail_armed(volume, status);
        if (extfs_be32(source) == EXTFS_JBD2_MAGIC) {
            extfs_copy(block, source, volume->block_size);
            extfs_zero(block, 4U);
            status = extfs_write_block(volume, physical, block);
        } else {
            status = extfs_write_block(volume, physical, source);
        }
        if (status != EXTFS_OK)
            return extfs_journal_fail_armed(volume, status);
        log_block = extfs_jbd2_next_block(journal, log_block);
    }
    status = extfs_flush(volume);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);

    status = extfs_journal_map_block(volume, journal, commit_block,
                                     &physical, block, scratch_size);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);
    extfs_jbd2_build_commit(volume, journal, tid, commit_seconds,
                            commit_nanoseconds, block);
    status = extfs_write_block(volume, physical, block);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);
    status = extfs_flush(volume);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);

    /* Only a durable commit record permits checkpointing to home locations. */
    for (i = 0U; i < item_count; ++i) {
        status = extfs_write_block(volume, items[i].home_block,
                                   items[i].block_data);
        if (status != EXTFS_OK)
            return extfs_journal_fail_armed(volume, status);
    }
    status = extfs_flush(volume);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);

    status = extfs_journal_store_superblock(volume, journal, tid, 0U,
                                            next_head, block, scratch_size);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);
    status = extfs_flush(volume);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);

    status = extfs_set_recovery_required(volume, 0, block, scratch_size);
    if (status != EXTFS_OK) return extfs_journal_fail_armed(volume, status);

    /* Keep the in-memory parse synchronized only after both on-disk recovery
     * markers say the transaction is fully checkpointed and clean. */
    volume->feature_incompat &= ~EXTFS_INCOMPAT_RECOVER;
    volume->unsupported_incompat =
        volume->feature_incompat & ~EXTFS_SUPPORTED_INCOMPAT;
    journal->sequence = tid;
    journal->start = 0U;
    journal->head = next_head;
    journal->clean = 1U;
    return EXTFS_OK;
}


/*
 * First journaled metadata mutation target: legacy ext3 direct-block files.
 *
 * The operation deliberately keeps the allocation surface small enough to
 * reason about exhaustively.  One resize may change free-space metadata in at
 * most one block group, while inode, group-descriptor, bitmap and superblock
 * images are committed atomically through one JBD2 transaction.  ext4 is not
 * routed here: extents, metadata checksums, 64-bit descriptors and modern
 * allocation rules need their own metadata builders.
 */
extfs_status extfs_resize_file_ext3_journaled_direct(extfs_volume *volume,
                                                      extfs_inode *inode,
                                                      extfs_u64 new_size,
                                                      void *scratch,
                                                      extfs_u32 scratch_size)
{
    extfs_u8 *bitmap_image = (extfs_u8 *)scratch;
    extfs_u8 *gdt_image;
    extfs_u8 *inode_image;
    extfs_u8 *super_image;
    extfs_u8 *journal_scratch;
    extfs_u8 descriptor[64] = {0U};
    extfs_u64 allocated[EXTFS_DIRECT_BLOCK_COUNT] = {0U};
    extfs_u64 freed[EXTFS_DIRECT_BLOCK_COUNT] = {0U};
    extfs_journal_metadata items[4];
    extfs_journal journal;
    extfs_u64 inode_offset;
    extfs_u64 inode_home;
    extfs_u64 descriptor_offset;
    extfs_u64 gdt_home = 0U;
    extfs_u64 super_home = 0U;
    extfs_u64 bitmap_block = 0U;
    extfs_u64 maximum_size;
    extfs_u64 new_free_blocks = volume != 0 ? volume->free_blocks : 0U;
    extfs_u32 inode_within;
    extfs_u32 descriptor_within = 0U;
    extfs_u32 super_within = 0U;
    extfs_u32 old_blocks;
    extfs_u32 new_blocks;
    extfs_u32 allocate_count = 0U;
    extfs_u32 free_count = 0U;
    extfs_u32 inode_group;
    extfs_u32 touched_group = 0U;
    extfs_u32 item_count = 0U;
    extfs_u32 i;
    int has_touched_group = 0;
    extfs_status status;

    if (volume == 0 || inode == 0 || scratch == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    if (volume->block_size > EXTFS_MAX_BLOCK_SIZE ||
        scratch_size < volume->block_size * 5U) {
        return EXTFS_ERR_BUFFER_TOO_SMALL;
    }

    /*
     * This checkpoint targets the ext3-on-ext2-layout case only.  Refusing
     * ext4-ish feature combinations here is intentional; a journal alone does
     * not make legacy bitmap/descriptor update rules valid for those layouts.
     */
    if (volume->kind != EXTFS_KIND_EXT3 ||
        volume->descriptor_size != 32U ||
        volume->metadata_checksums != 0U ||
        volume->blocks_per_group > 0xFFFFU ||
        (volume->feature_incompat & ~EXTFS_INCOMPAT_FILETYPE) != 0U ||
        (volume->feature_ro_compat &
         ~(EXTFS_RO_COMPAT_SPARSE_SUPER |
           EXTFS_RO_COMPAT_LARGE_FILE |
           EXTFS_RO_COMPAT_BTREE_DIR)) != 0U ||
        extfs_inode_write_assess(volume, inode) != EXTFS_OK ||
        (inode->flags & EXTFS_INODE_FLAG_EXTENTS) != 0U) {
        return EXTFS_ERR_UNSUPPORTED;
    }

    maximum_size = (extfs_u64)EXTFS_DIRECT_BLOCK_COUNT * volume->block_size;
    if (inode->size > maximum_size || new_size > maximum_size) {
        return EXTFS_ERR_UNSUPPORTED;
    }
    if (new_size == inode->size) {
        return EXTFS_OK;
    }

    gdt_image = bitmap_image + volume->block_size;
    inode_image = gdt_image + volume->block_size;
    super_image = inode_image + volume->block_size;
    journal_scratch = super_image + volume->block_size;

    /*
     * Refuse unsupported/dirty journals before zeroing even one byte beyond
     * EOF.  extfs_journal_commit_metadata() will repeat its own assessment at
     * commit time, but this early gate makes the pre-transaction data phase
     * side-effect free when journaling itself is unavailable.
     */
    status = extfs_journal_open(volume, &journal, journal_scratch,
                                volume->block_size);
    if (status != EXTFS_OK) return status;
    {
        extfs_u32 journal_risks = 0U;
        status = extfs_journal_write_assess(volume, &journal, &journal_risks);
        if (status != EXTFS_OK) return EXTFS_ERR_UNSUPPORTED;
    }

    old_blocks = (extfs_u32)extfs_div_round_up_u64(inode->size,
                                                    volume->block_size);
    new_blocks = (extfs_u32)extfs_div_round_up_u64(new_size,
                                                    volume->block_size);

    /* Validate the complete current direct mapping and allocation bitmap before
     * selecting any new blocks.  Sparse files and indirect pointers remain on
     * the bounded same-size writer only. */
    for (i = 0U; i < old_blocks; ++i) {
        extfs_u64 block = extfs_le32(inode->block_map + i * 4U);
        extfs_u32 group;
        extfs_u32 bit;
        extfs_u32 j;
        extfs_u64 current_bitmap;
        if (block == 0U || block >= volume->total_blocks) {
            return EXTFS_ERR_UNSUPPORTED;
        }
        for (j = 0U; j < i; ++j) {
            if (block == extfs_le32(inode->block_map + j * 4U)) {
                return EXTFS_ERR_CORRUPT;
            }
        }
        status = extfs_ext2_group_for_block(volume, block, &group, &bit);
        if (status != EXTFS_OK) return status;
        status = extfs_read_group_descriptor(volume, group, descriptor);
        if (status != EXTFS_OK) return status;
        if (extfs_ext2_block_is_known_metadata(volume, group, descriptor,
                                                block)) {
            return EXTFS_ERR_CORRUPT;
        }
        current_bitmap = extfs_le32(descriptor + 0x00U);
        status = extfs_read_block(volume, current_bitmap, bitmap_image);
        if (status != EXTFS_OK) return status;
        if (!extfs_bitmap_test(bitmap_image, bit)) return EXTFS_ERR_CORRUPT;
    }
    for (i = old_blocks; i < 15U; ++i) {
        if (extfs_le32(inode->block_map + i * 4U) != 0U) {
            return EXTFS_ERR_UNSUPPORTED;
        }
    }

    status = extfs_inode_byte_offset(volume, inode->number, &inode_offset,
                                     &inode_group);
    if (status != EXTFS_OK) return status;
    inode_home = inode_offset / volume->block_size;
    inode_within = (extfs_u32)(inode_offset % volume->block_size);
    if (inode_home >= volume->total_blocks ||
        inode_within > volume->block_size - volume->inode_size) {
        return EXTFS_ERR_UNSUPPORTED;
    }

    if (new_blocks > old_blocks) {
        extfs_u32 needed = new_blocks - old_blocks;
        extfs_u32 pass;
        if (volume->free_blocks < needed) return EXTFS_ERR_NO_SPACE;

        /*
         * Keep one resize transaction to one allocation group.  This is not an
         * ext3 format limitation; it bounds the number of mutable metadata
         * blocks until multi-group transaction assembly is added.
         */
        for (pass = 0U; pass < volume->group_count; ++pass) {
            extfs_u32 group = (inode_group + pass) % volume->group_count;
            extfs_u64 first;
            extfs_u32 group_blocks;
            extfs_u16 descriptor_free;
            extfs_u32 bit;
            extfs_u32 found = 0U;

            status = extfs_read_group_descriptor(volume, group, descriptor);
            if (status != EXTFS_OK) return status;
            descriptor_free = extfs_le16(descriptor + 0x0CU);
            if (descriptor_free < needed) continue;
            bitmap_block = extfs_le32(descriptor + 0x00U);
            if (bitmap_block == 0U || bitmap_block >= volume->total_blocks)
                return EXTFS_ERR_CORRUPT;
            status = extfs_ext2_group_bounds(volume, group, &first,
                                              &group_blocks);
            if (status != EXTFS_OK) return status;
            status = extfs_read_block(volume, bitmap_block, bitmap_image);
            if (status != EXTFS_OK) return status;
            for (bit = 0U; bit < group_blocks && found < needed; ++bit) {
                extfs_u64 candidate = first + bit;
                if (!extfs_bitmap_test(bitmap_image, bit) &&
                    !extfs_ext2_block_is_known_metadata(volume, group,
                                                        descriptor,
                                                        candidate)) {
                    allocated[found++] = candidate;
                }
            }
            if (found == needed) {
                allocate_count = found;
                touched_group = group;
                has_touched_group = 1;
                break;
            }
        }
        if (allocate_count != needed) {
            /* Space may exist across several groups, but the bounded ext3 resizer keeps one allocation group per transaction. */
            return EXTFS_ERR_UNSUPPORTED;
        }
        new_free_blocks = volume->free_blocks - needed;
    } else if (new_blocks < old_blocks) {
        for (i = new_blocks; i < old_blocks; ++i) {
            extfs_u64 block = extfs_le32(inode->block_map + i * 4U);
            extfs_u32 group;
            extfs_u32 bit;
            extfs_u64 current_bitmap;
            status = extfs_ext2_group_for_block(volume, block, &group, &bit);
            if (status != EXTFS_OK) return status;
            if (!has_touched_group) {
                touched_group = group;
                has_touched_group = 1;
            } else if (group != touched_group) {
                return EXTFS_ERR_UNSUPPORTED;
            }
            status = extfs_read_group_descriptor(volume, group, descriptor);
            if (status != EXTFS_OK) return status;
            current_bitmap = extfs_le32(descriptor + 0x00U);
            status = extfs_read_block(volume, current_bitmap, bitmap_image);
            if (status != EXTFS_OK) return status;
            if (!extfs_bitmap_test(bitmap_image, bit))
                return EXTFS_ERR_CORRUPT;
            freed[free_count++] = block;
        }
        if (volume->free_blocks + free_count > volume->total_blocks)
            return EXTFS_ERR_CORRUPT;
        new_free_blocks = volume->free_blocks + free_count;
    }

    /*
     * Ordered-data rule for growth: every byte that the new inode size could
     * expose is zeroed and durably flushed before its metadata transaction can
     * commit.  Writing zeros into still-free candidate blocks is harmless if a
     * crash occurs before the bitmap transaction.
     */
    if (new_size > inode->size) {
        extfs_zero(journal_scratch, volume->block_size);

        if (inode->size != 0U &&
            (inode->size % volume->block_size) != 0U) {
            extfs_u64 old_last_end = extfs_div_round_up_u64(
                inode->size, volume->block_size) * volume->block_size;
            extfs_u64 zero_end = new_size < old_last_end
                ? new_size : old_last_end;
            if (zero_end > inode->size) {
                extfs_u32 index =
                    (extfs_u32)(inode->size / volume->block_size);
                extfs_u64 block =
                    extfs_le32(inode->block_map + index * 4U);
                extfs_u32 within =
                    (extfs_u32)(inode->size % volume->block_size);
                extfs_u32 count = (extfs_u32)(zero_end - inode->size);
                extfs_u64 zero_offset;
                status = extfs_block_byte_offset(volume, block, within,
                                                 &zero_offset);
                if (status == EXTFS_OK)
                    status = extfs_write_bytes(volume, zero_offset,
                                               journal_scratch, count);
                if (status != EXTFS_OK) return status;
            }
        }

        for (i = 0U; i < allocate_count; ++i) {
            status = extfs_write_block(volume, allocated[i], journal_scratch);
            if (status != EXTFS_OK) return status;
        }

        status = extfs_flush(volume);
        if (status != EXTFS_OK) return status;
    }

    /*
     * Build the complete inode-table block image.  Compare the key fields with
     * the parsed inode first so a stale lower-device image cannot silently
     * overwrite unrelated metadata during checkpoint.
     */
    status = extfs_read_block(volume, inode_home, inode_image);
    if (status != EXTFS_OK) return status;
    {
        extfs_u8 *raw = inode_image + inode_within;
        extfs_u64 raw_size = (extfs_u64)extfs_le32(raw + 0x04U) |
                             ((extfs_u64)extfs_le32(raw + 0x6CU) << 32);
        extfs_u32 sectors = extfs_le32(raw + 0x1CU);
        extfs_u32 sectors_per_block = volume->block_size / 512U;

        if (extfs_le16(raw + 0x00U) != inode->mode ||
            raw_size != inode->size) {
            return EXTFS_ERR_CORRUPT;
        }
        for (i = 0U; i < 15U; ++i) {
            if (extfs_le32(raw + 0x28U + i * 4U) !=
                extfs_le32(inode->block_map + i * 4U)) {
                return EXTFS_ERR_CORRUPT;
            }
        }

        if (allocate_count != 0U) {
            extfs_u64 add =
                (extfs_u64)allocate_count * sectors_per_block;
            if (add > 0xFFFFFFFFULL - sectors) return EXTFS_ERR_RANGE;
            for (i = 0U; i < allocate_count; ++i) {
                extfs_store_le32(raw + 0x28U + (old_blocks + i) * 4U,
                                 (extfs_u32)allocated[i]);
            }
            extfs_store_le32(raw + 0x1CU, sectors + (extfs_u32)add);
        } else if (free_count != 0U) {
            extfs_u64 remove =
                (extfs_u64)free_count * sectors_per_block;
            if (remove > sectors) return EXTFS_ERR_CORRUPT;
            for (i = new_blocks; i < old_blocks; ++i)
                extfs_store_le32(raw + 0x28U + i * 4U, 0U);
            extfs_store_le32(raw + 0x1CU, sectors - (extfs_u32)remove);
        }
        extfs_store_le32(raw + 0x04U, (extfs_u32)new_size);
        extfs_store_le32(raw + 0x6CU, (extfs_u32)(new_size >> 32));
    }

    /*
     * Allocation/freeing touches one bitmap and its group descriptor.  Read
     * complete home-block images because JBD2 journals blocks, not arbitrary
     * byte ranges.
     */
    if (has_touched_group) {
        extfs_u16 descriptor_free;
        extfs_u16 new_descriptor_free;
        extfs_u32 k;

        status = extfs_group_descriptor_byte_offset(volume, touched_group,
                                                     &descriptor_offset);
        if (status != EXTFS_OK) return status;
        gdt_home = descriptor_offset / volume->block_size;
        descriptor_within =
            (extfs_u32)(descriptor_offset % volume->block_size);
        if (gdt_home >= volume->total_blocks ||
            descriptor_within > volume->block_size - volume->descriptor_size)
            return EXTFS_ERR_CORRUPT;

        status = extfs_read_block(volume, gdt_home, gdt_image);
        if (status != EXTFS_OK) return status;
        extfs_copy(descriptor, gdt_image + descriptor_within,
                   volume->descriptor_size);
        descriptor_free = extfs_le16(descriptor + 0x0CU);
        bitmap_block = extfs_le32(descriptor + 0x00U);
        if (bitmap_block == 0U || bitmap_block >= volume->total_blocks)
            return EXTFS_ERR_CORRUPT;
        if (inode_home == bitmap_block || inode_home == gdt_home)
            return EXTFS_ERR_CORRUPT;

        status = extfs_read_block(volume, bitmap_block, bitmap_image);
        if (status != EXTFS_OK) return status;

        if (allocate_count != 0U) {
            if (descriptor_free < allocate_count) return EXTFS_ERR_CORRUPT;
            new_descriptor_free =
                (extfs_u16)(descriptor_free - allocate_count);
            for (k = 0U; k < allocate_count; ++k) {
                extfs_u32 group;
                extfs_u32 bit;
                status = extfs_ext2_group_for_block(volume, allocated[k],
                                                     &group, &bit);
                if (status != EXTFS_OK) return status;
                if (group != touched_group ||
                    extfs_bitmap_test(bitmap_image, bit)) {
                    return EXTFS_ERR_CORRUPT;
                }
                extfs_bitmap_set(bitmap_image, bit);
            }
        } else {
            if ((extfs_u32)descriptor_free + free_count >
                volume->blocks_per_group) {
                return EXTFS_ERR_CORRUPT;
            }
            new_descriptor_free =
                (extfs_u16)(descriptor_free + free_count);
            for (k = 0U; k < free_count; ++k) {
                extfs_u32 group;
                extfs_u32 bit;
                status = extfs_ext2_group_for_block(volume, freed[k],
                                                     &group, &bit);
                if (status != EXTFS_OK) return status;
                if (group != touched_group ||
                    !extfs_bitmap_test(bitmap_image, bit)) {
                    return EXTFS_ERR_CORRUPT;
                }
                extfs_bitmap_clear(bitmap_image, bit);
            }
        }
        extfs_store_le16(gdt_image + descriptor_within + 0x0CU,
                         new_descriptor_free);

        status = extfs_primary_superblock_block_location(
            volume, &super_home, &super_within);
        if (status != EXTFS_OK) return status;
        if (super_home == bitmap_block || super_home == gdt_home ||
            super_home == inode_home) {
            return EXTFS_ERR_CORRUPT;
        }
        status = extfs_read_block(volume, super_home, super_image);
        if (status != EXTFS_OK) return status;
        {
            extfs_u8 *sb = super_image + super_within;
            if (extfs_le16(sb + 0x38U) != 0xEF53U ||
                !extfs_bytes_equal((const char *)(sb + 0x68U),
                                   (const char *)volume->uuid, 16U) ||
                extfs_le32(sb + 0x0CU) !=
                    (extfs_u32)volume->free_blocks ||
                extfs_le32(sb + 0x60U) != volume->feature_incompat ||
                new_free_blocks > 0xFFFFFFFFULL) {
                return EXTFS_ERR_CORRUPT;
            }
            extfs_store_le32(sb + 0x0CU, (extfs_u32)new_free_blocks);
            /*
             * This exact image is later checkpointed by JBD2.  RECOVER stays
             * set in it; extfs_journal_commit_metadata() clears the bit only
             * after the journal superblock has been durably marked empty.
             */
            extfs_store_le32(sb + 0x60U,
                             volume->feature_incompat |
                             EXTFS_INCOMPAT_RECOVER);
        }

        items[item_count].home_block = bitmap_block;
        items[item_count++].block_data = bitmap_image;
        items[item_count].home_block = gdt_home;
        items[item_count++].block_data = gdt_image;
    }

    items[item_count].home_block = inode_home;
    items[item_count++].block_data = inode_image;

    if (has_touched_group) {
        items[item_count].home_block = super_home;
        items[item_count++].block_data = super_image;
    }

    status = extfs_journal_commit_metadata(volume, &journal, items, item_count,
                                           journal_scratch,
                                           volume->block_size);
    if (status != EXTFS_OK) return status;

    /* Publish the new in-memory view only after journal commit, checkpoint,
     * journal-empty state and RECOVER clearing have all completed. */
    if (allocate_count != 0U) {
        for (i = 0U; i < allocate_count; ++i) {
            extfs_store_le32(inode->block_map + (old_blocks + i) * 4U,
                             (extfs_u32)allocated[i]);
        }
    } else if (free_count != 0U) {
        for (i = new_blocks; i < old_blocks; ++i)
            extfs_store_le32(inode->block_map + i * 4U, 0U);
    }
    inode->size = new_size;
    volume->free_blocks = new_free_blocks;
    return EXTFS_OK;
}

/*
 * ext4 bounded extent-tree mutation helpers.
 *
 * The inode-resident root can hold four leaf extents directly. 0.9.1 also
 * accepts one external depth-0 leaf referenced by a depth-1 inode root. This
 * keeps the first external-tree write checkpoint deliberately bounded while
 * exercising the real ext4 index/leaf format, extent-block checksum and
 * metadata-block accounting needed by deeper trees later.
 */
#define EXTFS_INLINE_EXTENT_CAPACITY       4U
#define EXTFS_EXTENT_INITIALIZED_MAX       32768U
#define EXTFS_EXTERNAL_EXTENT_RECORD_LIMIT 1024U
#define EXTFS_EXT4_RESIZE_SCRATCH_BLOCKS   8U

typedef struct extfs_inline_extent {
    extfs_u32 logical;
    extfs_u32 length;
    extfs_u64 physical;
} extfs_inline_extent;

static extfs_u32 extfs_ext4_external_extent_capacity(
    const extfs_volume *volume)
{
    if (volume == 0 || volume->block_size < 16U) return 0U;
    return (volume->block_size - 12U) / 12U;
}

static extfs_status extfs_ext4_parse_extent_records(
    const extfs_volume *volume,
    const extfs_u8 *node,
    extfs_u32 node_size,
    extfs_u16 expected_depth,
    extfs_u32 expected_blocks,
    extfs_inline_extent *extents,
    extfs_u32 extent_capacity,
    extfs_u32 *extent_count)
{
    extfs_u32 entries;
    extfs_u32 maximum;
    extfs_u32 logical_end = 0U;
    extfs_u32 i;

    if (volume == 0 || node == 0 || extents == 0 || extent_count == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    if (node_size < 12U || extfs_le16(node + 0x00U) != EXTFS_EXTENT_MAGIC ||
        extfs_le16(node + 0x06U) != expected_depth)
        return EXTFS_ERR_UNSUPPORTED;

    entries = extfs_le16(node + 0x02U);
    maximum = extfs_le16(node + 0x04U);
    if (maximum > (node_size - 12U) / 12U || entries > maximum ||
        entries > extent_capacity ||
        entries > EXTFS_EXTERNAL_EXTENT_RECORD_LIMIT)
        return EXTFS_ERR_UNSUPPORTED;
    if ((expected_blocks == 0U && entries != 0U) ||
        (expected_blocks != 0U && entries == 0U))
        return EXTFS_ERR_UNSUPPORTED;

    for (i = 0U; i < entries; ++i) {
        const extfs_u8 *raw = node + 12U + i * 12U;
        extfs_u16 encoded_length = extfs_le16(raw + 0x04U);
        extfs_u64 physical = extfs_le32(raw + 0x08U);
        extfs_u32 logical = extfs_le32(raw + 0x00U);
        extfs_u32 j;

        physical |= (extfs_u64)extfs_le16(raw + 0x06U) << 32;
        if (encoded_length == 0U ||
            encoded_length > EXTFS_EXTENT_INITIALIZED_MAX)
            return EXTFS_ERR_UNSUPPORTED; /* unwritten extent */
        if (logical != logical_end)
            return EXTFS_ERR_UNSUPPORTED; /* sparse/gapped tree */
        if (physical < volume->first_data_block ||
            physical >= volume->total_blocks ||
            (extfs_u64)encoded_length > volume->total_blocks - physical)
            return EXTFS_ERR_CORRUPT;
        if ((extfs_u64)logical_end + encoded_length > 0xFFFFFFFFULL)
            return EXTFS_ERR_RANGE;

        extents[i].logical = logical;
        extents[i].length = encoded_length;
        extents[i].physical = physical;
        logical_end += encoded_length;

        /* A block bitmap cannot reveal physical aliasing by two extent
         * records, so reject it explicitly before any mutation. The bounded
         * 0.9 writer caps the external leaf at 1024 records to keep this
         * validation predictable under the filesystem metadata lock. */
        for (j = 0U; j < i; ++j) {
            extfs_u64 a0 = extents[j].physical;
            extfs_u64 a1 = a0 + extents[j].length;
            extfs_u64 b0 = physical;
            extfs_u64 b1 = physical + encoded_length;
            if (a0 < b1 && b0 < a1) return EXTFS_ERR_CORRUPT;
        }
    }
    if (logical_end != expected_blocks) return EXTFS_ERR_UNSUPPORTED;
    *extent_count = entries;
    return EXTFS_OK;
}

static void extfs_ext4_store_extent_records(
    extfs_u8 *node,
    extfs_u32 maximum,
    const extfs_inline_extent *extents,
    extfs_u32 extent_count)
{
    extfs_u32 i;
    extfs_u32 bytes = 12U + maximum * 12U;
    extfs_zero(node, bytes);
    extfs_store_le16(node + 0x00U, EXTFS_EXTENT_MAGIC);
    extfs_store_le16(node + 0x02U, (extfs_u16)extent_count);
    extfs_store_le16(node + 0x04U, (extfs_u16)maximum);
    extfs_store_le16(node + 0x06U, 0U);
    extfs_store_le32(node + 0x08U, 0U);
    for (i = 0U; i < extent_count; ++i) {
        extfs_u8 *raw = node + 12U + i * 12U;
        extfs_store_le32(raw + 0x00U, extents[i].logical);
        extfs_store_le16(raw + 0x04U, (extfs_u16)extents[i].length);
        extfs_store_le16(raw + 0x06U,
                         (extfs_u16)(extents[i].physical >> 32));
        extfs_store_le32(raw + 0x08U,
                         (extfs_u32)extents[i].physical);
    }
}

static void extfs_ext4_store_inline_extent_root(
    extfs_u8 root[60],
    const extfs_inline_extent *extents,
    extfs_u32 extent_count)
{
    extfs_zero(root, 60U);
    extfs_ext4_store_extent_records(root, EXTFS_INLINE_EXTENT_CAPACITY,
                                    extents, extent_count);
}

static void extfs_ext4_store_depth1_root(extfs_u8 root[60],
                                         extfs_u64 leaf_block)
{
    extfs_u8 *index;
    extfs_zero(root, 60U);
    extfs_store_le16(root + 0x00U, EXTFS_EXTENT_MAGIC);
    extfs_store_le16(root + 0x02U, 1U);
    extfs_store_le16(root + 0x04U, EXTFS_INLINE_EXTENT_CAPACITY);
    extfs_store_le16(root + 0x06U, 1U);
    extfs_store_le32(root + 0x08U, 0U);
    index = root + 12U;
    extfs_store_le32(index + 0x00U, 0U);
    extfs_store_le32(index + 0x04U, (extfs_u32)leaf_block);
    extfs_store_le16(index + 0x08U, (extfs_u16)(leaf_block >> 32));
    extfs_store_le16(index + 0x0AU, 0U);
}

static extfs_status extfs_ext4_store_extent_block_checksum(
    const extfs_volume *volume,
    const extfs_inode *inode,
    extfs_u8 *block)
{
    extfs_u8 number_le[4];
    extfs_u8 generation_le[4];
    extfs_u32 maximum;
    extfs_u32 tail_offset;
    extfs_u32 crc;

    if (volume == 0 || inode == 0 || block == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    if (volume->metadata_checksums == 0U) return EXTFS_OK;
    maximum = extfs_le16(block + 0x04U);
    if (maximum > (volume->block_size - 12U) / 12U)
        return EXTFS_ERR_CORRUPT;
    tail_offset = 12U + maximum * 12U;
    if (tail_offset > volume->block_size - 4U)
        return EXTFS_ERR_CORRUPT;

    extfs_store_le32(number_le, inode->number);
    extfs_store_le32(generation_le, inode->generation);
    extfs_store_le32(block + tail_offset, 0U);
    crc = extfs_crc32c(volume->checksum_seed, number_le, 4U);
    crc = extfs_crc32c(crc, generation_le, 4U);
    crc = extfs_crc32c(crc, block, tail_offset);
    extfs_store_le32(block + tail_offset, crc);
    return EXTFS_OK;
}

static extfs_status extfs_ext4_parse_bounded_extent_tree(
    const extfs_volume *volume,
    const extfs_inode *inode,
    extfs_u32 expected_blocks,
    extfs_inline_extent *extents,
    extfs_u32 extent_capacity,
    extfs_u8 *leaf_image,
    extfs_u32 *extent_count,
    extfs_u16 *tree_depth,
    extfs_u64 *leaf_block)
{
    const extfs_u8 *root;
    extfs_u16 depth;
    extfs_u16 entries;
    extfs_u16 maximum;
    extfs_u32 leaf_capacity;
    extfs_status status;

    if (volume == 0 || inode == 0 || extents == 0 || leaf_image == 0 ||
        extent_count == 0 || tree_depth == 0 || leaf_block == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    root = inode->block_map;
    if (extfs_le16(root + 0x00U) != EXTFS_EXTENT_MAGIC)
        return EXTFS_ERR_UNSUPPORTED;
    entries = extfs_le16(root + 0x02U);
    maximum = extfs_le16(root + 0x04U);
    depth = extfs_le16(root + 0x06U);
    if (maximum != EXTFS_INLINE_EXTENT_CAPACITY || entries > maximum)
        return EXTFS_ERR_UNSUPPORTED;

    if (depth == 0U) {
        *leaf_block = 0U;
        *tree_depth = 0U;
        return extfs_ext4_parse_extent_records(
            volume, root, 60U, 0U, expected_blocks, extents,
            extent_capacity, extent_count);
    }
    if (depth != 1U || entries != 1U || expected_blocks == 0U)
        return EXTFS_ERR_UNSUPPORTED;
    if (extfs_le32(root + 12U) != 0U)
        return EXTFS_ERR_UNSUPPORTED; /* dense file must start at lblk 0 */

    *leaf_block = extfs_le32(root + 16U);
    *leaf_block |= (extfs_u64)extfs_le16(root + 20U) << 32;
    if (*leaf_block < volume->first_data_block ||
        *leaf_block >= volume->total_blocks)
        return EXTFS_ERR_CORRUPT;
    status = extfs_read_block(volume, *leaf_block, leaf_image);
    if (status != EXTFS_OK) return status;
    status = extfs_validate_extent_block(volume, inode, leaf_image);
    if (status != EXTFS_OK) return status;

    leaf_capacity = extfs_ext4_external_extent_capacity(volume);
    if (leaf_capacity == 0U ||
        extfs_le16(leaf_image + 0x04U) != leaf_capacity)
        return EXTFS_ERR_UNSUPPORTED;
    status = extfs_ext4_parse_extent_records(
        volume, leaf_image, volume->block_size, 0U, expected_blocks,
        extents, extent_capacity, extent_count);
    if (status != EXTFS_OK) return status;
    *tree_depth = 1U;
    return EXTFS_OK;
}

static extfs_status extfs_ext4_extent_map(
    const extfs_inline_extent *extents,
    extfs_u32 extent_count,
    extfs_u32 logical_block,
    extfs_u64 *physical_block)
{
    extfs_u32 i;
    if (extents == 0 || physical_block == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    for (i = 0U; i < extent_count; ++i) {
        if (logical_block >= extents[i].logical &&
            logical_block - extents[i].logical < extents[i].length) {
            *physical_block = extents[i].physical +
                (logical_block - extents[i].logical);
            return EXTFS_OK;
        }
    }
    return EXTFS_ERR_NOT_FOUND;
}

static extfs_status extfs_ext4_load_group_allocation(
    const extfs_volume *volume,
    extfs_u32 group,
    extfs_u8 descriptor[64],
    extfs_u8 *bitmap,
    extfs_u64 *bitmap_block)
{
    extfs_status status;
    extfs_u64 block;

    if (volume == 0 || descriptor == 0 || bitmap == 0 || bitmap_block == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    status = extfs_read_group_descriptor(volume, group, descriptor);
    if (status != EXTFS_OK) return status;
    if ((extfs_le16(descriptor + 0x12U) & EXTFS_BG_BLOCK_UNINIT) != 0U)
        return EXTFS_ERR_UNSUPPORTED;
    block = extfs_le32(descriptor + 0x00U);
    if (block == 0U || block >= volume->total_blocks)
        return EXTFS_ERR_CORRUPT;
    status = extfs_read_block(volume, block, bitmap);
    if (status != EXTFS_OK) return status;
    status = extfs_ext4_validate_block_bitmap_checksum(
        volume, bitmap, descriptor);
    if (status != EXTFS_OK) return status;
    *bitmap_block = block;
    return EXTFS_OK;
}

static extfs_status extfs_ext4_validate_extent_allocations(
    const extfs_volume *volume,
    const extfs_inline_extent *extents,
    extfs_u32 extent_count,
    extfs_u8 *bitmap,
    extfs_u8 descriptor[64])
{
    extfs_u32 i;

    for (i = 0U; i < extent_count; ++i) {
        extfs_u64 physical = extents[i].physical;
        extfs_u32 remaining = extents[i].length;
        while (remaining != 0U) {
            extfs_u32 group;
            extfs_u32 bit;
            extfs_u64 group_first;
            extfs_u32 group_blocks;
            extfs_u32 chunk;
            extfs_u64 bitmap_block;
            extfs_u32 j;
            extfs_status status;

            status = extfs_ext2_group_for_block(volume, physical,
                                                &group, &bit);
            if (status != EXTFS_OK) return status;
            status = extfs_ext2_group_bounds(volume, group,
                                              &group_first, &group_blocks);
            if (status != EXTFS_OK) return status;
            chunk = group_blocks - bit;
            if (chunk > remaining) chunk = remaining;
            status = extfs_ext4_load_group_allocation(
                volume, group, descriptor, bitmap, &bitmap_block);
            if (status != EXTFS_OK) return status;
            (void)group_first;
            (void)bitmap_block;

            for (j = 0U; j < chunk; ++j) {
                extfs_u64 block = physical + j;
                if (!extfs_bitmap_test(bitmap, bit + j) ||
                    extfs_ext2_block_is_known_metadata(
                        volume, group, descriptor, block))
                    return EXTFS_ERR_CORRUPT;
            }
            physical += chunk;
            remaining -= chunk;
        }
    }
    return EXTFS_OK;
}

static extfs_status extfs_ext4_validate_tree_block_allocation(
    const extfs_volume *volume,
    const extfs_inline_extent *extents,
    extfs_u32 extent_count,
    extfs_u64 tree_block,
    extfs_u8 *bitmap,
    extfs_u8 descriptor[64])
{
    extfs_u32 group;
    extfs_u32 bit;
    extfs_u64 bitmap_block;
    extfs_u32 i;
    extfs_status status;

    if (tree_block == 0U) return EXTFS_OK;
    for (i = 0U; i < extent_count; ++i) {
        if (tree_block >= extents[i].physical &&
            tree_block - extents[i].physical < extents[i].length)
            return EXTFS_ERR_CORRUPT;
    }
    status = extfs_ext2_group_for_block(volume, tree_block, &group, &bit);
    if (status != EXTFS_OK) return status;
    status = extfs_ext4_load_group_allocation(
        volume, group, descriptor, bitmap, &bitmap_block);
    if (status != EXTFS_OK) return status;
    if (!extfs_bitmap_test(bitmap, bit) ||
        extfs_ext2_block_is_known_metadata(volume, group, descriptor,
                                           tree_block))
        return EXTFS_ERR_CORRUPT;
    return EXTFS_OK;
}

static extfs_status extfs_ext4_find_free_run_in_loaded_group(
    const extfs_volume *volume,
    extfs_u32 group,
    const extfs_u8 descriptor[64],
    const extfs_u8 *bitmap,
    extfs_u32 needed,
    extfs_u64 *run_start)
{
    extfs_u64 first;
    extfs_u32 count;
    extfs_u32 run = 0U;
    extfs_u32 run_bit = 0U;
    extfs_u32 bit;
    extfs_status status;

    if (needed == 0U || run_start == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    status = extfs_ext2_group_bounds(volume, group, &first, &count);
    if (status != EXTFS_OK) return status;
    if (needed > count || extfs_le16(descriptor + 0x0CU) < needed)
        return EXTFS_ERR_NO_SPACE;

    for (bit = 0U; bit < count; ++bit) {
        extfs_u64 block = first + bit;
        int available = !extfs_bitmap_test(bitmap, bit) &&
            !extfs_ext2_block_is_known_metadata(
                volume, group, descriptor, block);
        if (available) {
            if (run == 0U) run_bit = bit;
            ++run;
            if (run == needed) {
                *run_start = first + run_bit;
                return EXTFS_OK;
            }
        } else {
            run = 0U;
        }
    }
    return EXTFS_ERR_NO_SPACE;
}

static extfs_status extfs_ext4_select_growth_run(
    const extfs_volume *volume,
    const extfs_inode *inode,
    const extfs_inline_extent *extents,
    extfs_u32 extent_count,
    extfs_u32 needed,
    extfs_u32 new_extent_overhead,
    int allow_new_extent,
    extfs_u8 *bitmap,
    extfs_u8 descriptor[64],
    extfs_u32 *selected_group,
    extfs_u64 *selected_bitmap_block,
    extfs_u64 *selected_start,
    int *merge_last)
{
    extfs_u32 preferred_group = 0U;
    extfs_u32 inode_group = 0U;
    extfs_u64 ignored_inode_offset;
    extfs_u32 attempt;
    extfs_status status;

    if (needed == 0U || selected_group == 0 || selected_bitmap_block == 0 ||
        selected_start == 0 || merge_last == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    if (needed > EXTFS_EXTENT_INITIALIZED_MAX ||
        new_extent_overhead > 1U)
        return EXTFS_ERR_UNSUPPORTED;

    status = extfs_inode_byte_offset(volume, inode->number,
                                     &ignored_inode_offset, &inode_group);
    if (status != EXTFS_OK) return status;
    preferred_group = inode_group;

    /* First preference is always in-place extension of the last extent. */
    if (extent_count != 0U) {
        extfs_u64 candidate = extents[extent_count - 1U].physical +
                              extents[extent_count - 1U].length;
        extfs_u32 group;
        extfs_u32 bit;
        extfs_u64 group_first;
        extfs_u32 group_blocks;
        if (candidate < volume->total_blocks &&
            extfs_ext2_group_for_block(volume, candidate,
                                       &group, &bit) == EXTFS_OK &&
            extfs_ext2_group_bounds(volume, group,
                                    &group_first, &group_blocks) == EXTFS_OK &&
            needed <= group_blocks - bit) {
            extfs_u32 j;
            int free_run = 1;
            status = extfs_ext4_load_group_allocation(
                volume, group, descriptor, bitmap, selected_bitmap_block);
            if (status != EXTFS_OK && status != EXTFS_ERR_UNSUPPORTED)
                return status;
            if (status == EXTFS_OK) {
                for (j = 0U; j < needed; ++j) {
                    extfs_u64 block = candidate + j;
                    if (extfs_bitmap_test(bitmap, bit + j) ||
                        extfs_ext2_block_is_known_metadata(
                            volume, group, descriptor, block)) {
                        free_run = 0;
                        break;
                    }
                }
                if (free_run &&
                    extents[extent_count - 1U].length + needed <=
                        EXTFS_EXTENT_INITIALIZED_MAX) {
                    *selected_group = group;
                    *selected_start = candidate;
                    *merge_last = 1;
                    return EXTFS_OK;
                }
            }
            preferred_group = group;
            (void)group_first;
        }
    }

    if (!allow_new_extent) return EXTFS_ERR_UNSUPPORTED;
    if (needed > 0xFFFFFFFFU - new_extent_overhead)
        return EXTFS_ERR_RANGE;

    /* A conversion from the four-entry inline root reserves one metadata block
     * in the same free run as the appended data. This keeps the first external
     * tree transaction to one bitmap/descriptor pair. */
    for (attempt = 0U; attempt < volume->group_count; ++attempt) {
        extfs_u32 group = (preferred_group + attempt) % volume->group_count;
        extfs_u64 bitmap_block;
        extfs_u64 run_start;
        status = extfs_ext4_load_group_allocation(
            volume, group, descriptor, bitmap, &bitmap_block);
        if (status == EXTFS_ERR_UNSUPPORTED) continue;
        if (status != EXTFS_OK) return status;
        status = extfs_ext4_find_free_run_in_loaded_group(
            volume, group, descriptor, bitmap,
            needed + new_extent_overhead, &run_start);
        if (status == EXTFS_ERR_NO_SPACE) continue;
        if (status != EXTFS_OK) return status;
        *selected_group = group;
        *selected_bitmap_block = bitmap_block;
        *selected_start = run_start;
        *merge_last = 0;
        return EXTFS_OK;
    }
    return EXTFS_ERR_NO_SPACE;
}

/*
 * Resize an ext4 regular file whose extent tree is either the depth-0 inode
 * root or one checksummed external depth-0 leaf referenced by a depth-1 inode
 * root. One resize may change one allocation group. The bounded depth-1 form
 * can hold up to min(real leaf capacity, 1024) initialized, logically dense
 * extents. Deeper trees, multiple index entries, holes and unwritten extents
 * remain fail-closed for a later checkpoint.
 */
extfs_status extfs_resize_file_ext4_journaled_extent_tree(
    extfs_volume *volume,
    extfs_inode *inode,
    extfs_u64 new_size,
    void *scratch,
    extfs_u32 scratch_size)
{
    extfs_u8 *bitmap_image = (extfs_u8 *)scratch;
    extfs_u8 *gdt_image;
    extfs_u8 *inode_image;
    extfs_u8 *super_image;
    extfs_u8 *leaf_image;
    extfs_u8 *journal_scratch;
    extfs_inline_extent *extents;
    extfs_u8 descriptor[64] = {0U};
    extfs_journal_metadata items[5];
    extfs_journal journal;
    extfs_u64 inode_offset;
    extfs_u64 inode_home;
    extfs_u64 descriptor_offset;
    extfs_u64 gdt_home = 0U;
    extfs_u64 super_home = 0U;
    extfs_u64 bitmap_block = 0U;
    extfs_u64 allocation_run_start = 0U;
    extfs_u64 allocation_start = 0U;
    extfs_u64 old_leaf_block = 0U;
    extfs_u64 new_leaf_block = 0U;
    extfs_u64 new_free_blocks;
    extfs_u64 old_blocks64;
    extfs_u64 new_blocks64;
    extfs_u32 inode_within;
    extfs_u32 descriptor_within = 0U;
    extfs_u32 super_within = 0U;
    extfs_u32 old_blocks;
    extfs_u32 new_blocks;
    extfs_u32 extent_count = 0U;
    extfs_u32 new_extent_count = 0U;
    extfs_u32 leaf_capacity;
    extfs_u32 workspace_capacity;
    extfs_u32 touched_group = 0U;
    extfs_u32 item_count = 0U;
    extfs_u32 data_changed = 0U;
    extfs_u32 allocation_changed = 0U;
    extfs_u32 tree_allocated = 0U;
    extfs_u32 tree_freed = 0U;
    extfs_u32 i;
    extfs_u16 old_depth = 0U;
    extfs_u16 new_depth = 0U;
    extfs_status status;
    int changes_allocation = 0;
    int merge_last = 0;

    if (volume == 0 || inode == 0 || scratch == 0)
        return EXTFS_ERR_INVALID_ARGUMENT;
    if (volume->block_size > EXTFS_MAX_BLOCK_SIZE ||
        scratch_size < volume->block_size * EXTFS_EXT4_RESIZE_SCRATCH_BLOCKS)
        return EXTFS_ERR_BUFFER_TOO_SMALL;

    /* 64-bit group descriptors, flex_bg, bigalloc and quota accounting still
     * need separate allocator/checksum work. */
    if (volume->kind != EXTFS_KIND_EXT4 ||
        volume->descriptor_size != 32U ||
        volume->metadata_checksums == 0U ||
        (volume->feature_compat & EXTFS_COMPAT_HAS_JOURNAL) == 0U ||
        (volume->feature_incompat &
         ~(EXTFS_INCOMPAT_FILETYPE | EXTFS_INCOMPAT_EXTENTS |
           EXTFS_INCOMPAT_CSUM_SEED)) != 0U ||
        (volume->feature_ro_compat &
         ~(EXTFS_RO_COMPAT_SPARSE_SUPER |
           EXTFS_RO_COMPAT_LARGE_FILE |
           EXTFS_RO_COMPAT_BTREE_DIR |
           EXTFS_RO_COMPAT_EXTRA_ISIZE |
           EXTFS_RO_COMPAT_METADATA_CSUM)) != 0U ||
        volume->blocks_per_group > 0xFFFFU ||
        (volume->blocks_per_group & 7U) != 0U ||
        extfs_inode_write_assess(volume, inode) != EXTFS_OK ||
        (inode->flags & EXTFS_INODE_FLAG_EXTENTS) == 0U ||
        (inode->flags & EXTFS_INODE_FLAG_EOFBLOCKS) != 0U)
        return EXTFS_ERR_UNSUPPORTED;
    if (new_size == inode->size) return EXTFS_OK;

    old_blocks64 = extfs_div_round_up_u64(inode->size, volume->block_size);
    new_blocks64 = extfs_div_round_up_u64(new_size, volume->block_size);
    if (old_blocks64 > 0xFFFFFFFFULL || new_blocks64 > 0xFFFFFFFFULL)
        return EXTFS_ERR_UNSUPPORTED;
    old_blocks = (extfs_u32)old_blocks64;
    new_blocks = (extfs_u32)new_blocks64;

    gdt_image = bitmap_image + volume->block_size;
    inode_image = gdt_image + volume->block_size;
    super_image = inode_image + volume->block_size;
    leaf_image = super_image + volume->block_size;
    journal_scratch = leaf_image + volume->block_size;
    extents = (extfs_inline_extent *)(void *)(journal_scratch +
                                               volume->block_size);
    workspace_capacity = (volume->block_size * 2U) /
                         (extfs_u32)sizeof(*extents);
    leaf_capacity = extfs_ext4_external_extent_capacity(volume);
    if (workspace_capacity < EXTFS_INLINE_EXTENT_CAPACITY ||
        leaf_capacity < EXTFS_INLINE_EXTENT_CAPACITY ||
        workspace_capacity <
            (leaf_capacity < EXTFS_EXTERNAL_EXTENT_RECORD_LIMIT ?
             leaf_capacity : EXTFS_EXTERNAL_EXTENT_RECORD_LIMIT))
        return EXTFS_ERR_BUFFER_TOO_SMALL;

    status = extfs_ext4_parse_bounded_extent_tree(
        volume, inode, old_blocks, extents, workspace_capacity, leaf_image,
        &extent_count, &old_depth, &old_leaf_block);
    if (status != EXTFS_OK) return status;
    new_extent_count = extent_count;
    new_depth = old_depth;
    new_leaf_block = old_leaf_block;

    /* Authenticate the current inode and its allocation accounting before any
     * ordered-data zeroing can touch free blocks. External xattr blocks are
     * intentionally refused in this bounded resizer because i_blocks would
     * otherwise include metadata outside the extent tree. */
    status = extfs_inode_byte_offset(volume, inode->number, &inode_offset, 0);
    if (status != EXTFS_OK) return status;
    inode_home = inode_offset / volume->block_size;
    inode_within = (extfs_u32)(inode_offset % volume->block_size);
    if (inode_home >= volume->total_blocks ||
        inode_within > volume->block_size - volume->inode_size)
        return EXTFS_ERR_CORRUPT;
    status = extfs_read_block(volume, inode_home, inode_image);
    if (status != EXTFS_OK) return status;
    {
        const extfs_u8 *raw = inode_image + inode_within;
        extfs_u64 raw_size = (extfs_u64)extfs_le32(raw + 0x04U) |
                             ((extfs_u64)extfs_le32(raw + 0x6CU) << 32);
        extfs_u64 old_allocated_blocks = old_blocks +
                                         (old_depth == 1U ? 1U : 0U);
        extfs_u64 sectors = old_allocated_blocks *
                            (volume->block_size / 512U);
        if (extfs_validate_inode_checksum(volume, inode->number, raw) !=
                EXTFS_OK ||
            extfs_le16(raw + 0x00U) != inode->mode ||
            extfs_le32(raw + 0x20U) != inode->flags ||
            extfs_le32(raw + 0x64U) != inode->generation ||
            extfs_le32(raw + 0x68U) != 0U ||
            extfs_le16(raw + 0x76U) != 0U ||
            raw_size != inode->size ||
            extfs_le16(raw + 0x74U) != 0U ||
            sectors > 0xFFFFFFFFULL ||
            extfs_le32(raw + 0x1CU) != (extfs_u32)sectors ||
            !extfs_bytes_equal((const char *)(raw + 0x28U),
                               (const char *)inode->block_map, 60U))
            return EXTFS_ERR_UNSUPPORTED;
    }

    status = extfs_journal_open(volume, &journal, journal_scratch,
                                volume->block_size);
    if (status != EXTFS_OK) return status;
    {
        extfs_u32 journal_risks = 0U;
        if (extfs_journal_write_assess(volume, &journal, &journal_risks) !=
            EXTFS_OK) return EXTFS_ERR_UNSUPPORTED;
    }

    status = extfs_ext4_validate_extent_allocations(
        volume, extents, extent_count, bitmap_image, descriptor);
    if (status != EXTFS_OK) return status;
    if (old_depth == 1U) {
        status = extfs_ext4_validate_tree_block_allocation(
            volume, extents, extent_count, old_leaf_block,
            bitmap_image, descriptor);
        if (status != EXTFS_OK) return status;
    }

    new_free_blocks = volume->free_blocks;
    if (new_blocks > old_blocks) {
        extfs_u32 needed = new_blocks - old_blocks;
        extfs_u32 new_extent_overhead = 0U;
        int allow_new_extent = 1;
        extfs_u32 first_bit;

        if (needed > EXTFS_EXTENT_INITIALIZED_MAX)
            return EXTFS_ERR_UNSUPPORTED;
        if (old_depth == 0U && extent_count >= EXTFS_INLINE_EXTENT_CAPACITY)
            new_extent_overhead = 1U;
        if (old_depth == 1U && extent_count >= leaf_capacity)
            allow_new_extent = 0;
        if (old_depth == 1U && extent_count >= EXTFS_EXTERNAL_EXTENT_RECORD_LIMIT)
            allow_new_extent = 0;
        if (volume->free_blocks < (extfs_u64)needed + new_extent_overhead)
            return EXTFS_ERR_NO_SPACE;

        status = extfs_ext4_select_growth_run(
            volume, inode, extents, extent_count, needed,
            new_extent_overhead, allow_new_extent,
            bitmap_image, descriptor, &touched_group, &bitmap_block,
            &allocation_run_start, &merge_last);
        if (status != EXTFS_OK) return status;

        if (merge_last) {
            allocation_start = allocation_run_start;
            extents[extent_count - 1U].length += needed;
        } else {
            if (old_depth == 0U && extent_count >= EXTFS_INLINE_EXTENT_CAPACITY) {
                new_leaf_block = allocation_run_start;
                allocation_start = allocation_run_start + 1U;
                new_depth = 1U;
                tree_allocated = 1U;
            } else {
                allocation_start = allocation_run_start;
            }
            if (new_extent_count >= workspace_capacity ||
                (new_depth == 1U &&
                 new_extent_count >= EXTFS_EXTERNAL_EXTENT_RECORD_LIMIT))
                return EXTFS_ERR_UNSUPPORTED;
            extents[new_extent_count].logical = old_blocks;
            extents[new_extent_count].length = needed;
            extents[new_extent_count].physical = allocation_start;
            ++new_extent_count;
        }

        status = extfs_ext2_group_for_block(volume, allocation_run_start,
                                            &touched_group, &first_bit);
        if (status != EXTFS_OK) return status;
        status = extfs_ext4_load_group_allocation(
            volume, touched_group, descriptor, bitmap_image, &bitmap_block);
        if (status != EXTFS_OK) return status;
        allocation_changed = needed + tree_allocated;
        data_changed = needed;
        if (extfs_le16(descriptor + 0x0CU) < allocation_changed)
            return EXTFS_ERR_CORRUPT;
        for (i = 0U; i < allocation_changed; ++i) {
            extfs_u64 block = allocation_run_start + i;
            extfs_u32 group;
            extfs_u32 bit;
            status = extfs_ext2_group_for_block(volume, block, &group, &bit);
            if (status != EXTFS_OK || group != touched_group ||
                extfs_bitmap_test(bitmap_image, bit) ||
                extfs_ext2_block_is_known_metadata(
                    volume, group, descriptor, block))
                return EXTFS_ERR_CORRUPT;
        }
        new_free_blocks -= allocation_changed;
        changes_allocation = 1;
        (void)first_bit;
    } else if (new_blocks < old_blocks) {
        extfs_u32 released = old_blocks - new_blocks;
        extfs_u32 release_group = 0U;
        int have_release_group = 0;

        if (volume->free_blocks + released > volume->total_blocks)
            return EXTFS_ERR_CORRUPT;
        for (i = new_blocks; i < old_blocks; ++i) {
            extfs_u64 block;
            extfs_u32 group;
            extfs_u32 bit;
            status = extfs_ext4_extent_map(extents, extent_count, i, &block);
            if (status != EXTFS_OK) return EXTFS_ERR_CORRUPT;
            status = extfs_ext2_group_for_block(volume, block, &group, &bit);
            if (status != EXTFS_OK) return status;
            if (!have_release_group) {
                release_group = group;
                have_release_group = 1;
            } else if (group != release_group) {
                return EXTFS_ERR_UNSUPPORTED;
            }
            (void)bit;
        }

        /* Trim/remove trailing extents in-place. */
        new_extent_count = 0U;
        for (i = 0U; i < extent_count; ++i) {
            extfs_u32 start = extents[i].logical;
            extfs_u32 end = start + extents[i].length;
            if (start >= new_blocks) break;
            if (end > new_blocks) extents[i].length = new_blocks - start;
            ++new_extent_count;
        }
        if (old_depth == 1U && new_extent_count <= EXTFS_INLINE_EXTENT_CAPACITY) {
            extfs_u32 leaf_group;
            extfs_u32 leaf_bit;
            status = extfs_ext2_group_for_block(
                volume, old_leaf_block, &leaf_group, &leaf_bit);
            if (status != EXTFS_OK) return status;
            if (have_release_group && leaf_group != release_group)
                return EXTFS_ERR_UNSUPPORTED;
            release_group = leaf_group;
            have_release_group = 1;
            new_depth = 0U;
            new_leaf_block = 0U;
            tree_freed = 1U;
            (void)leaf_bit;
        }

        if (!have_release_group) return EXTFS_ERR_CORRUPT;
        touched_group = release_group;
        status = extfs_ext4_load_group_allocation(
            volume, touched_group, descriptor, bitmap_image, &bitmap_block);
        if (status != EXTFS_OK) return status;
        allocation_changed = released + tree_freed;
        data_changed = released;
        if ((extfs_u32)extfs_le16(descriptor + 0x0CU) + allocation_changed >
            volume->blocks_per_group)
            return EXTFS_ERR_CORRUPT;
        new_free_blocks += allocation_changed;
        changes_allocation = 1;
    }

    status = extfs_inode_byte_offset(volume, inode->number, &inode_offset, 0);
    if (status != EXTFS_OK) return status;
    inode_home = inode_offset / volume->block_size;
    inode_within = (extfs_u32)(inode_offset % volume->block_size);
    if (inode_home >= volume->total_blocks ||
        inode_within > volume->block_size - volume->inode_size)
        return EXTFS_ERR_CORRUPT;

    /* Ordered-data growth: newly visible bytes become durable before the JBD2
     * transaction can expose the larger i_size. The new tree block is metadata
     * and is therefore journaled rather than written in this phase. */
    if (new_size > inode->size) {
        extfs_zero(journal_scratch, volume->block_size);
        if (old_blocks != 0U && (inode->size % volume->block_size) != 0U) {
            extfs_u64 old_last_end = extfs_div_round_up_u64(
                inode->size, volume->block_size) * volume->block_size;
            extfs_u64 zero_end = new_size < old_last_end ?
                new_size : old_last_end;
            if (zero_end > inode->size) {
                extfs_u64 last_physical;
                extfs_u64 zero_offset;
                extfs_u32 within =
                    (extfs_u32)(inode->size % volume->block_size);
                extfs_u32 count = (extfs_u32)(zero_end - inode->size);
                int old_hole = 0;
                status = extfs_map_file_block(volume, inode, old_blocks - 1U,
                                              &last_physical, &old_hole,
                                              journal_scratch,
                                              volume->block_size);
                if (status != EXTFS_OK || old_hole != 0)
                    return EXTFS_ERR_CORRUPT;
                status = extfs_block_byte_offset(
                    volume, last_physical, within, &zero_offset);
                if (status == EXTFS_OK)
                    status = extfs_write_bytes(volume, zero_offset,
                                               journal_scratch, count);
                if (status != EXTFS_OK) return status;
            }
        }
        if (new_blocks > old_blocks) {
            for (i = 0U; i < data_changed; ++i) {
                status = extfs_write_block(volume, allocation_start + i,
                                           journal_scratch);
                if (status != EXTFS_OK) return status;
            }
        }
        status = extfs_flush(volume);
        if (status != EXTFS_OK) return status;
    }

    /* Build and authenticate the complete inode-table block image. */
    status = extfs_read_block(volume, inode_home, inode_image);
    if (status != EXTFS_OK) return status;
    {
        extfs_u8 *raw = inode_image + inode_within;
        extfs_u64 raw_size = (extfs_u64)extfs_le32(raw + 0x04U) |
                             ((extfs_u64)extfs_le32(raw + 0x6CU) << 32);
        extfs_u64 sectors_per_block = volume->block_size / 512U;
        extfs_u64 old_allocated_blocks = old_blocks +
                                         (old_depth == 1U ? 1U : 0U);
        extfs_u64 new_allocated_blocks = new_blocks +
                                         (new_depth == 1U ? 1U : 0U);
        extfs_u64 sectors = old_allocated_blocks * sectors_per_block;
        extfs_u64 new_sectors = new_allocated_blocks * sectors_per_block;
        extfs_u8 *root = raw + 0x28U;

        if (extfs_validate_inode_checksum(volume, inode->number, raw) !=
                EXTFS_OK ||
            extfs_le16(raw + 0x00U) != inode->mode ||
            extfs_le32(raw + 0x20U) != inode->flags ||
            extfs_le32(raw + 0x64U) != inode->generation ||
            extfs_le32(raw + 0x68U) != 0U || /* external xattr block */
            extfs_le16(raw + 0x76U) != 0U || /* high external-xattr bits */
            raw_size != inode->size ||
            extfs_le16(raw + 0x74U) != 0U ||
            sectors > 0xFFFFFFFFULL ||
            extfs_le32(raw + 0x1CU) != (extfs_u32)sectors ||
            new_sectors > 0xFFFFFFFFULL ||
            !extfs_bytes_equal((const char *)root,
                               (const char *)inode->block_map, 60U))
            return EXTFS_ERR_CORRUPT;

        extfs_store_le32(raw + 0x04U, (extfs_u32)new_size);
        extfs_store_le32(raw + 0x6CU, (extfs_u32)(new_size >> 32));
        extfs_store_le32(raw + 0x1CU, (extfs_u32)new_sectors);
        if (new_depth == 0U)
            extfs_ext4_store_inline_extent_root(root, extents,
                                                new_extent_count);
        else
            extfs_ext4_store_depth1_root(root, new_leaf_block);
        status = extfs_store_inode_checksum(volume, inode->number, raw);
        if (status != EXTFS_OK) return status;
    }

    /* Prepare an external leaf image whenever the resulting tree still has
     * depth one. Conversion allocates a new leaf; existing trees update the
     * already authenticated leaf block in place. */
    if (new_depth == 1U) {
        extfs_zero(leaf_image, volume->block_size);
        extfs_ext4_store_extent_records(
            leaf_image, leaf_capacity, extents, new_extent_count);
        status = extfs_ext4_store_extent_block_checksum(
            volume, inode, leaf_image);
        if (status != EXTFS_OK) return status;
    }

    if (changes_allocation != 0) {
        extfs_u16 descriptor_free;

        status = extfs_group_descriptor_byte_offset(volume, touched_group,
                                                     &descriptor_offset);
        if (status != EXTFS_OK) return status;
        gdt_home = descriptor_offset / volume->block_size;
        descriptor_within =
            (extfs_u32)(descriptor_offset % volume->block_size);
        if (gdt_home >= volume->total_blocks ||
            descriptor_within > volume->block_size - volume->descriptor_size)
            return EXTFS_ERR_CORRUPT;
        if (gdt_home == bitmap_block || inode_home == bitmap_block ||
            inode_home == gdt_home ||
            (new_depth == 1U && new_leaf_block == gdt_home) ||
            (new_depth == 1U && new_leaf_block == inode_home) ||
            (new_depth == 1U && new_leaf_block == bitmap_block))
            return EXTFS_ERR_CORRUPT;

        status = extfs_read_block(volume, gdt_home, gdt_image);
        if (status != EXTFS_OK) return status;
        if (!extfs_bytes_equal((const char *)(gdt_image + descriptor_within),
                               (const char *)descriptor,
                               volume->descriptor_size))
            return EXTFS_ERR_CORRUPT;

        descriptor_free = extfs_le16(descriptor + 0x0CU);
        if (new_blocks > old_blocks) {
            if (descriptor_free < allocation_changed)
                return EXTFS_ERR_CORRUPT;
            for (i = 0U; i < allocation_changed; ++i) {
                extfs_u64 block = allocation_run_start + i;
                extfs_u32 group;
                extfs_u32 bit;
                status = extfs_ext2_group_for_block(
                    volume, block, &group, &bit);
                if (status != EXTFS_OK || group != touched_group ||
                    extfs_bitmap_test(bitmap_image, bit))
                    return EXTFS_ERR_CORRUPT;
                extfs_bitmap_set(bitmap_image, bit);
            }
            extfs_store_le16(descriptor + 0x0CU,
                             (extfs_u16)(descriptor_free -
                                         allocation_changed));
        } else {
            if ((extfs_u32)descriptor_free + allocation_changed >
                volume->blocks_per_group)
                return EXTFS_ERR_CORRUPT;
            for (i = new_blocks; i < old_blocks; ++i) {
                extfs_u64 block;
                extfs_u32 group;
                extfs_u32 bit;
                status = extfs_ext4_extent_map(
                    extents, extent_count, i, &block);
                /* extents was trimmed in-place. Use the on-disk mapping if
                 * the released logical block is no longer represented. */
                if (status != EXTFS_OK) {
                    int hole = 0;
                    status = extfs_map_file_block(
                        volume, inode, i, &block, &hole, journal_scratch,
                        volume->block_size);
                    if (status != EXTFS_OK || hole != 0)
                        return EXTFS_ERR_CORRUPT;
                }
                status = extfs_ext2_group_for_block(
                    volume, block, &group, &bit);
                if (status != EXTFS_OK || group != touched_group ||
                    !extfs_bitmap_test(bitmap_image, bit))
                    return EXTFS_ERR_CORRUPT;
                extfs_bitmap_clear(bitmap_image, bit);
            }
            if (tree_freed != 0U) {
                extfs_u32 group;
                extfs_u32 bit;
                status = extfs_ext2_group_for_block(
                    volume, old_leaf_block, &group, &bit);
                if (status != EXTFS_OK || group != touched_group ||
                    !extfs_bitmap_test(bitmap_image, bit))
                    return EXTFS_ERR_CORRUPT;
                extfs_bitmap_clear(bitmap_image, bit);
            }
            extfs_store_le16(descriptor + 0x0CU,
                             (extfs_u16)(descriptor_free +
                                         allocation_changed));
        }
        status = extfs_ext4_store_bitmap_group_checksums(
            volume, touched_group, bitmap_image, descriptor);
        if (status != EXTFS_OK) return status;
        extfs_copy(gdt_image + descriptor_within, descriptor,
                   volume->descriptor_size);

        status = extfs_primary_superblock_block_location(
            volume, &super_home, &super_within);
        if (status != EXTFS_OK) return status;
        if (super_home == bitmap_block || super_home == gdt_home ||
            super_home == inode_home ||
            (new_depth == 1U && super_home == new_leaf_block))
            return EXTFS_ERR_CORRUPT;
        status = extfs_read_block(volume, super_home, super_image);
        if (status != EXTFS_OK) return status;
        {
            extfs_u8 *sb = super_image + super_within;
            if (extfs_validate_superblock_checksum(sb) != EXTFS_OK ||
                extfs_le16(sb + 0x38U) != 0xEF53U ||
                !extfs_bytes_equal((const char *)(sb + 0x68U),
                                   (const char *)volume->uuid, 16U) ||
                extfs_le32(sb + 0x0CU) != (extfs_u32)volume->free_blocks ||
                extfs_le32(sb + 0x158U) != 0U ||
                extfs_le32(sb + 0x60U) != volume->feature_incompat ||
                new_free_blocks > 0xFFFFFFFFULL)
                return EXTFS_ERR_CORRUPT;
            extfs_store_le32(sb + 0x0CU, (extfs_u32)new_free_blocks);
            extfs_store_le32(sb + 0x60U,
                             volume->feature_incompat |
                             EXTFS_INCOMPAT_RECOVER);
            extfs_ext4_store_superblock_checksum(sb);
        }

        items[item_count].home_block = bitmap_block;
        items[item_count++].block_data = bitmap_image;
        items[item_count].home_block = gdt_home;
        items[item_count++].block_data = gdt_image;
    }

    items[item_count].home_block = inode_home;
    items[item_count++].block_data = inode_image;
    if (new_depth == 1U) {
        items[item_count].home_block = new_leaf_block;
        items[item_count++].block_data = leaf_image;
    }
    if (changes_allocation != 0) {
        items[item_count].home_block = super_home;
        items[item_count++].block_data = super_image;
    }

    status = extfs_journal_commit_metadata(volume, &journal, items,
                                           item_count, journal_scratch,
                                           volume->block_size);
    if (status != EXTFS_OK) return status;

    if (new_depth == 0U)
        extfs_ext4_store_inline_extent_root(inode->block_map, extents,
                                            new_extent_count);
    else
        extfs_ext4_store_depth1_root(inode->block_map, new_leaf_block);
    inode->size = new_size;
    volume->free_blocks = new_free_blocks;
    return EXTFS_OK;
}


/*
 * Modern ext4 directory blocks end in a fake dirent carrying the CRC32C.  Hash
 * index root/node metadata may precede ordinary entries, but the checksum still
 * covers the complete block with the stored checksum field treated as zero.
 */
static extfs_status extfs_validate_directory_block(const extfs_volume *volume,
                                                   const extfs_inode *directory,
                                                   extfs_u32 logical_block,
                                                   const extfs_u8 *block,
                                                   int *skip_index_block)
{
    const extfs_u8 *tail;
    extfs_u8 number_le[4];
    extfs_u8 generation_le[4];
    extfs_u32 crc;
    *skip_index_block = 0;
    if (volume->metadata_checksums == 0U || volume->block_size < 12U) {
        return EXTFS_OK;
    }
    /*
     * ExtFS enumerates indexed directories by walking their file blocks, not
     * by trusting the hash tree.  Root and internal HTree blocks therefore
     * carry no names we need.  Skipping them also keeps unverified index
     * metadata outside the lookup trust boundary.
     */
    if (((directory->flags & EXTFS_INODE_FLAG_INDEX) != 0U &&
         logical_block == 0U) ||
        (extfs_le32(block) == 0U &&
         extfs_le16(block + 4U) == volume->block_size)) {
        *skip_index_block = 1;
        return EXTFS_OK;
    }
    tail = block + volume->block_size - 12U;
    if (extfs_le32(tail) != 0U || extfs_le16(tail + 4U) != 12U ||
        tail[6] != 0U || tail[7] != 0xDEU) {
        return EXTFS_ERR_CHECKSUM;
    }
    extfs_store_le32(number_le, directory->number);
    extfs_store_le32(generation_le, directory->generation);
    crc = extfs_crc32c(volume->checksum_seed, number_le, 4U);
    crc = extfs_crc32c(crc, generation_le, 4U);
    crc = extfs_crc32c(crc, block, volume->block_size - 12U);
    return crc == extfs_le32(tail + 8U)
        ? EXTFS_OK : EXTFS_ERR_CHECKSUM;
}

static extfs_node_type extfs_dirent_type(extfs_u8 type)
{
    switch (type) {
        case 1U: return EXTFS_NODE_REGULAR;
        case 2U: return EXTFS_NODE_DIRECTORY;
        case 3U: return EXTFS_NODE_CHARACTER;
        case 4U: return EXTFS_NODE_BLOCK;
        case 5U: return EXTFS_NODE_FIFO;
        case 6U: return EXTFS_NODE_SOCKET;
        case 7U: return EXTFS_NODE_SYMLINK;
        default: return EXTFS_NODE_UNKNOWN;
    }
}

/*
 * Directory records are variable length and may share a block with HTree index
 * metadata.  Validate each record boundary before exposing a name and skip the
 * reserved checksum/index structures rather than presenting them as files.
 */
extfs_status extfs_iterate_directory(const extfs_volume *volume,
                                     const extfs_inode *directory,
                                     extfs_directory_callback callback,
                                     void *callback_user,
                                     void *scratch,
                                     extfs_u32 scratch_size)
{
    extfs_u64 logical_count;
    extfs_u64 logical;
    extfs_u8 *block = (extfs_u8 *)scratch;
    if (volume == 0 || directory == 0 || callback == 0 || scratch == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    if (extfs_inode_type(directory) != EXTFS_NODE_DIRECTORY) {
        return EXTFS_ERR_NOT_DIRECTORY;
    }
    if (scratch_size < volume->block_size) {
        return EXTFS_ERR_BUFFER_TOO_SMALL;
    }
    logical_count = extfs_div_round_up_u64(directory->size,
                                           volume->block_size);
    if (logical_count > 0x100000000ULL) {
        return EXTFS_ERR_RANGE;
    }
    for (logical = 0U; logical < logical_count; ++logical) {
        extfs_u64 physical;
        int hole;
        int skip_index_block;
        extfs_u32 position = 0U;
        extfs_status status = extfs_map_file_block(
            volume, directory, (extfs_u32)logical, &physical, &hole,
            scratch, scratch_size);
        if (status != EXTFS_OK) {
            return status;
        }
        if (hole != 0) {
            continue;
        }
        {
            extfs_u64 physical_offset;
            status = extfs_block_byte_offset(volume, physical, 0U,
                                             &physical_offset);
            if (status != EXTFS_OK) {
                return status;
            }
            status = extfs_read_bytes(volume, physical_offset, block,
                                      volume->block_size);
        }
        if (status != EXTFS_OK) {
            return status;
        }
        status = extfs_validate_directory_block(
            volume, directory, (extfs_u32)logical, block, &skip_index_block);
        if (status != EXTFS_OK) {
            return status;
        }
        if (skip_index_block != 0) {
            continue;
        }
        while (position + 8U <= volume->block_size) {
            const extfs_u8 *entry = block + position;
            extfs_u32 inode_number = extfs_le32(entry);
            extfs_u16 record_length = extfs_le16(entry + 4U);
            extfs_u8 name_length = entry[6];
            extfs_u8 file_type = (volume->feature_incompat &
                                  EXTFS_INCOMPAT_FILETYPE) != 0U
                ? entry[7] : 0U;
            if (record_length < 8U || (record_length & 3U) != 0U ||
                position + record_length > volume->block_size ||
                name_length > record_length - 8U) {
                return EXTFS_ERR_CORRUPT;
            }
            if (inode_number > volume->total_inodes) {
                return EXTFS_ERR_CORRUPT;
            }
            if (inode_number != 0U && name_length != 0U) {
                int callback_status = callback(
                    callback_user, inode_number, extfs_dirent_type(file_type),
                    (const char *)(entry + 8U), name_length);
                if (callback_status != 0) {
                    return EXTFS_STOP;
                }
            }
            position += record_length;
            if (position == volume->block_size) {
                break;
            }
        }
        if (position != volume->block_size) {
            return EXTFS_ERR_CORRUPT;
        }
    }
    return EXTFS_OK;
}

typedef struct extfs_lookup_context {
    const char *name;
    extfs_u8 length;
    extfs_u32 inode_number;
    int found;
} extfs_lookup_context;

static int extfs_lookup_callback(void *user,
                                 extfs_u32 inode_number,
                                 extfs_node_type type,
                                 const char *name,
                                 extfs_u8 name_length)
{
    extfs_lookup_context *context = (extfs_lookup_context *)user;
    (void)type;
    if (name_length == context->length &&
        extfs_bytes_equal(name, context->name, name_length) != 0) {
        context->inode_number = inode_number;
        context->found = 1;
        return 1;
    }
    return 0;
}

extfs_status extfs_lookup(const extfs_volume *volume,
                          const extfs_inode *directory,
                          const char *name,
                          extfs_u8 name_length,
                          extfs_u32 *inode_number,
                          void *scratch,
                          extfs_u32 scratch_size)
{
    extfs_lookup_context context;
    extfs_status status;
    if (name == 0 || inode_number == 0 || name_length == 0U) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    context.name = name;
    context.length = name_length;
    context.inode_number = 0U;
    context.found = 0;
    status = extfs_iterate_directory(volume, directory, extfs_lookup_callback,
                                     &context, scratch, scratch_size);
    if (status != EXTFS_OK && status != EXTFS_STOP) {
        return status;
    }
    if (context.found == 0) {
        return EXTFS_ERR_NOT_FOUND;
    }
    *inode_number = context.inode_number;
    return EXTFS_OK;
}

/* Resolve root-relative UTF-8 paths component-by-component from inode 2. */
extfs_status extfs_resolve_path(const extfs_volume *volume,
                                const char *path,
                                extfs_inode *inode,
                                void *scratch,
                                extfs_u32 scratch_size)
{
    const char *cursor;
    extfs_status status;
    if (volume == 0 || path == 0 || inode == 0 || scratch == 0) {
        return EXTFS_ERR_INVALID_ARGUMENT;
    }
    status = extfs_read_inode(volume, EXTFS_ROOT_INODE, inode,
                              scratch, scratch_size);
    if (status != EXTFS_OK) {
        return status;
    }
    cursor = path;
    while (*cursor == '/') {
        ++cursor;
    }
    while (*cursor != '\0') {
        const char *start = cursor;
        extfs_u32 length = 0U;
        extfs_u32 next_inode;
        while (*cursor != '\0' && *cursor != '/') {
            ++cursor;
            ++length;
            if (length > EXTFS_MAX_NAME_LENGTH) {
                return EXTFS_ERR_RANGE;
            }
        }
        if (length != 0U) {
            status = extfs_lookup(volume, inode, start, (extfs_u8)length,
                                  &next_inode, scratch, scratch_size);
            if (status != EXTFS_OK) {
                return status;
            }
            status = extfs_read_inode(volume, next_inode, inode,
                                      scratch, scratch_size);
            if (status != EXTFS_OK) {
                return status;
            }
        }
        while (*cursor == '/') {
            ++cursor;
        }
        if (*cursor != '\0' &&
            extfs_inode_type(inode) != EXTFS_NODE_DIRECTORY) {
            return EXTFS_ERR_NOT_DIRECTORY;
        }
    }
    return EXTFS_OK;
}

extfs_node_type extfs_inode_type(const extfs_inode *inode)
{
    if (inode == 0) {
        return EXTFS_NODE_UNKNOWN;
    }
    switch (inode->mode & EXTFS_MODE_TYPE_MASK) {
        case EXTFS_MODE_REGULAR:   return EXTFS_NODE_REGULAR;
        case EXTFS_MODE_DIRECTORY: return EXTFS_NODE_DIRECTORY;
        case EXTFS_MODE_SYMLINK:   return EXTFS_NODE_SYMLINK;
        case EXTFS_MODE_CHARACTER: return EXTFS_NODE_CHARACTER;
        case EXTFS_MODE_BLOCK:     return EXTFS_NODE_BLOCK;
        case EXTFS_MODE_FIFO:      return EXTFS_NODE_FIFO;
        case EXTFS_MODE_SOCKET:    return EXTFS_NODE_SOCKET;
        default:                   return EXTFS_NODE_UNKNOWN;
    }
}

const char *extfs_status_string(extfs_status status)
{
    switch (status) {
        case EXTFS_OK:                   return "success";
        case EXTFS_STOP:                 return "iteration stopped";
        case EXTFS_ERR_INVALID_ARGUMENT: return "invalid argument";
        case EXTFS_ERR_IO:               return "input/output error";
        case EXTFS_ERR_NOT_EXT:          return "not an ext filesystem";
        case EXTFS_ERR_CORRUPT:          return "corrupt filesystem metadata";
        case EXTFS_ERR_UNSUPPORTED:      return "unsupported filesystem feature";
        case EXTFS_ERR_CHECKSUM:         return "metadata checksum mismatch";
        case EXTFS_ERR_BUFFER_TOO_SMALL: return "scratch buffer too small";
        case EXTFS_ERR_RANGE:            return "value outside supported range";
        case EXTFS_ERR_NOT_FOUND:        return "path not found";
        case EXTFS_ERR_NOT_DIRECTORY:    return "path component is not a directory";
        case EXTFS_ERR_IS_DIRECTORY:     return "requested object is a directory";
        case EXTFS_ERR_NO_SPACE:        return "no space left on device";
        default:                         return "unknown error";
    }
}

const char *extfs_kind_string(extfs_kind kind)
{
    switch (kind) {
        case EXTFS_KIND_EXT2: return "ext2";
        case EXTFS_KIND_EXT3: return "ext3";
        case EXTFS_KIND_EXT4: return "ext4";
        default:              return "unknown";
    }
}
