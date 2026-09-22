// SPDX-License-Identifier: GPL-2.0

/*
 * EXT2 — Regular-file VFS operations
 *
 * Purpose:
 *   Implements the regular-file interface, including open/read/write/mmap/direct-I/O related paths and any small file-facing operations consolidated into this unit.
 *
 * Filesystem model:
 *   This file belongs to a deliberately strict, non-journalled EXT2 VFS implementation.
 *
 * Correctness focus:
 *   I/O ordering, size visibility, writeback and error propagation must agree with the filesystem's allocation and journaling rules.
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

#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/fileattr.h>
#include <linux/mount.h>
#include <linux/uaccess.h>


#include <linux/time.h>
#include <linux/pagemap.h>
#include <linux/dax.h>
#include <linux/quotaops.h>
#include <linux/iomap.h>
#include <linux/uio.h>
#include <linux/buffer_head.h>
#include "ext2.h"

#ifdef CONFIG_FS_DAX


/**
 * ext2_dax_read_iter - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext2_dax_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	ssize_t ret;

	if (!iov_iter_count(to))
		return 0;

	inode_lock_shared(inode);
	ret = dax_iomap_rw(iocb, to, &ext2_iomap_ops);
	inode_unlock_shared(inode);

	file_accessed(iocb->ki_filp);
	return ret;
}


/**
 * ext2_dax_write_iter - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext2_dax_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	ssize_t ret;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out_unlock;
	ret = file_remove_privs(file);
	if (ret)
		goto out_unlock;
	ret = file_update_time(file);
	if (ret)
		goto out_unlock;

	ret = dax_iomap_rw(iocb, from, &ext2_iomap_ops);
	if (ret > 0 && iocb->ki_pos > i_size_read(inode)) {
		i_size_write(inode, iocb->ki_pos);
		mark_inode_dirty(inode);
	}

out_unlock:
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}


/**
 * ext2_dax_fault - Implements the dax fault operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static vm_fault_t ext2_dax_fault(struct vm_fault *vmf)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	vm_fault_t ret;
	bool write = (vmf->flags & FAULT_FLAG_WRITE) &&
		(vmf->vma->vm_flags & VM_SHARED);

	if (write) {
		sb_start_pagefault(inode->i_sb);
		file_update_time(vmf->vma->vm_file);
	}
	filemap_invalidate_lock_shared(inode->i_mapping);

	ret = dax_iomap_fault(vmf, 0, NULL, NULL, &ext2_iomap_ops);

	filemap_invalidate_unlock_shared(inode->i_mapping);
	if (write)
		sb_end_pagefault(inode->i_sb);
	return ret;
}

static const struct vm_operations_struct ext2_dax_vm_ops = {
	.fault		= ext2_dax_fault,


	.page_mkwrite	= ext2_dax_fault,
	.pfn_mkwrite	= ext2_dax_fault,
};


/**
 * ext2_file_mmap - Implements the file mmap operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext2_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!IS_DAX(file_inode(file)))
		return generic_file_mmap(file, vma);

	file_accessed(file);
	vma->vm_ops = &ext2_dax_vm_ops;
	return 0;
}
#else
#define ext2_file_mmap	generic_file_mmap
#endif


/**
 * ext2_release_file - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext2_release_file (struct inode * inode, struct file * filp)
{
	if (filp->f_mode & FMODE_WRITE) {
		mutex_lock(&EXT2_I(inode)->truncate_mutex);
		ext2_discard_reservation(inode);
		mutex_unlock(&EXT2_I(inode)->truncate_mutex);
	}
	return 0;
}


/**
 * ext2_fsync - Drives pending state toward the durability guarantee required by the calling VFS or journal interface.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext2_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	int ret;
	struct super_block *sb = file->f_mapping->host->i_sb;

	ret = generic_buffers_fsync(file, start, end, datasync);
	if (ret == -EIO)

		ext2_error(sb, __func__,
			   "detected IO error when writing metadata buffers");
	return ret;
}


/**
 * ext2_dio_read_iter - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext2_dio_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	ssize_t ret;

	inode_lock_shared(inode);
	ret = iomap_dio_rw(iocb, to, &ext2_iomap_ops, NULL, 0, NULL, 0);
	inode_unlock_shared(inode);

	return ret;
}


/**
 * ext2_dio_write_end_io - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext2_dio_write_end_io(struct kiocb *iocb, ssize_t size,
				 int error, unsigned int flags)
{
	loff_t pos = iocb->ki_pos;
	struct inode *inode = file_inode(iocb->ki_filp);

	if (error)
		goto out;


	pos += size;
	if (pos > i_size_read(inode)) {
		i_size_write(inode, pos);
		mark_inode_dirty(inode);
	}
out:
	return error;
}

static const struct iomap_dio_ops ext2_dio_write_ops = {
	.end_io = ext2_dio_write_end_io,
};


/**
 * ext2_dio_write_iter - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext2_dio_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file->f_mapping->host;
	ssize_t ret;
	unsigned int flags = 0;
	unsigned long blocksize = inode->i_sb->s_blocksize;
	loff_t offset = iocb->ki_pos;
	loff_t count = iov_iter_count(from);
	ssize_t status = 0;

	inode_lock(inode);
	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out_unlock;

	ret = kiocb_modified(iocb);
	if (ret)
		goto out_unlock;


	if (iocb->ki_pos + iov_iter_count(from) > i_size_read(inode) ||
	   (!IS_ALIGNED(iocb->ki_pos | iov_iter_alignment(from), blocksize)))
		flags |= IOMAP_DIO_FORCE_WAIT;

	ret = iomap_dio_rw(iocb, from, &ext2_iomap_ops, &ext2_dio_write_ops,
			   flags, NULL, 0);


	if (ret == -ENOTBLK)
		ret = 0;

	if (ret < 0 && ret != -EIOCBQUEUED)
		ext2_write_failed(inode->i_mapping, offset + count);


	if (ret >= 0 && iov_iter_count(from)) {
		loff_t pos, endbyte;
		int ret2;

		iocb->ki_flags &= ~IOCB_DIRECT;
		pos = iocb->ki_pos;
		status = generic_perform_write(iocb, from);
		if (unlikely(status < 0)) {
			ret = status;
			goto out_unlock;
		}

		ret += status;
		endbyte = pos + status - 1;
		ret2 = filemap_write_and_wait_range(inode->i_mapping, pos,
						    endbyte);
		if (!ret2) {
			invalidate_mapping_pages(inode->i_mapping,
						 pos >> PAGE_SHIFT,
						 endbyte >> PAGE_SHIFT);
			if (ret > 0)
				ret = generic_write_sync(iocb, ret);
		} else {
			ret = ret2;
		}
	}

out_unlock:
	inode_unlock(inode);
	if (status)
	return ret;
}


/**
 * ext2_file_read_iter - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext2_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
#ifdef CONFIG_FS_DAX
	if (IS_DAX(iocb->ki_filp->f_mapping->host))
		return ext2_dax_read_iter(iocb, to);
#endif
	if (iocb->ki_flags & IOCB_DIRECT)
		return ext2_dio_read_iter(iocb, to);

	return generic_file_read_iter(iocb, to);
}


/**
 * ext2_file_write_iter - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext2_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
#ifdef CONFIG_FS_DAX
	if (IS_DAX(iocb->ki_filp->f_mapping->host))
		return ext2_dax_write_iter(iocb, from);
#endif
	if (iocb->ki_flags & IOCB_DIRECT)
		return ext2_dio_write_iter(iocb, from);

	return generic_file_write_iter(iocb, from);
}


/**
 * ext2_file_open - Implements the file open operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext2_file_open(struct inode *inode, struct file *filp)
{
	filp->f_mode |= FMODE_CAN_ODIRECT;
	return dquot_file_open(inode, filp);
}

const struct file_operations ext2_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= ext2_file_read_iter,
	.write_iter	= ext2_file_write_iter,
	.unlocked_ioctl = ext2_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ext2_compat_ioctl,
#endif
	.mmap		= ext2_file_mmap,
	.open		= ext2_file_open,
	.release	= ext2_release_file,
	.fsync		= ext2_fsync,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
};

const struct inode_operations ext2_file_inode_operations = {
	.listxattr	= ext2_listxattr,
	.getattr	= ext2_getattr,
	.setattr	= ext2_setattr,
	.get_inode_acl	= ext2_get_acl,
	.set_acl	= ext2_set_acl,
	.fiemap		= ext2_fiemap,
	.fileattr_get	= ext2_fileattr_get,
	.fileattr_set	= ext2_fileattr_set,
};


/**
 * ext2_fileattr_get - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext2_fileattr_get(struct dentry *dentry, struct fileattr *fa)
{
	struct ext2_inode_info *ei = EXT2_I(d_inode(dentry));

	fileattr_fill_flags(fa, ei->i_flags & EXT2_FL_USER_VISIBLE);

	return 0;
}


/**
 * ext2_fileattr_set - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext2_fileattr_set(struct mnt_idmap *idmap,
		      struct dentry *dentry, struct fileattr *fa)
{
	struct inode *inode = d_inode(dentry);
	struct ext2_inode_info *ei = EXT2_I(inode);

	if (fileattr_has_fsx(fa))
		return -EOPNOTSUPP;


	if (IS_NOQUOTA(inode))
		return -EPERM;

	ei->i_flags = (ei->i_flags & ~EXT2_FL_USER_MODIFIABLE) |
		(fa->flags & EXT2_FL_USER_MODIFIABLE);

	ext2_set_inode_flags(inode);
	inode_set_ctime_current(inode);
	mark_inode_dirty(inode);

	return 0;
}


/**
 * ext2_ioctl - Handles a filesystem-specific control operation exposed through the file API.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
long ext2_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct ext2_inode_info *ei = EXT2_I(inode);
	unsigned short rsv_window_size;
	int ret;

	ext2_debug ("cmd = %u, arg = %lu\n", cmd, arg);

	switch (cmd) {
	case EXT2_IOC_GETVERSION:
		return put_user(inode->i_generation, (int __user *) arg);
	case EXT2_IOC_SETVERSION: {
		__u32 generation;

		if (!inode_owner_or_capable(&nop_mnt_idmap, inode))
			return -EPERM;
		ret = mnt_want_write_file(filp);
		if (ret)
			return ret;
		if (get_user(generation, (int __user *) arg)) {
			ret = -EFAULT;
			goto setversion_out;
		}

		inode_lock(inode);
		inode_set_ctime_current(inode);
		inode->i_generation = generation;
		inode_unlock(inode);

		mark_inode_dirty(inode);
setversion_out:
		mnt_drop_write_file(filp);
		return ret;
	}
	case EXT2_IOC_GETRSVSZ:
		if (test_opt(inode->i_sb, RESERVATION)
			&& S_ISREG(inode->i_mode)
			&& ei->i_block_alloc_info) {
			rsv_window_size = ei->i_block_alloc_info->rsv_window_node.rsv_goal_size;
			return put_user(rsv_window_size, (int __user *)arg);
		}
		return -ENOTTY;
	case EXT2_IOC_SETRSVSZ: {

		if (!test_opt(inode->i_sb, RESERVATION) ||!S_ISREG(inode->i_mode))
			return -ENOTTY;

		if (!inode_owner_or_capable(&nop_mnt_idmap, inode))
			return -EACCES;

		if (get_user(rsv_window_size, (int __user *)arg))
			return -EFAULT;

		ret = mnt_want_write_file(filp);
		if (ret)
			return ret;

		if (rsv_window_size > EXT2_MAX_RESERVE_BLOCKS)
			rsv_window_size = EXT2_MAX_RESERVE_BLOCKS;


		mutex_lock(&ei->truncate_mutex);
		if (!ei->i_block_alloc_info)
			ext2_init_block_alloc_info(inode);

		if (ei->i_block_alloc_info){
			struct ext2_reserve_window_node *rsv = &ei->i_block_alloc_info->rsv_window_node;
			rsv->rsv_goal_size = rsv_window_size;
		} else {
			ret = -ENOMEM;
		}

		mutex_unlock(&ei->truncate_mutex);
		mnt_drop_write_file(filp);
		return ret;
	}
	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT


/**
 * ext2_compat_ioctl - Handles a filesystem-specific control operation exposed through the file API.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT2
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
long ext2_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{

	switch (cmd) {
	case EXT2_IOC32_GETVERSION:
		cmd = EXT2_IOC_GETVERSION;
		break;
	case EXT2_IOC32_SETVERSION:
		cmd = EXT2_IOC_SETVERSION;
		break;
	default:
		return -ENOIOCTLCMD;
	}
	return ext2_ioctl(file, cmd, (unsigned long) compat_ptr(arg));
}
#endif
