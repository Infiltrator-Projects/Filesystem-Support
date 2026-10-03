/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Only GNOME Disks loads this display-name extension. The distribution's
 * UDisks client remains installed and handles every other identifier. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <glib.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

typedef gchar *(*DisplayFunction)(void *, const gchar *, const gchar *,
                                  const gchar *, gboolean);
static DisplayFunction stock_display;
static pthread_once_t stock_once = PTHREAD_ONCE_INIT;

static void find_stock_display(void)
{
    stock_display = (DisplayFunction)dlsym(RTLD_NEXT,
                                          "udisks_client_get_id_for_display");
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
    static const struct {
        const char *type, *name, *short_name;
        gboolean version_word;
    } names[] = {
        {"ofs", "Amiga Original File System", "Amiga OFS", FALSE},
        {"ffs", "Amiga Fast File System", "Amiga FFS", FALSE},
        {"sfs", "Amiga Smart File System", "Amiga SFS", TRUE},
        {"sfs2", "Amiga Smart File System 2", "Amiga SFS2", TRUE},
        {"pfs3", "Amiga Professional File System 3", "Amiga PFS3", FALSE},
    };
    if (g_strcmp0(usage, "filesystem") == 0) {
        for (size_t i = 0; i < G_N_ELEMENTS(names); ++i) {
            if (g_strcmp0(type, names[i].type) != 0)
                continue;
            if (!long_name)
                return g_strdup(names[i].short_name);
            if (version == NULL || *version == '\0')
                return g_strdup(names[i].name);
            return g_strdup_printf(names[i].version_word ? "%s (version %s)"
                                                        : "%s (%s)",
                                   names[i].name, version);
        }
    }
    pthread_once(&stock_once, find_stock_display);
    if (stock_display != NULL)
        return stock_display(client, usage, type, version, long_name);
    /* The package depends on the stock client; this guards a broken direct
     * preload without changing ownership of a single stock library file. */
    g_warning("Filesystem Support could not find the stock UDisks display function");
    return g_strdup("Unknown");
}
