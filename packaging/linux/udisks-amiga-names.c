/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Only GNOME Disks loads this display-name extension. The distribution's
 * UDisks client remains installed and handles every other identifier. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <glib.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
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
#error "Select one filesystem when compiling its Disks naming file"
#endif

typedef gchar *(*DisplayFunction)(void *, const gchar *, const gchar *,
                                  const gchar *, gboolean);
static DisplayFunction stock_display;
static pthread_once_t stock_once = PTHREAD_ONCE_INIT;

static void find_stock_display(void)
{
    void *symbol = dlsym(RTLD_NEXT, "udisks_client_get_id_for_display");
    _Static_assert(sizeof(stock_display) == sizeof(symbol), "POSIX function pointer ABI");
    memcpy(&stock_display, &symbol, sizeof(stock_display));
}

/* The wrapper records the caller's preload environment. Restore it before
 * Disks can launch children, keeping this extension inside Disks itself. */
__attribute__((constructor)) static void restore_preload_environment(void)
{
    const char *was_set = getenv("INFILTRATOR_DISKS_PRELOAD_SET");
    if (was_set != NULL) {
        const char *previous = getenv("INFILTRATOR_DISKS_PRELOAD_VALUE");
        if (strcmp(was_set, "yes") == 0)
            setenv("LD_PRELOAD", previous != NULL ? previous : "", 1);
        else
            unsetenv("LD_PRELOAD");
        unsetenv("INFILTRATOR_DISKS_PRELOAD_SET");
        unsetenv("INFILTRATOR_DISKS_PRELOAD_VALUE");
    }
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
    pthread_once(&stock_once, find_stock_display);
    if (stock_display != NULL)
        return stock_display(client, usage, type, version, long_name);
    /* The package depends on the stock client; this guards a broken direct
     * preload without changing ownership of a single stock library file. */
    g_warning("Filesystem Support could not find the stock UDisks display function");
    return g_strdup("Unknown");
}
