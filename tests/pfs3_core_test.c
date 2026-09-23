/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "pfs3_core.h"
#include <stdio.h>

static int fail(const char *message)
{
    fprintf(stderr, "pfs3 core test: %s\n", message);
    return 1;
}

int main(void)
{
    if (ifs_pfs3_classify_disk_type(IFS_PFS3_DISK_PFS1) !=
        IFS_PFS3_FORMAT_PFS1)
        return fail("PFS1 disk type not recognized");

    if (ifs_pfs3_classify_disk_type(IFS_PFS3_DISK_PFS2) !=
        IFS_PFS3_FORMAT_PFS2)
        return fail("PFS2 disk type not recognized");

    if (ifs_pfs3_classify_disk_type(0x50465303U) !=
        IFS_PFS3_FORMAT_INVALID)
        return fail("invented PFS3 disk type was accepted");

    if (ifs_pfs3_validate_root_geometry(
            IFS_PFS3_DISK_PFS1,
            IFS_PFS3_MODE_HARDDISK | IFS_PFS3_MODE_SPLITTED_ANODES,
            512U, 1024U, 2U, 2U, 65U, 20U) != IFS_PFS3_ROOT_OK)
        return fail("valid PFS1 geometry rejected");

    if (ifs_pfs3_validate_root_geometry(
            IFS_PFS3_DISK_PFS1,
            IFS_PFS3_MODE_HARDDISK | IFS_PFS3_MODE_LARGEFILE,
            512U, 1024U, 2U, 2U, 65U, 20U) !=
        IFS_PFS3_ROOT_CLASSIC_FEATURE_CONFLICT)
        return fail("PFS1 large-file conflict not rejected");

    if (ifs_pfs3_validate_root_geometry(
            IFS_PFS3_DISK_PFS2,
            IFS_PFS3_MODE_HARDDISK | IFS_PFS3_MODE_LARGEFILE,
            512U, 4096U, 2U, 2U, 257U, 20U) != IFS_PFS3_ROOT_OK)
        return fail("valid PFS2 geometry rejected");

    if (ifs_pfs3_validate_root_geometry(
            IFS_PFS3_DISK_PFS2, IFS_PFS3_MODE_HARDDISK,
            512U, 3072U, 2U, 2U, 257U, 20U) !=
        IFS_PFS3_ROOT_INVALID_RESERVED_BLOCK_SIZE)
        return fail("non-power-of-two reserved block size accepted");

    return 0;
}
