/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef INFILTRATOR_FFS_PRIMITIVES_H
#define INFILTRATOR_FFS_PRIMITIVES_H

#if defined(__KERNEL__)
#include <linux/types.h>
typedef u8 ifs_ffs_u8;
typedef u32 ifs_ffs_u32;
#else
#include <stdint.h>
typedef uint8_t ifs_ffs_u8;
typedef uint32_t ifs_ffs_u32;
#endif

int ifs_ffs_data_block_valid(
    ifs_ffs_u32 block,
    ifs_ffs_u32 reserved_blocks,
    ifs_ffs_u32 partition_blocks);

int ifs_ffs_bitmap_geometry(
    ifs_ffs_u32 block_size,
    ifs_ffs_u32 reserved_blocks,
    ifs_ffs_u32 partition_blocks,
    ifs_ffs_u32 *bits_per_bitmap,
    ifs_ffs_u32 *bitmap_count);

ifs_ffs_u32 ifs_ffs_bitmap_bit_mask(ifs_ffs_u32 bit_offset);
ifs_ffs_u32 ifs_ffs_bitmap_scan_mask(ifs_ffs_u32 bit_offset);

int ifs_ffs_bitmap_location(
    ifs_ffs_u32 block,
    ifs_ffs_u32 reserved_blocks,
    ifs_ffs_u32 partition_blocks,
    ifs_ffs_u32 bits_per_bitmap,
    ifs_ffs_u32 *bitmap_index,
    ifs_ffs_u32 *bit_index);

ifs_ffs_u32 ifs_ffs_bitmap_valid_word_mask(ifs_ffs_u32 valid_bits);

int ifs_ffs_bitmap_select_free_run(
    ifs_ffs_u32 word,
    ifs_ffs_u32 start_bit,
    ifs_ffs_u32 valid_bits,
    ifs_ffs_u32 *first_bit,
    ifs_ffs_u32 *run_mask,
    ifs_ffs_u32 *run_length);

int ifs_ffs_file_block_location(
    ifs_ffs_u32 logical_block,
    ifs_ffs_u32 entries_per_extension,
    ifs_ffs_u32 *extension_index,
    ifs_ffs_u32 *entry_index);

int ifs_ffs_file_block_count(
    ifs_ffs_u32 file_size,
    ifs_ffs_u32 data_bytes_per_block,
    ifs_ffs_u32 *block_count);

int ifs_ffs_file_extension_count(
    ifs_ffs_u32 block_count,
    ifs_ffs_u32 entries_per_extension,
    ifs_ffs_u32 *extension_count);

#define IFS_FFS_DOS_NAME_MAX 30U

typedef enum IfsFfsNameStatus {
    IFS_FFS_NAME_OK = 0,
    IFS_FFS_NAME_TOO_LONG,
    IFS_FFS_NAME_INVALID_CHARACTER
} IfsFfsNameStatus;

IfsFfsNameStatus ifs_ffs_validate_name(
    const ifs_ffs_u8 *name,
    ifs_ffs_u32 length,
    int no_truncate);

ifs_ffs_u8 ifs_ffs_fold_character(
    ifs_ffs_u8 character,
    int international_mode);

ifs_ffs_u32 ifs_ffs_directory_hash(
    const ifs_ffs_u8 *name,
    ifs_ffs_u32 length,
    ifs_ffs_u32 hash_table_size,
    int international_mode);

ifs_ffs_u32 ifs_ffs_block_checksum(
    const ifs_ffs_u8 *block,
    ifs_ffs_u32 block_size);

ifs_ffs_u32 ifs_ffs_checksum_word_value(
    const ifs_ffs_u8 *block,
    ifs_ffs_u32 block_size,
    ifs_ffs_u32 checksum_word_index);

typedef enum IfsFfsSymlinkStatus {
    IFS_FFS_SYMLINK_OK = 0,
    IFS_FFS_SYMLINK_INVALID_ARGUMENT,
    IFS_FFS_SYMLINK_SOURCE_UNTERMINATED,
    IFS_FFS_SYMLINK_OUTPUT_TOO_SMALL
} IfsFfsSymlinkStatus;

IfsFfsSymlinkStatus ifs_ffs_translate_symlink(
    const ifs_ffs_u8 *source,
    ifs_ffs_u32 source_capacity,
    const ifs_ffs_u8 *volume_prefix,
    ifs_ffs_u32 volume_prefix_length,
    ifs_ffs_u8 *output,
    ifs_ffs_u32 output_capacity,
    ifs_ffs_u32 *output_length);

IfsFfsSymlinkStatus ifs_ffs_encode_symlink(
    const ifs_ffs_u8 *source,
    ifs_ffs_u32 source_capacity,
    const ifs_ffs_u8 *volume_name,
    ifs_ffs_u32 volume_name_length,
    ifs_ffs_u8 *output,
    ifs_ffs_u32 output_capacity,
    ifs_ffs_u32 *output_length);

#endif
