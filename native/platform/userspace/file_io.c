// SPDX-License-Identifier: GPL-3.0-or-later
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include "file_io.h"

#include <infiltratr/posix_io.h>

#include <fcntl.h>
#include <linux/fs.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

static IfsStatus file_read_at(void *context,
                              const ifs_u64 offset,
                              void *buffer,
                              const size_t length)
{
    const int descriptor = *(const int *)context;
    return infiltratr_pread_full(
               descriptor, buffer, length, (uint64_t)offset) == 0
        ? IFS_OK
        : IFS_ERROR_IO;
}

static IfsStatus descriptor_size(const int descriptor, ifs_u64 *size)
{
    struct stat status;

    if (size == NULL || fstat(descriptor, &status) != 0)
        return IFS_ERROR_IO;

    if (S_ISBLK(status.st_mode)) {
        uint64_t bytes = 0U;
        if (ioctl(descriptor, BLKGETSIZE64, &bytes) != 0)
            return IFS_ERROR_IO;
        *size = (ifs_u64)bytes;
        return IFS_OK;
    }

    if (!S_ISREG(status.st_mode) || status.st_size < 0)
        return IFS_ERROR_UNSUPPORTED;

    *size = (ifs_u64)status.st_size;
    return IFS_OK;
}

IfsStatus ifs_userspace_file_open_readonly(
    const char *path, IfsUserspaceFile *file)
{
    IfsStatus status;
    int descriptor;

    if (path == NULL || path[0] == '\0' || file == NULL)
        return IFS_ERROR_ARGUMENT;

    memset(file, 0, sizeof(*file));
    file->descriptor = -1;

    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor < 0)
        return IFS_ERROR_IO;

    file->descriptor = descriptor;
    status = descriptor_size(descriptor, &file->io.size_bytes);
    if (status != IFS_OK) {
        ifs_userspace_file_close(file);
        return status;
    }

    file->io.context = &file->descriptor;
    file->io.flags = IFS_IO_READABLE;
    file->io.read_at = file_read_at;
    file->io.write_at = NULL;
    return IFS_OK;
}

void ifs_userspace_file_close(IfsUserspaceFile *file)
{
    if (file == NULL)
        return;

    if (file->descriptor >= 0)
        (void)close(file->descriptor);

    memset(file, 0, sizeof(*file));
    file->descriptor = -1;
}
