/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Per-filesystem desktop integration for project-native Amiga filesystems.
 *
 * The distribution UDisks client and GIO implementation remain authoritative
 * for every unrelated filesystem. A compiled copy of this file is activated
 * only when its matching project-native driver is installed.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <gio/gio.h>
#include <glib.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include "../../src/native_amiga_support.h"

/* Each compiled file contains exactly one filesystem's names. The installer
 * deploys that file only with that filesystem's own native driver. */
#if IFS_NAMES_VARIANT == 1
#define IFS_TYPE "ofs"
#define IFS_LONG_NAME "Amiga Original File System"
#define IFS_SHORT_NAME "Amiga OFS"
#define IFS_VERSION_WORD 0
#elif IFS_NAMES_VARIANT == 2
#define IFS_TYPE "ffs"
#define IFS_LONG_NAME "Amiga Fast File System"
#define IFS_SHORT_NAME "Amiga FFS"
#define IFS_VERSION_WORD 0
#elif IFS_NAMES_VARIANT == 3
#define IFS_TYPE "sfs"
#define IFS_LONG_NAME "Amiga Smart File System"
#define IFS_SHORT_NAME "Amiga SFS"
#define IFS_VERSION_WORD 1
#elif IFS_NAMES_VARIANT == 4
#define IFS_TYPE "sfs2"
#define IFS_LONG_NAME "Amiga Smart File System 2"
#define IFS_SHORT_NAME "Amiga SFS2"
#define IFS_VERSION_WORD 1
#elif IFS_NAMES_VARIANT == 5
#define IFS_TYPE "pfs3"
#define IFS_LONG_NAME "Amiga Professional File System 3"
#define IFS_SHORT_NAME "Amiga PFS3"
#define IFS_VERSION_WORD 0
#else
#error "Select one filesystem when compiling its desktop integration file"
#endif

typedef gchar *(*DisplayFunction)(void *, const gchar *, const gchar *,
                                  const gchar *, gboolean);
typedef GFileInfo *(*QueryFilesystemInfoFunction)(GFile *, const char *,
                                                   GCancellable *, GError **);

static DisplayFunction stock_display;
static QueryFilesystemInfoFunction stock_query_filesystem_info;
static pthread_once_t stock_display_once = PTHREAD_ONCE_INIT;
static pthread_once_t stock_query_once = PTHREAD_ONCE_INIT;

static void find_stock_display(void)
{
    void *symbol = dlsym(RTLD_NEXT, "udisks_client_get_id_for_display");
    _Static_assert(sizeof(stock_display) == sizeof(symbol), "POSIX function pointer ABI");
    memcpy(&stock_display, &symbol, sizeof(stock_display));
}

static void find_stock_query_filesystem_info(void)
{
    void *symbol = dlsym(RTLD_NEXT, "g_file_query_filesystem_info");
    _Static_assert(sizeof(stock_query_filesystem_info) == sizeof(symbol),
                   "POSIX function pointer ABI");
    memcpy(&stock_query_filesystem_info, &symbol,
           sizeof(stock_query_filesystem_info));
}

static void restore_preload_pair(const char *set_name, const char *value_name)
{
    const char *was_set = getenv(set_name);

    if (was_set != NULL) {
        const char *previous = getenv(value_name);
        if (strcmp(was_set, "yes") == 0)
            setenv("LD_PRELOAD", previous != NULL ? previous : "", 1);
        else
            unsetenv("LD_PRELOAD");
        unsetenv(set_name);
        unsetenv(value_name);
    }
}

/* The wrappers record the caller's preload environment. Restore it before
 * Disks or Nemo can launch children, keeping this extension inside the
 * application process that was deliberately started through our wrapper. */
__attribute__((constructor)) static void restore_preload_environment(void)
{
    restore_preload_pair("INFILTRATOR_DISKS_PRELOAD_SET",
                         "INFILTRATOR_DISKS_PRELOAD_VALUE");
    restore_preload_pair("INFILTRATOR_NEMO_PRELOAD_SET",
                         "INFILTRATOR_NEMO_PRELOAD_VALUE");
}

static gboolean file_is_matching_native_mount(GFile *file)
{
#ifdef IFS_DESKTOP_TESTING
    const char *forced = getenv("FSUPPORT_TEST_MOUNT_TYPE");
    if (forced != NULL)
        return strcmp(forced, IFS_TYPE) == 0;
#endif

    char *path = g_file_get_path(file);
    struct stat status;
    FILE *mountinfo;
    char *line = NULL;
    size_t capacity = 0;
    gboolean matches = FALSE;

    if (path == NULL)
        return FALSE;
    if (stat(path, &status) != 0) {
        g_free(path);
        return FALSE;
    }
    g_free(path);

    mountinfo = fopen("/proc/self/mountinfo", "re");
    if (mountinfo == NULL)
        return FALSE;

    while (getline(&line, &capacity, mountinfo) >= 0) {
        unsigned int device_major = 0;
        unsigned int device_minor = 0;
        char filesystem[64];
        char *separator;

        if (sscanf(line, "%*u %*u %u:%u", &device_major, &device_minor) != 2)
            continue;
        if (device_major != major(status.st_dev) ||
            device_minor != minor(status.st_dev))
            continue;

        separator = strstr(line, " - ");
        if (separator == NULL)
            continue;
        filesystem[0] = '\0';
        if (sscanf(separator + 3, "%63s", filesystem) != 1)
            continue;
        if (strcmp(filesystem, IFS_TYPE) == 0) {
            matches = TRUE;
            break;
        }
    }

    free(line);
    fclose(mountinfo);
    return matches;
}

/* Nemo obtains the Properties-window filesystem type from
 * G_FILE_ATTRIBUTE_FILESYSTEM_TYPE. GLib derives that value solely from
 * statfs().f_type on Linux; OFS and FFS deliberately retain the standard AFFS
 * superblock magic for on-disk/VFS compatibility, so stock GLib calls both
 * variants "affs". Keep the kernel ABI intact and correct only the userspace
 * identity for a mount that the kernel itself registered as this native type. */
__attribute__((visibility("default")))
GFileInfo *g_file_query_filesystem_info(GFile *file, const char *attributes,
                                        GCancellable *cancellable, GError **error)
{
    GFileInfo *info;

    pthread_once(&stock_query_once, find_stock_query_filesystem_info);
    if (stock_query_filesystem_info == NULL)
        return NULL;

    info = stock_query_filesystem_info(file, attributes, cancellable, error);
    if (info != NULL && file_is_matching_native_mount(file))
        g_file_info_set_attribute_string(info,
                                         G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
                                         IFS_TYPE);
    return info;
}

__attribute__((visibility("default")))
gchar *udisks_client_get_id_for_display(void *client, const gchar *usage,
                                        const gchar *type, const gchar *version,
                                        gboolean long_name)
{
    if (g_strcmp0(usage, "filesystem") == 0 &&
        g_strcmp0(type, IFS_TYPE) == 0 && ifs_native_amiga_installed(IFS_TYPE)) {
        if (!long_name)
            return g_strdup(IFS_SHORT_NAME);
        if (version == NULL || *version == '\0')
            return g_strdup(IFS_LONG_NAME);
        return g_strdup_printf(IFS_VERSION_WORD ? "%s (version %s)" : "%s (%s)",
                               IFS_LONG_NAME, version);
    }
    pthread_once(&stock_display_once, find_stock_display);
    if (stock_display != NULL)
        return stock_display(client, usage, type, version, long_name);
    g_warning("Filesystem Support could not find the stock UDisks display function");
    return g_strdup("Unknown");
}
