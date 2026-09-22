// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratr/fs/byte_reader.h"
#include "infiltratr/fs/endian.h"

void ifs_byte_reader_init(IfsByteReader *reader,
                          const void *bytes,
                          const size_t length)
{
    if (reader == NULL)
        return;

    reader->bytes = (const ifs_u8 *)bytes;
    reader->length = bytes == NULL ? 0U : length;
    reader->cursor = 0U;
}

size_t ifs_byte_reader_remaining(const IfsByteReader *reader)
{
    if (reader == NULL || reader->cursor > reader->length)
        return 0U;
    return reader->length - reader->cursor;
}

IfsStatus ifs_byte_reader_skip(IfsByteReader *reader, const size_t length)
{
    if (reader == NULL)
        return IFS_ERROR_ARGUMENT;
    if (length > ifs_byte_reader_remaining(reader))
        return IFS_ERROR_RANGE;

    reader->cursor += length;
    return IFS_OK;
}

IfsStatus ifs_byte_reader_take(IfsByteReader *reader,
                               const size_t length,
                               const ifs_u8 **bytes)
{
    if (reader == NULL || bytes == NULL)
        return IFS_ERROR_ARGUMENT;
    if (length > ifs_byte_reader_remaining(reader))
        return IFS_ERROR_RANGE;

    *bytes = reader->bytes + reader->cursor;
    reader->cursor += length;
    return IFS_OK;
}

IfsStatus ifs_byte_reader_u8(IfsByteReader *reader, ifs_u8 *value)
{
    const ifs_u8 *bytes = NULL;
    IfsStatus status;

    if (value == NULL)
        return IFS_ERROR_ARGUMENT;

    status = ifs_byte_reader_take(reader, 1U, &bytes);
    if (status != IFS_OK)
        return status;

    *value = bytes[0];
    return IFS_OK;
}

#define IFS_READER_INTEGER(name, type, width, loader)                    \
    IfsStatus name(IfsByteReader *reader, type *value)                   \
    {                                                                    \
        const ifs_u8 *bytes = NULL;                                      \
        IfsStatus status;                                                \
        if (value == NULL)                                               \
            return IFS_ERROR_ARGUMENT;                                   \
        status = ifs_byte_reader_take(reader, width, &bytes);            \
        if (status != IFS_OK)                                            \
            return status;                                               \
        *value = loader(bytes);                                          \
        return IFS_OK;                                                   \
    }

IFS_READER_INTEGER(ifs_byte_reader_le16, ifs_u16, 2U, ifs_load_le16)
IFS_READER_INTEGER(ifs_byte_reader_le32, ifs_u32, 4U, ifs_load_le32)
IFS_READER_INTEGER(ifs_byte_reader_le64, ifs_u64, 8U, ifs_load_le64)
IFS_READER_INTEGER(ifs_byte_reader_be16, ifs_u16, 2U, ifs_load_be16)
IFS_READER_INTEGER(ifs_byte_reader_be32, ifs_u32, 4U, ifs_load_be32)
IFS_READER_INTEGER(ifs_byte_reader_be64, ifs_u64, 8U, ifs_load_be64)

#undef IFS_READER_INTEGER
