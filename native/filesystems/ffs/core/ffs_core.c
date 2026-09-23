/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ffs_core.h"
int ifs_ffs_classify_dostype(
    const ifs_ffs_u32 dostype, ifs_ffs_u32 *const variant_flags)
{
    ifs_ffs_u32 flags = 0U;
    if (variant_flags == 0) return -1;
    switch (dostype) {
    case IFS_FFS_DOS_FFS:
        break;
    case IFS_FFS_MUFS_FFS:
        flags |= IFS_FFS_VARIANT_MUFS;
        break;
    case IFS_FFS_MUFS_GENERIC:
    case IFS_FFS_MUFS_INTL_FFS:
        flags |= IFS_FFS_VARIANT_MUFS;
        /* fall through */
    case IFS_FFS_DOS_INTL_FFS:
        flags |= IFS_FFS_VARIANT_INTL;
        break;
    case IFS_FFS_MUFS_DC_FFS:
        flags |= IFS_FFS_VARIANT_MUFS;
        /* fall through */
    case IFS_FFS_DOS_DC_FFS:
        flags |= IFS_FFS_VARIANT_INTL | IFS_FFS_VARIANT_DIRCACHE;
        break;
    default:
        *variant_flags = 0U;
        return -1;
    }
    *variant_flags = flags;
    return 0;
}
