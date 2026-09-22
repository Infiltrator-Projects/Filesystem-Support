// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_io.h"
#include "infiltratr/fs/io.h"
#include "infiltratr/fs/status.h"

#include <inttypes.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    IfsUserspaceFile file;
    IfsStatus status;
    unsigned char prefix[16];
    size_t index;

    if (argc != 2) {
        fprintf(stderr, "usage: fsinspect <image-or-block-device>\n");
        return 2;
    }

    status = ifs_userspace_file_open_readonly(argv[1], &file);
    if (status != IFS_OK) {
        fprintf(stderr, "fsinspect: open failed: %s\n",
                ifs_status_string(status));
        return 1;
    }

    printf("size_bytes=%" PRIu64 "\n", (uint64_t)file.io.size_bytes);

    if (file.io.size_bytes != 0U) {
        const size_t length =
            file.io.size_bytes < sizeof(prefix)
                ? (size_t)file.io.size_bytes
                : sizeof(prefix);

        status = ifs_io_read_exact(&file.io, 0U, prefix, length);
        if (status != IFS_OK) {
            fprintf(stderr, "fsinspect: read failed: %s\n",
                    ifs_status_string(status));
            ifs_userspace_file_close(&file);
            return 1;
        }

        fputs("prefix=", stdout);
        for (index = 0U; index < length; ++index)
            printf("%02x", prefix[index]);
        fputc('\n', stdout);
    }

    ifs_userspace_file_close(&file);
    return 0;
}
