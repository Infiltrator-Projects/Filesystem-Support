#include "ffs_core.h"
int ifs_ffs_classify_dostype(
    const ifs_ffs_u32 dostype, ifs_ffs_u32 *const variant_flags)
{
    if (variant_flags == 0)
        return -1;

    switch (dostype) {
    case IFS_FFS_DOS_FFS:
        *variant_flags = 0U;
        return 0;
    case IFS_FFS_DOS_INTL_FFS:
        *variant_flags = IFS_FFS_VARIANT_INTL;
        return 0;
    case IFS_FFS_DOS_DC_FFS:
        *variant_flags =
            IFS_FFS_VARIANT_INTL | IFS_FFS_VARIANT_DIRCACHE;
        return 0;
    case IFS_FFS_MUFS_FFS:
        *variant_flags = IFS_FFS_VARIANT_MUFS;
        return 0;
    case IFS_FFS_MUFS_GENERIC:
    case IFS_FFS_MUFS_INTL_FFS:
        *variant_flags =
            IFS_FFS_VARIANT_MUFS | IFS_FFS_VARIANT_INTL;
        return 0;
    case IFS_FFS_MUFS_DC_FFS:
        *variant_flags =
            IFS_FFS_VARIANT_MUFS |
            IFS_FFS_VARIANT_INTL |
            IFS_FFS_VARIANT_DIRCACHE;
        return 0;
    default:
        *variant_flags = 0U;
        return -1;
    }
}
