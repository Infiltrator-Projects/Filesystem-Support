/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Infiltrator Filesystem Support — EXT4 regular-file VFS adapter.
 *
 * This unit is project-authored glue between the VFS, iomap/DAX, the EXT4
 * allocation/mapping engine and the project journal policy.
 */

// SPDX-License-Identifier: GPL-2.0

#include <linux/backing-dev.h>
#include <linux/dax.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/mman.h>
#include <linux/mount.h>
#include <linux/path.h>
#include <linux/quotaops.h>
#include <linux/uio.h>

#include "ext4.h"
#include "ext4_jbd2.h"
#include "truncate.h"
#include "xattr.h"

static bool ifs_ext4_dio_aligned(struct inode *inode,
				 loff_t pos,
				 struct iov_iter *iter)
{
	u32 align = ext4_dio_alignment(inode);

	if (align == 0)
		return false;
	if (align == 1)
		return true;
	return IS_ALIGNED(pos | iov_iter_alignment(iter), align);
}

static ssize_t ifs_ext4_validate_write(struct kiocb *iocb,
				       struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	ssize_t count;
	int err;

	if (IS_IMMUTABLE(inode))
		return -EPERM;

	count = generic_write_checks(iocb, from);
	if (count <= 0)
		return count;

	if (!ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)) {
		if (iocb->ki_pos >= sbi->s_bitmap_maxbytes)
			return -EFBIG;
		iov_iter_truncate(from, sbi->s_bitmap_maxbytes - iocb->ki_pos);
		count = iov_iter_count(from);
	}

	err = file_modified(iocb->ki_filp);
	if (err)
		return err;

	return count;
}

static ssize_t ifs_ext4_buffered_write(struct kiocb *iocb,
				       struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t written;

	if (iocb->ki_flags & IOCB_NOWAIT)
		return -EOPNOTSUPP;

	inode_lock(inode);
	written = ifs_ext4_validate_write(iocb, from);
	if (written > 0)
		written = generic_perform_write(iocb, from);
	inode_unlock(inode);

	if (written <= 0)
		return written;
	return generic_write_sync(iocb, written);
}

static ssize_t ifs_ext4_direct_read(struct kiocb *iocb,
				    struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t result;

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (!inode_trylock_shared(inode))
			return -EAGAIN;
	} else {
		inode_lock_shared(inode);
	}

	if (!ifs_ext4_dio_aligned(inode, iocb->ki_pos, to)) {
		inode_unlock_shared(inode);
		iocb->ki_flags &= ~IOCB_DIRECT;
		return generic_file_read_iter(iocb, to);
	}

	result = iomap_dio_rw(iocb, to, &ext4_iomap_ops, NULL, 0, NULL, 0);
	inode_unlock_shared(inode);
	if (result >= 0)
		file_accessed(iocb->ki_filp);
	return result;
}

#ifdef CONFIG_FS_DAX
static ssize_t ifs_ext4_dax_read(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t result;

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (!inode_trylock_shared(inode))
			return -EAGAIN;
	} else {
		inode_lock_shared(inode);
	}

	if (!IS_DAX(inode)) {
		inode_unlock_shared(inode);
		return generic_file_read_iter(iocb, to);
	}

	result = dax_iomap_rw(iocb, to, &ext4_iomap_ops);
	inode_unlock_shared(inode);
	if (result >= 0)
		file_accessed(iocb->ki_filp);
	return result;
}
#endif

static ssize_t ifs_ext4_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);

	if (ext4_forced_shutdown(inode->i_sb))
		return -EIO;
	if (!iov_iter_count(to))
		return 0;

#ifdef CONFIG_FS_DAX
	if (IS_DAX(inode))
		return ifs_ext4_dax_read(iocb, to);
#endif
	if (iocb->ki_flags & IOCB_DIRECT)
		return ifs_ext4_direct_read(iocb, to);
	return generic_file_read_iter(iocb, to);
}

static ssize_t ifs_ext4_splice_read(struct file *file,
				    loff_t *position,
				    struct pipe_inode_info *pipe,
				    size_t length,
				    unsigned int flags)
{
	if (ext4_forced_shutdown(file_inode(file)->i_sb))
		return -EIO;
	return filemap_splice_read(file, position, pipe, length, flags);
}

static int ifs_ext4_release(struct inode *inode, struct file *file)
{
	struct ext4_inode_info *ei = EXT4_I(inode);

	if (ext4_test_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE)) {
		ext4_alloc_da_blocks(inode);
		ext4_clear_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE);
	}

	if ((file->f_mode & FMODE_WRITE) &&
	    atomic_read(&inode->i_writecount) == 1 &&
	    !ei->i_reserved_data_blocks) {
		down_write(&ei->i_data_sem);
		ext4_discard_preallocations(inode);
		up_write(&ei->i_data_sem);
	}

	if (is_dx(inode) && file->private_data)
		ext4_htree_free_dir_info(file->private_data);
	return 0;
}

static bool ifs_ext4_range_extends_inode(struct inode *inode,
					 loff_t pos,
					 size_t length)
{
	loff_t end;

	if (check_add_overflow(pos, (loff_t)length, &end))
		return true;
	return end > i_size_read(inode) || end > EXT4_I(inode)->i_disksize;
}

static bool ifs_ext4_range_is_unaligned(struct inode *inode,
					loff_t pos,
					struct iov_iter *from)
{
	unsigned long mask = inode->i_sb->s_blocksize - 1;

	return ((unsigned long)pos | iov_iter_alignment(from)) & mask;
}

static bool ifs_ext4_range_is_mapped(struct inode *inode,
				     loff_t pos,
				     loff_t length,
				     bool *unwritten)
{
	struct ext4_map_blocks map = { };
	unsigned int bits = inode->i_blkbits;
	int expected;
	int mapped;

	if (length <= 0 || pos + length > i_size_read(inode))
		return false;

	map.m_lblk = pos >> bits;
	map.m_len = EXT4_MAX_BLOCKS(length, pos, bits);
	expected = map.m_len;
	mapped = ext4_map_blocks(NULL, inode, &map, 0);
	if (mapped != expected)
		return false;

	*unwritten = !(map.m_flags & EXT4_MAP_MAPPED);
	return true;
}

static int ifs_ext4_begin_extension(struct inode *inode)
{
	handle_t *handle;
	int err;

	handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	err = ext4_orphan_add(handle, inode);
	ext4_journal_stop(handle);
	return err;
}

static void ifs_ext4_finish_extension(struct inode *inode,
				      loff_t start,
				      ssize_t written,
				      size_t requested)
{
	handle_t *handle;

	if (written < 0) {
		ext4_truncate_failed_write(inode);
		if (inode->i_nlink)
			ext4_orphan_del(NULL, inode);
		return;
	}

	handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
	if (IS_ERR(handle)) {
		if (inode->i_nlink)
			ext4_orphan_del(NULL, inode);
		return;
	}

	if (written > 0 && ext4_update_inode_size(inode, start + written))
		ext4_mark_inode_dirty(handle, inode);

	if ((size_t)written == requested && inode->i_nlink)
		ext4_orphan_del(handle, inode);
	ext4_journal_stop(handle);

	if ((size_t)written < requested)
		ext4_truncate_failed_write(inode);
}

static int ifs_ext4_dio_end_io(struct kiocb *iocb,
			       ssize_t size,
			       int error,
			       unsigned int flags)
{
	struct inode *inode = file_inode(iocb->ki_filp);

	if (!error && size > 0 && (flags & IOMAP_DIO_UNWRITTEN))
		error = ext4_convert_unwritten_extents(
			NULL, inode, iocb->ki_pos, size);
	if (error)
		return error;

	if (iocb->ki_pos + size > READ_ONCE(EXT4_I(inode)->i_disksize) ||
	    iocb->ki_pos + size > i_size_read(inode)) {
		handle_t *handle =
			ext4_journal_start(inode, EXT4_HT_INODE, 2);

		if (IS_ERR(handle))
			return PTR_ERR(handle);
		if (ext4_update_inode_size(inode, iocb->ki_pos + size))
			error = ext4_mark_inode_dirty(handle, inode);
		ext4_journal_stop(handle);
		if (error)
			return error;
	}

	return size;
}

static const struct iomap_dio_ops ifs_ext4_dio_ops = {
	.end_io = ifs_ext4_dio_end_io,
};

static ssize_t ifs_ext4_direct_write(struct kiocb *iocb,
				     struct iov_iter *from)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	const struct iomap_ops *ops = &ext4_iomap_ops;
	loff_t start = iocb->ki_pos;
	size_t requested = iov_iter_count(from);
	bool exclusive = start + requested > i_size_read(inode);
	bool extend;
	bool unwritten = false;
	bool unaligned;
	bool mapped;
	int dio_flags = 0;
	ssize_t count;
	ssize_t result;

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (exclusive) {
			if (!inode_trylock(inode))
				return -EAGAIN;
		} else if (!inode_trylock_shared(inode)) {
			return -EAGAIN;
		}
	} else if (exclusive) {
		inode_lock(inode);
	} else {
		inode_lock_shared(inode);
	}

	if (!ifs_ext4_dio_aligned(inode, start, from)) {
		if (exclusive)
			inode_unlock(inode);
		else
			inode_unlock_shared(inode);
		return ifs_ext4_buffered_write(iocb, from);
	}

	count = ifs_ext4_validate_write(iocb, from);
	if (count <= 0)
		goto unlock;

	start = iocb->ki_pos;
	requested = count;
	extend = ifs_ext4_range_extends_inode(inode, start, requested);
	unaligned = ifs_ext4_range_is_unaligned(inode, start, from);
	mapped = ifs_ext4_range_is_mapped(
		inode, start, requested, &unwritten);

	if (!exclusive && (extend || !mapped || (unaligned && unwritten))) {
		if (iocb->ki_flags & IOCB_NOWAIT) {
			result = -EAGAIN;
			goto unlock_result;
		}
		inode_unlock_shared(inode);
		exclusive = true;
		inode_lock(inode);

		count = ifs_ext4_validate_write(iocb, from);
		if (count <= 0)
			return count;
		start = iocb->ki_pos;
		requested = count;
		extend = ifs_ext4_range_extends_inode(inode, start, requested);
		unaligned = ifs_ext4_range_is_unaligned(inode, start, from);
		mapped = ifs_ext4_range_is_mapped(
			inode, start, requested, &unwritten);
	}

	if (unaligned && (!mapped || unwritten)) {
		if (iocb->ki_flags & IOCB_NOWAIT) {
			result = -EAGAIN;
			goto unlock_result;
		}
		inode_dio_wait(inode);
		dio_flags |= IOMAP_DIO_FORCE_WAIT;
	}

	ext4_clear_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);

	if (extend) {
		result = ifs_ext4_begin_extension(inode);
		if (result)
			goto unlock_result;
	}

	if (!exclusive && !unwritten)
		ops = &ext4_iomap_overwrite_ops;

	result = iomap_dio_rw(
		iocb, from, ops, &ifs_ext4_dio_ops, dio_flags, NULL, 0);
	if (result == -ENOTBLK)
		result = 0;

	if (extend)
		ifs_ext4_finish_extension(
			inode, start, result, requested);

unlock_result:
	if (exclusive)
		inode_unlock(inode);
	else
		inode_unlock_shared(inode);

	if (result >= 0 && iov_iter_count(from)) {
		loff_t buffered_start = iocb->ki_pos;
		ssize_t buffered = ifs_ext4_buffered_write(iocb, from);

		if (buffered < 0)
			return buffered;
		if (buffered > 0) {
			loff_t last = buffered_start + buffered - 1;
			int err = filemap_write_and_wait_range(
				file->f_mapping, buffered_start, last);

			if (err)
				return err;
			invalidate_mapping_pages(
				file->f_mapping,
				buffered_start >> PAGE_SHIFT,
				last >> PAGE_SHIFT);
		}
		result += buffered;
	}

	return result;

unlock:
	result = count;
	goto unlock_result;
}

#ifdef CONFIG_FS_DAX
static ssize_t ifs_ext4_dax_write(struct kiocb *iocb,
				   struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t start;
	size_t requested;
	bool extend;
	ssize_t count;
	ssize_t result;

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (!inode_trylock(inode))
			return -EAGAIN;
	} else {
		inode_lock(inode);
	}

	count = ifs_ext4_validate_write(iocb, from);
	if (count <= 0) {
		inode_unlock(inode);
		return count;
	}

	start = iocb->ki_pos;
	requested = count;
	extend = ifs_ext4_range_extends_inode(inode, start, requested);
	if (extend) {
		result = ifs_ext4_begin_extension(inode);
		if (result) {
			inode_unlock(inode);
			return result;
		}
	}

	result = dax_iomap_rw(iocb, from, &ext4_iomap_ops);
	if (extend)
		ifs_ext4_finish_extension(
			inode, start, result, requested);
	inode_unlock(inode);

	if (result > 0)
		result = generic_write_sync(iocb, result);
	return result;
}
#endif

static ssize_t ifs_ext4_write_iter(struct kiocb *iocb,
				    struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);

	if (ext4_forced_shutdown(inode->i_sb))
		return -EIO;
#ifdef CONFIG_FS_DAX
	if (IS_DAX(inode))
		return ifs_ext4_dax_write(iocb, from);
#endif
	if (iocb->ki_flags & IOCB_DIRECT)
		return ifs_ext4_direct_write(iocb, from);
	return ifs_ext4_buffered_write(iocb, from);
}

static const struct vm_operations_struct ifs_ext4_buffered_vm_ops = {
	.fault = filemap_fault,
	.map_pages = filemap_map_pages,
	.page_mkwrite = ext4_page_mkwrite,
};

#ifdef CONFIG_FS_DAX
static vm_fault_t ifs_ext4_dax_fault_order(struct vm_fault *vmf,
					   unsigned int order)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	bool write = (vmf->flags & FAULT_FLAG_WRITE) &&
		     (vmf->vma->vm_flags & VM_SHARED);
	handle_t *handle = NULL;
	vm_fault_t result;
	pfn_t pfn;
	int error = 0;
	int retries = 0;

	if (write) {
		sb_start_pagefault(inode->i_sb);
		file_update_time(vmf->vma->vm_file);
	}
	filemap_invalidate_lock_shared(mapping);

retry:
	if (write) {
		handle = ext4_journal_start_sb(
			inode->i_sb, EXT4_HT_WRITE_PAGE,
			EXT4_DATA_TRANS_BLOCKS(inode->i_sb));
		if (IS_ERR(handle)) {
			result = VM_FAULT_SIGBUS;
			goto out;
		}
	}

	result = dax_iomap_fault(
		vmf, order, &pfn, &error, &ext4_iomap_ops);

	if (write) {
		ext4_journal_stop(handle);
		handle = NULL;
		if ((result & VM_FAULT_ERROR) &&
		    error == -ENOSPC &&
		    ext4_should_retry_alloc(inode->i_sb, &retries))
			goto retry;
		if (result & VM_FAULT_NEEDDSYNC)
			result = dax_finish_sync_fault(vmf, order, pfn);
	}

out:
	filemap_invalidate_unlock_shared(mapping);
	if (write)
		sb_end_pagefault(inode->i_sb);
	return result;
}

static vm_fault_t ifs_ext4_dax_fault(struct vm_fault *vmf)
{
	return ifs_ext4_dax_fault_order(vmf, 0);
}

static const struct vm_operations_struct ifs_ext4_dax_vm_ops = {
	.fault = ifs_ext4_dax_fault,
	.huge_fault = ifs_ext4_dax_fault_order,
	.page_mkwrite = ifs_ext4_dax_fault,
	.pfn_mkwrite = ifs_ext4_dax_fault,
};
#endif

static int ifs_ext4_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(file);

	if (ext4_forced_shutdown(inode->i_sb))
		return -EIO;
	if (!daxdev_mapping_supported(
		    vma, EXT4_SB(inode->i_sb)->s_daxdev))
		return -EOPNOTSUPP;

	file_accessed(file);
#ifdef CONFIG_FS_DAX
	if (IS_DAX(inode)) {
		vma->vm_ops = &ifs_ext4_dax_vm_ops;
		vm_flags_set(vma, VM_HUGEPAGE);
		return 0;
	}
#endif
	vma->vm_ops = &ifs_ext4_buffered_vm_ops;
	return 0;
}

static int ifs_ext4_record_mount_path(struct super_block *sb,
				      struct vfsmount *mount)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct path path = {
		.mnt = mount,
		.dentry = mount->mnt_root,
	};
	handle_t *handle;
	char storage[64] = { 0 };
	char *name;
	int err = 0;

	if (ext4_test_mount_flag(sb, EXT4_MF_MNTDIR_SAMPLED))
		return 0;
	if (sb_rdonly(sb) || !sb_start_intwrite_trylock(sb))
		return 0;

	ext4_set_mount_flag(sb, EXT4_MF_MNTDIR_SAMPLED);
	name = d_path(&path, storage, sizeof(storage));
	if (IS_ERR(name))
		goto out;

	handle = ext4_journal_start_sb(sb, EXT4_HT_MISC, 1);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out;
	}

	err = ext4_journal_get_write_access(
		handle, sb, sbi->s_sbh, EXT4_JTR_NONE);
	if (!err) {
		lock_buffer(sbi->s_sbh);
		strtomem_pad(sbi->s_es->s_last_mounted, name, 0);
		ext4_superblock_csum_set(sb);
		unlock_buffer(sbi->s_sbh);
		err = ext4_handle_dirty_metadata(
			handle, NULL, sbi->s_sbh);
	}
	ext4_journal_stop(handle);

out:
	sb_end_intwrite(sb);
	return err;
}

static int ifs_ext4_open(struct inode *inode, struct file *file)
{
	int err;

	if (ext4_forced_shutdown(inode->i_sb))
		return -EIO;

	err = ifs_ext4_record_mount_path(inode->i_sb, file->f_path.mnt);
	if (err)
		return err;
	err = fscrypt_file_open(inode, file);
	if (err)
		return err;
	err = fsverity_file_open(inode, file);
	if (err)
		return err;

	if (file->f_mode & FMODE_WRITE) {
		err = ext4_inode_attach_jinode(inode);
		if (err)
			return err;
	}

	file->f_mode |= FMODE_NOWAIT | FMODE_CAN_ODIRECT;
	return dquot_file_open(inode, file);
}

loff_t ext4_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file_inode(file);
	loff_t limit = ext4_get_maxbytes(inode);
	loff_t result;

	if (whence != SEEK_HOLE && whence != SEEK_DATA)
		return generic_file_llseek_size(
			file, offset, whence, limit, i_size_read(inode));

	inode_lock_shared(inode);
	if (whence == SEEK_HOLE)
		result = iomap_seek_hole(
			inode, offset, &ext4_iomap_report_ops);
	else
		result = iomap_seek_data(
			inode, offset, &ext4_iomap_report_ops);
	inode_unlock_shared(inode);

	if (result < 0)
		return result;
	return vfs_setpos(file, result, limit);
}

static int ifs_ext4_sync_parent_chain(struct inode *inode)
{
	struct dentry *dentry;
	int err = 0;

	if (!ext4_test_inode_state(inode, EXT4_STATE_NEWENTRY))
		return 0;

	dentry = d_find_any_alias(inode);
	if (!dentry)
		return 0;

	while (ext4_test_inode_state(inode, EXT4_STATE_NEWENTRY)) {
		struct dentry *parent;

		ext4_clear_inode_state(inode, EXT4_STATE_NEWENTRY);
		parent = dget_parent(dentry);
		dput(dentry);
		dentry = parent;
		inode = d_inode(dentry);

		err = sync_mapping_buffers(inode->i_mapping);
		if (!err)
			err = sync_inode_metadata(inode, 1);
		if (err)
			break;
	}

	dput(dentry);
	return err;
}

static int ifs_ext4_sync_without_journal(struct file *file,
					 loff_t start,
					 loff_t end,
					 int datasync,
					 bool *flush_device)
{
	struct inode *inode = file_inode(file);
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = 0,
	};
	int err;

	err = generic_buffers_fsync_noflush(
		file, start, end, datasync);
	if (err)
		return err;

	err = ext4_write_inode(inode, &wbc);
	if (!err)
		err = ifs_ext4_sync_parent_chain(inode);
	if (!err && test_opt(inode->i_sb, BARRIER))
		*flush_device = true;
	return err;
}

static int ifs_ext4_sync_with_journal(struct inode *inode,
				      bool datasync,
				      bool *flush_device)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	journal_t *journal = EXT4_SB(inode->i_sb)->s_journal;
	tid_t tid = datasync ? ei->i_datasync_tid : ei->i_sync_tid;

	if (!S_ISREG(inode->i_mode))
		return ext4_force_commit(inode->i_sb);

	if ((journal->j_flags & JBD2_BARRIER) &&
	    !jbd2_trans_will_send_data_barrier(journal, tid))
		*flush_device = true;
	return ext4_fc_commit(journal, tid);
}

int ext4_sync_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file_inode(file);
	bool flush_device = false;
	int err;
	int wb_err;

	if (ext4_forced_shutdown(inode->i_sb))
		return -EIO;
	ASSERT(ext4_journal_current_handle() == NULL);

	if (sb_rdonly(inode->i_sb)) {
		smp_rmb();
		err = ext4_forced_shutdown(inode->i_sb) ? -EROFS : 0;
		goto finish;
	}

	err = file_write_and_wait_range(file, start, end);
	if (err)
		goto finish;

	if (EXT4_SB(inode->i_sb)->s_journal)
		err = ifs_ext4_sync_with_journal(
			inode, datasync != 0, &flush_device);
	else
		err = ifs_ext4_sync_without_journal(
			file, start, end, datasync, &flush_device);

	if (flush_device) {
		int flush_err = blkdev_issue_flush(inode->i_sb->s_bdev);

		if (!err)
			err = flush_err;
	}

finish:
	wb_err = file_check_and_advance_wb_err(file);
	return err ? err : wb_err;
}

const struct file_operations ext4_file_operations = {
	.llseek = ext4_llseek,
	.read_iter = ifs_ext4_read_iter,
	.write_iter = ifs_ext4_write_iter,
	.iopoll = iocb_bio_iopoll,
	.unlocked_ioctl = ext4_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ext4_compat_ioctl,
#endif
	.mmap = ifs_ext4_mmap,
	.open = ifs_ext4_open,
	.release = ifs_ext4_release,
	.fsync = ext4_sync_file,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read = ifs_ext4_splice_read,
	.splice_write = iter_file_splice_write,
	.fallocate = ext4_fallocate,
	.fop_flags = FOP_MMAP_SYNC | FOP_BUFFER_RASYNC |
		     FOP_DIO_PARALLEL_WRITE,
};

const struct inode_operations ext4_file_inode_operations = {
	.setattr = ext4_setattr,
	.getattr = ext4_file_getattr,
	.listxattr = ext4_listxattr,
	.get_inode_acl = ext4_get_acl,
	.set_acl = ext4_set_acl,
	.fiemap = ext4_fiemap,
	.fileattr_get = ext4_fileattr_get,
	.fileattr_set = ext4_fileattr_set,
};
