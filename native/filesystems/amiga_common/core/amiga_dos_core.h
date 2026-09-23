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

#endif
