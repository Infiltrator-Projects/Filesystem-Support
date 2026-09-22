// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATR_FS_BYTE_READER_H
#define INFILTRATR_FS_BYTE_READER_H

#include "status.h"
#include "types.h"

#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#endif

typedef struct {
    const ifs_u8 *bytes;
    size_t length;
    size_t cursor;
} IfsByteReader;

void ifs_byte_reader_init(
    IfsByteReader *reader, const void *bytes, size_t length);
size_t ifs_byte_reader_remaining(const IfsByteReader *reader);
IfsStatus ifs_byte_reader_skip(IfsByteReader *reader, size_t length);
IfsStatus ifs_byte_reader_take(
    IfsByteReader *reader, size_t length, const ifs_u8 **bytes);
IfsStatus ifs_byte_reader_u8(IfsByteReader *reader, ifs_u8 *value);
IfsStatus ifs_byte_reader_le16(IfsByteReader *reader, ifs_u16 *value);
IfsStatus ifs_byte_reader_le32(IfsByteReader *reader, ifs_u32 *value);
IfsStatus ifs_byte_reader_le64(IfsByteReader *reader, ifs_u64 *value);
IfsStatus ifs_byte_reader_be16(IfsByteReader *reader, ifs_u16 *value);
IfsStatus ifs_byte_reader_be32(IfsByteReader *reader, ifs_u32 *value);
IfsStatus ifs_byte_reader_be64(IfsByteReader *reader, ifs_u64 *value);

#endif
