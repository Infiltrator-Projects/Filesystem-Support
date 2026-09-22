// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATR_FS_USERSPACE_FILE_IO_H
#define INFILTRATR_FS_USERSPACE_FILE_IO_H

#include "infiltratr/fs/io.h"

typedef struct {
    int descriptor;
    IfsIo io;
} IfsUserspaceFile;

IfsStatus ifs_userspace_file_open_readonly(
    const char *path, IfsUserspaceFile *file);
void ifs_userspace_file_close(IfsUserspaceFile *file);

#endif
