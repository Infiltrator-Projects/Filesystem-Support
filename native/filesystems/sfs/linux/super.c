/*
 *
 * Amiga Smart File System, Linux implementation
 *
 * version: 1.0beta12 for 2.6.19 kernel
 *  
 * Copyright (C) 2003,2004,2005,2006  Marek 'March' Szyprowski <marek@amiga.pl>
 *
 * NLS support by Pavel Fedin (C) 2005
 *
 *
 * Thanks to Marcin Kurek (Morgoth/Dreamolers-CAPS) for help and parts 
 * of original amiga version of SmartFilesystem source code. 
 *
 * SmartFilesystem is copyrighted (C) 2003 by: John Hendrikx, 
 * Ralph Schmidt, Emmanuel Lesueur, David Gerber and Marcin Kurek
 * 
 *
 * ASFS is based on the Amiga FFS filesystem for Linux
 * Copyright (C) 1993  Ray Burr
 * Copyright (C) 1996  Hans-Joachim Widmaier
 *
 * Earlier versions were based on the Linux implementation of 
 * the ROMFS file system
 * Copyright (C) 1997-1999  Janos Farkas <chexum@shadow.banki.hu>
 *
 * ASFS used some parts of the smbfs filesystem:
 * Copyright (C) 1995, 1996 by Paal-Kr. Engstad and Volker Lendecke
 * Copyright (C) 1997 by Volker Lendecke
 *
 * and parts of the Minix filesystem additionally
 * Copyright (C) 1991, 1992  Linus Torvalds
 * Copyright (C) 1996  Gertjan van Wingerde 
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version
 * 2 of the License, or (at your option) any later version.
 *
 */

/* todo:
 * - remove bugs
 * - add missing features (maybe safe-delete, other...)
 * - create other fs tools like mkfs.asfs and fsck.asfs, some data-recovery tools
 */

#define ASFS_VERSION "1.0beta12 (03.12.2006)"

#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/iversion.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include <linux/parser.h>
#include <linux/nls.h>
#include "asfs_fs.h"
#include "../core/sfs_core.h"

#include <asm/byteorder.h>

static void asfs_put_super(struct super_block *sb);
static int asfs_statfs(struct dentry *dentry, struct kstatfs *buf);
#ifdef CONFIG_ASFS_RW
static int asfs_remount(struct super_block *sb, int *flags, char *data);
#endif
static struct inode *asfs_alloc_inode(struct super_block *sb);
static void asfs_free_inode(struct inode *inode);

static char asfs_default_codepage[] = CONFIG_ASFS_DEFAULT_CODEPAGE;
static char asfs_default_iocharset[] = CONFIG_NLS_DEFAULT;

u32 asfs_calcchecksum(void *block, u32 blocksize)
{
	return ifs_sfs_calculate_block_checksum(block, blocksize);
}

static const struct super_operations asfs_ops = {
	.alloc_inode	= asfs_alloc_inode,
	.free_inode		= asfs_free_inode,
	.put_super		= asfs_put_super,
	.statfs			= asfs_statfs,
//	.show_options   = generic_show_options,
#ifdef CONFIG_ASFS_RW
	.remount_fs		= asfs_remount,
#endif
};

extern const struct dentry_operations asfs_dentry_operations;

enum {
	Opt_mode, Opt_setgid, Opt_setuid, Opt_prefix, Opt_volume, 
	Opt_lcvol, Opt_iocharset, Opt_codepage, Opt_ignore, Opt_err
};

static match_table_t tokens = {
	{Opt_mode, "mode=%o"},
	{Opt_setgid, "setgid=%u"},
	{Opt_setuid, "setuid=%u"},
	{Opt_prefix, "prefix=%s"},
	{Opt_volume, "volume=%s"},
	{Opt_lcvol, "lowercasevol"},
	{Opt_iocharset, "iocharset=%s"},
	{Opt_codepage, "codepage=%s"},
	{Opt_ignore, "grpquota"},
	{Opt_ignore, "noquota"},
	{Opt_ignore, "quota"},
	{Opt_ignore, "usrquota"},
	{Opt_err, NULL},
};

static int asfs_parse_options(char *options, struct super_block *sb)
{
	char *p;
	substring_t args[MAX_OPT_ARGS];

	if (!options)
		return 1;
	while ((p = strsep(&options, ",")) != NULL) {
		int token, option;
		if (!*p)
			continue;
		token = match_token(p, tokens, args);

		switch (token) {
		case Opt_mode:
			if (match_octal(&args[0], &option))
				goto no_arg;
			ASFS_SB(sb)->mode = option & 0777;
			break;
		case Opt_setgid:
			if (match_int(&args[0], &option))
				goto no_arg;
			ASFS_SB(sb)->gid = option;
			break;
		case Opt_setuid:
			if (match_int(&args[0], &option))
				goto no_arg;
			ASFS_SB(sb)->uid = option;
			break;
		case Opt_prefix:
			if (ASFS_SB(sb)->prefix) {
				kfree(ASFS_SB(sb)->prefix);
				ASFS_SB(sb)->prefix = NULL;
			}
			ASFS_SB(sb)->prefix = match_strdup(&args[0]);
			if (! ASFS_SB(sb)->prefix)
				return 0;
			break;
		case Opt_volume:
			if (ASFS_SB(sb)->root_volume) {
				kfree(ASFS_SB(sb)->root_volume);
				ASFS_SB(sb)->root_volume = NULL;
			}
			ASFS_SB(sb)->root_volume = match_strdup(&args[0]);
			if (! ASFS_SB(sb)->root_volume)
				return 0;
			break;
		case Opt_lcvol:
			ASFS_SB(sb)->flags |= ASFS_VOL_LOWERCASE;
			break;
		case Opt_iocharset:
			if (ASFS_SB(sb)->iocharset != asfs_default_iocharset) {
				kfree(ASFS_SB(sb)->iocharset);
				ASFS_SB(sb)->iocharset = NULL;
			}
			ASFS_SB(sb)->iocharset = match_strdup(&args[0]);
			if (!ASFS_SB(sb)->iocharset)
				return 0;
			break;
		case Opt_codepage:
			if (ASFS_SB(sb)->codepage != asfs_default_codepage) {
				kfree(ASFS_SB(sb)->codepage);
				ASFS_SB(sb)->codepage = NULL;
			}
			ASFS_SB(sb)->codepage = match_strdup(&args[0]);
			if (!ASFS_SB(sb)->codepage)
				return 0;
		case Opt_ignore:
		 	/* Silently ignore the quota options */
			break;
		default:
no_arg:
			printk("ASFS: Unrecognized mount option \"%s\" "
					"or missing value\n", p);
			return 0;
		}
	}
	return 1;
}

static IfsSfsRootStatus asfs_root_layout_status(
	const struct fsRootBlock *rootblock)
{
	return ifs_sfs_validate_root_layout(
		be32_to_cpu(rootblock->bheader.id),
		be16_to_cpu(rootblock->version),
		be32_to_cpu(rootblock->blocksize),
		be32_to_cpu(rootblock->totalblocks),
		be32_to_cpu(rootblock->bitmapbase),
		be32_to_cpu(rootblock->adminspacecontainer),
		be32_to_cpu(rootblock->rootobjectcontainer),
		be32_to_cpu(rootblock->extentbnoderoot),
		be32_to_cpu(rootblock->objectnoderoot));
}

static int asfs_apply_root_layout(
	struct super_block *sb,
	const struct fsRootBlock *rootblock,
	int silent)
{
	const IfsSfsRootStatus status = asfs_root_layout_status(rootblock);
	u32 blocks_inbitmap;
	u32 blocks_bitmap;

	if (status != IFS_SFS_ROOT_OK) {
		if (!silent)
			printk(KERN_ERR
			       "ASFS: invalid SFS root layout on dev %s: %s\n",
			       sb->s_id, ifs_sfs_root_status_string(status));
		return -EINVAL;
	}

	if (ifs_sfs_compute_bitmap_layout(
			be32_to_cpu(rootblock->blocksize),
			be32_to_cpu(rootblock->totalblocks),
			&blocks_inbitmap, &blocks_bitmap) != 0)
		return -EINVAL;

	ASFS_SB(sb)->totalblocks = be32_to_cpu(rootblock->totalblocks);
	ASFS_SB(sb)->rootobjectcontainer =
		be32_to_cpu(rootblock->rootobjectcontainer);
	ASFS_SB(sb)->extentbnoderoot = be32_to_cpu(rootblock->extentbnoderoot);
	ASFS_SB(sb)->objectnoderoot = be32_to_cpu(rootblock->objectnoderoot);
	ASFS_SB(sb)->flags |= 0xff & rootblock->bits;
	ASFS_SB(sb)->adminspacecontainer =
		be32_to_cpu(rootblock->adminspacecontainer);
	ASFS_SB(sb)->bitmapbase = be32_to_cpu(rootblock->bitmapbase);
	ASFS_SB(sb)->blocks_inbitmap = blocks_inbitmap;
	ASFS_SB(sb)->blocks_bitmap = blocks_bitmap;
	ASFS_SB(sb)->block_rovingblockptr = 0;
	return 0;
}

static int asfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct asfs_sb_info *sbi;
	struct buffer_head *bh;
	struct fsRootBlock *rootblock;
	struct inode *rootinode;

	sbi = kzalloc(sizeof(struct asfs_sb_info), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;
	sb->s_fs_info = sbi;
	mutex_init(&sbi->lock);

	/* Fill in defaults */
	ASFS_SB(sb)->uid = ASFS_DEFAULT_UID;
	ASFS_SB(sb)->gid = ASFS_DEFAULT_GID;
	ASFS_SB(sb)->mode = ASFS_DEFAULT_MODE;
	ASFS_SB(sb)->prefix = NULL;
	ASFS_SB(sb)->root_volume = NULL;
	ASFS_SB(sb)->flags = 0;
	ASFS_SB(sb)->iocharset = asfs_default_iocharset;
	ASFS_SB(sb)->codepage = asfs_default_codepage;

	if (!asfs_parse_options(data, sb)) {
		printk(KERN_ERR "ASFS: Error parsing options\n");
		return -EINVAL;
	}

	if (!sb_set_blocksize(sb, 512))
		return -EINVAL;
	sb->s_maxbytes = ASFS_MAXFILESIZE;

	bh = sb_bread(sb, 0);
	if (!bh) {
		printk(KERN_ERR "ASFS: unable to read superblock\n");
		return -EINVAL;
	}

	rootblock = (struct fsRootBlock *)bh->b_data;
	{
		const u32 blocksize = be32_to_cpu(rootblock->blocksize);
		const u32 totalblocks = be32_to_cpu(rootblock->totalblocks);
		const IfsSfsRootStatus probe_status =
			ifs_sfs_validate_root_probe(
				be32_to_cpu(rootblock->bheader.id),
				be16_to_cpu(rootblock->version),
				blocksize, totalblocks);
		struct buffer_head *primary_bh;
		struct buffer_head *backup_bh;
		struct fsRootBlock *primary_root;
		struct fsRootBlock *backup_root;
		int primary_valid;
		int backup_valid;
		int selected_copy;

		asfs_brelse(bh);
		bh = NULL;

		if (probe_status != IFS_SFS_ROOT_OK) {
			if (!silent)
				printk(KERN_ERR
				       "ASFS: invalid SFS root probe on dev %s: %s\n",
				       sb->s_id,
				       ifs_sfs_root_status_string(probe_status));
			return -EINVAL;
		}

		if (!sb_set_blocksize(sb, blocksize)) {
			printk(KERN_ERR
			       "ASFS: SFS blocksize %u is not supported on dev %s\n",
			       blocksize, sb->s_id);
			return -EINVAL;
		}

		primary_bh = sb_bread(sb, 0);
		backup_bh = sb_bread(sb, totalblocks - 1U);
		primary_root = primary_bh ?
			(struct fsRootBlock *)primary_bh->b_data : NULL;
		backup_root = backup_bh ?
			(struct fsRootBlock *)backup_bh->b_data : NULL;

		primary_valid = primary_root != NULL &&
			asfs_check_block((void *)primary_root, blocksize,
					 0, ASFS_ROOTID) &&
			asfs_root_layout_status(primary_root) == IFS_SFS_ROOT_OK;
		backup_valid = backup_root != NULL &&
			asfs_check_block((void *)backup_root, blocksize,
					 totalblocks - 1U, ASFS_ROOTID) &&
			asfs_root_layout_status(backup_root) == IFS_SFS_ROOT_OK;

		selected_copy = ifs_sfs_select_root_copy(
			primary_valid,
			primary_valid ? be16_to_cpu(primary_root->sequencenumber) : 0U,
			backup_valid,
			backup_valid ? be16_to_cpu(backup_root->sequencenumber) : 0U);
		if (selected_copy < 0) {
			if (!silent)
				printk(KERN_ERR
				       "ASFS: neither SFS root copy is valid on dev %s\n",
				       sb->s_id);
			asfs_brelse(primary_bh);
			asfs_brelse(backup_bh);
			return -EINVAL;
		}

		rootblock = selected_copy == 0 ? primary_root : backup_root;
		if (asfs_apply_root_layout(sb, rootblock, silent) != 0) {
			asfs_brelse(primary_bh);
			asfs_brelse(backup_bh);
			return -EINVAL;
		}

		/*
		 * Losing either redundant root copy is recoverable for reads, but it
		 * is not safe to continue writable until repair has restored the pair.
		 */
		if (!primary_valid || !backup_valid) {
			printk(KERN_NOTICE
			       "ASFS: one SFS root copy is invalid on dev %s; mounting read-only\n",
			       sb->s_id);
			ASFS_SB(sb)->flags |= ASFS_READONLY;
		}

		asfs_brelse(primary_bh);
		asfs_brelse(backup_bh);
	}

#ifdef CONFIG_ASFS_RW
	{
		struct buffer_head *tmpbh;

		tmpbh = asfs_breadcheck(
			sb, ASFS_SB(sb)->rootobjectcontainer,
			ASFS_OBJECTCONTAINER_ID);
		if (tmpbh) {
			struct fsRootInfo *ri =
				(struct fsRootInfo *)((u8 *)tmpbh->b_data +
					sb->s_blocksize - sizeof(struct fsRootInfo));
			ASFS_SB(sb)->freeblocks = be32_to_cpu(ri->freeblocks);
			asfs_brelse(tmpbh);
		} else {
			ASFS_SB(sb)->freeblocks = 0;
			ASFS_SB(sb)->flags |= ASFS_READONLY;
		}

		tmpbh = asfs_breadcheck(
			sb, ASFS_SB(sb)->rootobjectcontainer + 2U,
			ASFS_TRANSACTIONFAILURE_ID);
		if (tmpbh) {
			printk(KERN_NOTICE
			       "VFS: Found unfinished SFS transaction on dev %s; mounting read-only.\n",
			       sb->s_id);
			ASFS_SB(sb)->flags |= ASFS_READONLY;
			asfs_brelse(tmpbh);
		}

		if (!(ASFS_SB(sb)->flags & ASFS_READONLY))
			printk(KERN_NOTICE "VFS: Found Amiga SFS RootBlock on dev %s.\n",
			       sb->s_id);
	}
#else
	ASFS_SB(sb)->freeblocks = 0;
	ASFS_SB(sb)->flags |= ASFS_READONLY;
	printk(KERN_NOTICE "VFS: Found Amiga SFS RootBlock on dev %s.\n",
	       sb->s_id);
#endif

	sb->s_magic = ASFS_MAGIC;
	sb->s_flags |= SB_NODEV | SB_NOSUID;
	if (ASFS_SB(sb)->flags & ASFS_READONLY) 
		sb->s_flags |= SB_RDONLY;
	sb->s_op = &asfs_ops;
	asfs_debug("Case sensitive: %s\n", (ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE) ? "yes" : "no");

	if (ASFS_SB(sb)->codepage[0] != '\0' && strcmp(ASFS_SB(sb)->codepage, "none") != 0) {
		ASFS_SB(sb)->nls_disk = load_nls(ASFS_SB(sb)->codepage);
		if (!ASFS_SB(sb)->nls_disk) {
			printk(KERN_ERR "ASFS: codepage %s not found\n", ASFS_SB(sb)->codepage);
			return -EINVAL;
		}
		ASFS_SB(sb)->nls_io = load_nls(ASFS_SB(sb)->iocharset);
		if (!ASFS_SB(sb)->nls_io) {
			printk(KERN_ERR "ASFS: IO charset %s not found\n", ASFS_SB(sb)->iocharset);
			goto out2;
		}
	} else {
		ASFS_SB(sb)->nls_io = NULL;
		ASFS_SB(sb)->nls_disk = NULL;
	}

	if ((rootinode = asfs_get_root_inode(sb))) {
		sb->s_root = d_make_root(rootinode);
		if (sb->s_root) {
			d_set_d_op(sb->s_root, &asfs_dentry_operations);
			return 0;
		}
	}
	unload_nls(ASFS_SB(sb)->nls_io);
out2:
	unload_nls(ASFS_SB(sb)->nls_disk);
	return -EINVAL;

}

#ifdef CONFIG_ASFS_RW
static int asfs_remount(struct super_block *sb, int *flags, char *data)
{
	asfs_debug("ASFS: remount (flags=0x%x, opts=\"%s\")\n",*flags,data);

	if (!asfs_parse_options(data,sb))
		return -EINVAL;

	if ((*flags & SB_RDONLY) == (sb->s_flags & SB_RDONLY))
		return 0;

	if (*flags & SB_RDONLY) {
		sb->s_flags |= SB_RDONLY;
	} else if (!(ASFS_SB(sb)->flags & ASFS_READONLY)) {
		sb->s_flags &= ~SB_RDONLY;
	} else {
		printk("VFS: Can't remount Amiga SFS on dev %s read/write because of errors.", sb->s_id);
		return -EINVAL;
	}
	return 0;
}
#endif

static void asfs_put_super(struct super_block *sb)
{
	struct asfs_sb_info *sbi = ASFS_SB(sb);

	if (ASFS_SB(sb)->prefix)
		kfree(ASFS_SB(sb)->prefix);
	if (ASFS_SB(sb)->root_volume)
		kfree(ASFS_SB(sb)->root_volume);
	if (ASFS_SB(sb)->nls_disk)
		unload_nls(ASFS_SB(sb)->nls_disk);
	if (ASFS_SB(sb)->nls_io)
		unload_nls(ASFS_SB(sb)->nls_io);
	if (ASFS_SB(sb)->iocharset != asfs_default_iocharset)
		kfree(ASFS_SB(sb)->iocharset);
	if (ASFS_SB(sb)->codepage != asfs_default_codepage)
		kfree(ASFS_SB(sb)->codepage);

	kfree(sbi);
	sb->s_fs_info = NULL;
	return;
}

/* That's simple too. */
static int asfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;

	buf->f_type = ASFS_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_bfree = buf->f_bavail = ASFS_SB(sb)->freeblocks;
	buf->f_blocks = ASFS_SB(sb)->totalblocks;
	buf->f_namelen = ASFS_MAXFN;
	return 0;
}

/* --- new in 2.6.x --- */
static struct kmem_cache * asfs_inode_cachep;
static struct inode *asfs_alloc_inode(struct super_block *sb)
{
	struct asfs_inode_info *i;
	i = alloc_inode_sb(sb, asfs_inode_cachep, GFP_KERNEL);
	if (!i)
		return NULL;
	inode_set_iversion(&i->vfs_inode, 1);
	return &i->vfs_inode;
}

static void asfs_free_inode(struct inode *inode)
{
	kmem_cache_free(asfs_inode_cachep, ASFS_I(inode));
}
static void init_once(void *foo)
{
	struct asfs_inode_info *ei = (struct asfs_inode_info *) foo;
	inode_init_once(&ei->vfs_inode);
}

static int init_inodecache(void)
{
	asfs_inode_cachep = kmem_cache_create("asfs_inode_cache",
					     sizeof(struct asfs_inode_info),
					     0, SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
					     init_once);
	if (asfs_inode_cachep == NULL)
		return -ENOMEM;
	return 0;
}

static void destroy_inodecache(void)
{
	rcu_barrier();
	kmem_cache_destroy(asfs_inode_cachep);
}

static struct dentry *asfs_mount(struct file_system_type *fs_type,
	int flags, const char *dev_name, void *data)
{
	return mount_bdev(fs_type, flags, dev_name, data, asfs_fill_super);
}

static struct file_system_type asfs_fs_type = {
	.owner		= THIS_MODULE,
	.name		= "sfs",
	.mount		= asfs_mount,
	.kill_sb	= kill_block_super,
	.fs_flags	= FS_REQUIRES_DEV,
};

static int __init init_asfs_fs(void)
{
	int err = init_inodecache();
	if (err)
		goto out1;
	err = register_filesystem(&asfs_fs_type);
	if (err)
		goto out;
	return 0;
out:
	destroy_inodecache();
out1:
	return err;
}

static void __exit exit_asfs_fs(void)
{
	unregister_filesystem(&asfs_fs_type);
	destroy_inodecache();
}

/* Yes, works even as a module... :) */

#ifdef CONFIG_ASFS_RW
MODULE_DESCRIPTION("Amiga Smart File System (read/write) support v" ASFS_VERSION);
#else
MODULE_DESCRIPTION("Amiga Smart File System (read-only) support v" ASFS_VERSION);
#endif
MODULE_LICENSE("GPL");
MODULE_AUTHOR("Marek Szyprowski <marek@amiga.pl>");
MODULE_ALIAS_FS("sfs");

module_init(init_asfs_fs)
module_exit(exit_asfs_fs)
