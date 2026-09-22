// SPDX-License-Identifier: GPL-3.0-or-later
#include "infiltratr/fs/filesystem.h"

static bool ifs_nonempty(const char *text)
{
    return text != NULL && text[0] != '\0';
}

bool ifs_filesystem_descriptor_is_valid(
    const IfsFilesystemDescriptor *descriptor)
{
    if (descriptor == NULL ||
        descriptor->struct_size < sizeof(*descriptor) ||
        descriptor->abi_version != IFS_FILESYSTEM_DESCRIPTOR_ABI ||
        !ifs_nonempty(descriptor->id) ||
        !ifs_nonempty(descriptor->display_name) ||
        !ifs_nonempty(descriptor->linux_type_name) ||
        !ifs_nonempty(descriptor->module_name))
        return false;

    if ((descriptor->capabilities & IFS_CAPABILITY_PROBE) != 0U &&
        descriptor->probe == NULL)
        return false;

    if ((descriptor->capabilities & IFS_CAPABILITY_WRITE) != 0U &&
        (descriptor->capabilities & IFS_CAPABILITY_READ) == 0U)
        return false;

    return true;
}
