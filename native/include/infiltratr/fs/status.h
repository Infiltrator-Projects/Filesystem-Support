// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATR_FS_STATUS_H
#define INFILTRATR_FS_STATUS_H

typedef enum {
    IFS_OK = 0,
    IFS_ERROR_ARGUMENT,
    IFS_ERROR_RANGE,
    IFS_ERROR_IO,
    IFS_ERROR_CORRUPT,
    IFS_ERROR_UNSUPPORTED,
    IFS_ERROR_READ_ONLY,
    IFS_ERROR_BUSY,
    IFS_ERROR_NO_MEMORY
} IfsStatus;

const char *ifs_status_string(IfsStatus status);

#endif
