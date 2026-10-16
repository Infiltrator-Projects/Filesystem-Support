/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FILESYSTEMS_NATIVE_AMIGA_SUPPORT_H
#define FILESYSTEMS_NATIVE_AMIGA_SUPPORT_H

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>

/* Only a separately compiled test executable changes this root. Installed
 * programs always use /lib/modules, with no environment override. */
#ifndef IFS_NATIVE_MODULES_ROOT
#define IFS_NATIVE_MODULES_ROOT "/lib/modules"
#endif

/* This helper answers only whether a caller-supplied native module identity is
 * installed for the running kernel. Desktop membership is owned by
 * data/desktop-filesystems.tsv and must not be duplicated here. The token
 * validator is a path-safety boundary, not a filesystem allowlist. */
static inline int ifs_native_module_token_valid(const char *type)
{
    const unsigned char *cursor;

    if (type == NULL || *type == '\0')
        return 0;
    for (cursor = (const unsigned char *)type; *cursor != '\0'; ++cursor) {
        if ((*cursor >= 'a' && *cursor <= 'z') ||
            (*cursor >= 'A' && *cursor <= 'Z') ||
            (*cursor >= '0' && *cursor <= '9') ||
            *cursor == '_' || *cursor == '.' || *cursor == '+' || *cursor == '-')
            continue;
        return 0;
    }
    return 1;
}

static inline int ifs_native_amiga_installed(const char *type)
{
    struct utsname kernel;
    struct stat module;
    char path[512];
    int length;

    if (!ifs_native_module_token_valid(type))
        return 0;
    if (uname(&kernel) != 0)
        return 0;
    length = snprintf(path, sizeof(path), "%s/%s/updates/infiltrator/%s.ko",
                      IFS_NATIVE_MODULES_ROOT, kernel.release, type);
    if (length < 0 || (size_t)length >= sizeof(path))
        return 0;
    return stat(path, &module) == 0 && S_ISREG(module.st_mode);
}

#endif
