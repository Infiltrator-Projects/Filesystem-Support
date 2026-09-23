// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "extfs/extfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

typedef struct image_file {
    FILE *stream;
} image_file;

static int image_read_at(void *user,
                         extfs_u64 byte_offset,
                         void *destination,
                         extfs_u32 byte_count)
{
    image_file *image = (image_file *)user;
    if (image == NULL || image->stream == NULL || destination == NULL) return -1;
    if (fseeko(image->stream, (off_t)byte_offset, SEEK_SET) != 0) return -1;
    return fread(destination, 1U, byte_count, image->stream) == byte_count ? 0 : -1;
}

static int image_write_at(void *user,
                          extfs_u64 byte_offset,
                          const void *source,
                          extfs_u32 byte_count)
{
    image_file *image = (image_file *)user;
    if (image == NULL || image->stream == NULL || source == NULL) return -1;
    if (fseeko(image->stream, (off_t)byte_offset, SEEK_SET) != 0) return -1;
    if (fwrite(source, 1U, byte_count, image->stream) != byte_count) return -1;
    return fflush(image->stream) == 0 ? 0 : -1;
}

static int image_flush(void *user)
{
    image_file *image = (image_file *)user;
    if (image == NULL || image->stream == NULL) return -1;
    if (fflush(image->stream) != 0) return -1;
    return fsync(fileno(image->stream)) == 0 ? 0 : -1;
}

static int image_time_now(void *user,
                          extfs_u64 *seconds,
                          extfs_u32 *nanoseconds)
{
    struct timespec now;
    (void)user;
    if (seconds == NULL || nanoseconds == NULL) return -1;
    if (clock_gettime(CLOCK_REALTIME, &now) != 0 || now.tv_sec < 0 ||
        now.tv_nsec < 0 || now.tv_nsec >= 1000000000L) {
        return -1;
    }
    *seconds = (extfs_u64)now.tv_sec;
    *nanoseconds = (extfs_u32)now.tv_nsec;
    return 0;
}

static int parse_size(const char *text, extfs_u64 *value)
{
    char *end = NULL;
    unsigned long long parsed;
    if (text == NULL || value == NULL || *text == '\0' || *text == '-') return -1;
    errno = 0;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') return -1;
    *value = (extfs_u64)parsed;
    return 0;
}

static extfs_status resize_inode(extfs_volume *volume,
                                 extfs_inode *inode,
                                 extfs_u64 new_size,
                                 void *scratch,
                                 extfs_u32 scratch_size)
{
    switch (volume->kind) {
        case EXTFS_KIND_EXT2:
            return extfs_resize_file_ext2_direct(volume, inode, new_size,
                                                  scratch, scratch_size);
        case EXTFS_KIND_EXT3:
            return extfs_resize_file_ext3_journaled_direct(volume, inode,
                                                            new_size, scratch,
                                                            scratch_size);
        case EXTFS_KIND_EXT4:
            return extfs_resize_file_ext4_journaled_extent_tree(volume, inode,
                                                                 new_size,
                                                                 scratch,
                                                                 scratch_size);
        default:
            return EXTFS_ERR_UNSUPPORTED;
    }
}

static void usage(FILE *stream)
{
    fputs("Usage: extfs-mutate-test resize IMAGE PATH NEW_SIZE\n", stream);
}

int main(int argc, char **argv)
{
    const char *image_path;
    const char *path;
    extfs_u64 new_size;
    image_file image;
    extfs_io io;
    extfs_volume volume;
    extfs_volume verify_volume;
    extfs_inode inode;
    extfs_inode verify_inode;
    extfs_status status;
    extfs_u32 scratch_size;
    void *scratch;
    int result = 1;

    if (argc != 5 || strcmp(argv[1], "resize") != 0 ||
        parse_size(argv[4], &new_size) != 0) {
        usage(stderr);
        return 2;
    }
    image_path = argv[2];
    path = argv[3];
    image.stream = fopen(image_path, "r+b");
    if (image.stream == NULL) {
        fprintf(stderr, "extfs-mutate-test: cannot open %s: %s\n",
                image_path, strerror(errno));
        return 1;
    }

    memset(&io, 0, sizeof(io));
    io.read_at = image_read_at;
    io.write_at = image_write_at;
    io.flush = image_flush;
    io.time_now = image_time_now;
    io.user = &image;

    status = extfs_open(&volume, &io);
    if (status != EXTFS_OK) {
        fprintf(stderr, "extfs-mutate-test: open failed: %s\n",
                extfs_status_string(status));
        goto Exit;
    }
    if (volume.block_size == 0U || volume.block_size > EXTFS_MAX_BLOCK_SIZE) {
        fprintf(stderr, "extfs-mutate-test: invalid scratch geometry\n");
        goto Exit;
    }
    scratch_size = volume.block_size * 8U;
    scratch = malloc(scratch_size);
    if (scratch == NULL) {
        fprintf(stderr, "extfs-mutate-test: out of memory\n");
        goto Exit;
    }

    status = extfs_resolve_path(&volume, path, &inode, scratch, scratch_size);
    if (status != EXTFS_OK) {
        fprintf(stderr, "extfs-mutate-test: %s: %s\n", path,
                extfs_status_string(status));
        free(scratch);
        goto Exit;
    }
    if (extfs_inode_type(&inode) != EXTFS_NODE_REGULAR) {
        fprintf(stderr, "extfs-mutate-test: target is not a regular file\n");
        free(scratch);
        goto Exit;
    }

    status = resize_inode(&volume, &inode, new_size, scratch, scratch_size);
    if (status != EXTFS_OK) {
        fprintf(stderr, "extfs-mutate-test: resize failed: %s\n",
                extfs_status_string(status));
        free(scratch);
        goto Exit;
    }
    if (image_flush(&image) != 0) {
        fprintf(stderr, "extfs-mutate-test: final host flush failed\n");
        free(scratch);
        goto Exit;
    }
    free(scratch);

    if (fclose(image.stream) != 0) {
        image.stream = NULL;
        fprintf(stderr, "extfs-mutate-test: close after resize failed\n");
        return 1;
    }
    image.stream = fopen(image_path, "rb");
    if (image.stream == NULL) {
        fprintf(stderr, "extfs-mutate-test: cannot reopen %s: %s\n",
                image_path, strerror(errno));
        return 1;
    }
    io.write_at = NULL;
    io.flush = NULL;
    io.time_now = NULL;
    status = extfs_open(&verify_volume, &io);
    if (status != EXTFS_OK) {
        fprintf(stderr, "extfs-mutate-test: reopen failed: %s\n",
                extfs_status_string(status));
        goto Exit;
    }
    scratch = malloc(verify_volume.block_size);
    if (scratch == NULL) {
        fprintf(stderr, "extfs-mutate-test: out of memory on reopen\n");
        goto Exit;
    }
    status = extfs_resolve_path(&verify_volume, path, &verify_inode, scratch,
                                verify_volume.block_size);
    free(scratch);
    if (status != EXTFS_OK || verify_inode.size != new_size) {
        fprintf(stderr,
                "extfs-mutate-test: persisted size verification failed: %s, size=%llu expected=%llu\n",
                extfs_status_string(status),
                status == EXTFS_OK ? verify_inode.size : 0ULL,
                new_size);
        goto Exit;
    }

    printf("%s %s resized to %llu bytes and reopened successfully\n",
           extfs_kind_string(verify_volume.kind), path, new_size);
    result = 0;

Exit:
    if (image.stream != NULL && fclose(image.stream) != 0) result = 1;
    return result;
}
