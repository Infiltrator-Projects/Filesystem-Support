// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratr/fs/io.h"

bool ifs_io_is_valid(const IfsIo *io)
{
    if (io == NULL || io->read_at == NULL ||
        (io->flags & IFS_IO_READABLE) == 0U)
        return false;

    if ((io->flags & IFS_IO_WRITABLE) != 0U && io->write_at == NULL)
        return false;

    return true;
}

bool ifs_io_range_valid(const IfsIo *io, const ifs_u64 offset,
                        const size_t length)
{
    if (!ifs_io_is_valid(io) || offset > io->size_bytes)
        return false;

    return (ifs_u64)length <= io->size_bytes - offset;
}

IfsStatus ifs_io_read_exact(const IfsIo *io, const ifs_u64 offset,
                            void *buffer, const size_t length)
{
    if (!ifs_io_is_valid(io) || (length != 0U && buffer == NULL))
        return IFS_ERROR_ARGUMENT;
    if (!ifs_io_range_valid(io, offset, length))
        return IFS_ERROR_RANGE;
    if (length == 0U)
        return IFS_OK;

    return io->read_at(io->context, offset, buffer, length);
}

IfsStatus ifs_io_write_exact(const IfsIo *io, const ifs_u64 offset,
                             const void *buffer, const size_t length)
{
    if (!ifs_io_is_valid(io) || (length != 0U && buffer == NULL))
        return IFS_ERROR_ARGUMENT;
    if ((io->flags & IFS_IO_WRITABLE) == 0U || io->write_at == NULL)
        return IFS_ERROR_READ_ONLY;
    if (!ifs_io_range_valid(io, offset, length))
        return IFS_ERROR_RANGE;
    if (length == 0U)
        return IFS_OK;

    return io->write_at(io->context, offset, buffer, length);
}
