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
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include "asfs_fs.h"

#include <asm/byteorder.h>

extern const struct dentry_operations asfs_dentry_operations;

int asfs_readdir(struct file *filp, struct dir_context *ctx)
{
	struct inode *dir = file_inode(filp);
	struct super_block *sb = dir->i_sb;
	struct nls_table *nls_io = ASFS_SB(sb)->nls_io;
	struct nls_table *nls_disk = ASFS_SB(sb)->nls_disk;
	u8 buf[512];
	struct buffer_head *bh;
	struct fsObjectContainer *objcont;
	struct fsObject *obj;
	u32 block;
	u32 startnode;
	bool add;

	if (ctx->pos >= ASFS_SB(sb)->totalblocks)
		return 0;

	if (!dir_emit_dots(filp, ctx))
		return 0;

	if (ASFS_I(dir)->firstblock == 0) {
		ctx->pos = ASFS_SB(sb)->totalblocks;
		ASFS_I(dir)->modified = 0;
		return 0;
	}

	if (ctx->pos == 2) {
		block = ASFS_I(dir)->firstblock;
		add = true;
		startnode = 0;
	} else {
		startnode = (u32)(unsigned long)filp->private_data;
		add = false;
		block = ASFS_I(dir)->modified == 0
			? (u32)ctx->pos
			: ASFS_I(dir)->firstblock;
	}

	do {
		bh = asfs_breadcheck(sb, block, ASFS_OBJECTCONTAINER_ID);
		if (!bh)
			return -EIO;

		objcont = (struct fsObjectContainer *)bh->b_data;
		obj = &objcont->object[0];

		while (be32_to_cpu(obj->objectnode) > 0 &&
		       ((char *)obj - (char *)objcont) +
		       sizeof(struct fsObject) + 2 < sb->s_blocksize) {
			u32 objectnode = be32_to_cpu(obj->objectnode);
			unsigned int type;

			if (!add && objectnode == startnode)
				add = true;

			if (add && !(obj->bits & OTYPE_HIDDEN)) {
				asfs_translate(buf, obj->name, nls_io, nls_disk,
				               sizeof(buf));
				ctx->pos = block;

				if (obj->bits & OTYPE_DIR)
					type = DT_DIR;
				else if ((obj->bits & OTYPE_LINK) &&
				         !(obj->bits & OTYPE_HARDLINK))
					type = DT_LNK;
				else
					type = DT_REG;

				if (!dir_emit(ctx, buf, strlen(buf), objectnode, type)) {
					filp->private_data =
						(void *)(unsigned long)objectnode;
					ASFS_I(dir)->modified = 0;
					asfs_brelse(bh);
					return 0;
				}
			}

			obj = asfs_nextobject(obj);
		}

		block = be32_to_cpu(objcont->next);
		asfs_brelse(bh);
	} while (block != 0);

	ctx->pos = ASFS_SB(sb)->totalblocks;
	ASFS_I(dir)->modified = 0;
	filp->private_data = NULL;
	return 0;
}

static struct fsObject *asfs_find_obj_by_name_nls(struct super_block *sb, struct fsObjectContainer *objcont, u8 * name)
{
	struct fsObject *obj;
	u8 buf[512];

	obj = &(objcont->object[0]);
	while (be32_to_cpu(obj->objectnode) > 0 && ((char *) obj - (char *) objcont) + sizeof(struct fsObject) + 2 < sb->s_blocksize) {
		asfs_translate(buf, obj->name, ASFS_SB(sb)->nls_io, ASFS_SB(sb)->nls_disk, 512);
		if (asfs_namecmp(buf, name, ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE, ASFS_SB(sb)->nls_io) == 0) {
			asfs_debug("Object found! Node %u, Name %s, Type %x, inCont %u\n", be32_to_cpu(obj->objectnode), obj->name, obj->bits, be32_to_cpu(objcont->bheader.ownblock));
			return obj;
		}
		obj = asfs_nextobject(obj);
	}
	return NULL;
}

struct dentry *asfs_lookup(struct inode *dir, struct dentry *dentry,
                           unsigned int flags)
{
	int res = -EACCES;       /* placeholder for invalid/corrupt lookup state */
	(void)flags;
	struct inode *inode;
	struct super_block *sb = dir->i_sb;
	u8 *name = (u8 *) dentry->d_name.name;
	struct buffer_head *bh;
	struct fsObject *obj;
	u8 bufname[ASFS_MAXFN_BUF];

	asfs_translate(bufname, name, ASFS_SB(sb)->nls_disk, ASFS_SB(sb)->nls_io, ASFS_MAXFN_BUF);

	asfs_debug("asfs_lookup: (searching \"%s\"...) ", name);

	mutex_lock(&ASFS_SB(sb)->lock);

	if ((!strchr(name, '?')) && (ASFS_I(dir)->hashtable != 0)) {	/* hashtable block is available and name can be reverse translated, quick search */
		struct fsObjectNode *node_p;
		struct buffer_head *node_bh;
		u32 node;
		u16 hash16;

		asfs_debug("(quick search) ");

		if (!(bh = asfs_breadcheck(sb, ASFS_I(dir)->hashtable, ASFS_HASHTABLE_ID))) {
			unmutex_lock(&ASFS_SB(sb)->lock);
			return ERR_PTR(res);
		}
		hash16 = asfs_hash(bufname, ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE); 
		node = be32_to_cpu(((struct fsHashTable *) bh->b_data)->hashentry[HASHCHAIN(hash16)]);
		asfs_brelse(bh);

		while (node != 0) {
			if (asfs_getnode(sb, node, &node_bh, &node_p) != 0)
				goto not_found;
			if (be16_to_cpu(node_p->hash16) == hash16) {
				if (!(bh = asfs_breadcheck(sb, be32_to_cpu(node_p->node.data), ASFS_OBJECTCONTAINER_ID))) {
					asfs_brelse(node_bh);
					unmutex_lock(&ASFS_SB(sb)->lock);
					return ERR_PTR(res);
				}
				if ((obj = asfs_find_obj_by_name(sb, (struct fsObjectContainer *) bh->b_data, bufname)) != NULL) {
					asfs_brelse(node_bh);
					goto found_inode;
				}
				asfs_brelse(bh);
			}
			node = be32_to_cpu(node_p->next);
			asfs_brelse(node_bh);
		}
	} else { /* hashtable not available or name can't be reverse-translated, long search */
		struct fsObjectContainer *objcont;
		u32 block;

		asfs_debug("(long search) ");
		block = ASFS_I(dir)->firstblock;
		while (block != 0) {
			if (!(bh = asfs_breadcheck(sb, block, ASFS_OBJECTCONTAINER_ID))) {
				unmutex_lock(&ASFS_SB(sb)->lock);
				return ERR_PTR(res);
			}
			objcont = (struct fsObjectContainer *) bh->b_data;
			if ((obj = asfs_find_obj_by_name_nls(sb, objcont, name)) != NULL)
				goto found_inode;
			block = be32_to_cpu(objcont->next);
			asfs_brelse(bh);
		}
	}

not_found:
	unmutex_lock(&ASFS_SB(sb)->lock);
	inode = NULL;
	asfs_debug("object not found.\n");
	if (0) {
found_inode:
		unmutex_lock(&ASFS_SB(sb)->lock);
		if (!(inode = iget_locked(sb, be32_to_cpu(obj->objectnode)))) {
			asfs_debug("ASFS: Strange - no inode allocated.\n");
			return ERR_PTR(res);
		}
		if (inode->i_state & I_NEW) {
			asfs_read_locked_inode(inode, obj);
			unlock_new_inode(inode);
		}
		asfs_brelse(bh);
	}
	res = 0;
	d_set_d_op(dentry, &asfs_dentry_operations);
	d_add(dentry, inode);
	return NULL;
}
