// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratr/fs/byte_reader.h"
#include "infiltratr/fs/filesystem.h"
#include "infiltratr/fs/io.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    const unsigned char *bytes;
    size_t length;
} MemoryIo;

static IfsStatus memory_read(void *context,
                             const ifs_u64 offset,
                             void *buffer,
                             const size_t length)
{
    const MemoryIo *memory = context;

    if (memory == NULL || buffer == NULL ||
        offset > memory->length ||
        (ifs_u64)length > (ifs_u64)memory->length - offset)
        return IFS_ERROR_RANGE;

    memcpy(buffer, memory->bytes + (size_t)offset, length);
    return IFS_OK;
}

static int fail(const char *message)
{
    fprintf(stderr, "native_core_test: %s\n", message);
    return 1;
}

int main(void)
{
    static const unsigned char bytes[] = {
        0x11U, 0x22U, 0x33U, 0x44U,
        0x55U, 0x66U, 0x77U, 0x88U
    };
    MemoryIo memory = {bytes, sizeof(bytes)};
    IfsIo io = {
        .context = &memory,
        .size_bytes = sizeof(bytes),
        .flags = IFS_IO_READABLE,
        .read_at = memory_read,
        .write_at = NULL
    };
    unsigned char copied[4] = {0};
    IfsByteReader reader;
    ifs_u16 be16 = 0U;
    ifs_u32 le32 = 0U;
    IfsFilesystemDescriptor descriptor = {
        .struct_size = sizeof(IfsFilesystemDescriptor),
        .abi_version = IFS_FILESYSTEM_DESCRIPTOR_ABI,
        .id = "test",
        .display_name = "Test",
        .linux_type_name = "infiltratr-test",
        .module_name = "infiltratr_test",
        .capabilities = IFS_CAPABILITY_READ,
        .probe = NULL
    };

    if (!ifs_io_is_valid(&io))
        return fail("valid memory I/O rejected");
    if (ifs_io_read_exact(&io, 2U, copied, sizeof(copied)) != IFS_OK ||
        memcmp(copied, bytes + 2U, sizeof(copied)) != 0)
        return fail("exact bounded read failed");
    if (ifs_io_read_exact(&io, 7U, copied, 2U) != IFS_ERROR_RANGE)
        return fail("out-of-range read was not rejected");
    if (ifs_io_write_exact(&io, 0U, copied, 1U) != IFS_ERROR_READ_ONLY)
        return fail("read-only I/O accepted a write");

    ifs_byte_reader_init(&reader, bytes, sizeof(bytes));
    if (ifs_byte_reader_be16(&reader, &be16) != IFS_OK ||
        be16 != 0x1122U)
        return fail("big-endian decode failed");
    if (ifs_byte_reader_le32(&reader, &le32) != IFS_OK ||
        le32 != 0x66554433U)
        return fail("little-endian decode failed");
    if (ifs_byte_reader_skip(&reader, 3U) != IFS_ERROR_RANGE)
        return fail("reader overrun was not rejected");

    if (!ifs_filesystem_descriptor_is_valid(&descriptor))
        return fail("valid descriptor rejected");
    descriptor.capabilities = IFS_CAPABILITY_PROBE;
    if (ifs_filesystem_descriptor_is_valid(&descriptor))
        return fail("probe capability accepted without probe function");

    return 0;
}
