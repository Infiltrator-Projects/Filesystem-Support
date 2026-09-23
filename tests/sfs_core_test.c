/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "sfs_core.h"
#include <stdio.h>
static int fail(const char *m){ fprintf(stderr,"sfs core test: %s\n",m); return 1; }
int main(void)
{
    ifs_sfs_u32 cap=0U,count=0U;
    if (ifs_sfs_validate_root_layout(IFS_SFS_ROOT_ID,3U,512U,100000U,3U,4U,10U,5U,6U)!=IFS_SFS_ROOT_OK)
        return fail("valid root rejected");
    if (ifs_sfs_validate_root_layout(0U,3U,512U,100000U,3U,4U,10U,5U,6U)!=IFS_SFS_ROOT_BAD_ID)
        return fail("bad id accepted");
    if (ifs_sfs_validate_root_layout(IFS_SFS_ROOT_ID,4U,512U,100000U,3U,4U,10U,5U,6U)!=IFS_SFS_ROOT_BAD_VERSION)
        return fail("SFS2 accepted as SFS");
    if (ifs_sfs_validate_root_layout(IFS_SFS_ROOT_ID,3U,768U,100000U,3U,4U,10U,5U,6U)!=IFS_SFS_ROOT_INVALID_BLOCK_SIZE)
        return fail("invalid block size accepted");
    if (ifs_sfs_validate_root_layout(IFS_SFS_ROOT_ID,3U,512U,100U,100U,4U,10U,5U,6U)!=IFS_SFS_ROOT_BLOCK_REFERENCE_OUT_OF_RANGE)
        return fail("bad block reference accepted");
    if (ifs_sfs_validate_root_layout(IFS_SFS_ROOT_ID,3U,512U,100U,3U,4U,98U,5U,6U)!=IFS_SFS_ROOT_TRANSACTION_BLOCK_OUT_OF_RANGE)
        return fail("bad transaction block accepted");
    if (ifs_sfs_compute_bitmap_layout(512U,100000U,&cap,&count)!=0 || cap!=4000U || count!=25U)
        return fail("bitmap geometry wrong");
    return 0;
}
