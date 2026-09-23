/*
 *
 * Amiga Smart File System, Linux implementation
 * version: 1.0beta12
 *  
 * Copyright (C) 2003,2004,2005,2006  Marek 'March' Szyprowski <marek@amiga.pl>
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
#include <linux/time.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include <linux/dirent.h>
#include "asfs_fs.h"

#include <asm/byteorder.h>

#ifdef CONFIG_ASFS_RW
static int asfs_create(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl);
static int asfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode);
static int asfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, const char *symname);
static int asfs_rmdir(struct inode *dir, struct dentry *dentry);
static int asfs_unlink(struct inode *dir, struct dentry *dentry);
static int asfs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
		       struct dentry *old_dentry, struct inode *new_dir,
		       struct dentry *new_dentry, unsigned int flags);
static int asfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			struct iattr *attr);
#endif

/* Mapping from our types to the kernel */

static const struct address_space_operations asfs_aops = {
	.dirty_folio = block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
	.read_folio = asfs_read_folio,
	.readahead = asfs_readahead,
	.bmap = asfs_bmap,
#ifdef CONFIG_ASFS_RW
	.write_begin = asfs_write_begin,
	.write_end = generic_write_end,
	.writepages = asfs_writepages,
#endif
};

static const struct file_operations asfs_file_operations = {
	.llseek = generic_file_llseek,
	.read_iter = generic_file_read_iter,
	.mmap = generic_file_mmap,
	.splice_read = filemap_splice_read,
#ifdef CONFIG_ASFS_RW
	.write_iter = generic_file_write_iter,
	.open = asfs_file_open,
	.release = asfs_file_release,
	.fsync = generic_file_fsync,
	.splice_write = iter_file_splice_write,
#endif
};

static const struct file_operations asfs_dir_operations = {
	.read = generic_read_dir,
	.iterate_shared = asfs_readdir,
	.llseek = generic_file_llseek,
};

static const struct inode_operations asfs_dir_inode_operations = {
	.lookup = asfs_lookup,
#ifdef CONFIG_ASFS_RW
	.create = asfs_create,
	.unlink = asfs_unlink,
	.symlink = asfs_symlink,
	.mkdir = asfs_mkdir,
	.rmdir = asfs_rmdir,
	.rename = asfs_rename,
	.setattr = asfs_setattr,
#endif
};

static const struct inode_operations asfs_file_inode_operations = {
#ifdef CONFIG_ASFS_RW
	.setattr = asfs_setattr,
#endif
};

static const struct inode_operations asfs_symlink_inode_operations = {
	.get_link = asfs_get_link,
#ifdef CONFIG_ASFS_RW
	.setattr = asfs_setattr,
#endif
};

void asfs_read_locked_inode(struct inode *inode, void *arg)
{
	struct super_block *sb = inode->i_sb;
	struct fsObject *obj = arg;

	time64_t timestamp =
		(time64_t)be32_to_cpu(obj->datemodified) +
		(365 * 8 + 2) * 24 * 60 * 60;

	inode->i_mode = ASFS_SB(sb)->mode;
	/* Linux timestamps start in 1970; SFS timestamps start in 1978. */
	inode_set_atime(inode, timestamp, 0);
	inode_set_mtime(inode, timestamp, 0);
	inode_set_ctime(inode, timestamp, 0);
	i_uid_write(inode, ASFS_SB(sb)->uid);
	i_gid_write(inode, ASFS_SB(sb)->gid);
	atomic_set(&ASFS_I(inode)->i_opencnt, 0);

	asfs_debug("asfs_read_inode2: Setting-up node %lu... ", inode->i_ino);

	if (obj->bits & OTYPE_DIR) {
		asfs_debug("dir (FirstdirBlock: %u, HashTable %u)\n", \
		           be32_to_cpu(obj->object.dir.firstdirblock), be32_to_cpu(obj->object.dir.hashtable));

		inode->i_size = 0;
		inode->i_op = &asfs_dir_inode_operations;
		inode->i_fop = &asfs_dir_operations;
		inode->i_mode |= S_IFDIR | ((inode->i_mode & 0400) ? 0100 : 0) | 
		              ((inode->i_mode & 0040) ? 0010 : 0) | ((inode->i_mode & 0004) ? 0001 : 0);
		ASFS_I(inode)->firstblock = be32_to_cpu(obj->object.dir.firstdirblock);
		ASFS_I(inode)->hashtable = be32_to_cpu(obj->object.dir.hashtable);
		ASFS_I(inode)->modified = 0;
	} else if (obj->bits & OTYPE_LINK && !(obj->bits & OTYPE_HARDLINK)) {
		asfs_debug("symlink\n");
		inode->i_size = 0;
		inode->i_op = &asfs_symlink_inode_operations;
		inode->i_mode |= S_IFLNK | S_IRWXUGO;
		ASFS_I(inode)->firstblock = be32_to_cpu(obj->object.file.data);
	} else {
		asfs_debug("file (Size: %u, FirstBlock: %u)\n", be32_to_cpu(obj->object.file.size), be32_to_cpu(obj->object.file.data));
		inode->i_size = be32_to_cpu(obj->object.file.size);
		inode->i_blocks = (be32_to_cpu(obj->object.file.size) + sb->s_blocksize - 1) >> sb->s_blocksize_bits;
		inode->i_op = &asfs_file_inode_operations;
		inode->i_fop = &asfs_file_operations;
		inode->i_mapping->a_ops = &asfs_aops;
		inode->i_mode |= S_IFREG;
		ASFS_I(inode)->firstblock = be32_to_cpu(obj->object.file.data);
		ASFS_I(inode)->ext_cache.startblock = 0;
		ASFS_I(inode)->ext_cache.key = 0;
		ASFS_I(inode)->mmu_private = inode->i_size;
	}
	return;	
}

struct inode *asfs_get_root_inode(struct super_block *sb)
{
	struct inode *result = NULL;
	struct fsObject *obj;
	struct buffer_head *bh;

	asfs_debug("asfs_get_root_inode\n");

	if ((bh = asfs_breadcheck(sb, ASFS_SB(sb)->rootobjectcontainer, ASFS_OBJECTCONTAINER_ID))) {
		obj = &(((struct fsObjectContainer *)bh->b_data)->object[0]);
		if (be32_to_cpu(obj->objectnode) > 0)
			result = iget_locked(sb, be32_to_cpu(obj->objectnode));

		if (result != NULL && result->i_state & I_NEW) {
			asfs_read_locked_inode(result, obj);
			unlock_new_inode(result);
		}
		asfs_brelse(bh);
	}
	return result;
}

#ifdef CONFIG_ASFS_RW

static void asfs_set_current_times(struct inode *inode)
{
	struct timespec64 now = current_time(inode);

	inode_set_atime(inode, now.tv_sec, now.tv_nsec);
	inode_set_mtime_to_ts(inode, now);
	inode_set_ctime_to_ts(inode, now);
}

static void asfs_sync_dir_inode(struct inode *dir, struct fsObject *obj)
{
	ASFS_I(dir)->firstblock = be32_to_cpu(obj->object.dir.firstdirblock);
	ASFS_I(dir)->modified = 1;
	asfs_set_current_times(dir);
	obj->datemodified = cpu_to_be32(
		inode_get_mtime_sec(dir) - (365 * 8 + 2) * 24 * 60 * 60);
}

enum { it_file, it_dir, it_link };

static int asfs_create_object(struct inode *dir, struct dentry *dentry,
			      umode_t mode, int type, const char *symname)
{
	int error;
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	struct buffer_head *bh, *dir_bh;
	struct fsObject obj_data, *dir_obj, *obj;
	u8 *name = (u8 *)dentry->d_name.name;
	u8 bufname[ASFS_MAXFN_BUF];

	asfs_translate(bufname, name, ASFS_SB(sb)->nls_disk,
		       ASFS_SB(sb)->nls_io, ASFS_MAXFN_BUF);
	error = asfs_check_name(bufname, strlen(bufname));
	if (error)
		return error;

	inode = new_inode(sb);
	if (!inode)
		return -ENOMEM;

	asfs_set_current_times(inode);
	memset(&obj_data, 0, sizeof(obj_data));
	obj_data.protection =
		cpu_to_be32(FIBF_READ | FIBF_WRITE | FIBF_EXECUTE | FIBF_DELETE);
	obj_data.datemodified = cpu_to_be32(
		inode_get_mtime_sec(inode) - (365 * 8 + 2) * 24 * 60 * 60);

	switch (type) {
	case it_dir:
		obj_data.bits = OTYPE_DIR;
		break;
	case it_link:
		obj_data.bits = OTYPE_LINK;
		break;
	default:
		break;
	}

	mutex_lock(&ASFS_SB(sb)->lock);

	error = asfs_readobject(sb, dir->i_ino, &dir_bh, &dir_obj);
	if (error) {
		mutex_unlock(&ASFS_SB(sb)->lock);
		iput(inode);
		return error;
	}

	bh = dir_bh;
	obj = dir_obj;
	error = asfs_createobject(sb, &bh, &obj, &obj_data, bufname, FALSE);
	if (error) {
		if (bh != dir_bh)
			asfs_brelse(bh);
		asfs_brelse(dir_bh);
		mutex_unlock(&ASFS_SB(sb)->lock);
		iput(inode);
		return error;
	}

	inode->i_ino = be32_to_cpu(obj->objectnode);
	inode->i_size = 0;
	inode->i_blocks = 0;
	i_uid_write(inode, i_uid_read(dir));
	i_gid_write(inode, i_gid_read(dir));
	inode->i_mode = mode | ASFS_SB(sb)->mode;

	switch (type) {
	case it_dir:
		inode->i_mode |= S_IFDIR |
			((inode->i_mode & 0400) ? 0100 : 0) |
			((inode->i_mode & 0040) ? 0010 : 0) |
			((inode->i_mode & 0004) ? 0001 : 0);
		inode->i_op = &asfs_dir_inode_operations;
		inode->i_fop = &asfs_dir_operations;
		ASFS_I(inode)->firstblock =
			be32_to_cpu(obj->object.dir.firstdirblock);
		ASFS_I(inode)->hashtable =
			be32_to_cpu(obj->object.dir.hashtable);
		ASFS_I(inode)->modified = 0;
		break;
	case it_file:
		inode->i_mode |= S_IFREG;
		inode->i_op = &asfs_file_inode_operations;
		inode->i_fop = &asfs_file_operations;
		inode->i_mapping->a_ops = &asfs_aops;
		ASFS_I(inode)->firstblock =
			be32_to_cpu(obj->object.file.data);
		ASFS_I(inode)->ext_cache.startblock = 0;
		ASFS_I(inode)->ext_cache.key = 0;
		ASFS_I(inode)->mmu_private = 0;
		break;
	case it_link:
		inode->i_mode = S_IFLNK | S_IRWXUGO;
		inode->i_op = &asfs_symlink_inode_operations;
		ASFS_I(inode)->firstblock =
			be32_to_cpu(obj->object.file.data);
		error = asfs_write_symlink(inode, symname);
		break;
	default:
		error = -EINVAL;
		break;
	}

	if (!error) {
		asfs_bstore(sb, bh);
		insert_inode_hash(inode);
		mark_inode_dirty(inode);
		d_instantiate(dentry, inode);
		asfs_sync_dir_inode(dir, dir_obj);
		asfs_bstore(sb, dir_bh);
	}

	asfs_brelse(bh);
	asfs_brelse(dir_bh);
	mutex_unlock(&ASFS_SB(sb)->lock);

	if (error)
		iput(inode);
	return error;
}

static int asfs_create(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode, bool excl)
{
	(void)idmap;
	(void)excl;
	return asfs_create_object(dir, dentry, mode, it_file, NULL);
}

static int asfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		      struct dentry *dentry, umode_t mode)
{
	(void)idmap;
	return asfs_create_object(dir, dentry, mode, it_dir, NULL);
}

static int asfs_symlink(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, const char *symname)
{
	(void)idmap;
	return asfs_create_object(dir, dentry, 0, it_link, symname);
}

static int asfs_rmdir(struct inode *dir, struct dentry *dentry)
{
	if (ASFS_I(d_inode(dentry))->firstblock != 0)
		return -ENOTEMPTY;

	return asfs_unlink(dir, dentry);
}

static int asfs_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct super_block *sb = dir->i_sb;
	struct buffer_head *bh, *dir_bh;
	struct fsObject *dir_obj, *obj;
	int error;

	mutex_lock(&ASFS_SB(sb)->lock);

	error = asfs_readobject(sb, inode->i_ino, &bh, &obj);
	if (error)
		goto out_unlock;

	error = asfs_deleteobject(sb, bh, obj);
	asfs_brelse(bh);
	if (error)
		goto out_unlock;

	error = asfs_readobject(sb, dir->i_ino, &dir_bh, &dir_obj);
	if (error)
		goto out_unlock;

	asfs_sync_dir_inode(dir, dir_obj);
	asfs_bstore(sb, dir_bh);
	asfs_brelse(dir_bh);
	drop_nlink(inode);
	mark_inode_dirty(inode);

out_unlock:
	mutex_unlock(&ASFS_SB(sb)->lock);
	return error;
}

static int asfs_rename(struct mnt_idmap *idmap, struct inode *old_dir,
		       struct dentry *old_dentry, struct inode *new_dir,
		       struct dentry *new_dentry, unsigned int flags)
{
	struct super_block *sb = old_dir->i_sb;
	struct buffer_head *src_bh, *old_bh, *new_bh;
	struct fsObject *src_obj, *old_obj, *new_obj;
	u8 bufname[ASFS_MAXFN_BUF];
	int error;

	(void)idmap;
	if (flags)
		return -EINVAL;

	asfs_translate(bufname, (u8 *)new_dentry->d_name.name,
		       ASFS_SB(sb)->nls_disk, ASFS_SB(sb)->nls_io,
		       ASFS_MAXFN_BUF);
	error = asfs_check_name(bufname, strlen(bufname));
	if (error)
		return error;

	if (d_really_is_positive(new_dentry)) {
		error = asfs_unlink(new_dir, new_dentry);
		if (error)
			return error;
	}

	mutex_lock(&ASFS_SB(sb)->lock);

	error = asfs_readobject(sb, d_inode(old_dentry)->i_ino,
				&src_bh, &src_obj);
	if (error)
		goto out_unlock;

	error = asfs_readobject(sb, new_dir->i_ino, &new_bh, &new_obj);
	if (error) {
		asfs_brelse(src_bh);
		goto out_unlock;
	}

	error = asfs_renameobject(sb, src_bh, src_obj,
				 new_bh, new_obj, bufname);
	asfs_brelse(src_bh);
	asfs_brelse(new_bh);
	if (error)
		goto out_unlock;

	error = asfs_readobject(sb, old_dir->i_ino, &old_bh, &old_obj);
	if (error)
		goto out_unlock;

	error = asfs_readobject(sb, new_dir->i_ino, &new_bh, &new_obj);
	if (error) {
		asfs_brelse(old_bh);
		goto out_unlock;
	}

	asfs_sync_dir_inode(old_dir, old_obj);
	asfs_sync_dir_inode(new_dir, new_obj);
	asfs_bstore(sb, new_bh);
	asfs_bstore(sb, old_bh);
	asfs_brelse(old_bh);
	asfs_brelse(new_bh);
	mark_inode_dirty(old_dir);
	mark_inode_dirty(new_dir);

out_unlock:
	mutex_unlock(&ASFS_SB(sb)->lock);
	return error;
}

static int asfs_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
			struct iattr *attr)
{
	struct inode *inode = d_inode(dentry);
	loff_t old_size = i_size_read(inode);
	int error;

	error = setattr_prepare(idmap, dentry, attr);
	if (error)
		return error;

	if ((attr->ia_valid & ATTR_SIZE) && attr->ia_size != old_size) {
		if (attr->ia_size > ASFS_I(inode)->mmu_private)
			return -EOPNOTSUPP;

		truncate_setsize(inode, attr->ia_size);
		error = asfs_truncate(inode);
		if (error) {
			truncate_setsize(inode, old_size);
			return error;
		}
	}

	setattr_copy(idmap, inode, attr);
	mark_inode_dirty(inode);
	return 0;
}

#endif
