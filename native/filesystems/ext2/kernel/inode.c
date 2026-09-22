// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ext2/inode.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/inode.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  Goal-directed block allocation by Stephen Tweedie
 * 	(sct@dcs.ed.ac.uk), 1993, 1998
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 * 	(jj@sunsite.ms.mff.cuni.cz)
 *
 *  Assorted race fixes, rewrite of ext2_get_block() by Al Viro, 2000
 */

/*
 * EXT2 — Inode mapping and lifecycle
 *
 * Purpose:
 *   Implements inode read/write conversion, logical-to-physical block mapping, truncation, address-space operations and inode lifecycle transitions.
 *
 * Filesystem model:
 *   This file belongs to a deliberately strict, non-journalled EXT2 VFS implementation.
 *
 * Correctness focus:
 *   The in-memory inode and its durable representation may advance at different times; crash-safe ordering and orphan handling must preserve a recoverable disk state.
 *
 * Project rules:
 *   - Do not accept a journalled EXT3 volume as EXT2.
 *   - Keep on-disk compatibility fields when they are required to parse or reject media correctly.
 *   - Keep xattr/ACL/cache code inside ext2.ko rather than creating helper modules.
 *
 * Commentary policy:
 *   Comments explain invariants, ownership, persistence ordering and
 *   non-obvious design intent. They deliberately avoid restating C syntax.
 */

#include <linux/time.h>
#include <linux/highuid.h>
#include <linux/pagemap.h>
#include <linux/dax.h>
#include <linux/blkdev.h>
#include <linux/quotaops.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/fiemap.h>
#include <linux/iomap.h>
#include <linux/namei.h>
#include <linux/uio.h>
#include "ext2.h"

static int __ext2_write_inode(struct inode *inode, int do_sync);


/**
 * ext2_inode_is_fast_symlink - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int ext2_inode_is_fast_symlink(struct inode *inode)
{
	int ea_blocks = EXT2_I(inode)->i_file_acl ?
		(inode->i_sb->s_blocksize >> 9) : 0;

	return (S_ISLNK(inode->i_mode) &&
		inode->i_blocks - ea_blocks == 0);
}

static void ext2_truncate_blocks(struct inode *inode, loff_t offset);

/**
 * ext2_write_failed - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext2_write_failed(struct address_space *mapping, loff_t to)
{
	struct inode *inode = mapping->host;

	if (to > inode->i_size) {
		truncate_pagecache(inode, inode->i_size);
		ext2_truncate_blocks(inode, inode->i_size);
	}
}


/**
 * ext2_evict_inode - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext2_evict_inode(struct inode * inode)
{
	struct ext2_block_alloc_info *rsv;
	int want_delete = 0;

	if (!inode->i_nlink && !is_bad_inode(inode)) {
		want_delete = 1;
		dquot_initialize(inode);
	} else {
		dquot_drop(inode);
	}

	truncate_inode_pages_final(&inode->i_data);

	if (want_delete) {
		sb_start_intwrite(inode->i_sb);

		EXT2_I(inode)->i_dtime	= ktime_get_real_seconds();
		mark_inode_dirty(inode);
		__ext2_write_inode(inode, inode_needs_sync(inode));

		inode->i_size = 0;
		if (inode->i_blocks)
			ext2_truncate_blocks(inode, 0);
		ext2_xattr_delete_inode(inode);
	}

	invalidate_inode_buffers(inode);
	clear_inode(inode);

	ext2_discard_reservation(inode);
	rsv = EXT2_I(inode)->i_block_alloc_info;
	EXT2_I(inode)->i_block_alloc_info = NULL;
	if (unlikely(rsv))
		kfree(rsv);

	if (want_delete) {
		ext2_free_inode(inode);
		sb_end_intwrite(inode->i_sb);
	}
}

typedef struct {
	__le32	*p;
	__le32	key;
	struct buffer_head *bh;
} Indirect;

/**
 * add_chain - Implements the add chain operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void add_chain(Indirect *p, struct buffer_head *bh, __le32 *v)
{
	p->key = *(p->p = v);
	p->bh = bh;
}

/**
 * verify_chain - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int verify_chain(Indirect *from, Indirect *to)
{
	while (from <= to && from->key == *from->p)
		from++;
	return (from > to);
}


/**
 * ext2_block_to_path - Implements the block to path operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext2_block_to_path(struct inode *inode,
			long i_block, int offsets[4], int *boundary)
{
	int ptrs = EXT2_ADDR_PER_BLOCK(inode->i_sb);
	int ptrs_bits = EXT2_ADDR_PER_BLOCK_BITS(inode->i_sb);
	const long direct_blocks = EXT2_NDIR_BLOCKS,
		indirect_blocks = ptrs,
		double_blocks = (1 << (ptrs_bits * 2));
	int n = 0;
	int final = 0;

	if (i_block < 0) {
		ext2_msg(inode->i_sb, KERN_WARNING,
			"warning: %s: block < 0", __func__);
	} else if (i_block < direct_blocks) {
		offsets[n++] = i_block;
		final = direct_blocks;
	} else if ( (i_block -= direct_blocks) < indirect_blocks) {
		offsets[n++] = EXT2_IND_BLOCK;
		offsets[n++] = i_block;
		final = ptrs;
	} else if ((i_block -= indirect_blocks) < double_blocks) {
		offsets[n++] = EXT2_DIND_BLOCK;
		offsets[n++] = i_block >> ptrs_bits;
		offsets[n++] = i_block & (ptrs - 1);
		final = ptrs;
	} else if (((i_block -= double_blocks) >> (ptrs_bits * 2)) < ptrs) {
		offsets[n++] = EXT2_TIND_BLOCK;
		offsets[n++] = i_block >> (ptrs_bits * 2);
		offsets[n++] = (i_block >> ptrs_bits) & (ptrs - 1);
		offsets[n++] = i_block & (ptrs - 1);
		final = ptrs;
	} else {
		ext2_msg(inode->i_sb, KERN_WARNING,
			"warning: %s: block is too big", __func__);
	}
	if (boundary)
		*boundary = final - 1 - (i_block & (ptrs - 1));

	return n;
}


/**
 * ext2_get_branch - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static Indirect *ext2_get_branch(struct inode *inode,
				 int depth,
				 int *offsets,
				 Indirect chain[4],
				 int *err)
{
	struct super_block *sb = inode->i_sb;
	Indirect *p = chain;
	struct buffer_head *bh;

	*err = 0;

	add_chain (chain, NULL, EXT2_I(inode)->i_data + *offsets);
	if (!p->key)
		goto no_block;
	while (--depth) {
		bh = sb_bread(sb, le32_to_cpu(p->key));
		if (!bh)
			goto failure;
		read_lock(&EXT2_I(inode)->i_meta_lock);
		if (!verify_chain(chain, p))
			goto changed;
		add_chain(++p, bh, (__le32*)bh->b_data + *++offsets);
		read_unlock(&EXT2_I(inode)->i_meta_lock);
		if (!p->key)
			goto no_block;
	}
	return NULL;

changed:
	read_unlock(&EXT2_I(inode)->i_meta_lock);
	brelse(bh);
	*err = -EAGAIN;
	goto no_block;
failure:
	*err = -EIO;
no_block:
	return p;
}


/**
 * ext2_find_near - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ext2_fsblk_t ext2_find_near(struct inode *inode, Indirect *ind)
{
	struct ext2_inode_info *ei = EXT2_I(inode);
	__le32 *start = ind->bh ? (__le32 *) ind->bh->b_data : ei->i_data;
	__le32 *p;
	ext2_fsblk_t bg_start;
	ext2_fsblk_t colour;


	for (p = ind->p - 1; p >= start; p--)
		if (*p)
			return le32_to_cpu(*p);


	if (ind->bh)
		return ind->bh->b_blocknr;


	bg_start = ext2_group_first_block_no(inode->i_sb, ei->i_block_group);
	colour = (current->pid % 16) *
			(EXT2_BLOCKS_PER_GROUP(inode->i_sb) / 16);
	return bg_start + colour;
}


/**
 * ext2_find_goal - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline ext2_fsblk_t ext2_find_goal(struct inode *inode, long block,
					  Indirect *partial)
{
	struct ext2_block_alloc_info *block_i;

	block_i = EXT2_I(inode)->i_block_alloc_info;


	if (block_i && (block == block_i->last_alloc_logical_block + 1)
		&& (block_i->last_alloc_physical_block != 0)) {
		return block_i->last_alloc_physical_block + 1;
	}

	return ext2_find_near(inode, partial);
}


/**
 * ext2_blks_to_allocate - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext2_blks_to_allocate(Indirect * branch, int k, unsigned long blks,
		int blocks_to_boundary)
{
	unsigned long count = 0;


	if (k > 0) {

		if (blks < blocks_to_boundary + 1)
			count += blks;
		else
			count += blocks_to_boundary + 1;
		return count;
	}

	count++;
	while (count < blks && count <= blocks_to_boundary
		&& le32_to_cpu(*(branch[0].p + count)) == 0) {
		count++;
	}
	return count;
}


/**
 * ext2_alloc_blocks - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext2_alloc_blocks(struct inode *inode,
			ext2_fsblk_t goal, int indirect_blks, int blks,
			ext2_fsblk_t new_blocks[4], int *err)
{
	int target, i;
	unsigned long count = 0;
	int index = 0;
	ext2_fsblk_t current_block = 0;
	int ret = 0;


	target = blks + indirect_blks;

	while (1) {
		count = target;

		current_block = ext2_new_blocks(inode, goal, &count, err, 0);
		if (*err)
			goto failed_out;

		target -= count;

		while (index < indirect_blks && count) {
			new_blocks[index++] = current_block++;
			count--;
		}

		if (count > 0)
			break;
	}


	new_blocks[index] = current_block;


	ret = count;
	*err = 0;
	return ret;
failed_out:
	for (i = 0; i <index; i++)
		ext2_free_blocks(inode, new_blocks[i], 1);
	if (index)
		mark_inode_dirty(inode);
	return ret;
}


/**
 * ext2_alloc_branch - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext2_alloc_branch(struct inode *inode,
			int indirect_blks, int *blks, ext2_fsblk_t goal,
			int *offsets, Indirect *branch)
{
	int blocksize = inode->i_sb->s_blocksize;
	int i, n = 0;
	int err = 0;
	struct buffer_head *bh;
	int num;
	ext2_fsblk_t new_blocks[4];
	ext2_fsblk_t current_block;

	num = ext2_alloc_blocks(inode, goal, indirect_blks,
				*blks, new_blocks, &err);
	if (err)
		return err;

	branch[0].key = cpu_to_le32(new_blocks[0]);


	for (n = 1; n <= indirect_blks;  n++) {


		bh = sb_getblk(inode->i_sb, new_blocks[n-1]);
		if (unlikely(!bh)) {
			err = -ENOMEM;
			goto failed;
		}
		branch[n].bh = bh;
		lock_buffer(bh);
		memset(bh->b_data, 0, blocksize);
		branch[n].p = (__le32 *) bh->b_data + offsets[n];
		branch[n].key = cpu_to_le32(new_blocks[n]);
		*branch[n].p = branch[n].key;
		if ( n == indirect_blks) {
			current_block = new_blocks[n];


			for (i=1; i < num; i++)
				*(branch[n].p + i) = cpu_to_le32(++current_block);
		}
		set_buffer_uptodate(bh);
		unlock_buffer(bh);
		mark_buffer_dirty_inode(bh, inode);


		if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode))
			sync_dirty_buffer(bh);
	}
	*blks = num;
	return err;

failed:
	for (i = 1; i < n; i++)
		bforget(branch[i].bh);
	for (i = 0; i < indirect_blks; i++)
		ext2_free_blocks(inode, new_blocks[i], 1);
	ext2_free_blocks(inode, new_blocks[i], num);
	return err;
}


/**
 * ext2_splice_branch - Implements the splice branch operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext2_splice_branch(struct inode *inode,
			long block, Indirect *where, int num, int blks)
{
	int i;
	struct ext2_block_alloc_info *block_i;
	ext2_fsblk_t current_block;

	block_i = EXT2_I(inode)->i_block_alloc_info;


	*where->p = where->key;


	if (num == 0 && blks > 1) {
		current_block = le32_to_cpu(where->key) + 1;
		for (i = 1; i < blks; i++)
			*(where->p + i ) = cpu_to_le32(current_block++);
	}


	if (block_i) {
		block_i->last_alloc_logical_block = block + blks - 1;
		block_i->last_alloc_physical_block =
				le32_to_cpu(where[num].key) + blks - 1;
	}


	if (where->bh)
		mark_buffer_dirty_inode(where->bh, inode);

	inode_set_ctime_current(inode);
	mark_inode_dirty(inode);
}


/**
 * ext2_get_blocks - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext2_get_blocks(struct inode *inode,
			   sector_t iblock, unsigned long maxblocks,
			   u32 *bno, bool *new, bool *boundary,
			   int create)
{
	int err;
	int offsets[4];
	Indirect chain[4];
	Indirect *partial;
	ext2_fsblk_t goal;
	int indirect_blks;
	int blocks_to_boundary = 0;
	int depth;
	struct ext2_inode_info *ei = EXT2_I(inode);
	int count = 0;
	ext2_fsblk_t first_block = 0;

	BUG_ON(maxblocks == 0);

	depth = ext2_block_to_path(inode,iblock,offsets,&blocks_to_boundary);

	if (depth == 0)
		return -EIO;

	partial = ext2_get_branch(inode, depth, offsets, chain, &err);

	if (!partial) {
		first_block = le32_to_cpu(chain[depth - 1].key);
		count++;

		while (count < maxblocks && count <= blocks_to_boundary) {
			ext2_fsblk_t blk;

			if (!verify_chain(chain, chain + depth - 1)) {


				err = -EAGAIN;
				count = 0;
				partial = chain + depth - 1;
				break;
			}
			blk = le32_to_cpu(*(chain[depth-1].p + count));
			if (blk == first_block + count)
				count++;
			else
				break;
		}
		if (err != -EAGAIN)
			goto got_it;
	}


	if (!create || err == -EIO)
		goto cleanup;

	mutex_lock(&ei->truncate_mutex);


	if (err == -EAGAIN || !verify_chain(chain, partial)) {
		while (partial > chain) {
			brelse(partial->bh);
			partial--;
		}
		partial = ext2_get_branch(inode, depth, offsets, chain, &err);
		if (!partial) {
			count++;
			mutex_unlock(&ei->truncate_mutex);
			goto got_it;
		}

		if (err) {
			mutex_unlock(&ei->truncate_mutex);
			goto cleanup;
		}
	}


	if (S_ISREG(inode->i_mode) && (!ei->i_block_alloc_info))
		ext2_init_block_alloc_info(inode);

	goal = ext2_find_goal(inode, iblock, partial);


	indirect_blks = (chain + depth) - partial - 1;


	count = ext2_blks_to_allocate(partial, indirect_blks,
					maxblocks, blocks_to_boundary);


	err = ext2_alloc_branch(inode, indirect_blks, &count, goal,
				offsets + (partial - chain), partial);

	if (err) {
		mutex_unlock(&ei->truncate_mutex);
		goto cleanup;
	}

	if (IS_DAX(inode)) {


		clean_bdev_aliases(inode->i_sb->s_bdev,
				   le32_to_cpu(chain[depth-1].key),
				   count);


		err = sb_issue_zeroout(inode->i_sb,
				le32_to_cpu(chain[depth-1].key), count,
				GFP_KERNEL);
		if (err) {
			mutex_unlock(&ei->truncate_mutex);
			goto cleanup;
		}
	}
	*new = true;

	ext2_splice_branch(inode, iblock, partial, indirect_blks, count);
	mutex_unlock(&ei->truncate_mutex);
got_it:
	if (count > blocks_to_boundary)
		*boundary = true;
	err = count;

	partial = chain + depth - 1;
cleanup:
	while (partial > chain) {
		brelse(partial->bh);
		partial--;
	}
	if (err > 0)
		*bno = le32_to_cpu(chain[depth-1].key);
	return err;
}

/**
 * ext2_get_block - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext2_get_block(struct inode *inode, sector_t iblock,
		struct buffer_head *bh_result, int create)
{
	unsigned max_blocks = bh_result->b_size >> inode->i_blkbits;
	bool new = false, boundary = false;
	u32 bno;
	int ret;

	ret = ext2_get_blocks(inode, iblock, max_blocks, &bno, &new, &boundary,
			create);
	if (ret <= 0)
		return ret;

	map_bh(bh_result, inode->i_sb, bno);
	bh_result->b_size = (ret << inode->i_blkbits);
	if (new)
		set_buffer_new(bh_result);
	if (boundary)
		set_buffer_boundary(bh_result);
	return 0;

}

/**
 * ext2_iomap_begin - Implements the iomap begin operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext2_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		unsigned flags, struct iomap *iomap, struct iomap *srcmap)
{
	unsigned int blkbits = inode->i_blkbits;
	unsigned long first_block = offset >> blkbits;
	unsigned long max_blocks = (length + (1 << blkbits) - 1) >> blkbits;
	struct ext2_sb_info *sbi = EXT2_SB(inode->i_sb);
	bool new = false, boundary = false;
	u32 bno;
	int ret;
	bool create = flags & IOMAP_WRITE;


	if ((flags & IOMAP_DIRECT) &&
	    (first_block << blkbits) < i_size_read(inode))
		create = 0;


	if ((flags & IOMAP_WRITE) && offset + length > i_size_read(inode))
		iomap->flags |= IOMAP_F_DIRTY;

	ret = ext2_get_blocks(inode, first_block, max_blocks,
			&bno, &new, &boundary, create);
	if (ret < 0)
		return ret;

	iomap->flags = 0;
	iomap->offset = (u64)first_block << blkbits;
	if (flags & IOMAP_DAX)
		iomap->dax_dev = sbi->s_daxdev;
	else
		iomap->bdev = inode->i_sb->s_bdev;

	if (ret == 0) {


		if (!create && (flags & IOMAP_WRITE) && (flags & IOMAP_DIRECT))
			return -ENOTBLK;
		iomap->type = IOMAP_HOLE;
		iomap->addr = IOMAP_NULL_ADDR;
		iomap->length = 1 << blkbits;
	} else {
		iomap->type = IOMAP_MAPPED;
		iomap->addr = (u64)bno << blkbits;
		if (flags & IOMAP_DAX)
			iomap->addr += sbi->s_dax_part_off;
		iomap->length = (u64)ret << blkbits;
		iomap->flags |= IOMAP_F_MERGED;
	}

	if (new)
		iomap->flags |= IOMAP_F_NEW;
	return 0;
}

/**
 * ext2_iomap_end - Implements the iomap end operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext2_iomap_end(struct inode *inode, loff_t offset, loff_t length,
		ssize_t written, unsigned flags, struct iomap *iomap)
{


	if ((flags & IOMAP_DIRECT) && (flags & IOMAP_WRITE) && written == 0)
		return -ENOTBLK;

	if (iomap->type == IOMAP_MAPPED &&
	    written < length &&
	    (flags & IOMAP_WRITE))
		ext2_write_failed(inode->i_mapping, offset + length);
	return 0;
}

const struct iomap_ops ext2_iomap_ops = {
	.iomap_begin		= ext2_iomap_begin,
	.iomap_end		= ext2_iomap_end,
};

/**
 * ext2_fiemap - Implements the fiemap operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext2_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
		u64 start, u64 len)
{
	int ret;
	loff_t i_size;

	inode_lock(inode);
	i_size = i_size_read(inode);


	if (i_size == 0)
		i_size = 1;
	len = min_t(u64, len, i_size);
	ret = iomap_fiemap(inode, fieinfo, start, len, &ext2_iomap_ops);
	inode_unlock(inode);

	return ret;
}

/**
 * ext2_read_folio - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext2_read_folio(struct file *file, struct folio *folio)
{
	return mpage_read_folio(folio, ext2_get_block);
}

/**
 * ext2_readahead - Implements the readahead operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext2_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ext2_get_block);
}

/**
 * ext2_write_begin - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext2_write_begin(struct file *file, struct address_space *mapping,
		loff_t pos, unsigned len, struct folio **foliop, void **fsdata)
{
	int ret;

	ret = block_write_begin(mapping, pos, len, foliop, ext2_get_block);
	if (ret < 0)
		ext2_write_failed(mapping, pos + len);
	return ret;
}

/**
 * ext2_write_end - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext2_write_end(struct file *file, struct address_space *mapping,
			loff_t pos, unsigned len, unsigned copied,
			struct folio *folio, void *fsdata)
{
	int ret;

	ret = generic_write_end(file, mapping, pos, len, copied, folio, fsdata);
	if (ret < len)
		ext2_write_failed(mapping, pos + len);
	return ret;
}

/**
 * ext2_bmap - Implements the bmap operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static sector_t ext2_bmap(struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping,block,ext2_get_block);
}

/**
 * ext2_writepages - Implements the writepages operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext2_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, ext2_get_block);
}

/**
 * ext2_dax_writepages - Implements the dax writepages operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext2_dax_writepages(struct address_space *mapping, struct writeback_control *wbc)
{
	struct ext2_sb_info *sbi = EXT2_SB(mapping->host->i_sb);

	return dax_writeback_mapping_range(mapping, sbi->s_daxdev, wbc);
}

const struct address_space_operations ext2_aops = {
	.dirty_folio		= block_dirty_folio,
	.invalidate_folio	= block_invalidate_folio,
	.read_folio		= ext2_read_folio,
	.readahead		= ext2_readahead,
	.write_begin		= ext2_write_begin,
	.write_end		= ext2_write_end,
	.bmap			= ext2_bmap,
	.writepages		= ext2_writepages,
	.migrate_folio		= buffer_migrate_folio,
	.is_partially_uptodate	= block_is_partially_uptodate,
	.error_remove_folio	= generic_error_remove_folio,
};

static const struct address_space_operations ext2_dax_aops = {
	.writepages		= ext2_dax_writepages,
	.dirty_folio		= noop_dirty_folio,
};


/**
 * all_zeroes - Implements the all zeroes operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int all_zeroes(__le32 *p, __le32 *q)
{
	while (p < q)
		if (*p++)
			return 0;
	return 1;
}


/**
 * ext2_find_shared - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static Indirect *ext2_find_shared(struct inode *inode,
				int depth,
				int offsets[4],
				Indirect chain[4],
				__le32 *top)
{
	Indirect *partial, *p;
	int k, err;

	*top = 0;
	for (k = depth; k > 1 && !offsets[k-1]; k--)
		;
	partial = ext2_get_branch(inode, k, offsets, chain, &err);
	if (!partial)
		partial = chain + k-1;


	write_lock(&EXT2_I(inode)->i_meta_lock);
	if (!partial->key && *partial->p) {
		write_unlock(&EXT2_I(inode)->i_meta_lock);
		goto no_top;
	}
	for (p=partial; p>chain && all_zeroes((__le32*)p->bh->b_data,p->p); p--)
		;


	if (p == chain + k - 1 && p > chain) {
		p->p--;
	} else {
		*top = *p->p;
		*p->p = 0;
	}
	write_unlock(&EXT2_I(inode)->i_meta_lock);

	while(partial > p)
	{
		brelse(partial->bh);
		partial--;
	}
no_top:
	return partial;
}


/**
 * ext2_free_data - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline void ext2_free_data(struct inode *inode, __le32 *p, __le32 *q)
{
	ext2_fsblk_t block_to_free = 0, count = 0;
	ext2_fsblk_t nr;

	for ( ; p < q ; p++) {
		nr = le32_to_cpu(*p);
		if (nr) {
			*p = 0;

			if (count == 0)
				goto free_this;
			else if (block_to_free == nr - count)
				count++;
			else {
				ext2_free_blocks (inode, block_to_free, count);
				mark_inode_dirty(inode);
			free_this:
				block_to_free = nr;
				count = 1;
			}
		}
	}
	if (count > 0) {
		ext2_free_blocks (inode, block_to_free, count);
		mark_inode_dirty(inode);
	}
}


/**
 * ext2_free_branches - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext2_free_branches(struct inode *inode, __le32 *p, __le32 *q, int depth)
{
	struct buffer_head * bh;
	ext2_fsblk_t nr;

	if (depth--) {
		int addr_per_block = EXT2_ADDR_PER_BLOCK(inode->i_sb);
		for ( ; p < q ; p++) {
			nr = le32_to_cpu(*p);
			if (!nr)
				continue;
			*p = 0;
			bh = sb_bread(inode->i_sb, nr);


			if (!bh) {
				ext2_error(inode->i_sb, "ext2_free_branches",
					"Read failure, inode=%ld, block=%ld",
					inode->i_ino, nr);
				continue;
			}
			ext2_free_branches(inode,
					   (__le32*)bh->b_data,
					   (__le32*)bh->b_data + addr_per_block,
					   depth);
			bforget(bh);
			ext2_free_blocks(inode, nr, 1);
			mark_inode_dirty(inode);
		}
	} else
		ext2_free_data(inode, p, q);
}


/**
 * __ext2_truncate_blocks - Implements the truncate blocks operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void __ext2_truncate_blocks(struct inode *inode, loff_t offset)
{
	__le32 *i_data = EXT2_I(inode)->i_data;
	struct ext2_inode_info *ei = EXT2_I(inode);
	int addr_per_block = EXT2_ADDR_PER_BLOCK(inode->i_sb);
	int offsets[4];
	Indirect chain[4];
	Indirect *partial;
	__le32 nr = 0;
	int n;
	long iblock;
	unsigned blocksize;
	blocksize = inode->i_sb->s_blocksize;
	iblock = (offset + blocksize-1) >> EXT2_BLOCK_SIZE_BITS(inode->i_sb);

#ifdef CONFIG_FS_DAX
	WARN_ON(!rwsem_is_locked(&inode->i_mapping->invalidate_lock));
#endif

	n = ext2_block_to_path(inode, iblock, offsets, NULL);
	if (n == 0)
		return;


	mutex_lock(&ei->truncate_mutex);

	if (n == 1) {
		ext2_free_data(inode, i_data+offsets[0],
					i_data + EXT2_NDIR_BLOCKS);
		goto do_indirects;
	}

	partial = ext2_find_shared(inode, n, offsets, chain, &nr);

	if (nr) {
		if (partial == chain)
			mark_inode_dirty(inode);
		else
			mark_buffer_dirty_inode(partial->bh, inode);
		ext2_free_branches(inode, &nr, &nr+1, (chain+n-1) - partial);
	}

	while (partial > chain) {
		ext2_free_branches(inode,
				   partial->p + 1,
				   (__le32*)partial->bh->b_data+addr_per_block,
				   (chain+n-1) - partial);
		mark_buffer_dirty_inode(partial->bh, inode);
		brelse (partial->bh);
		partial--;
	}
do_indirects:

	switch (offsets[0]) {
		default:
			nr = i_data[EXT2_IND_BLOCK];
			if (nr) {
				i_data[EXT2_IND_BLOCK] = 0;
				mark_inode_dirty(inode);
				ext2_free_branches(inode, &nr, &nr+1, 1);
			}
			fallthrough;
		case EXT2_IND_BLOCK:
			nr = i_data[EXT2_DIND_BLOCK];
			if (nr) {
				i_data[EXT2_DIND_BLOCK] = 0;
				mark_inode_dirty(inode);
				ext2_free_branches(inode, &nr, &nr+1, 2);
			}
			fallthrough;
		case EXT2_DIND_BLOCK:
			nr = i_data[EXT2_TIND_BLOCK];
			if (nr) {
				i_data[EXT2_TIND_BLOCK] = 0;
				mark_inode_dirty(inode);
				ext2_free_branches(inode, &nr, &nr+1, 3);
			}
			break;
		case EXT2_TIND_BLOCK:
			;
	}

	ext2_discard_reservation(inode);

	mutex_unlock(&ei->truncate_mutex);
}

/**
 * ext2_truncate_blocks - Implements the truncate blocks operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext2_truncate_blocks(struct inode *inode, loff_t offset)
{
	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	    S_ISLNK(inode->i_mode)))
		return;
	if (ext2_inode_is_fast_symlink(inode))
		return;

	filemap_invalidate_lock(inode->i_mapping);
	__ext2_truncate_blocks(inode, offset);
	filemap_invalidate_unlock(inode->i_mapping);
}

/**
 * ext2_setsize - Implements the setsize operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext2_setsize(struct inode *inode, loff_t newsize)
{
	int error;

	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	    S_ISLNK(inode->i_mode)))
		return -EINVAL;
	if (ext2_inode_is_fast_symlink(inode))
		return -EINVAL;
	if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
		return -EPERM;

	inode_dio_wait(inode);

	if (IS_DAX(inode))
		error = dax_truncate_page(inode, newsize, NULL,
					  &ext2_iomap_ops);
	else
		error = block_truncate_page(inode->i_mapping,
				newsize, ext2_get_block);
	if (error)
		return error;

	filemap_invalidate_lock(inode->i_mapping);
	truncate_setsize(inode, newsize);
	__ext2_truncate_blocks(inode, newsize);
	filemap_invalidate_unlock(inode->i_mapping);

	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	if (inode_needs_sync(inode)) {
		sync_mapping_buffers(inode->i_mapping);
		sync_inode_metadata(inode, 1);
	} else {
		mark_inode_dirty(inode);
	}

	return 0;
}

/**
 * ext2_get_inode - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct ext2_inode *ext2_get_inode(struct super_block *sb, ino_t ino,
					struct buffer_head **p)
{
	struct buffer_head * bh;
	unsigned long block_group;
	unsigned long block;
	unsigned long offset;
	struct ext2_group_desc * gdp;

	*p = NULL;
	if ((ino != EXT2_ROOT_INO && ino < EXT2_FIRST_INO(sb)) ||
	    ino > le32_to_cpu(EXT2_SB(sb)->s_es->s_inodes_count))
		goto Einval;

	block_group = (ino - 1) / EXT2_INODES_PER_GROUP(sb);
	gdp = ext2_get_group_desc(sb, block_group, NULL);
	if (!gdp)
		goto Egdp;


	offset = ((ino - 1) % EXT2_INODES_PER_GROUP(sb)) * EXT2_INODE_SIZE(sb);
	block = le32_to_cpu(gdp->bg_inode_table) +
		(offset >> EXT2_BLOCK_SIZE_BITS(sb));
	if (!(bh = sb_bread(sb, block)))
		goto Eio;

	*p = bh;
	offset &= (EXT2_BLOCK_SIZE(sb) - 1);
	return (struct ext2_inode *) (bh->b_data + offset);

Einval:
	ext2_error(sb, "ext2_get_inode", "bad inode number: %lu",
		   (unsigned long) ino);
	return ERR_PTR(-EINVAL);
Eio:
	ext2_error(sb, "ext2_get_inode",
		   "unable to read inode block - inode=%lu, block=%lu",
		   (unsigned long) ino, block);
Egdp:
	return ERR_PTR(-EIO);
}

/**
 * ext2_set_inode_flags - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext2_set_inode_flags(struct inode *inode)
{
	unsigned int flags = EXT2_I(inode)->i_flags;

	inode->i_flags &= ~(S_SYNC | S_APPEND | S_IMMUTABLE | S_NOATIME |
				S_DIRSYNC | S_DAX);
	if (flags & EXT2_SYNC_FL)
		inode->i_flags |= S_SYNC;
	if (flags & EXT2_APPEND_FL)
		inode->i_flags |= S_APPEND;
	if (flags & EXT2_IMMUTABLE_FL)
		inode->i_flags |= S_IMMUTABLE;
	if (flags & EXT2_NOATIME_FL)
		inode->i_flags |= S_NOATIME;
	if (flags & EXT2_DIRSYNC_FL)
		inode->i_flags |= S_DIRSYNC;
	if (test_opt(inode->i_sb, DAX) && S_ISREG(inode->i_mode))
		inode->i_flags |= S_DAX;
}

/**
 * ext2_set_file_ops - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext2_set_file_ops(struct inode *inode)
{
	inode->i_op = &ext2_file_inode_operations;
	inode->i_fop = &ext2_file_operations;
	if (IS_DAX(inode))
		inode->i_mapping->a_ops = &ext2_dax_aops;
	else
		inode->i_mapping->a_ops = &ext2_aops;
}

/**
 * ext2_iget - Implements the iget operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct inode *ext2_iget (struct super_block *sb, unsigned long ino)
{
	struct ext2_inode_info *ei;
	struct buffer_head * bh = NULL;
	struct ext2_inode *raw_inode;
	struct inode *inode;
	long ret = -EIO;
	int n;
	uid_t i_uid;
	gid_t i_gid;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	ei = EXT2_I(inode);
	ei->i_block_alloc_info = NULL;

	raw_inode = ext2_get_inode(inode->i_sb, ino, &bh);
	if (IS_ERR(raw_inode)) {
		ret = PTR_ERR(raw_inode);
 		goto bad_inode;
	}

	inode->i_mode = le16_to_cpu(raw_inode->i_mode);
	i_uid = (uid_t)le16_to_cpu(raw_inode->i_uid_low);
	i_gid = (gid_t)le16_to_cpu(raw_inode->i_gid_low);
	if (!(test_opt (inode->i_sb, NO_UID32))) {
		i_uid |= le16_to_cpu(raw_inode->i_uid_high) << 16;
		i_gid |= le16_to_cpu(raw_inode->i_gid_high) << 16;
	}
	i_uid_write(inode, i_uid);
	i_gid_write(inode, i_gid);
	set_nlink(inode, le16_to_cpu(raw_inode->i_links_count));
	inode->i_size = le32_to_cpu(raw_inode->i_size);
	inode_set_atime(inode, (signed)le32_to_cpu(raw_inode->i_atime), 0);
	inode_set_ctime(inode, (signed)le32_to_cpu(raw_inode->i_ctime), 0);
	inode_set_mtime(inode, (signed)le32_to_cpu(raw_inode->i_mtime), 0);
	ei->i_dtime = le32_to_cpu(raw_inode->i_dtime);


	if (inode->i_nlink == 0) {
		if (inode->i_mode == 0 || ei->i_dtime) {

			ret = -ESTALE;
		} else {
			ext2_error(sb, __func__,
				   "inode %lu has zero i_nlink with mode 0%o and no dtime, "
				   "filesystem may be corrupt",
				   ino, inode->i_mode);
			ret = -EFSCORRUPTED;
		}
		goto bad_inode;
	}
	inode->i_blocks = le32_to_cpu(raw_inode->i_blocks);
	ei->i_flags = le32_to_cpu(raw_inode->i_flags);
	ext2_set_inode_flags(inode);
	ei->i_faddr = le32_to_cpu(raw_inode->i_faddr);
	ei->i_frag_no = raw_inode->i_frag;
	ei->i_frag_size = raw_inode->i_fsize;
	ei->i_file_acl = le32_to_cpu(raw_inode->i_file_acl);
	ei->i_dir_acl = 0;

	if (ei->i_file_acl &&
	    !ext2_data_block_valid(EXT2_SB(sb), ei->i_file_acl, 1)) {
		ext2_error(sb, "ext2_iget", "bad extended attribute block %u",
			   ei->i_file_acl);
		ret = -EFSCORRUPTED;
		goto bad_inode;
	}

	if (S_ISREG(inode->i_mode))
		inode->i_size |= ((__u64)le32_to_cpu(raw_inode->i_size_high)) << 32;
	else
		ei->i_dir_acl = le32_to_cpu(raw_inode->i_dir_acl);
	if (i_size_read(inode) < 0) {
		ret = -EFSCORRUPTED;
		goto bad_inode;
	}
	ei->i_dtime = 0;
	inode->i_generation = le32_to_cpu(raw_inode->i_generation);
	ei->i_state = 0;
	ei->i_block_group = (ino - 1) / EXT2_INODES_PER_GROUP(inode->i_sb);
	ei->i_dir_start_lookup = 0;


	for (n = 0; n < EXT2_N_BLOCKS; n++)
		ei->i_data[n] = raw_inode->i_block[n];

	if (S_ISREG(inode->i_mode)) {
		ext2_set_file_ops(inode);
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &ext2_dir_inode_operations;
		inode->i_fop = &ext2_dir_operations;
		inode->i_mapping->a_ops = &ext2_aops;
	} else if (S_ISLNK(inode->i_mode)) {
		if (ext2_inode_is_fast_symlink(inode)) {
			inode->i_link = (char *)ei->i_data;
			inode->i_op = &ext2_fast_symlink_inode_operations;
			nd_terminate_link(ei->i_data, inode->i_size,
				sizeof(ei->i_data) - 1);
		} else {
			inode->i_op = &ext2_symlink_inode_operations;
			inode_nohighmem(inode);
			inode->i_mapping->a_ops = &ext2_aops;
		}
	} else {
		inode->i_op = &ext2_special_inode_operations;
		if (raw_inode->i_block[0])
			init_special_inode(inode, inode->i_mode,
			   old_decode_dev(le32_to_cpu(raw_inode->i_block[0])));
		else
			init_special_inode(inode, inode->i_mode,
			   new_decode_dev(le32_to_cpu(raw_inode->i_block[1])));
	}
	brelse (bh);
	unlock_new_inode(inode);
	return inode;

bad_inode:
	brelse(bh);
	iget_failed(inode);
	return ERR_PTR(ret);
}

/**
 * __ext2_write_inode - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __ext2_write_inode(struct inode *inode, int do_sync)
{
	struct ext2_inode_info *ei = EXT2_I(inode);
	struct super_block *sb = inode->i_sb;
	ino_t ino = inode->i_ino;
	uid_t uid = i_uid_read(inode);
	gid_t gid = i_gid_read(inode);
	struct buffer_head * bh;
	struct ext2_inode * raw_inode = ext2_get_inode(sb, ino, &bh);
	int n;
	int err = 0;

	if (IS_ERR(raw_inode))
 		return -EIO;


	if (ei->i_state & EXT2_STATE_NEW)
		memset(raw_inode, 0, EXT2_SB(sb)->s_inode_size);

	raw_inode->i_mode = cpu_to_le16(inode->i_mode);
	if (!(test_opt(sb, NO_UID32))) {
		raw_inode->i_uid_low = cpu_to_le16(low_16_bits(uid));
		raw_inode->i_gid_low = cpu_to_le16(low_16_bits(gid));


		if (!ei->i_dtime) {
			raw_inode->i_uid_high = cpu_to_le16(high_16_bits(uid));
			raw_inode->i_gid_high = cpu_to_le16(high_16_bits(gid));
		} else {
			raw_inode->i_uid_high = 0;
			raw_inode->i_gid_high = 0;
		}
	} else {
		raw_inode->i_uid_low = cpu_to_le16(fs_high2lowuid(uid));
		raw_inode->i_gid_low = cpu_to_le16(fs_high2lowgid(gid));
		raw_inode->i_uid_high = 0;
		raw_inode->i_gid_high = 0;
	}
	raw_inode->i_links_count = cpu_to_le16(inode->i_nlink);
	raw_inode->i_size = cpu_to_le32(inode->i_size);
	raw_inode->i_atime = cpu_to_le32(inode_get_atime_sec(inode));
	raw_inode->i_ctime = cpu_to_le32(inode_get_ctime_sec(inode));
	raw_inode->i_mtime = cpu_to_le32(inode_get_mtime_sec(inode));

	raw_inode->i_blocks = cpu_to_le32(inode->i_blocks);
	raw_inode->i_dtime = cpu_to_le32(ei->i_dtime);
	raw_inode->i_flags = cpu_to_le32(ei->i_flags);
	raw_inode->i_faddr = cpu_to_le32(ei->i_faddr);
	raw_inode->i_frag = ei->i_frag_no;
	raw_inode->i_fsize = ei->i_frag_size;
	raw_inode->i_file_acl = cpu_to_le32(ei->i_file_acl);
	if (!S_ISREG(inode->i_mode))
		raw_inode->i_dir_acl = cpu_to_le32(ei->i_dir_acl);
	else {
		raw_inode->i_size_high = cpu_to_le32(inode->i_size >> 32);
		if (inode->i_size > 0x7fffffffULL) {
			if (!EXT2_HAS_RO_COMPAT_FEATURE(sb,
					EXT2_FEATURE_RO_COMPAT_LARGE_FILE) ||
			    EXT2_SB(sb)->s_es->s_rev_level ==
					cpu_to_le32(EXT2_GOOD_OLD_REV)) {


				spin_lock(&EXT2_SB(sb)->s_lock);
				ext2_update_dynamic_rev(sb);
				EXT2_SET_RO_COMPAT_FEATURE(sb,
					EXT2_FEATURE_RO_COMPAT_LARGE_FILE);
				spin_unlock(&EXT2_SB(sb)->s_lock);
				ext2_sync_super(sb, EXT2_SB(sb)->s_es, 1);
			}
		}
	}

	raw_inode->i_generation = cpu_to_le32(inode->i_generation);
	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		if (old_valid_dev(inode->i_rdev)) {
			raw_inode->i_block[0] =
				cpu_to_le32(old_encode_dev(inode->i_rdev));
			raw_inode->i_block[1] = 0;
		} else {
			raw_inode->i_block[0] = 0;
			raw_inode->i_block[1] =
				cpu_to_le32(new_encode_dev(inode->i_rdev));
			raw_inode->i_block[2] = 0;
		}
	} else for (n = 0; n < EXT2_N_BLOCKS; n++)
		raw_inode->i_block[n] = ei->i_data[n];
	mark_buffer_dirty(bh);
	if (do_sync) {
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh)) {
			printk ("IO error syncing ext2 inode [%s:%08lx]\n",
				sb->s_id, (unsigned long) ino);
			err = -EIO;
		}
	}
	ei->i_state &= ~EXT2_STATE_NEW;
	brelse (bh);
	return err;
}

/**
 * ext2_write_inode - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext2_write_inode(struct inode *inode, struct writeback_control *wbc)
{
	return __ext2_write_inode(inode, wbc->sync_mode == WB_SYNC_ALL);
}

/**
 * ext2_getattr - Implements the getattr operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext2_getattr(struct mnt_idmap *idmap, const struct path *path,
		 struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	struct ext2_inode_info *ei = EXT2_I(inode);
	unsigned int flags;

	flags = ei->i_flags & EXT2_FL_USER_VISIBLE;
	if (flags & EXT2_APPEND_FL)
		stat->attributes |= STATX_ATTR_APPEND;
	if (flags & EXT2_COMPR_FL)
		stat->attributes |= STATX_ATTR_COMPRESSED;
	if (flags & EXT2_IMMUTABLE_FL)
		stat->attributes |= STATX_ATTR_IMMUTABLE;
	if (flags & EXT2_NODUMP_FL)
		stat->attributes |= STATX_ATTR_NODUMP;
	stat->attributes_mask |= (STATX_ATTR_APPEND |
			STATX_ATTR_COMPRESSED |
			STATX_ATTR_ENCRYPTED |
			STATX_ATTR_IMMUTABLE |
			STATX_ATTR_NODUMP);

	generic_fillattr(&nop_mnt_idmap, request_mask, inode, stat);
	return 0;
}

/**
 * ext2_setattr - Implements the setattr operation within the inode mapping and lifecycle subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext2_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
		 struct iattr *iattr)
{
	struct inode *inode = d_inode(dentry);
	int error;

	error = setattr_prepare(&nop_mnt_idmap, dentry, iattr);
	if (error)
		return error;

	if (is_quota_modification(&nop_mnt_idmap, inode, iattr)) {
		error = dquot_initialize(inode);
		if (error)
			return error;
	}
	if (i_uid_needs_update(&nop_mnt_idmap, iattr, inode) ||
	    i_gid_needs_update(&nop_mnt_idmap, iattr, inode)) {
		error = dquot_transfer(&nop_mnt_idmap, inode, iattr);
		if (error)
			return error;
	}
	if (iattr->ia_valid & ATTR_SIZE && iattr->ia_size != inode->i_size) {
		error = ext2_setsize(inode, iattr->ia_size);
		if (error)
			return error;
	}
	setattr_copy(&nop_mnt_idmap, inode, iattr);
	if (iattr->ia_valid & ATTR_MODE)
		error = posix_acl_chmod(&nop_mnt_idmap, dentry, inode->i_mode);
	mark_inode_dirty(inode);

	return error;
}


const struct inode_operations ext2_symlink_inode_operations = {
	.get_link	= page_get_link,
	.getattr	= ext2_getattr,
	.setattr	= ext2_setattr,
	.listxattr	= ext2_listxattr,
};

const struct inode_operations ext2_fast_symlink_inode_operations = {
	.get_link	= simple_get_link,
	.getattr	= ext2_getattr,
	.setattr	= ext2_setattr,
	.listxattr	= ext2_listxattr,
};
