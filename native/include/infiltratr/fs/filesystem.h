// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATR_FS_FILESYSTEM_H
#define INFILTRATR_FS_FILESYSTEM_H

#include "io.h"

#define IFS_FILESYSTEM_DESCRIPTOR_ABI 1U

enum {
    IFS_CAPABILITY_PROBE = 1U << 0,
    IFS_CAPABILITY_READ = 1U << 1,
    IFS_CAPABILITY_WRITE = 1U << 2,
    IFS_CAPABILITY_CREATE = 1U << 3,
    IFS_CAPABILITY_CHECK = 1U << 4
};

typedef IfsStatus (*IfsProbeFn)(const IfsIo *io, bool *matches);

typedef struct {
    size_t struct_size;
    unsigned int abi_version;
    const char *id;
    const char *display_name;
    const char *linux_type_name;
    const char *module_name;
    unsigned int capabilities;
    IfsProbeFn probe;
} IfsFilesystemDescriptor;

bool ifs_filesystem_descriptor_is_valid(
    const IfsFilesystemDescriptor *descriptor);

#endif
