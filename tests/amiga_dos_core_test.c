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
    const ifs_amiga_u32 checksum = 0xfffffff9U;

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

    return 0;
}
