/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ofs_core.h"
int ifs_ofs_classify_dostype(
    const ifs_ofs_u32 dostype, ifs_ofs_u32 *const variant_flags)
{
    ifs_ofs_u32 flags = 0U;
    if (variant_flags == 0) return -1;
    switch (dostype) {
    case IFS_OFS_MUFS_OFS:
        flags |= IFS_OFS_VARIANT_MUFS;
        break;
    case IFS_OFS_DOS_OFS:
        break;
    case IFS_OFS_MUFS_DC_OFS:
        flags |= IFS_OFS_VARIANT_MUFS;
        /* fall through */
    case IFS_OFS_DOS_DC_OFS:
        flags |= IFS_OFS_VARIANT_DIRCACHE;
        /* fall through */
    case IFS_OFS_DOS_INTL_OFS:
        flags |= IFS_OFS_VARIANT_INTL;
        break;
    case IFS_OFS_MUFS_INTL_OFS:
        flags |= IFS_OFS_VARIANT_MUFS | IFS_OFS_VARIANT_INTL;
        break;
    default:
        *variant_flags = 0U;
        return -1;
    }
    *variant_flags = flags;
    return 0;
}
