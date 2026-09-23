/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "amiga_dos_core.h"

static ifs_amiga_u32 ifs_amiga_read_be32(const ifs_amiga_u8 *data)
{
    return ((ifs_amiga_u32)data[0] << 24) |
           ((ifs_amiga_u32)data[1] << 16) |
           ((ifs_amiga_u32)data[2] << 8) |
           (ifs_amiga_u32)data[3];
}

IfsAmigaNameStatus ifs_amiga_validate_name(
    const ifs_amiga_u8 *const name,
    const ifs_amiga_u32 length,
    const int no_truncate)
{
    ifs_amiga_u32 index;
    ifs_amiga_u32 checked_length = length;

    if (name == 0 && length != 0U)
        return IFS_AMIGA_NAME_INVALID_CHARACTER;

    if (checked_length > IFS_AMIGA_DOS_NAME_MAX) {
        if (no_truncate != 0)
            return IFS_AMIGA_NAME_TOO_LONG;
        checked_length = IFS_AMIGA_DOS_NAME_MAX;
    }

    for (index = 0U; index < checked_length; ++index) {
        const ifs_amiga_u8 ch = name[index];

        if (ch < 0x20U || ch == 0x3aU ||
            (ch > 0x7eU && ch < 0xa0U))
            return IFS_AMIGA_NAME_INVALID_CHARACTER;
    }

    return IFS_AMIGA_NAME_OK;
}

ifs_amiga_u8 ifs_amiga_fold_character(
    const ifs_amiga_u8 character,
    const int international_mode)
{
    if ((character >= (ifs_amiga_u8)'a' &&
         character <= (ifs_amiga_u8)'z') ||
        (international_mode != 0 &&
         character >= 0xe0U && character <= 0xfeU &&
         character != 0xf7U))
        return (ifs_amiga_u8)(character - 0x20U);

    return character;
}

ifs_amiga_u32 ifs_amiga_directory_hash(
    const ifs_amiga_u8 *const name,
    ifs_amiga_u32 length,
    const ifs_amiga_u32 hash_table_size,
    const int international_mode)
{
    ifs_amiga_u32 hash;
    ifs_amiga_u32 index;

    if (name == 0 || hash_table_size == 0U)
        return 0U;

    if (length > IFS_AMIGA_DOS_NAME_MAX)
        length = IFS_AMIGA_DOS_NAME_MAX;

    hash = length;
    for (index = 0U; index < length; ++index) {
        hash = (hash * 13U +
                ifs_amiga_fold_character(name[index], international_mode)) &
               0x7ffU;
    }

    return hash % hash_table_size;
}

ifs_amiga_u32 ifs_amiga_block_checksum(
    const ifs_amiga_u8 *const block,
    const ifs_amiga_u32 block_size)
{
    ifs_amiga_u32 sum = 0U;
    ifs_amiga_u32 offset;

    if (block == 0 || (block_size & 3U) != 0U)
        return 0U;

    for (offset = 0U; offset < block_size; offset += 4U)
        sum += ifs_amiga_read_be32(block + offset);

    return sum;
}

ifs_amiga_u32 ifs_amiga_checksum_word_value(
    const ifs_amiga_u8 *const block,
    const ifs_amiga_u32 block_size,
    const ifs_amiga_u32 checksum_word_index)
{
    ifs_amiga_u32 sum = 0U;
    ifs_amiga_u32 offset;
    const ifs_amiga_u32 checksum_offset = checksum_word_index * 4U;

    if (block == 0 || (block_size & 3U) != 0U ||
        checksum_offset > block_size - 4U)
        return 0U;

    for (offset = 0U; offset < block_size; offset += 4U) {
        if (offset != checksum_offset)
            sum += ifs_amiga_read_be32(block + offset);
    }

    return 0U - sum;
}

int ifs_amiga_data_block_valid(
    const ifs_amiga_u32 block,
    const ifs_amiga_u32 reserved_blocks,
    const ifs_amiga_u32 partition_blocks)
{
    return reserved_blocks < partition_blocks &&
           block >= reserved_blocks &&
           block < partition_blocks;
}

int ifs_amiga_bitmap_geometry(
    const ifs_amiga_u32 block_size,
    const ifs_amiga_u32 reserved_blocks,
    const ifs_amiga_u32 partition_blocks,
    ifs_amiga_u32 *const bits_per_bitmap,
    ifs_amiga_u32 *const bitmap_count)
{
    ifs_amiga_u32 bits;
    ifs_amiga_u32 data_blocks;

    if (bits_per_bitmap == 0 || bitmap_count == 0 ||
        block_size < 8U ||
        block_size > 0x1fffffffU ||
        reserved_blocks >= partition_blocks)
        return -1;

    bits = block_size * 8U - 32U;
    if (bits == 0U)
        return -1;

    data_blocks = partition_blocks - reserved_blocks;
    *bits_per_bitmap = bits;
    *bitmap_count = data_blocks / bits +
        (data_blocks % bits != 0U ? 1U : 0U);
    return 0;
}

ifs_amiga_u32 ifs_amiga_bitmap_bit_mask(const ifs_amiga_u32 bit_offset)
{
    return 1U << (bit_offset & 31U);
}

ifs_amiga_u32 ifs_amiga_bitmap_scan_mask(const ifs_amiga_u32 bit_offset)
{
    return 0xffffffffU << (bit_offset & 31U);
}
