/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef FILESYSTEM_SUPPORT_NATIVE_AMIGA_SUPPORT_H
#define FILESYSTEM_SUPPORT_NATIVE_AMIGA_SUPPORT_H

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>

/* Only a separately compiled test executable changes this root. Installed
 * programs always use /lib/modules, with no environment override. */
#ifndef IFS_NATIVE_MODULES_ROOT
#define IFS_NATIVE_MODULES_ROOT "/lib/modules"
#endif

/* Match the manager and native-module helper: installing one project-owned
 * module grants desktop recognition only to that exact filesystem. */
static inline int ifs_native_amiga_installed(const char *type)
{
    struct utsname kernel;
    struct stat module;
    char path[512];
    int length;

    if (type == NULL ||
        (strcmp(type, "ofs") != 0 && strcmp(type, "ffs") != 0 &&
         strcmp(type, "sfs") != 0 && strcmp(type, "sfs2") != 0 &&
         strcmp(type, "pfs3") != 0))
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
