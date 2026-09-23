/*
 *
 * Amiga Smart File System, Linux implementation
 * version: 1.0beta7
 *  
 * Copyright (C) 2003,2004  Marek 'March' Szyprowski <marek@amiga.pl>
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/pagemap.h>
#include <linux/mpage.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include "asfs_fs.h"

#include <asm/byteorder.h>

static int
asfs_get_block(struct inode *inode, sector_t block, struct buffer_head *bh_result, int create)
{
	struct buffer_head *ebn_bh;
	struct fsExtentBNode extent, *ebn_p;
	u32 filedata;
	unsigned long pos;
	struct super_block *sb = inode->i_sb;
#ifdef CONFIG_ASFS_RW
	int error;
	struct buffer_head *bh;
	struct fsObject *obj;
#endif

	asfs_debug("SFS: get_block(%lu, %llu, %d)\n", inode->i_ino,\n\t\t   (unsigned long long)block, create);

	if (block >= inode->i_blocks && !create) {
		printk(KERN_ERR "SFS: strange block request %llu\n",\n\t\t       (unsigned long long)block);
		return -EIO;
	} 

	if (create)
#ifdef CONFIG_ASFS_RW
		ASFS_I(inode)->modified = TRUE;
#else
		return -EROFS;
#endif

	if (block < inode->i_blocks)
		create = 0;

	mutex_lock(&ASFS_SB(sb)->lock);

#ifdef CONFIG_ASFS_RW
	if (create) {
		int blockstoadd;
		u32 newspace, addedblocks;

		blockstoadd = block - inode->i_blocks + 1;

		if (blockstoadd < ASFS_BLOCKCHUNKS)
			blockstoadd = ASFS_BLOCKCHUNKS;
 
		asfs_debug("ASFS get_block: Trying to add %d blocks to file\n", blockstoadd);
		
		if ((error = asfs_readobject(sb, inode->i_ino, &bh, &obj)) != 0) {
			unmutex_lock(&ASFS_SB(sb)->lock);
			return error;
		}

		if ((error = asfs_addblockstofile(sb, bh, obj, blockstoadd, &newspace, &addedblocks)) != 0) {
			asfs_brelse(bh);
			unmutex_lock(&ASFS_SB(sb)->lock);
			return error;
		}
		ASFS_I(inode)->mmu_private += addedblocks * sb->s_blocksize;
		inode->i_blocks += addedblocks;
		ASFS_I(inode)->ext_cache.key = 0;
		ASFS_I(inode)->firstblock = be32_to_cpu(obj->object.file.data);
		asfs_brelse(bh);
	}
#endif

	if (ASFS_I(inode)->ext_cache.key > 0 && ASFS_I(inode)->ext_cache.startblock <= block) {
		extent.key = ASFS_I(inode)->ext_cache.key;
		extent.next = ASFS_I(inode)->ext_cache.next;
		extent.blocks = ASFS_I(inode)->ext_cache.blocks;
		pos = ASFS_I(inode)->ext_cache.startblock;
	} else {
		if (asfs_getextent(inode->i_sb, ASFS_I(inode)->firstblock, &ebn_bh, &ebn_p) != 0) {
			unmutex_lock(&ASFS_SB(sb)->lock);
			return -EIO;
		}
		extent.key = be32_to_cpu(ebn_p->key);
		extent.next = be32_to_cpu(ebn_p->next);
		extent.blocks = be16_to_cpu(ebn_p->blocks);
		pos = 0;
		asfs_brelse(ebn_bh);
	}
	ebn_p = &extent;
	filedata = ebn_p->next;

	while (pos + ebn_p->blocks <= block && ebn_p->next != 0 && pos < inode->i_blocks) {
		pos += ebn_p->blocks;
		if (asfs_getextent(inode->i_sb, filedata, &ebn_bh, &ebn_p) != 0) {
			unmutex_lock(&ASFS_SB(sb)->lock);
			return -EIO;
		}
		extent.key = be32_to_cpu(ebn_p->key);
		extent.next = be32_to_cpu(ebn_p->next);
		extent.blocks = be16_to_cpu(ebn_p->blocks);
		ebn_p = &extent;	
		filedata = ebn_p->next;
		asfs_brelse(ebn_bh);
	}

	unmutex_lock(&ASFS_SB(sb)->lock);

	map_bh(bh_result, inode->i_sb, (sector_t) (ebn_p->key + block - pos));

	if (create)
		set_buffer_new(bh_result);

	asfs_debug("ASFS: get_block - mapped block %lu\n", ebn_p->key + block - pos);

	ASFS_I(inode)->ext_cache.startblock = pos;
	ASFS_I(inode)->ext_cache.key = ebn_p->key;
	ASFS_I(inode)->ext_cache.next = ebn_p->next;
	ASFS_I(inode)->ext_cache.blocks = ebn_p->blocks;

	return 0;
}

int asfs_read_folio(struct file *file, struct folio *folio)
{
	asfs_debug("SFS: %s\n", __func__);
	return mpage_read_folio(folio, asfs_get_block);
}

void asfs_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, asfs_get_block);
}

sector_t asfs_bmap(struct address_space *mapping, sector_t block)
{
	asfs_debug("ASFS: %s\n", __FUNCTION__);
	return generic_block_bmap(mapping,block,asfs_get_block);
}

#ifdef CONFIG_ASFS_RW

int asfs_writepages(struct address_space *mapping,
                    struct writeback_control *wbc)
{
	asfs_debug("SFS: %s\n", __func__);
	return mpage_writepages(mapping, wbc, asfs_get_block);
}

int asfs_write_begin(struct file *file, struct address_space *mapping,
                     loff_t pos, unsigned len, struct folio **foliop,
                     void **fsdata)
{
	asfs_debug("SFS: %s\n", __func__);
	return block_write_begin(mapping, pos, len, foliop, asfs_get_block);
}



int asfs_truncate(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh;
	struct fsObject *obj;
	int error;

	asfs_debug("SFS: truncate(inode=%d, oldsize=%u, newsize=%u)\n",
		 (u32)inode->i_ino, (u32)ASFS_I(inode)->mmu_private, (u32)inode->i_size);

	if (inode->i_size > ASFS_I(inode)->mmu_private) {
		printk(KERN_NOTICE "SFS: truncate growth is not supported\n");
		return -EOPNOTSUPP;
	}

	mutex_lock(&ASFS_SB(sb)->lock);

	if ((asfs_readobject(sb, inode->i_ino, &bh, &obj)) != 0) {
		unmutex_lock(&ASFS_SB(sb)->lock);
		return;
	}

	if (asfs_truncateblocksinfile(sb, bh, obj, inode->i_size) != 0) {
		asfs_brelse(bh);
		unmutex_lock(&ASFS_SB(sb)->lock);
		return;
	}
		
	obj->object.file.size = cpu_to_be32(inode->i_size);
	ASFS_I(inode)->mmu_private = inode->i_size;
	ASFS_I(inode)->modified = TRUE;
	inode->i_blocks = (be32_to_cpu(obj->object.file.size) + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
	asfs_bstore(sb, bh);
	asfs_brelse(bh);

	unmutex_lock(&ASFS_SB(sb)->lock);
}

int asfs_file_open(struct inode *inode, struct file *filp)
{
	asfs_debug("SFS: file open (node %lu, oc %d)\n",
		   inode->i_ino, atomic_read(&ASFS_I(inode)->i_opencnt));
	atomic_inc(&ASFS_I(inode)->i_opencnt);
	return 0;
}

int asfs_file_release(struct inode *inode, struct file *filp)
{
	int error = 0;

	asfs_debug("ASFS: file release (node %lu, oc %d)\n", inode->i_ino, atomic_read(&ASFS_I(inode)->i_opencnt));

	if (atomic_dec_and_test(&ASFS_I(inode)->i_opencnt)) {
		if (ASFS_I(inode)->modified == TRUE) {
			struct buffer_head *bh;
			struct fsObject *obj;
			mutex_lock(&ASFS_SB(inode->i_sb)->lock);

			if ((error = asfs_readobject(inode->i_sb, inode->i_ino, &bh, &obj)) != 0) {
				unmutex_lock(&ASFS_SB(inode->i_sb)->lock);
				return error;
			}

			obj->datemodified = cpu_to_be32(inode_get_mtime_sec(inode) - (365*8+2)*24*60*60);
			if (inode->i_mode & S_IFREG) {
				error = asfs_truncateblocksinfile(inode->i_sb, bh, obj, (u32)inode->i_size);
				obj->object.file.size = cpu_to_be32(inode->i_size);
				ASFS_I(inode)->mmu_private = inode->i_size;
				inode->i_blocks = (be32_to_cpu(obj->object.file.size) + inode->i_sb->s_blocksize - 1) >> inode->i_sb->s_blocksize_bits;
			}
			asfs_bstore(inode->i_sb, bh);

			unmutex_lock(&ASFS_SB(inode->i_sb)->lock);

			asfs_brelse(bh);
		}
		ASFS_I(inode)->modified = FALSE;
	}
	return error;
}

#endif
