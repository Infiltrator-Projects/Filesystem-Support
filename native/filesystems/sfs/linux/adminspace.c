/*
 *
 * Amiga Smart File System, Linux implementation
 * version: 1.0beta7
 *
 * This file contains some parts of the original amiga version of 
 * SmartFilesystem source code.
 *
 * SmartFilesystem is copyrighted (C) 2003 by: John Hendrikx, 
 * Ralph Schmidt, Emmanuel Lesueur, David Gerber, and Marcin Kurek
 * 
 * Adapted and modified by Marek 'March' Szyprowski <marek@amiga.pl>
 *
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include "asfs_fs.h"
#include "bitfuncs.h"

#include <asm/byteorder.h>

#ifdef CONFIG_ASFS_RW

static int setfreeblocks(struct super_block *sb, u32 freeblocks)
{
	struct buffer_head *bh;
	if ((bh = asfs_breadcheck(sb, ASFS_SB(sb)->rootobjectcontainer, ASFS_OBJECTCONTAINER_ID))) {
		struct fsRootInfo *ri = (struct fsRootInfo *) ((u8 *) bh->b_data + sb->s_blocksize - sizeof(struct fsRootInfo));
		ASFS_SB(sb)->freeblocks = freeblocks;
		ri->freeblocks = cpu_to_be32(freeblocks);
		asfs_bstore(sb, bh);
		asfs_brelse(bh);
		return 0;
	}
	return -EIO;
}

static inline int enoughspace(struct super_block *sb, u32 blocks)
{
	return ifs_sfs_has_allocation_headroom(
		ASFS_SB(sb)->freeblocks, blocks, ASFS_ALWAYSFREE) ?
		TRUE : FALSE;
}

	/* Determines the amount of free blocks starting from block /block/.
	   If there are no blocks found or if there was an error -1 is returned,
	   otherwise this function will count the number of free blocks until
	   an allocated block is encountered or until maxneeded has been
	   exceeded. */

static int availablespace(struct super_block *sb, u32 block, u32 maxneeded)
{
	struct buffer_head *bh = NULL;
	struct fsBitmap *b;
	u32 longs = ASFS_SB(sb)->blocks_inbitmap >> 5;
	u32 maxbitmapblock = ASFS_SB(sb)->bitmapbase + ASFS_SB(sb)->blocks_bitmap;
	int blocksfound = 0;
	u32 bitstart;
	int bitend;
	u32 nextblock = ASFS_SB(sb)->bitmapbase + block / ASFS_SB(sb)->blocks_inbitmap;

	bitstart = block % ASFS_SB(sb)->blocks_inbitmap;

	while (nextblock < maxbitmapblock && (bh = asfs_breadcheck(sb, nextblock++, ASFS_BITMAP_ID))) {
		b = (void *) bh->b_data;

		if ((bitend = bmffz(b->bitmap, longs, bitstart)) >= 0) {
			blocksfound += bitend - bitstart;
			asfs_brelse(bh);
			return blocksfound;
		}
		blocksfound += ASFS_SB(sb)->blocks_inbitmap - bitstart;
		if (blocksfound >= maxneeded) {
			asfs_brelse(bh);
			return blocksfound;
		}
		bitstart = 0;
		asfs_brelse(bh);
	}

	if (bh == NULL)
		return (-1);

	return (blocksfound);
}

int asfs_findspace(struct super_block *sb, u32 maxneeded, u32 start, u32 end, u32 * returned_block, u32 * returned_blocks)
{
	struct buffer_head *bh;
	u32 longs = ASFS_SB(sb)->blocks_inbitmap >> 5;
	u32 space = 0;
	u32 block;
	u32 bitmapblock = ASFS_SB(sb)->bitmapbase + start / ASFS_SB(sb)->blocks_inbitmap;
	u32 breakpoint;
	int bitstart, bitend;
	int reads;

	if (enoughspace(sb, maxneeded) == FALSE) {
		*returned_block = 0;
		*returned_blocks = 0;
		return -ENOSPC;
	}

	if (start >= ASFS_SB(sb)->totalblocks)
		start -= ASFS_SB(sb)->totalblocks;

	if (end == 0)
		end = ASFS_SB(sb)->totalblocks;

	reads = ((end - 1) / ASFS_SB(sb)->blocks_inbitmap) + 1 - start / ASFS_SB(sb)->blocks_inbitmap;

	if (start >= end)
		reads += (ASFS_SB(sb)->totalblocks - 1) / ASFS_SB(sb)->blocks_inbitmap + 1;

	breakpoint = (start < end ? end : ASFS_SB(sb)->totalblocks);

	*returned_block = 0;
	*returned_blocks = 0;

	bitend = start % ASFS_SB(sb)->blocks_inbitmap;
	block = start - bitend;

	while ((bh = asfs_breadcheck(sb, bitmapblock++, ASFS_BITMAP_ID))) {
		struct fsBitmap *b = (void *) bh->b_data;
		u32 localbreakpoint = breakpoint - block;

		if (localbreakpoint > ASFS_SB(sb)->blocks_inbitmap)
			localbreakpoint = ASFS_SB(sb)->blocks_inbitmap;

		/* At this point space contains the amount of free blocks at
		   the end of the previous bitmap block.  If there are no
		   free blocks at the start of this bitmap block, space will
		   be set to zero, since in that case the space isn't adjacent. */

		while ((bitstart = bmffo(b->bitmap, longs, bitend)) < ASFS_SB(sb)->blocks_inbitmap) {
			/* found the start of an empty space, now find out how large it is */

			if (bitstart >= localbreakpoint)
				break;

			if (bitstart != 0)
				space = 0;

			bitend = bmffz(b->bitmap, longs, bitstart);

			if (bitend > localbreakpoint)
				bitend = localbreakpoint;

			space += bitend - bitstart;

			if (*returned_blocks < space) {
				*returned_block = block + bitend - space;
				if (space >= maxneeded) {
					*returned_blocks = maxneeded;
					asfs_brelse(bh);
					return 0;
				}
				*returned_blocks = space;
			}

			if (bitend >= localbreakpoint)
				break;
		}

		if (--reads == 0)
			break;

		/* no (more) empty spaces found in this block */

		if (bitend != ASFS_SB(sb)->blocks_inbitmap)
			space = 0;

		bitend = 0;
		block += ASFS_SB(sb)->blocks_inbitmap;

		if (block >= ASFS_SB(sb)->totalblocks) {
			block = 0;
			space = 0;
			breakpoint = end;
			bitmapblock = ASFS_SB(sb)->bitmapbase;
		}
		asfs_brelse(bh);
	}

	if (bh == NULL)
		return -EIO;

	asfs_brelse(bh);

	if (*returned_blocks == 0)
		return -ENOSPC;
	else
		return 0;
}

static int asfs_update_bitmap_range(
	struct super_block *sb, u32 block, u32 blocks, int allocate)
{
	struct buffer_head **buffers;
	u32 first_bitmap;
	u32 last_bitmap;
	u32 buffer_count;
	u32 new_freeblocks;
	u32 index;
	int errorcode = 0;

	if (blocks == 0U || ASFS_SB(sb)->blocks_inbitmap == 0U ||
	    block >= ASFS_SB(sb)->totalblocks ||
	    blocks > ASFS_SB(sb)->totalblocks - block)
		return -EINVAL;

	first_bitmap = block / ASFS_SB(sb)->blocks_inbitmap;
	last_bitmap =
		(block + blocks - 1U) / ASFS_SB(sb)->blocks_inbitmap;
	if (last_bitmap >= ASFS_SB(sb)->blocks_bitmap)
		return -EUCLEAN;
	buffer_count = last_bitmap - first_bitmap + 1U;

	buffers = kcalloc(buffer_count, sizeof(*buffers), GFP_NOFS);
	if (!buffers)
		return -ENOMEM;

	for (index = 0U; index < buffer_count; ++index) {
		buffers[index] = asfs_breadcheck(
			sb, ASFS_SB(sb)->bitmapbase + first_bitmap + index,
			ASFS_BITMAP_ID);
		if (!buffers[index]) {
			errorcode = -EIO;
			goto out_release;
		}
	}

	/*
	 * Verify the complete range before changing any bitmap word. SFS uses
	 * one for free and zero for allocated, so this also detects double
	 * allocation and double free without trusting the cached free counter.
	 */
	for (index = 0U; index < blocks; ++index) {
		u32 logical = block + index;
		u32 bitmap_index =
			logical / ASFS_SB(sb)->blocks_inbitmap - first_bitmap;
		u32 bit_index = logical % ASFS_SB(sb)->blocks_inbitmap;
		u32 word_index = bit_index >> 5;
		u32 bit_in_word = bit_index & 31U;
		struct fsBitmap *bitmap =
			(void *)buffers[bitmap_index]->b_data;
		u32 word = be32_to_cpu(bitmap->bitmap[word_index]);
		u32 mask = 1U << (31U - bit_in_word);
		int is_free = (word & mask) != 0U;

		if ((allocate && !is_free) || (!allocate && is_free)) {
			errorcode = -EUCLEAN;
			goto out_release;
		}
	}

	if (allocate) {
		if (ifs_sfs_free_count_after_allocate(
				ASFS_SB(sb)->freeblocks, blocks,
				&new_freeblocks) != 0) {
			errorcode = -EUCLEAN;
			goto out_release;
		}
	} else {
		if (ifs_sfs_free_count_after_release(
				ASFS_SB(sb)->freeblocks, blocks,
				ASFS_SB(sb)->totalblocks,
				&new_freeblocks) != 0) {
			errorcode = -EUCLEAN;
			goto out_release;
		}
	}

	for (index = 0U; index < blocks; ++index) {
		u32 logical = block + index;
		u32 bitmap_index =
			logical / ASFS_SB(sb)->blocks_inbitmap - first_bitmap;
		u32 bit_index = logical % ASFS_SB(sb)->blocks_inbitmap;
		u32 word_index = bit_index >> 5;
		u32 bit_in_word = bit_index & 31U;
		struct fsBitmap *bitmap =
			(void *)buffers[bitmap_index]->b_data;
		u32 word = be32_to_cpu(bitmap->bitmap[word_index]);
		u32 mask = 1U << (31U - bit_in_word);

		if (allocate)
			word &= ~mask;
		else
			word |= mask;
		bitmap->bitmap[word_index] = cpu_to_be32(word);
	}
	for (index = 0U; index < buffer_count; ++index)
		asfs_bstore(sb, buffers[index]);

	errorcode = setfreeblocks(sb, new_freeblocks);
	if (errorcode != 0) {
		/* Roll the in-memory/dirtied bitmap back while all buffers are held. */
		for (index = 0U; index < blocks; ++index) {
			u32 logical = block + index;
			u32 bitmap_index =
				logical / ASFS_SB(sb)->blocks_inbitmap - first_bitmap;
			u32 bit_index = logical % ASFS_SB(sb)->blocks_inbitmap;
			u32 word_index = bit_index >> 5;
			u32 bit_in_word = bit_index & 31U;
			struct fsBitmap *bitmap =
				(void *)buffers[bitmap_index]->b_data;
			u32 word = be32_to_cpu(bitmap->bitmap[word_index]);
			u32 mask = 1U << (31U - bit_in_word);

			if (allocate)
				word |= mask;
			else
				word &= ~mask;
			bitmap->bitmap[word_index] = cpu_to_be32(word);
		}
		for (index = 0U; index < buffer_count; ++index)
			asfs_bstore(sb, buffers[index]);
	}

out_release:
	for (index = 0U; index < buffer_count; ++index)
		if (buffers[index])
			asfs_brelse(buffers[index]);
	kfree(buffers);
	return errorcode;
}

int asfs_markspace(struct super_block *sb, u32 block, u32 blocks)
{
	asfs_debug("markspace: Marking %u blocks from block %u\n",
		   blocks, block);

	if (!ifs_sfs_has_allocation_headroom(
			ASFS_SB(sb)->freeblocks, blocks, ASFS_ALWAYSFREE))
		return -ENOSPC;

	return asfs_update_bitmap_range(sb, block, blocks, TRUE);
}
	/* This function checks the bitmap and tries to locate at least /blocksneeded/
	   adjacent unused blocks.  If found it sets returned_block to the start block
	   and returns no error.  If not found, ERROR_DISK_IS_FULL is returned and
	   returned_block is set to zero.  Any other errors are returned as well. */

static inline int internalfindspace(struct super_block *sb, u32 blocksneeded, u32 startblock, u32 endblock, u32 * returned_block)
{
	u32 blocks;
	int errorcode;

	if ((errorcode = asfs_findspace(sb, blocksneeded, startblock, endblock, returned_block, &blocks)) == 0)
		if (blocks != blocksneeded)
			return -ENOSPC;

	return errorcode;
}

static int findandmarkspace(struct super_block *sb, u32 blocksneeded, u32 * returned_block)
{
	int errorcode;

	if (enoughspace(sb, blocksneeded) != FALSE) {
		if ((errorcode = internalfindspace(sb, blocksneeded, 0, ASFS_SB(sb)->totalblocks, returned_block)) == 0)
			errorcode = asfs_markspace(sb, *returned_block, blocksneeded);
	} else
		errorcode = -ENOSPC;

	return (errorcode);
}

/* ************************** */

int asfs_freespace(struct super_block *sb, u32 block, u32 blocks)
{
	asfs_debug("freespace: Freeing %u blocks from block %u\n",
		   blocks, block);
	return asfs_update_bitmap_range(sb, block, blocks, FALSE);
}
/*************** admin space containers ****************/

int asfs_allocadminspace(struct super_block *sb, u32 *returned_block)
{
	struct buffer_head *bh;
	u32 adminspaceblock = ASFS_SB(sb)->adminspacecontainer;
	int errorcode = -EIO;

	asfs_debug("allocadminspace: allocating new block\n");

	while ((bh = asfs_breadcheck(sb, adminspaceblock, ASFS_ADMINSPACECONTAINER_ID))) {
		struct fsAdminSpaceContainer *asc1 = (void *) bh->b_data;
		struct fsAdminSpace *as1 = asc1->adminspace;
		int adminspaces1 = (sb->s_blocksize - sizeof(struct fsAdminSpaceContainer)) / sizeof(struct fsAdminSpace);

		while (adminspaces1-- > 0) {
			s16 bitoffset;

			if (as1->space != 0 && (bitoffset = bfffz(be32_to_cpu(as1->bits), 0)) >= 0) {
				u32 emptyadminblock = be32_to_cpu(as1->space) + bitoffset;
				as1->bits |= cpu_to_be32(1U << (31 - bitoffset));
				asfs_bstore(sb, bh);
				*returned_block = emptyadminblock;
				asfs_brelse(bh);
				asfs_debug("allocadminspace: found block %d\n", *returned_block);
				return 0;
			}
			as1++;
		}

		adminspaceblock = be32_to_cpu(asc1->next);
		asfs_brelse(bh);

		if (adminspaceblock == 0) {
			u32 startblock;

			asfs_debug("allocadminspace: allocating new adminspace area\n");

			/* If we get here it means current adminspace areas are all filled.
			   We would now need to find a new area and create a fsAdminSpace
			   structure in one of the AdminSpaceContainer blocks.  If these
			   don't have any room left for new adminspace areas a new
			   AdminSpaceContainer would have to be created first which is
			   placed as the first block in the newly found admin area. */

			adminspaceblock = ASFS_SB(sb)->adminspacecontainer;

			if ((errorcode = findandmarkspace(sb, 32, &startblock)))
				return errorcode;

			while ((bh = asfs_breadcheck(sb, adminspaceblock, ASFS_ADMINSPACECONTAINER_ID))) {
				struct fsAdminSpaceContainer *asc2 = (void *) bh->b_data;
				struct fsAdminSpace *as2 = asc2->adminspace;
				int adminspaces2 = (sb->s_blocksize - sizeof(struct fsAdminSpaceContainer)) / sizeof(struct fsAdminSpace);

				while (adminspaces2-- > 0 && as2->space != 0)
					as2++;

				if (adminspaces2 >= 0) {	/* Found a unused AdminSpace in this AdminSpaceContainer! */
					as2->space = cpu_to_be32(startblock);
					as2->bits = 0;
					asfs_bstore(sb, bh);
					asfs_brelse(bh);
					break;
				}

				if (asc2->next == 0) {
					/* Oh-oh... we marked our new adminspace area in use, but we couldn't
					   find space to store a fsAdminSpace structure in the existing
					   fsAdminSpaceContainer blocks.  This means we need to create and
					   link a new fsAdminSpaceContainer as the first block in our newly
					   marked adminspace. */

					asc2->next = cpu_to_be32(startblock);
					asfs_bstore(sb, bh);
					asfs_brelse(bh);

					/* Now preparing new AdminSpaceContainer */

					if ((bh = asfs_getzeroblk(sb, startblock)) == NULL)
						return -EIO;

					asc2 = (void *) bh->b_data;
					asc2->bheader.id = cpu_to_be32(ASFS_ADMINSPACECONTAINER_ID);
					asc2->bheader.ownblock = cpu_to_be32(startblock);
					asc2->previous = cpu_to_be32(adminspaceblock);
					asc2->adminspace[0].space = cpu_to_be32(startblock);
					asc2->adminspace[0].bits = cpu_to_be32(0x80000000);
					asc2->bits = 32;

					asfs_bstore(sb, bh);
					asfs_brelse(bh);

					adminspaceblock = startblock;
					break;	/* Breaks through to outer loop! */
				}
				adminspaceblock = be32_to_cpu(asc2->next);
				asfs_brelse(bh);
			}
		}
	}
	return errorcode;
}

int asfs_freeadminspace(struct super_block *sb, u32 block)
{
	struct buffer_head *bh;
	u32 adminspaceblock = ASFS_SB(sb)->adminspacecontainer;

	asfs_debug("freeadminspace: Entry -- freeing block %d\n", block);

	while ((bh = asfs_breadcheck(sb, adminspaceblock, ASFS_ADMINSPACECONTAINER_ID))) {
		struct fsAdminSpaceContainer *asc = (void *) bh->b_data;
		struct fsAdminSpace *as = asc->adminspace;
		int adminspaces = (sb->s_blocksize - sizeof(struct fsAdminSpaceContainer)) / sizeof(struct fsAdminSpace);

		while (adminspaces-- > 0) {
			u32 mask;

			if (ifs_sfs_adminspace_block_mask(
					be32_to_cpu(as->space), block, &mask) == 0) {
				u32 bits = be32_to_cpu(as->bits);

				if ((bits & mask) == 0U) {
					asfs_brelse(bh);
					return -EUCLEAN;
				}
				asfs_debug("freeadminspace: Block to be freed is located in AdminSpaceContainer block at %d\n", adminspaceblock);
				as->bits = cpu_to_be32(bits & ~mask);
				asfs_bstore(sb, bh);
				asfs_brelse(bh);
				return 0;
			}
			as++;
		}

		if ((adminspaceblock = be32_to_cpu(asc->next)) == 0)
			break;

		asfs_brelse(bh);
	}

	if (bh != NULL) {
		asfs_brelse(bh);
		printk("ASFS: Unable to free an administration block. The block cannot be found.");
		return -ENOENT;
	}

	return -EIO;
}

#endif
