// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64

#include "extfs/extfs.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/types.h>
#endif

typedef struct image_file {
    FILE *stream;
} image_file;

static int image_read_at(void *user,
                         extfs_u64 byte_offset,
                         void *destination,
                         extfs_u32 byte_count)
{
    image_file *image = (image_file *)user;
#if defined(_WIN32)
    if (_fseeki64(image->stream, (__int64)byte_offset, SEEK_SET) != 0) {
        return -1;
    }
#else
    if (fseeko(image->stream, (off_t)byte_offset, SEEK_SET) != 0) {
        return -1;
    }
#endif
    return fread(destination, 1U, byte_count, image->stream) == byte_count
        ? 0 : -1;
}

static const char *node_type_name(extfs_node_type type)
{
    switch (type) {
        case EXTFS_NODE_REGULAR:   return "file";
        case EXTFS_NODE_DIRECTORY: return "directory";
        case EXTFS_NODE_SYMLINK:   return "symlink";
        case EXTFS_NODE_CHARACTER: return "character-device";
        case EXTFS_NODE_BLOCK:     return "block-device";
        case EXTFS_NODE_FIFO:      return "fifo";
        case EXTFS_NODE_SOCKET:    return "socket";
        default:                   return "unknown";
    }
}

static char node_type_character(extfs_node_type type)
{
    switch (type) {
        case EXTFS_NODE_REGULAR:   return '-';
        case EXTFS_NODE_DIRECTORY: return 'd';
        case EXTFS_NODE_SYMLINK:   return 'l';
        case EXTFS_NODE_CHARACTER: return 'c';
        case EXTFS_NODE_BLOCK:     return 'b';
        case EXTFS_NODE_FIFO:      return 'p';
        case EXTFS_NODE_SOCKET:    return 's';
        default:                   return '?';
    }
}

static int print_directory_entry(void *user,
                                 extfs_u32 inode_number,
                                 extfs_node_type type,
                                 const char *name,
                                 extfs_u8 name_length)
{
    (void)user;
    printf("%c %10u  ", node_type_character(type), inode_number);
    (void)fwrite(name, 1U, name_length, stdout);
    putchar('\n');
    return 0;
}

static void print_uuid(const extfs_u8 uuid[16])
{
    unsigned int i;
    for (i = 0U; i < 16U; ++i) {
        printf("%02x", uuid[i]);
        if (i == 3U || i == 5U || i == 7U || i == 9U) {
            putchar('-');
        }
    }
}

static void print_risks(extfs_u32 risks)
{
    if (risks == 0U) {
        puts("Read/traversal policy: safe for the implemented feature set");
        return;
    }
    puts("Read/traversal policy: REFUSE MOUNT");
    if ((risks & EXTFS_READONLY_RISK_DIRTY) != 0U) {
        puts("  - filesystem was not cleanly unmounted");
    }
    if ((risks & EXTFS_READONLY_RISK_NEEDS_RECOVERY) != 0U) {
        puts("  - journal or orphan recovery is required");
    }
    if ((risks & EXTFS_READONLY_RISK_ERROR_STATE) != 0U) {
        puts("  - filesystem error state is recorded");
    }
    if ((risks & EXTFS_READONLY_RISK_UNSUPPORTED_INCOMPAT) != 0U) {
        puts("  - unknown incompatible feature flags are present");
    }
    if ((risks & EXTFS_READONLY_RISK_UNSUPPORTED_LAYOUT) != 0U) {
        puts("  - an on-disk layout feature is not implemented yet");
    }
    if ((risks & EXTFS_READONLY_RISK_UNVERIFIED_CHECKSUMS) != 0U) {
        puts("  - legacy group-descriptor checksums are not verified yet");
    }
}

static int command_info(const extfs_volume *volume)
{
    extfs_u32 risks = 0U;
    printf("Filesystem:       %s\n", extfs_kind_string(volume->kind));
    printf("Label:            %s\n", volume->label[0] != '\0'
           ? volume->label : "(none)");
    printf("UUID:             ");
    print_uuid(volume->uuid);
    putchar('\n');
    printf("Block size:       %u bytes\n", volume->block_size);
    printf("Blocks:           %llu\n", volume->total_blocks);
    printf("Inodes:           %u\n", volume->total_inodes);
    printf("Block groups:     %u\n", volume->group_count);
    printf("Inode size:       %u bytes\n", volume->inode_size);
    printf("Descriptor size:  %u bytes\n", volume->descriptor_size);
    printf("Compat features:  0x%08x\n", volume->feature_compat);
    printf("Incompat features:0x%08x\n", volume->feature_incompat);
    printf("RO features:      0x%08x\n", volume->feature_ro_compat);
    printf("Metadata checksum:%s\n", volume->metadata_checksums != 0U
           ? " CRC32C verified" : " not present");
    (void)extfs_readonly_assess(volume, &risks);
    print_risks(risks);
    return 0;
}

static int command_ls(const extfs_volume *volume,
                      const char *path,
                      void *scratch)
{
    extfs_inode inode;
    extfs_status status = extfs_resolve_path(
        volume, path, &inode, scratch, volume->block_size);
    if (status != EXTFS_OK) {
        fprintf(stderr, "extfs-tool: %s: %s\n", path,
                extfs_status_string(status));
        return 1;
    }
    if (extfs_inode_type(&inode) != EXTFS_NODE_DIRECTORY) {
        printf("%s  inode=%u  size=%llu\n", node_type_name(extfs_inode_type(&inode)),
               inode.number, inode.size);
        return 0;
    }
    status = extfs_iterate_directory(volume, &inode, print_directory_entry,
                                     0, scratch, volume->block_size);
    if (status != EXTFS_OK) {
        fprintf(stderr, "extfs-tool: directory read failed: %s\n",
                extfs_status_string(status));
        return 1;
    }
    return 0;
}

static int stream_inode(const extfs_volume *volume,
                        const extfs_inode *inode,
                        FILE *output,
                        void *scratch)
{
    extfs_u8 *buffer;
    extfs_u64 offset = 0U;
    extfs_u32 chunk_size = 65536U;
    int result = 0;
    if (extfs_inode_type(inode) == EXTFS_NODE_DIRECTORY) {
        fprintf(stderr, "extfs-tool: refusing to stream a directory\n");
        return 1;
    }
    buffer = (extfs_u8 *)malloc(chunk_size);
    if (buffer == 0) {
        fprintf(stderr, "extfs-tool: out of memory\n");
        return 1;
    }
    while (offset < inode->size) {
        extfs_u32 requested = chunk_size;
        extfs_u32 received = 0U;
        extfs_status status;
        if ((extfs_u64)requested > inode->size - offset) {
            requested = (extfs_u32)(inode->size - offset);
        }
        status = extfs_read_file(volume, inode, offset, buffer, requested,
                                 scratch, volume->block_size, &received);
        if (status != EXTFS_OK || received == 0U) {
            fprintf(stderr, "extfs-tool: file read failed at %llu: %s\n",
                    offset, extfs_status_string(status));
            result = 1;
            break;
        }
        if (fwrite(buffer, 1U, received, output) != received) {
            fprintf(stderr, "extfs-tool: output failed: %s\n", strerror(errno));
            result = 1;
            break;
        }
        offset += received;
    }
    free(buffer);
    return result;
}

static int command_stream(const extfs_volume *volume,
                          const char *path,
                          const char *output_path,
                          void *scratch)
{
    extfs_inode inode;
    extfs_status status;
    FILE *output = stdout;
    int result;
    status = extfs_resolve_path(volume, path, &inode, scratch,
                                volume->block_size);
    if (status != EXTFS_OK) {
        fprintf(stderr, "extfs-tool: %s: %s\n", path,
                extfs_status_string(status));
        return 1;
    }
    if (output_path != 0) {
        output = fopen(output_path, "wb");
        if (output == 0) {
            fprintf(stderr, "extfs-tool: cannot create %s: %s\n",
                    output_path, strerror(errno));
            return 1;
        }
    }
    result = stream_inode(volume, &inode, output, scratch);
    if (output_path != 0 && fclose(output) != 0) {
        fprintf(stderr, "extfs-tool: cannot close %s: %s\n",
                output_path, strerror(errno));
        result = 1;
    }
    return result;
}

static void usage(FILE *stream)
{
    fputs(
        "ExtFS image inspector 0.9.2\n"
        "Usage:\n"
        "  extfs-tool info IMAGE\n"
        "  extfs-tool ls IMAGE [PATH]\n"
        "  extfs-tool cat IMAGE PATH\n"
        "  extfs-tool extract IMAGE PATH OUTPUT\n",
        stream);
}

int main(int argc, char **argv)
{
    const char *command;
    const char *image_path;
    image_file image;
    extfs_io io;
    extfs_volume volume;
    extfs_status status;
    void *scratch;
    int result;

    if (argc < 3) {
        usage(stderr);
        return 2;
    }
    command = argv[1];
    image_path = argv[2];
    image.stream = fopen(image_path, "rb");
    if (image.stream == 0) {
        fprintf(stderr, "extfs-tool: cannot open %s: %s\n",
                image_path, strerror(errno));
        return 1;
    }
    io.read_at = image_read_at;
    io.write_at = NULL;
    io.flush = NULL;
    io.time_now = NULL;
    io.user = &image;
    status = extfs_open(&volume, &io);
    if (status != EXTFS_OK) {
        fprintf(stderr, "extfs-tool: %s: %s\n", image_path,
                extfs_status_string(status));
        (void)fclose(image.stream);
        return 1;
    }
    if ((strcmp(command, "ls") == 0 || strcmp(command, "cat") == 0 ||
         strcmp(command, "extract") == 0)) {
        extfs_u32 risks = 0U;
        status = extfs_readonly_assess(&volume, &risks);
        if (status != EXTFS_OK) {
            fprintf(stderr,
                    "extfs-tool: refusing metadata/data traversal for an "
                    "unsupported or unsafe layout\n");
            print_risks(risks);
            (void)fclose(image.stream);
            return 1;
        }
    }
    scratch = malloc(volume.block_size);
    if (scratch == 0) {
        fprintf(stderr, "extfs-tool: out of memory\n");
        (void)fclose(image.stream);
        return 1;
    }

    if (strcmp(command, "info") == 0 && argc == 3) {
        result = command_info(&volume);
    } else if (strcmp(command, "ls") == 0 && (argc == 3 || argc == 4)) {
        result = command_ls(&volume, argc == 4 ? argv[3] : "/", scratch);
    } else if (strcmp(command, "cat") == 0 && argc == 4) {
        result = command_stream(&volume, argv[3], 0, scratch);
    } else if (strcmp(command, "extract") == 0 && argc == 5) {
        result = command_stream(&volume, argv[3], argv[4], scratch);
    } else {
        usage(stderr);
        result = 2;
    }

    free(scratch);
    if (fclose(image.stream) != 0) {
        fprintf(stderr, "extfs-tool: cannot close %s: %s\n",
                image_path, strerror(errno));
        result = 1;
    }
    return result;
}
