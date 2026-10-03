/* SPDX-License-Identifier: GPL-3.0-or-later */
#define _XOPEN_SOURCE 700
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define SECTOR_SIZE 512U
#define PFS3_ROOT_SECTOR 2U

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static int read_exact(int fd, off_t offset, unsigned char *buffer, size_t bytes)
{
    size_t done = 0U;

    while (done < bytes) {
        ssize_t rc = pread(fd, buffer + done, bytes - done,
                           offset + (off_t)done);
        if (rc == 0)
            return 0;
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        done += (size_t)rc;
    }
    return 1;
}

static void emit(const char *type, const char *version, const char *name,
                 int read_only)
{
    puts("ID_FS_USAGE=filesystem");
    printf("ID_FS_TYPE=%s\n", type);
    printf("ID_FS_VERSION=%s\n", version);
    printf("UDISKS_NAME=%s\n", name);
    puts("UDISKS_ICON_NAME=drive-harddisk");
    puts("UDISKS_SYMBOLIC_ICON_NAME=drive-harddisk-symbolic");
    puts("UDISKS_MOUNT_OPTIONS_DEFAULTS=nodev,nosuid");
    if (read_only)
        puts("UDISKS_MOUNT_OPTIONS_ALLOW=exec,noexec,nodev,nosuid,atime,noatime,nodiratime,relatime,strictatime,lazytime,ro,sync,dirsync,nosymfollow");
    else
        puts("UDISKS_MOUNT_OPTIONS_ALLOW=exec,noexec,nodev,nosuid,atime,noatime,nodiratime,relatime,strictatime,lazytime,ro,rw,sync,dirsync,nosymfollow");
}

int main(int argc, char **argv)
{
    unsigned char sector0[SECTOR_SIZE];
    unsigned char sector2[SECTOR_SIZE];
    uint32_t id;
    int fd;

    if (argc != 2 || argv[1][0] != '/')
        return 2;

    fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0)
        return 0;

    if (read_exact(fd, 0, sector0, sizeof(sector0)) == 1) {
        /* Amiga DOS boot-block IDs are DOS\0..DOS\7.  Even variants are
         * OFS-family and odd variants are FFS-family. */
        if (sector0[0] == 'D' && sector0[1] == 'O' &&
            sector0[2] == 'S' && sector0[3] <= 7U) {
            char version[16];
            snprintf(version, sizeof(version), "DOS\\%u",
                     (unsigned int)sector0[3]);
            if ((sector0[3] & 1U) == 0U)
                emit("ofs", version, "Amiga OFS", 0);
            else
                emit("ffs", version, "Amiga FFS", 0);
            close(fd);
            return 0;
        }

        id = be32(sector0);
        if (id == UINT32_C(0x53465300)) {
            emit("sfs", "SFS/1", "Amiga Smart File System", 0);
            close(fd);
            return 0;
        }
        if (id == UINT32_C(0x53465302)) {
            emit("sfs2", "SFS/2", "Amiga Smart File System 2", 0);
            close(fd);
            return 0;
        }
    }

    if (read_exact(fd, (off_t)PFS3_ROOT_SECTOR * SECTOR_SIZE,
                   sector2, sizeof(sector2)) == 1) {
        id = be32(sector2);
        if (id == UINT32_C(0x50465301) || id == UINT32_C(0x50465302)) {
            emit("pfs3", id == UINT32_C(0x50465301) ? "PFS\\1" : "PFS\\2",
                 "Amiga Professional File System", 1);
        }
    }

    close(fd);
    return 0;
}
