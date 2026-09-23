#ifndef INFILTRATOR_FFS_CORE_H
#define INFILTRATOR_FFS_CORE_H
#if defined(__KERNEL__)
#include <linux/types.h>
typedef u32 ifs_ffs_u32;
#else
#include <stdint.h>
typedef uint32_t ifs_ffs_u32;
#endif
#define IFS_FFS_DOS_FFS       0x444F5301U
#define IFS_FFS_DOS_INTL_FFS  0x444F5303U
#define IFS_FFS_DOS_DC_FFS    0x444F5305U
#define IFS_FFS_MUFS_GENERIC  0x6d754653U
#define IFS_FFS_MUFS_FFS      0x6d754601U
#define IFS_FFS_MUFS_INTL_FFS 0x6d754603U
#define IFS_FFS_MUFS_DC_FFS   0x6d754605U
#define IFS_FFS_VARIANT_MUFS      0x01U
#define IFS_FFS_VARIANT_INTL      0x02U
#define IFS_FFS_VARIANT_DIRCACHE  0x04U
int ifs_ffs_classify_dostype(ifs_ffs_u32 dostype, ifs_ffs_u32 *variant_flags);
#endif
