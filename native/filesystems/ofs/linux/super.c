#include "affs.h"
#include "../core/ofs_core.h"

#define IFS_AMIGA_FS_NAME "ofs"
#define IFS_AMIGA_FORMAT_LABEL "OFS"
#define IFS_AMIGA_INODE_CACHE_NAME "ofs_inode_cache"
#define IFS_AMIGA_MODULE_DESCRIPTION "Amiga OFS filesystem support for Linux"
#define IFS_AMIGA_IS_OFS 1
#define IFS_AMIGA_VARIANT_DIRCACHE IFS_OFS_VARIANT_DIRCACHE
#define IFS_AMIGA_VARIANT_MUFS IFS_OFS_VARIANT_MUFS
#define IFS_AMIGA_VARIANT_INTL IFS_OFS_VARIANT_INTL

static int ifs_amiga_variant_classify(u32 dostype, u32 *flags)
{
    ifs_ofs_u32 value = 0U;
    const int result = ifs_ofs_classify_dostype(dostype, &value);

    *flags = value;
    return result;
}

#include "../../amiga_common/linux/super_adapter.c"
