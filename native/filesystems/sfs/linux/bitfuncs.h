#ifndef INFILTRATOR_SFS_LINUX_BITFUNCS_H
#define INFILTRATOR_SFS_LINUX_BITFUNCS_H

#include <linux/types.h>
#include "../core/sfs_core.h"

static inline int bfffo(u32 data, int bitoffset)
{
    if (bitoffset < 0)
        return -1;
    return ifs_sfs_bitmap_word_find_set(data, (ifs_sfs_u32)bitoffset);
}

static inline int bfffz(u32 data, int bitoffset)
{
    if (bitoffset < 0)
        return -1;
    return ifs_sfs_bitmap_word_find_zero(data, (ifs_sfs_u32)bitoffset);
}

static inline u32 bfset(u32 data, int bitoffset, int bits)
{
    if (bitoffset < 0 || bits < 0)
        return data;
    return ifs_sfs_bitmap_word_set(
        data, (ifs_sfs_u32)bitoffset, (ifs_sfs_u32)bits);
}

static inline u32 bfclr(u32 data, int bitoffset, int bits)
{
    if (bitoffset < 0 || bits < 0)
        return data;
    return ifs_sfs_bitmap_word_clear(
        data, (ifs_sfs_u32)bitoffset, (ifs_sfs_u32)bits);
}

int bmffo(u32 *bitmap, int longs, int bitoffset);
int bmffz(u32 *bitmap, int longs, int bitoffset);
int bmclr(u32 *bitmap, int longs, int bitoffset, int bits);
int bmset(u32 *bitmap, int longs, int bitoffset, int bits);

#endif
