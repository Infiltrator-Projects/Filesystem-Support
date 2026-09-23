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

    if (ifs_pfs3_validate_root_geometry(
            IFS_PFS3_DISK_PFS2, IFS_PFS3_MODE_HARDDISK,
            512U, 1024U, 2U, 2U,
            2U + 2U * IFS_PFS3_MAX_RESERVED_BLOCKS + 1U,
            20U) != IFS_PFS3_ROOT_RESERVED_COUNT_OUT_OF_RANGE)
        return fail("oversized reserved area accepted");
    if (ifs_pfs3_validate_root_geometry(
            IFS_PFS3_DISK_PFS2, IFS_PFS3_MODE_HARDDISK,
            512U, 2048U, 2U, 2U, 129U, 20U) !=
        IFS_PFS3_ROOT_INVALID_ROOT_CLUSTER_ALIGNMENT)
        return fail("misaligned PFS root cluster accepted");
    {
        unsigned char name[IFS_PFS3_DISK_NAME_BYTES] = {0};
        name[0]=4U; name[1]='T'; name[2]='e'; name[3]='s'; name[4]='t';
        if (ifs_pfs3_validate_disk_name(name) != 0)
            return fail("valid PFS disk name rejected");
        name[2]=':';
        if (ifs_pfs3_validate_disk_name(name) == 0)
            return fail("invalid PFS disk name accepted");
    }


    if (ifs_pfs3_validate_allocation_counts(1000U, 50U) != 0)
        return fail("valid PFS allocation counters rejected");
    if (ifs_pfs3_validate_allocation_counts(50U, 51U) == 0)
        return fail("PFS allocation reserve underflow accepted");


    {
        unsigned char raw[IFS_PFS3_ROOT_MIN_BYTES] = {0};
        IfsPfs3RootRecord root;
        raw[0]=0x50; raw[1]=0x46; raw[2]=0x53; raw[3]=0x02;
        raw[7]=IFS_PFS3_MODE_HARDDISK;
        raw[20]=4U; raw[21]='T'; raw[22]='e'; raw[23]='s'; raw[24]='t';
        raw[55]=65U;
        raw[59]=2U;
        raw[63]=20U;
        raw[64]=0x04; raw[65]=0x00;
        raw[67]=2U;
        raw[71]=100U;
        raw[75]=5U;
        raw[91]=70U;
        if (ifs_pfs3_decode_root(raw, sizeof(raw), &root) != 0)
            return fail("PFS root decoder failed");
        if (root.disk_type != IFS_PFS3_DISK_PFS2 ||
            root.options != IFS_PFS3_MODE_HARDDISK ||
            root.last_reserved != 65U ||
            root.first_reserved != 2U ||
            root.reserved_free != 20U ||
            root.reserved_block_size != 1024U ||
            root.root_block_cluster != 2U ||
            root.blocks_free != 100U ||
            root.always_free != 5U ||
            root.extension != 70U)
            return fail("PFS root decoder returned wrong fields");
        if (ifs_pfs3_validate_disk_name(root.disk_name) != 0)
            return fail("decoded PFS disk name rejected");
    }


    {
        ifs_pfs3_u16 effective = 0U;
        if (ifs_pfs3_effective_filename_size(0U, &effective) != 0 ||
            effective != IFS_PFS3_DEFAULT_FILENAME_SIZE)
            return fail("PFS zero fnsize default failed");
        if (ifs_pfs3_effective_filename_size(107U, &effective) != 0 ||
            effective != 107U)
            return fail("PFS 107-character fnsize rejected");
        if (ifs_pfs3_effective_filename_size(108U, &effective) == 0)
            return fail("PFS oversized fnsize accepted");
        if (ifs_pfs3_effective_filename_size(29U, &effective) == 0)
            return fail("PFS undersized fnsize accepted");
    }


    {
        unsigned char raw[IFS_PFS3_EXTENSION_MIN_BYTES] = {0};
        IfsPfs3ExtensionRecord extension;
        ifs_pfs3_u16 effective = 0U;

        raw[0] = 0x45; raw[1] = 0x58;
        raw[47] = 5U;
        raw[49] = 3U;
        raw[53] = 30U;
        raw[55] = 2U;
        raw[57] = 107U;

        if (ifs_pfs3_decode_extension(raw, sizeof(raw), &extension) != 0)
            return fail("PFS extension decoder failed");
        if (ifs_pfs3_validate_extension(&extension, 100U, &effective) != 0 ||
            effective != 107U)
            return fail("valid PFS extension rejected");

        extension.delete_directory_roving = 62U;
        if (ifs_pfs3_validate_extension(&extension, 100U, &effective) == 0)
            return fail("out-of-range PFS deldir roving accepted");

        extension.delete_directory_roving = 0U;
        extension.roving_bit = 32U;
        if (ifs_pfs3_validate_extension(&extension, 100U, &effective) == 0)
            return fail("out-of-range PFS bitmap roving bit accepted");
    }

    return 0;
}
