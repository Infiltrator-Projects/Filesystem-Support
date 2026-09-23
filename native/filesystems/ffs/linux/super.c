#include "affs.h"
#include "../core/ffs_core.h"

#define IFS_AMIGA_FS_NAME "ffs"
#define IFS_AMIGA_FORMAT_LABEL "FFS"
#define IFS_AMIGA_INODE_CACHE_NAME "ffs_inode_cache"
#define IFS_AMIGA_MODULE_DESCRIPTION "Amiga FFS filesystem support for Linux"
#define IFS_AMIGA_IS_OFS 0
#define IFS_AMIGA_VARIANT_DIRCACHE IFS_FFS_VARIANT_DIRCACHE
#define IFS_AMIGA_VARIANT_MUFS IFS_FFS_VARIANT_MUFS
#define IFS_AMIGA_VARIANT_INTL IFS_FFS_VARIANT_INTL

static int ifs_amiga_variant_classify(u32 dostype, u32 *flags)
{
    ifs_ffs_u32 value = 0U;
    const int result = ifs_ffs_classify_dostype(dostype, &value);

    *flags = value;
    return result;
}

#include "../../amiga_common/linux/super_adapter.c"
