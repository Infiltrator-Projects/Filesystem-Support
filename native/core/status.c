// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratr/fs/status.h"

const char *ifs_status_string(const IfsStatus status)
{
    switch (status) {
    case IFS_OK: return "ok";
    case IFS_ERROR_ARGUMENT: return "invalid argument";
    case IFS_ERROR_RANGE: return "out of range";
    case IFS_ERROR_IO: return "I/O error";
    case IFS_ERROR_CORRUPT: return "corrupt filesystem";
    case IFS_ERROR_UNSUPPORTED: return "unsupported feature";
    case IFS_ERROR_READ_ONLY: return "read-only";
    case IFS_ERROR_BUSY: return "busy";
    case IFS_ERROR_NO_MEMORY: return "out of memory";
    }
    return "unknown error";
}
