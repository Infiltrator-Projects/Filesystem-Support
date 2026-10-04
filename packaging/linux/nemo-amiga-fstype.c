/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * Nemo asks GIO for filesystem::type. On Linux GLib derives that string from
 * statfs(2).f_type and maps the historical AFFS magic to "affs". OFS and FFS
 * intentionally retain the AFFS-family superblock magic for compatibility,
 * while Linux VFS mounts them under the independent project names "ofs" and
 * "ffs". This Nemo-local interposer replaces only that one GIO attribute with
 * the actual mounted VFS type from /proc/self/mountinfo.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <gio/gio.h>
#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/native_amiga_support.h"

#ifndef IFS_MOUNTINFO_PATH
#define IFS_MOUNTINFO_PATH "/proc/self/mountinfo"
#endif

typedef GFileInfo *(*QueryFilesystemInfoFunction)(GFile *, const char *,
                                                   GCancellable *, GError **);

static QueryFilesystemInfoFunction stock_query_filesystem_info;
static pthread_once_t stock_once = PTHREAD_ONCE_INIT;

static void find_stock_query(void)
{
    void *symbol = dlsym(RTLD_NEXT, "g_file_query_filesystem_info");
    _Static_assert(sizeof(stock_query_filesystem_info) == sizeof(symbol),
                   "POSIX function pointer ABI");
    memcpy(&stock_query_filesystem_info, &symbol,
           sizeof(stock_query_filesystem_info));
}

/* Do not leak this process-local preload into applications launched by Nemo. */
__attribute__((constructor)) static void restore_preload_environment(void)
{
    const char *was_set = getenv("INFILTRATOR_NEMO_PRELOAD_SET");

    if (was_set == NULL)
        return;

    if (strcmp(was_set, "yes") == 0) {
        const char *previous = getenv("INFILTRATOR_NEMO_PRELOAD_VALUE");
        setenv("LD_PRELOAD", previous != NULL ? previous : "", 1);
    } else {
        unsetenv("LD_PRELOAD");
    }

    unsetenv("INFILTRATOR_NEMO_PRELOAD_SET");
    unsetenv("INFILTRATOR_NEMO_PRELOAD_VALUE");
}

static int octal_digit(char value)
{
    return value >= '0' && value <= '7';
}

/* mountinfo escapes whitespace and backslashes as three-digit octal values. */
static void unescape_mount_field(char *value)
{
    char *read = value;
    char *write = value;

    while (*read != '\0') {
        if (read[0] == '\\' && read[1] != '\0' && read[2] != '\0' &&
            read[3] != '\0' && octal_digit(read[1]) &&
            octal_digit(read[2]) && octal_digit(read[3])) {
            *write++ = (char)(((read[1] - '0') << 6) |
                              ((read[2] - '0') << 3) |
                              (read[3] - '0'));
            read += 4;
        } else {
            *write++ = *read++;
        }
    }
    *write = '\0';
}

static gboolean path_is_within_mount(const char *path, const char *mountpoint)
{
    size_t length;

    if (path == NULL || mountpoint == NULL || mountpoint[0] != '/')
        return FALSE;
    if (strcmp(mountpoint, "/") == 0)
        return path[0] == '/';

    length = strlen(mountpoint);
    return strncmp(path, mountpoint, length) == 0 &&
           (path[length] == '\0' || path[length] == '/');
}

static char *mounted_native_type_for_path(const char *path)
{
    FILE *mountinfo;
    char *line = NULL;
    size_t capacity = 0;
    char *best = NULL;
    size_t best_length = 0;

    if (path == NULL || path[0] != '/')
        return NULL;

    mountinfo = fopen(IFS_MOUNTINFO_PATH, "re");
    if (mountinfo == NULL)
        return NULL;

    while (getline(&line, &capacity, mountinfo) >= 0) {
        char *separator = strstr(line, " - ");
        char *left_copy;
        char *right_copy;
        char *save = NULL;
        char *token;
        char *mountpoint = NULL;
        char *fstype = NULL;
        int field = 0;
        size_t mount_length;

        if (separator == NULL)
            continue;
        *separator = '\0';
        left_copy = g_strdup(line);
        right_copy = g_strdup(separator + 3);

        for (token = strtok_r(left_copy, " ", &save);
             token != NULL;
             token = strtok_r(NULL, " ", &save)) {
            ++field;
            if (field == 5) {
                mountpoint = token;
                break;
            }
        }

        save = NULL;
        fstype = strtok_r(right_copy, " ", &save);
        if (mountpoint != NULL && fstype != NULL &&
            (strcmp(fstype, "ofs") == 0 || strcmp(fstype, "ffs") == 0)) {
            unescape_mount_field(mountpoint);
            mount_length = strlen(mountpoint);
            if (mount_length >= best_length &&
                path_is_within_mount(path, mountpoint) &&
                ifs_native_amiga_installed(fstype)) {
                g_free(best);
                best = g_strdup(fstype);
                best_length = mount_length;
            }
        }

        g_free(left_copy);
        g_free(right_copy);
    }

    free(line);
    fclose(mountinfo);
    return best;
}

__attribute__((visibility("default")))
GFileInfo *g_file_query_filesystem_info(GFile *file, const char *attributes,
                                        GCancellable *cancellable,
                                        GError **error)
{
    GFileInfo *info;
    char *path;
    char *mounted_type;

    pthread_once(&stock_once, find_stock_query);
    if (stock_query_filesystem_info == NULL) {
        g_warning("Filesystem Support could not find the stock GIO filesystem query");
        return NULL;
    }

    info = stock_query_filesystem_info(file, attributes, cancellable, error);
    if (info == NULL ||
        !g_file_info_has_attribute(info, G_FILE_ATTRIBUTE_FILESYSTEM_TYPE))
        return info;

    path = g_file_get_path(file);
    mounted_type = mounted_native_type_for_path(path);
    if (mounted_type != NULL) {
        g_file_info_set_attribute_string(info,
                                         G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
                                         mounted_type);
    }

    g_free(mounted_type);
    g_free(path);
    return info;
}
