/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef INFILTRATOR_AMIGA_DOS_CORE_H
#define INFILTRATOR_AMIGA_DOS_CORE_H

#if defined(__KERNEL__)
#include <linux/types.h>
typedef u8 ifs_amiga_u8;
typedef u32 ifs_amiga_u32;
#else
#include <stdint.h>
typedef uint8_t ifs_amiga_u8;
typedef uint32_t ifs_amiga_u32;
#endif

int ifs_amiga_data_block_valid(
    ifs_amiga_u32 block,
    ifs_amiga_u32 reserved_blocks,
    ifs_amiga_u32 partition_blocks);

int ifs_amiga_bitmap_geometry(
    ifs_amiga_u32 block_size,
    ifs_amiga_u32 reserved_blocks,
    ifs_amiga_u32 partition_blocks,
    ifs_amiga_u32 *bits_per_bitmap,
    ifs_amiga_u32 *bitmap_count);

ifs_amiga_u32 ifs_amiga_bitmap_bit_mask(ifs_amiga_u32 bit_offset);
ifs_amiga_u32 ifs_amiga_bitmap_scan_mask(ifs_amiga_u32 bit_offset);

int ifs_amiga_bitmap_location(
    ifs_amiga_u32 block,
    ifs_amiga_u32 reserved_blocks,
    ifs_amiga_u32 partition_blocks,
    ifs_amiga_u32 bits_per_bitmap,
    ifs_amiga_u32 *bitmap_index,
    ifs_amiga_u32 *bit_index);

ifs_amiga_u32 ifs_amiga_bitmap_valid_word_mask(ifs_amiga_u32 valid_bits);

int ifs_amiga_bitmap_select_free_run(
    ifs_amiga_u32 word,
    ifs_amiga_u32 start_bit,
    ifs_amiga_u32 valid_bits,
    ifs_amiga_u32 *first_bit,
    ifs_amiga_u32 *run_mask,
    ifs_amiga_u32 *run_length);

#define IFS_AMIGA_DOS_NAME_MAX 30U

typedef enum IfsAmigaNameStatus {
    IFS_AMIGA_NAME_OK = 0,
    IFS_AMIGA_NAME_TOO_LONG,
    IFS_AMIGA_NAME_INVALID_CHARACTER
} IfsAmigaNameStatus;

IfsAmigaNameStatus ifs_amiga_validate_name(
    const ifs_amiga_u8 *name,
    ifs_amiga_u32 length,
    int no_truncate);

ifs_amiga_u8 ifs_amiga_fold_character(
    ifs_amiga_u8 character,
    int international_mode);

ifs_amiga_u32 ifs_amiga_directory_hash(
    const ifs_amiga_u8 *name,
    ifs_amiga_u32 length,
    ifs_amiga_u32 hash_table_size,
    int international_mode);

ifs_amiga_u32 ifs_amiga_block_checksum(
    const ifs_amiga_u8 *block,
    ifs_amiga_u32 block_size);

ifs_amiga_u32 ifs_amiga_checksum_word_value(
    const ifs_amiga_u8 *block,
    ifs_amiga_u32 block_size,
    ifs_amiga_u32 checksum_word_index);

typedef enum IfsAmigaSymlinkStatus {
    IFS_AMIGA_SYMLINK_OK = 0,
    IFS_AMIGA_SYMLINK_INVALID_ARGUMENT,
    IFS_AMIGA_SYMLINK_SOURCE_UNTERMINATED,
    IFS_AMIGA_SYMLINK_OUTPUT_TOO_SMALL
} IfsAmigaSymlinkStatus;

IfsAmigaSymlinkStatus ifs_amiga_translate_symlink(
    const ifs_amiga_u8 *source,
    ifs_amiga_u32 source_capacity,
    const ifs_amiga_u8 *volume_prefix,
    ifs_amiga_u32 volume_prefix_length,
    ifs_amiga_u8 *output,
    ifs_amiga_u32 output_capacity,
    ifs_amiga_u32 *output_length);

#endif
