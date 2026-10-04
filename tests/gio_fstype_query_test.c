/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv)
{
    GFile *file;
    GFileInfo *info;
    GError *error = NULL;
    const char *type;
    int result = 1;

    if (argc != 3) {
        fprintf(stderr, "usage: %s PATH EXPECTED_TYPE\n", argv[0]);
        return 2;
    }

    file = g_file_new_for_path(argv[1]);
    info = g_file_query_filesystem_info(file,
                                        G_FILE_ATTRIBUTE_FILESYSTEM_TYPE,
                                        NULL, &error);
    if (info == NULL) {
        fprintf(stderr, "filesystem query failed: %s\n",
                error != NULL ? error->message : "unknown error");
        g_clear_error(&error);
        g_object_unref(file);
        return 1;
    }

    type = g_file_info_get_attribute_string(info,
                                             G_FILE_ATTRIBUTE_FILESYSTEM_TYPE);
    if (type != NULL)
        printf("%s\n", type);
    if (type != NULL && strcmp(type, argv[2]) == 0)
        result = 0;

    g_object_unref(info);
    g_object_unref(file);
    return result;
}
