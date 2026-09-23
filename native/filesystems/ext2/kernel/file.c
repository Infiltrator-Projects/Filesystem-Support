// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Filesystem Support EXT2 regular-file interface.
 *
 * This unit is an Infiltrator implementation of the Linux VFS-facing file
 * contract for EXT2.  It owns dispatch between buffered, direct and DAX I/O,
 * durability, reservation-window controls and file attributes.  Persistent
 * mapping/allocation semantics remain in the EXT2 inode/allocation units.
 */

#include <linux/buffer_head.h>
#include <linux/capability.h>
#include <linux/compat.h>
#include <linux/dax.h>
#include <linux/fileattr.h>
#include <linux/iomap.h>
#include <linux/mount.h>
#include <linux/pagemap.h>
#include <linux/quotaops.h>
#include <linux/uaccess.h>
#include <linux/uio.h>

#include "ext2.h"

static int ext2_regular_open(struct inode *inode, struct file *file)
{
	file->f_mode |= FMODE_CAN_ODIRECT;
	return dquot_file_open(inode, file);
}

static int ext2_regular_release(struct inode *inode, struct file *file)
{
	if (!(file->f_mode & FMODE_WRITE))
		return 0;

	mutex_lock(&EXT2_I(inode)->truncate_mutex);
	ext2_discard_reservation(inode);
	mutex_unlock(&EXT2_I(inode)->truncate_mutex);
	return 0;
}

int ext2_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	int err = generic_buffers_fsync(file, start, end, datasync);

	if (err == -EIO)
		ext2_error(file_inode(file)->i_sb, __func__,
			   "metadata writeback failed");
	return err;
}

#ifdef CONFIG_FS_DAX

static ssize_t ext2_dax_read(struct kiocb *iocb, struct iov_iter *to)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	ssize_t result;

	if (!iov_iter_count(to))
		return 0;

	inode_lock_shared(inode);
	result = dax_iomap_rw(iocb, to, &ext2_iomap_ops);
	inode_unlock_shared(inode);

	if (result >= 0)
		file_accessed(file);
	return result;
}

static ssize_t ext2_dax_write(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t result;

	inode_lock(inode);

	result = generic_write_checks(iocb, from);
	if (result <= 0)
		goto out_unlock;

	result = kiocb_modified(iocb);
	if (result)
		goto out_unlock;

	result = dax_iomap_rw(iocb, from, &ext2_iomap_ops);
	if (result > 0 && iocb->ki_pos > i_size_read(inode)) {
		i_size_write(inode, iocb->ki_pos);
		mark_inode_dirty(inode);
	}

out_unlock:
	inode_unlock(inode);
	if (result > 0)
		result = generic_write_sync(iocb, result);
	return result;
}

static vm_fault_t ext2_dax_fault(struct vm_fault *vmf)
{
	struct file *file = vmf->vma->vm_file;
	struct inode *inode = file_inode(file);
	const bool shared_write =
		(vmf->flags & FAULT_FLAG_WRITE) &&
		(vmf->vma->vm_flags & VM_SHARED);
	vm_fault_t result;

	if (shared_write) {
		sb_start_pagefault(inode->i_sb);
		file_update_time(file);
	}

	filemap_invalidate_lock_shared(inode->i_mapping);
	result = dax_iomap_fault(vmf, 0, NULL, NULL, &ext2_iomap_ops);
	filemap_invalidate_unlock_shared(inode->i_mapping);

	if (shared_write)
		sb_end_pagefault(inode->i_sb);

	return result;
}

static const struct vm_operations_struct ext2_dax_vm_ops = {
	.fault = ext2_dax_fault,
	.page_mkwrite = ext2_dax_fault,
	.pfn_mkwrite = ext2_dax_fault,
};

static int ext2_regular_mmap(struct file *file, struct vm_area_struct *vma)
{
	if (!IS_DAX(file_inode(file)))
		return generic_file_mmap(file, vma);

	file_accessed(file);
	vma->vm_ops = &ext2_dax_vm_ops;
	return 0;
}

#else

#define ext2_regular_mmap generic_file_mmap

#endif

static ssize_t ext2_direct_read(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t result;

	inode_lock_shared(inode);
	result = iomap_dio_rw(iocb, to, &ext2_iomap_ops, NULL, 0, NULL, 0);
	inode_unlock_shared(inode);
	return result;
}

static int ext2_direct_write_complete(struct kiocb *iocb, ssize_t size,
				      int error, unsigned int flags)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t end;

	if (error || size <= 0)
		return error;

	end = iocb->ki_pos + size;
	if (end > i_size_read(inode)) {
		i_size_write(inode, end);
		mark_inode_dirty(inode);
	}

	return 0;
}

static const struct iomap_dio_ops ext2_direct_write_ops = {
	.end_io = ext2_direct_write_complete,
};

static int ext2_flush_buffered_fallback(struct inode *inode,
					loff_t start, ssize_t written)
{
	loff_t end;
	int err;

	if (written <= 0)
		return 0;

	end = start + written - 1;
	err = filemap_write_and_wait_range(inode->i_mapping, start, end);
	if (err)
		return err;

	invalidate_mapping_pages(inode->i_mapping,
				 start >> PAGE_SHIFT,
				 end >> PAGE_SHIFT);
	return 0;
}

static ssize_t ext2_direct_write(struct kiocb *iocb, struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	const loff_t original_position = iocb->ki_pos;
	const size_t requested = iov_iter_count(from);
	const unsigned long block_size = inode->i_sb->s_blocksize;
	unsigned int dio_flags = 0;
	ssize_t direct_result;
	ssize_t buffered_result;
	int err;

	inode_lock(inode);

	direct_result = generic_write_checks(iocb, from);
	if (direct_result <= 0)
		goto out_unlock;

	err = kiocb_modified(iocb);
	if (err) {
		direct_result = err;
		goto out_unlock;
	}

	if (iocb->ki_pos + iov_iter_count(from) > i_size_read(inode) ||
	    !IS_ALIGNED(iocb->ki_pos | iov_iter_alignment(from), block_size))
		dio_flags |= IOMAP_DIO_FORCE_WAIT;

	direct_result = iomap_dio_rw(iocb, from, &ext2_iomap_ops,
				     &ext2_direct_write_ops, dio_flags,
				     NULL, 0);

	if (direct_result == -ENOTBLK)
		direct_result = 0;

	if (direct_result < 0 && direct_result != -EIOCBQUEUED) {
		ext2_write_failed(inode->i_mapping,
				  original_position + requested);
		goto out_unlock;
	}

	if (direct_result == -EIOCBQUEUED || !iov_iter_count(from))
		goto out_unlock;

	/*
	 * A partially completed direct request may leave an unaligned tail.
	 * Complete that tail through the buffered path, then push it to disk and
	 * invalidate the overlapping cache so direct-I/O visibility is coherent.
	 */
	iocb->ki_flags &= ~IOCB_DIRECT;
	{
		loff_t buffered_start = iocb->ki_pos;

		buffered_result = generic_perform_write(iocb, from);
		if (buffered_result < 0) {
			if (direct_result == 0)
				direct_result = buffered_result;
			goto out_unlock;
		}

		direct_result += buffered_result;
		err = ext2_flush_buffered_fallback(inode, buffered_start,
						   buffered_result);
		if (err) {
			direct_result = err;
			goto out_unlock;
		}
	}

	if (direct_result > 0)
		direct_result = generic_write_sync(iocb, direct_result);

out_unlock:
	inode_unlock(inode);
	return direct_result;
}

static ssize_t ext2_regular_read(struct kiocb *iocb, struct iov_iter *to)
{
#ifdef CONFIG_FS_DAX
	if (IS_DAX(file_inode(iocb->ki_filp)))
		return ext2_dax_read(iocb, to);
#endif
	if (iocb->ki_flags & IOCB_DIRECT)
		return ext2_direct_read(iocb, to);
	return generic_file_read_iter(iocb, to);
}

static ssize_t ext2_regular_write(struct kiocb *iocb, struct iov_iter *from)
{
#ifdef CONFIG_FS_DAX
	if (IS_DAX(file_inode(iocb->ki_filp)))
		return ext2_dax_write(iocb, from);
#endif
	if (iocb->ki_flags & IOCB_DIRECT)
		return ext2_direct_write(iocb, from);
	return generic_file_write_iter(iocb, from);
}

int ext2_fileattr_get(struct dentry *dentry, struct fileattr *fa)
{
	const struct ext2_inode_info *ei = EXT2_I(d_inode(dentry));

	fileattr_fill_flags(fa, ei->i_flags & EXT2_FL_USER_VISIBLE);
	return 0;
}

int ext2_fileattr_set(struct mnt_idmap *idmap,
		      struct dentry *dentry, struct fileattr *fa)
{
	struct inode *inode = d_inode(dentry);
	struct ext2_inode_info *ei = EXT2_I(inode);

	if (fileattr_has_fsx(fa))
		return -EOPNOTSUPP;
	if (IS_NOQUOTA(inode))
		return -EPERM;

	ei->i_flags &= ~EXT2_FL_USER_MODIFIABLE;
	ei->i_flags |= fa->flags & EXT2_FL_USER_MODIFIABLE;

	ext2_set_inode_flags(inode);
	inode_set_ctime_current(inode);
	mark_inode_dirty(inode);
	return 0;
}

static long ext2_set_generation(struct file *file, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	__u32 generation;
	int err;

	if (!inode_owner_or_capable(&nop_mnt_idmap, inode))
		return -EPERM;

	if (get_user(generation, (__u32 __user *)arg))
		return -EFAULT;

	err = mnt_want_write_file(file);
	if (err)
		return err;

	inode_lock(inode);
	inode->i_generation = generation;
	inode_set_ctime_current(inode);
	mark_inode_dirty(inode);
	inode_unlock(inode);

	mnt_drop_write_file(file);
	return 0;
}

static long ext2_get_reservation_size(struct inode *inode,
				      unsigned long arg)
{
	struct ext2_inode_info *ei = EXT2_I(inode);
	unsigned short size;

	if (!test_opt(inode->i_sb, RESERVATION) ||
	    !S_ISREG(inode->i_mode) ||
	    !ei->i_block_alloc_info)
		return -ENOTTY;

	size = ei->i_block_alloc_info->rsv_window_node.rsv_goal_size;
	return put_user(size, (int __user *)arg);
}

static long ext2_set_reservation_size(struct file *file, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct ext2_inode_info *ei = EXT2_I(inode);
	unsigned short size;
	int err;

	if (!test_opt(inode->i_sb, RESERVATION) || !S_ISREG(inode->i_mode))
		return -ENOTTY;
	if (!inode_owner_or_capable(&nop_mnt_idmap, inode))
		return -EACCES;
	if (get_user(size, (unsigned short __user *)arg))
		return -EFAULT;

	if (size > EXT2_MAX_RESERVE_BLOCKS)
		size = EXT2_MAX_RESERVE_BLOCKS;

	err = mnt_want_write_file(file);
	if (err)
		return err;

	mutex_lock(&ei->truncate_mutex);
	if (!ei->i_block_alloc_info)
		ext2_init_block_alloc_info(inode);

	if (ei->i_block_alloc_info)
		ei->i_block_alloc_info->rsv_window_node.rsv_goal_size = size;
	else
		err = -ENOMEM;
	mutex_unlock(&ei->truncate_mutex);

	mnt_drop_write_file(file);
	return err;
}

long ext2_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(file);

	switch (cmd) {
	case EXT2_IOC_GETVERSION:
		return put_user(inode->i_generation, (int __user *)arg);
	case EXT2_IOC_SETVERSION:
		return ext2_set_generation(file, arg);
	case EXT2_IOC_GETRSVSZ:
		return ext2_get_reservation_size(inode, arg);
	case EXT2_IOC_SETRSVSZ:
		return ext2_set_reservation_size(file, arg);
	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
long ext2_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	switch (cmd) {
	case EXT2_IOC32_GETVERSION:
		return ext2_ioctl(file, EXT2_IOC_GETVERSION,
				  (unsigned long)compat_ptr(arg));
	case EXT2_IOC32_SETVERSION:
		return ext2_ioctl(file, EXT2_IOC_SETVERSION,
				  (unsigned long)compat_ptr(arg));
	default:
		return -ENOIOCTLCMD;
	}
}
#endif

const struct file_operations ext2_file_operations = {
	.llseek = generic_file_llseek,
	.read_iter = ext2_regular_read,
	.write_iter = ext2_regular_write,
	.unlocked_ioctl = ext2_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ext2_compat_ioctl,
#endif
	.mmap = ext2_regular_mmap,
	.open = ext2_regular_open,
	.release = ext2_regular_release,
	.fsync = ext2_fsync,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read = filemap_splice_read,
	.splice_write = iter_file_splice_write,
};

const struct inode_operations ext2_file_inode_operations = {
	.listxattr = ext2_listxattr,
	.getattr = ext2_getattr,
	.setattr = ext2_setattr,
	.get_inode_acl = ext2_get_acl,
	.set_acl = ext2_set_acl,
	.fiemap = ext2_fiemap,
	.fileattr_get = ext2_fileattr_get,
	.fileattr_set = ext2_fileattr_set,
};
