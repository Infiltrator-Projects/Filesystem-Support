// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATR_FS_IO_H
#define INFILTRATR_FS_IO_H

#include "status.h"
#include "types.h"

#ifdef __KERNEL__
#include <linux/stddef.h>
#else
#include <stddef.h>
#include <stdbool.h>
#endif

enum {
    IFS_IO_READABLE = 1U << 0,
    IFS_IO_WRITABLE = 1U << 1
};

typedef IfsStatus (*IfsReadAtFn)(
    void *context, ifs_u64 offset, void *buffer, size_t length);
typedef IfsStatus (*IfsWriteAtFn)(
    void *context, ifs_u64 offset, const void *buffer, size_t length);

typedef struct {
    void *context;
    ifs_u64 size_bytes;
    unsigned int flags;
    IfsReadAtFn read_at;
    IfsWriteAtFn write_at;
} IfsIo;

bool ifs_io_is_valid(const IfsIo *io);
bool ifs_io_range_valid(const IfsIo *io, ifs_u64 offset, size_t length);
IfsStatus ifs_io_read_exact(
    const IfsIo *io, ifs_u64 offset, void *buffer, size_t length);
IfsStatus ifs_io_write_exact(
    const IfsIo *io, ifs_u64 offset, const void *buffer, size_t length);

#endif
