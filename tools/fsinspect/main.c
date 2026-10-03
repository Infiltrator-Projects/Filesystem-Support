// SPDX-License-Identifier: GPL-3.0-or-later
#include "file_io.h"
#include "infiltratr/fs/io.h"
#include "infiltratr/fs/status.h"
#include "ofs_core.h"
#include "ffs_core.h"
#include "sfs_core.h"
#include "sfs2_core.h"
#include "pfs3_core.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IFS_PROBE_PREFIX_BYTES 128U
#define IFS_PROBE_MAX_BLOCK_SIZE 65536U
#define IFS_PFS3_SECTOR_SIZE 512U
#define IFS_PFS3_ROOT_SECTOR 2U

typedef struct IfsProbeResult {
    const char *type;
    char version[24];
    uint32_t block_size;
    char label[IFS_PFS3_DISK_NAME_BYTES];
} IfsProbeResult;

static uint16_t read_be16(const unsigned char *bytes)
{
    return (uint16_t)(((uint16_t)bytes[0] << 8) |
                      (uint16_t)bytes[1]);
}

static uint32_t read_be32(const unsigned char *bytes)
{
    return ((uint32_t)bytes[0] << 24) |
           ((uint32_t)bytes[1] << 16) |
           ((uint32_t)bytes[2] << 8) |
           (uint32_t)bytes[3];
}

static int read_exact_at(
    IfsUserspaceFile *file,
    uint64_t offset,
    void *buffer,
    size_t length)
{
    if (file == NULL || buffer == NULL)
        return -1;
    if (offset > file->io.size_bytes ||
        (uint64_t)length > file->io.size_bytes - offset)
        return -1;

    return ifs_io_read_exact(&file->io, offset, buffer, length) == IFS_OK
        ? 0 : -1;
}

static int geometry_fits_media(
    uint32_t block_size,
    uint32_t total_blocks,
    uint64_t media_bytes)
{
    uint64_t declared_bytes;

    if (block_size == 0U || total_blocks == 0U)
        return 0;
    if ((uint64_t)total_blocks > UINT64_MAX / (uint64_t)block_size)
        return 0;

    declared_bytes = (uint64_t)total_blocks * (uint64_t)block_size;
    return declared_bytes <= media_bytes;
}

static void format_dostype_version(
    uint32_t dostype,
    char version[24])
{
    const unsigned char a = (unsigned char)(dostype >> 24);
    const unsigned char b = (unsigned char)(dostype >> 16);
    const unsigned char c = (unsigned char)(dostype >> 8);
    const unsigned char d = (unsigned char)dostype;

    if (a == 'D' && b == 'O' && c == 'S') {
        snprintf(version, 24U, "DOS/%u", (unsigned int)d);
    } else if (dostype == IFS_FFS_MUFS_GENERIC) {
        snprintf(version, 24U, "muFS");
    } else if (a == 'm' && b == 'u' && c == 'F') {
        snprintf(version, 24U, "muF/%u", (unsigned int)d);
    } else {
        snprintf(version, 24U, "%08" PRIx32, dostype);
    }
}

static int probe_ofs_ffs(
    const unsigned char prefix[IFS_PROBE_PREFIX_BYTES],
    IfsProbeResult *result)
{
    const uint32_t dostype = read_be32(prefix);
    uint32_t variants = 0U;

    if (ifs_ofs_classify_dostype(dostype, &variants) == 0) {
        (void)variants;
        result->type = "ofs";
        result->block_size = 512U;
        format_dostype_version(dostype, result->version);
        return 1;
    }

    if (ifs_ffs_classify_dostype(dostype, &variants) == 0) {
        (void)variants;
        result->type = "ffs";
        result->block_size = 512U;
        format_dostype_version(dostype, result->version);
        return 1;
    }

    return 0;
}

static int probe_sfs(
    IfsUserspaceFile *file,
    const unsigned char prefix[IFS_PROBE_PREFIX_BYTES],
    IfsProbeResult *result)
{
    const uint32_t id = read_be32(prefix + 0U);
    const uint32_t version = read_be16(prefix + 12U);
    const uint32_t total_blocks = read_be32(prefix + 48U);
    const uint32_t block_size = read_be32(prefix + 52U);
    const uint32_t bitmap_base = read_be32(prefix + 96U);
    const uint32_t adminspace = read_be32(prefix + 100U);
    const uint32_t root_object = read_be32(prefix + 104U);
    const uint32_t extent_root = read_be32(prefix + 108U);
    const uint32_t object_root = read_be32(prefix + 112U);
    unsigned char *block;
    int valid;

    if (ifs_sfs_validate_root_layout(
            id, version, block_size, total_blocks,
            bitmap_base, adminspace, root_object,
            extent_root, object_root) != IFS_SFS_ROOT_OK)
        return 0;

    if (block_size > IFS_PROBE_MAX_BLOCK_SIZE ||
        !geometry_fits_media(block_size, total_blocks, file->io.size_bytes))
        return 0;

    block = (unsigned char *)malloc(block_size);
    if (block == NULL)
        return 0;

    valid = read_exact_at(file, 0U, block, block_size) == 0 &&
            ifs_sfs_validate_block_header(
                block, block_size, 0U, IFS_SFS_ROOT_ID) != 0;
    free(block);
    if (!valid)
        return 0;

    result->type = "sfs";
    result->block_size = block_size;
    snprintf(result->version, sizeof(result->version), "%u",
             (unsigned int)version);
    return 1;
}

static int probe_sfs2(
    IfsUserspaceFile *file,
    const unsigned char prefix[IFS_PROBE_PREFIX_BYTES],
    IfsProbeResult *result)
{
    IfsSfs2RootRecord root;
    unsigned char *block;
    int valid;

    if (ifs_sfs2_decode_root(prefix, IFS_PROBE_PREFIX_BYTES, &root) != 0 ||
        ifs_sfs2_validate_root_record(&root) != 0)
        return 0;

    if (root.block_size > IFS_PROBE_MAX_BLOCK_SIZE ||
        !geometry_fits_media(
            root.block_size, root.total_blocks, file->io.size_bytes))
        return 0;

    block = (unsigned char *)malloc(root.block_size);
    if (block == NULL)
        return 0;

    valid = read_exact_at(file, 0U, block, root.block_size) == 0 &&
            ifs_sfs2_validate_block_header(
                block, root.block_size, 0U, IFS_SFS2_ROOT_ID) != 0;
    free(block);
    if (!valid)
        return 0;

    result->type = "sfs2";
    result->block_size = root.block_size;
    snprintf(result->version, sizeof(result->version), "%u",
             (unsigned int)root.version);
    return 1;
}

static void copy_pfs3_label(
    const unsigned char disk_name[IFS_PFS3_DISK_NAME_BYTES],
    char label[IFS_PFS3_DISK_NAME_BYTES])
{
    const unsigned int length = disk_name[0];
    unsigned int index;

    if (length == 0U || length > IFS_PFS3_MAX_DISK_NAME)
        return;

    for (index = 0U; index < length; ++index) {
        unsigned char character = disk_name[index + 1U];

        if (character < 0x20U || character == 0x7fU ||
            character == '=' || character == '\\')
            character = '_';
        label[index] = (char)character;
    }
    label[length] = '\0';
}

static int probe_pfs3(IfsUserspaceFile *file, IfsProbeResult *result)
{
    unsigned char sector[IFS_PFS3_SECTOR_SIZE];
    IfsPfs3RootRecord root;
    const uint64_t media_blocks = file->io.size_bytes / IFS_PFS3_SECTOR_SIZE;
    IfsPfs3Format format;

    if (media_blocks == 0U || media_blocks > UINT32_MAX)
        return 0;
    if (read_exact_at(
            file,
            (uint64_t)IFS_PFS3_ROOT_SECTOR * IFS_PFS3_SECTOR_SIZE,
            sector,
            sizeof(sector)) != 0)
        return 0;
    if (ifs_pfs3_decode_root(sector, sizeof(sector), &root) != 0)
        return 0;
    if (ifs_pfs3_validate_root_record(
            &root, IFS_PFS3_SECTOR_SIZE,
            (uint32_t)media_blocks) != IFS_PFS3_MEDIA_OK)
        return 0;

    format = ifs_pfs3_classify_disk_type(root.disk_type);
    if (format == IFS_PFS3_FORMAT_INVALID)
        return 0;

    result->type = "pfs3";
    result->block_size = IFS_PFS3_SECTOR_SIZE;
    snprintf(result->version, sizeof(result->version),
             format == IFS_PFS3_FORMAT_PFS1 ? "PFS/1" : "PFS/2");
    copy_pfs3_label(root.disk_name, result->label);
    return 1;
}

static int probe_amiga_filesystem(
    IfsUserspaceFile *file,
    IfsProbeResult *result)
{
    unsigned char prefix[IFS_PROBE_PREFIX_BYTES];

    memset(result, 0, sizeof(*result));
    if (file->io.size_bytes < sizeof(prefix) ||
        read_exact_at(file, 0U, prefix, sizeof(prefix)) != 0)
        return 0;

    if (probe_ofs_ffs(prefix, result))
        return 1;
    if (probe_sfs(file, prefix, result))
        return 1;
    if (probe_sfs2(file, prefix, result))
        return 1;
    if (probe_pfs3(file, result))
        return 1;
    return 0;
}

static void print_udev_result(const IfsProbeResult *result)
{
    printf("ID_FS_USAGE=filesystem\n");
    printf("ID_FS_TYPE=%s\n", result->type);
    if (result->version[0] != '\0')
        printf("ID_FS_VERSION=%s\n", result->version);
    if (result->label[0] != '\0')
        printf("ID_FS_LABEL=%s\n", result->label);
    if (result->block_size != 0U)
        printf("ID_FS_BLOCK_SIZE=%" PRIu32 "\n", result->block_size);
}

int main(int argc, char **argv)
{
    IfsUserspaceFile file;
    IfsProbeResult result;
    IfsStatus status;
    const char *target;
    int udev_mode = 0;
    int identified;

    if (argc == 2) {
        target = argv[1];
    } else if (argc == 3 && strcmp(argv[1], "--udev") == 0) {
        udev_mode = 1;
        target = argv[2];
    } else {
        fprintf(stderr,
                "usage: fsinspect [--udev] <image-or-block-device>\n");
        return 2;
    }

    status = ifs_userspace_file_open_readonly(target, &file);
    if (status != IFS_OK) {
        if (!udev_mode)
            fprintf(stderr, "fsinspect: open failed: %s\n",
                    ifs_status_string(status));
        return 1;
    }

    identified = probe_amiga_filesystem(&file, &result);
    if (!identified) {
        if (!udev_mode)
            printf("filesystem=unknown\nsize_bytes=%" PRIu64 "\n",
                   (uint64_t)file.io.size_bytes);
        ifs_userspace_file_close(&file);
        return 1;
    }

    if (udev_mode) {
        print_udev_result(&result);
    } else {
        printf("filesystem=%s\n", result.type);
        printf("version=%s\n", result.version);
        printf("block_size=%" PRIu32 "\n", result.block_size);
        if (result.label[0] != '\0')
            printf("label=%s\n", result.label);
        printf("size_bytes=%" PRIu64 "\n", (uint64_t)file.io.size_bytes);
    }

    ifs_userspace_file_close(&file);
    return 0;
}
