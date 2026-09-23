/*
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#ifndef __BITFUNCS_H
#define __BITFUNCS_H

#include <linux/types.h>
#include "../core/sfs_core.h"
#include <asm/byteorder.h>

#include <asm/bitops.h>
#include <linux/bitops.h>

/* Portable SFS bitmap semantics live in the canonical core. */
static inline int bfffo(u32 data, int bitoffset)
{
	return ifs_sfs_bitmap_word_find_set(
		data, bitoffset < 0 ? 32U : (ifs_sfs_u32)bitoffset);
}

static inline int bfffz(u32 data, int bitoffset)
{
	return ifs_sfs_bitmap_word_find_zero(
		data, bitoffset < 0 ? 32U : (ifs_sfs_u32)bitoffset);
}

static inline u32 bfset(u32 data, int bitoffset, int bits)
{
	return ifs_sfs_bitmap_word_set(
		data,
		bitoffset < 0 ? 32U : (ifs_sfs_u32)bitoffset,
		bits < 0 ? 0U : (ifs_sfs_u32)bits);
}

static inline u32 bfclr(u32 data, int bitoffset, int bits)
{
	return ifs_sfs_bitmap_word_clear(
		data,
		bitoffset < 0 ? 32U : (ifs_sfs_u32)bitoffset,
		bits < 0 ? 0U : (ifs_sfs_u32)bits);
}

/* bm??? functions assumes that in-memory bitmap is in bigendian byte order */
int bmffo(u32 *, int, int);
int bmffz(u32 *, int, int);
int bmclr(u32 *, int, int, int);
int bmset(u32 *, int, int, int);

#endif
