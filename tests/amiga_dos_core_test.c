/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "amiga_dos_core.h"
#include <stdio.h>

static int fail(const char *message)
{
    fprintf(stderr, "amiga dos core test: %s\n", message);
    return 1;
}

int main(void)
{
    static const ifs_amiga_u8 normal[] = "ReadMe";
    static const ifs_amiga_u8 bad[] = "bad:name";
    ifs_amiga_u8 block[24] = { 0 };
    const ifs_amiga_u32 checksum = 0xfffffffaU;

    if (ifs_amiga_validate_name(normal, 6U, 1) != IFS_AMIGA_NAME_OK)
        return fail("valid name rejected");

    if (ifs_amiga_validate_name(bad, 8U, 1) !=
        IFS_AMIGA_NAME_INVALID_CHARACTER)
        return fail("colon-containing name accepted");

    if (ifs_amiga_fold_character((ifs_amiga_u8)'z', 0) !=
        (ifs_amiga_u8)'Z')
        return fail("ASCII case fold failed");

    if (ifs_amiga_fold_character(0xe4U, 1) != 0xc4U)
        return fail("international case fold failed");

    if (ifs_amiga_fold_character(0xf7U, 1) != 0xf7U)
        return fail("division sign was incorrectly folded");

    if (ifs_amiga_directory_hash(normal, 6U, 72U, 0) >= 72U)
        return fail("directory hash escaped hash table");

    block[3] = 1U;
    block[7] = 2U;
    block[11] = 3U;
    if (ifs_amiga_block_checksum(block, sizeof(block)) != 6U)
        return fail("big-endian block checksum is wrong");

    if (ifs_amiga_checksum_word_value(block, sizeof(block), 5U) != checksum)
        return fail("checksum word calculation is wrong");


    {
        ifs_amiga_u32 bits = 0U;
        ifs_amiga_u32 count = 0U;

        if (ifs_amiga_data_block_valid(1U, 2U, 100U) != 0)
            return fail("reserved block accepted as data");
        if (ifs_amiga_data_block_valid(2U, 2U, 100U) == 0)
            return fail("first data block rejected");
        if (ifs_amiga_data_block_valid(100U, 2U, 100U) != 0)
            return fail("one-past-end block accepted");
        if (ifs_amiga_bitmap_geometry(
                512U, 2U, 10000U, &bits, &count) != 0 ||
            bits != 4064U || count != 3U)
            return fail("AmigaDOS bitmap geometry is wrong");
        if (ifs_amiga_bitmap_bit_mask(31U) != 0x80000000U)
            return fail("AmigaDOS bitmap bit mask is wrong");
        if (ifs_amiga_bitmap_scan_mask(4U) != 0xfffffff0U)
            return fail("AmigaDOS bitmap scan mask is wrong");
    }

    return 0;
}
