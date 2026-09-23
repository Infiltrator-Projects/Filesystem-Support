/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "sfs2_core.h"
#include <stdio.h>

static int fail(const char *message)
{
    fprintf(stderr, "sfs2 core test: %s\n", message);
    return 1;
}

int main(void)
{
    ifs_sfs2_u32 high = 0U;
    ifs_sfs2_u16 low = 0U;
    const ifs_sfs2_u64 size = 0x0000123456789ABCULL;

    if (IFS_SFS2_OBJECT_FIXED_SIZE != 27U ||
        IFS_SFS2_EXTENT_NODE_SIZE != 16U)
        return fail("SFS2 structure sizes are wrong");

    if (ifs_sfs2_validate_root_layout(
            IFS_SFS2_ROOT_ID, IFS_SFS2_STRUCTURE_VERSION,
            512U, 100000U, 3U, 4U, 10U, 5U, 6U) != IFS_SFS2_ROOT_OK)
        return fail("valid SFS2 root layout rejected");

    if (ifs_sfs2_validate_root_layout(
            0x53465300U, 3U,
            512U, 100000U, 3U, 4U, 10U, 5U, 6U) == IFS_SFS2_ROOT_OK)
        return fail("SFS0 was accepted as SFS2");

    if (ifs_sfs2_encode_file_size(size, &high, &low) != 0)
        return fail("valid 48-bit file size rejected");

    if (ifs_sfs2_decode_file_size(high, low) != size)
        return fail("48-bit file size roundtrip failed");

    if (ifs_sfs2_encode_file_size(
            IFS_SFS2_MAX_FILE_SIZE + 1U, &high, &low) == 0)
        return fail("oversized SFS2 file size accepted");

    return 0;
}
