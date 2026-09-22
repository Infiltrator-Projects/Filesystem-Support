// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ext4/file.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/file.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext4 fs regular file handling primitives
 *
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 *	(jj@sunsite.ms.mff.cuni.cz)
 */

/*
 * EXT4 — Regular-file VFS operations
 *
 * Purpose:
 *   Implements the regular-file interface, including open/read/write/mmap/direct-I/O related paths and any small file-facing operations consolidated into this unit.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   I/O ordering, size visibility, writeback and error propagation must agree with the filesystem's allocation and journaling rules.
 *
 * Project rules:
 *   - Register and implement EXT4 only; do not route EXT2 or EXT3 mounts through this module.
 *   - Preserve every valid EXT4 feature path supported by the pinned implementation.
 *   - Treat journaling, extents, allocation, checksums, recovery and feature negotiation as correctness-critical state machines.
 *
 * Commentary policy:
 *   Comments explain invariants, ownership, persistence ordering and
 *   non-obvious design intent. They deliberately avoid restating C syntax.
 */

#include <linux/time.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/mount.h>
#include <linux/path.h>
#include <linux/dax.h>
#include <linux/quotaops.h>
#include <linux/pagevec.h>
#include <linux/uio.h>
#include <linux/mman.h>
#include <linux/backing-dev.h>
#include "ext4.h"
#include "ext4_jbd2.h"
#include "xattr.h"
#include "truncate.h"


/**
 * ext4_should_use_dio - Implements the should use dio operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool ext4_should_use_dio(struct kiocb *iocb, struct iov_iter *iter)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	u32 dio_align = ext4_dio_alignment(inode);

	if (dio_align == 0)
		return false;

	if (dio_align == 1)
		return true;

	return IS_ALIGNED(iocb->ki_pos | iov_iter_alignment(iter), dio_align);
}

/**
 * ext4_dio_read_iter - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext4_dio_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	ssize_t ret;
	struct inode *inode = file_inode(iocb->ki_filp);

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (!inode_trylock_shared(inode))
			return -EAGAIN;
	} else {
		inode_lock_shared(inode);
	}

	if (!ext4_should_use_dio(iocb, to)) {
		inode_unlock_shared(inode);


		iocb->ki_flags &= ~IOCB_DIRECT;
		return generic_file_read_iter(iocb, to);
	}

	ret = iomap_dio_rw(iocb, to, &ext4_iomap_ops, NULL, 0, NULL, 0);
	inode_unlock_shared(inode);

	file_accessed(iocb->ki_filp);
	return ret;
}

#ifdef CONFIG_FS_DAX
/**
 * ext4_dax_read_iter - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext4_dax_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

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
	ret = dax_iomap_rw(iocb, to, &ext4_iomap_ops);
	inode_unlock_shared(inode);

	file_accessed(iocb->ki_filp);
	return ret;
}
#endif

/**
 * ext4_file_read_iter - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext4_file_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);

	if (unlikely(ext4_forced_shutdown(inode->i_sb)))
		return -EIO;

	if (!iov_iter_count(to))
		return 0;

#ifdef CONFIG_FS_DAX
	if (IS_DAX(inode))
		return ext4_dax_read_iter(iocb, to);
#endif
	if (iocb->ki_flags & IOCB_DIRECT)
		return ext4_dio_read_iter(iocb, to);

	return generic_file_read_iter(iocb, to);
}

/**
 * ext4_file_splice_read - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext4_file_splice_read(struct file *in, loff_t *ppos,
				     struct pipe_inode_info *pipe,
				     size_t len, unsigned int flags)
{
	struct inode *inode = file_inode(in);

	if (unlikely(ext4_forced_shutdown(inode->i_sb)))
		return -EIO;
	return filemap_splice_read(in, ppos, pipe, len, flags);
}


/**
 * ext4_release_file - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_release_file(struct inode *inode, struct file *filp)
{
	if (ext4_test_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE)) {
		ext4_alloc_da_blocks(inode);
		ext4_clear_inode_state(inode, EXT4_STATE_DA_ALLOC_CLOSE);
	}

	if ((filp->f_mode & FMODE_WRITE) &&
			(atomic_read(&inode->i_writecount) == 1) &&
			!EXT4_I(inode)->i_reserved_data_blocks) {
		down_write(&EXT4_I(inode)->i_data_sem);
		ext4_discard_preallocations(inode);
		up_write(&EXT4_I(inode)->i_data_sem);
	}
	if (is_dx(inode) && filp->private_data)
		ext4_htree_free_dir_info(filp->private_data);

	return 0;
}


/**
 * ext4_unaligned_io - Implements the unaligned io operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool
ext4_unaligned_io(struct inode *inode, struct iov_iter *from, loff_t pos)
{
	struct super_block *sb = inode->i_sb;
	unsigned long blockmask = sb->s_blocksize - 1;

	if ((pos | iov_iter_alignment(from)) & blockmask)
		return true;

	return false;
}

/**
 * ext4_extending_io - Implements the extending io operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool
ext4_extending_io(struct inode *inode, loff_t offset, size_t len)
{
	if (offset + len > i_size_read(inode) ||
	    offset + len > EXT4_I(inode)->i_disksize)
		return true;
	return false;
}


/**
 * ext4_overwrite_io - Implements the overwrite io operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool ext4_overwrite_io(struct inode *inode,
			      loff_t pos, loff_t len, bool *unwritten)
{
	struct ext4_map_blocks map;
	unsigned int blkbits = inode->i_blkbits;
	int err, blklen;

	if (pos + len > i_size_read(inode))
		return false;

	map.m_lblk = pos >> blkbits;
	map.m_len = EXT4_MAX_BLOCKS(len, pos, blkbits);
	blklen = map.m_len;

	err = ext4_map_blocks(NULL, inode, &map, 0);
	if (err != blklen)
		return false;


	*unwritten = !(map.m_flags & EXT4_MAP_MAPPED);
	return true;
}

/**
 * ext4_generic_write_checks - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext4_generic_write_checks(struct kiocb *iocb,
					 struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	ssize_t ret;

	if (unlikely(IS_IMMUTABLE(inode)))
		return -EPERM;

	ret = generic_write_checks(iocb, from);
	if (ret <= 0)
		return ret;


	if (!(ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))) {
		struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);

		if (iocb->ki_pos >= sbi->s_bitmap_maxbytes)
			return -EFBIG;
		iov_iter_truncate(from, sbi->s_bitmap_maxbytes - iocb->ki_pos);
	}

	return iov_iter_count(from);
}

/**
 * ext4_write_checks - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext4_write_checks(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t ret, count;

	count = ext4_generic_write_checks(iocb, from);
	if (count <= 0)
		return count;

	ret = file_modified(iocb->ki_filp);
	if (ret)
		return ret;
	return count;
}

/**
 * ext4_buffered_write_iter - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext4_buffered_write_iter(struct kiocb *iocb,
					struct iov_iter *from)
{
	ssize_t ret;
	struct inode *inode = file_inode(iocb->ki_filp);

	if (iocb->ki_flags & IOCB_NOWAIT)
		return -EOPNOTSUPP;

	inode_lock(inode);
	ret = ext4_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	ret = generic_perform_write(iocb, from);

out:
	inode_unlock(inode);
	if (unlikely(ret <= 0))
		return ret;
	return generic_write_sync(iocb, ret);
}

/**
 * ext4_handle_inode_extension - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext4_handle_inode_extension(struct inode *inode, loff_t offset,
					   ssize_t written, ssize_t count)
{
	handle_t *handle;

	lockdep_assert_held_write(&inode->i_rwsem);
	handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if (ext4_update_inode_size(inode, offset + written)) {
		int ret = ext4_mark_inode_dirty(handle, inode);
		if (unlikely(ret)) {
			ext4_journal_stop(handle);
			return ret;
		}
	}

	if ((written == count) && inode->i_nlink)
		ext4_orphan_del(handle, inode);
	ext4_journal_stop(handle);

	return written;
}


/**
 * ext4_inode_extension_cleanup - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_inode_extension_cleanup(struct inode *inode, bool need_trunc)
{
	lockdep_assert_held_write(&inode->i_rwsem);
	if (need_trunc) {
		ext4_truncate_failed_write(inode);


		if (inode->i_nlink)
			ext4_orphan_del(NULL, inode);
		return;
	}


	if (ext4_inode_orphan_tracked(inode) && inode->i_nlink) {
		handle_t *handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);

		if (IS_ERR(handle)) {


			ext4_orphan_del(NULL, inode);
			return;
		}
		ext4_orphan_del(handle, inode);
		ext4_journal_stop(handle);
	}
}

/**
 * ext4_dio_write_end_io - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_dio_write_end_io(struct kiocb *iocb, ssize_t size,
				 int error, unsigned int flags)
{
	loff_t pos = iocb->ki_pos;
	struct inode *inode = file_inode(iocb->ki_filp);

	if (!error && size && flags & IOMAP_DIO_UNWRITTEN)
		error = ext4_convert_unwritten_extents(NULL, inode, pos, size);
	if (error)
		return error;


	if (pos + size <= READ_ONCE(EXT4_I(inode)->i_disksize) &&
	    pos + size <= i_size_read(inode))
		return size;
	return ext4_handle_inode_extension(inode, pos, size, size);
}

static const struct iomap_dio_ops ext4_dio_write_ops = {
	.end_io = ext4_dio_write_end_io,
};


/**
 * ext4_dio_write_checks - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext4_dio_write_checks(struct kiocb *iocb, struct iov_iter *from,
				     bool *ilock_shared, bool *extend,
				     bool *unwritten, int *dio_flags)
{
	struct file *file = iocb->ki_filp;
	struct inode *inode = file_inode(file);
	loff_t offset;
	size_t count;
	ssize_t ret;
	bool overwrite, unaligned_io;

restart:
	ret = ext4_generic_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	offset = iocb->ki_pos;
	count = ret;

	unaligned_io = ext4_unaligned_io(inode, from, offset);
	*extend = ext4_extending_io(inode, offset, count);
	overwrite = ext4_overwrite_io(inode, offset, count, unwritten);


	if (*ilock_shared &&
	    ((!IS_NOSEC(inode) || *extend || !overwrite ||
	     (unaligned_io && *unwritten)))) {
		if (iocb->ki_flags & IOCB_NOWAIT) {
			ret = -EAGAIN;
			goto out;
		}
		inode_unlock_shared(inode);
		*ilock_shared = false;
		inode_lock(inode);
		goto restart;
	}


	if (!*ilock_shared && (unaligned_io || *extend)) {
		if (iocb->ki_flags & IOCB_NOWAIT) {
			ret = -EAGAIN;
			goto out;
		}
		if (unaligned_io && (!overwrite || *unwritten))
			inode_dio_wait(inode);
		*dio_flags = IOMAP_DIO_FORCE_WAIT;
	}

	ret = file_modified(file);
	if (ret < 0)
		goto out;

	return count;
out:
	if (*ilock_shared)
		inode_unlock_shared(inode);
	else
		inode_unlock(inode);
	return ret;
}

/**
 * ext4_dio_write_iter - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t ext4_dio_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t ret;
	handle_t *handle;
	struct inode *inode = file_inode(iocb->ki_filp);
	loff_t offset = iocb->ki_pos;
	size_t count = iov_iter_count(from);
	const struct iomap_ops *iomap_ops = &ext4_iomap_ops;
	bool extend = false, unwritten = false;
	bool ilock_shared = true;
	int dio_flags = 0;


	if (offset + count > i_size_read(inode))
		ilock_shared = false;

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (ilock_shared) {
			if (!inode_trylock_shared(inode))
				return -EAGAIN;
		} else {
			if (!inode_trylock(inode))
				return -EAGAIN;
		}
	} else {
		if (ilock_shared)
			inode_lock_shared(inode);
		else
			inode_lock(inode);
	}


	if (!ext4_should_use_dio(iocb, from)) {
		if (ilock_shared)
			inode_unlock_shared(inode);
		else
			inode_unlock(inode);
		return ext4_buffered_write_iter(iocb, from);
	}


	ext4_clear_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);

	ret = ext4_dio_write_checks(iocb, from, &ilock_shared, &extend,
				    &unwritten, &dio_flags);
	if (ret <= 0)
		return ret;

	offset = iocb->ki_pos;
	count = ret;

	if (extend) {
		handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			goto out;
		}

		ret = ext4_orphan_add(handle, inode);
		if (ret) {
			ext4_journal_stop(handle);
			goto out;
		}

		ext4_journal_stop(handle);
	}

	if (ilock_shared && !unwritten)
		iomap_ops = &ext4_iomap_overwrite_ops;
	ret = iomap_dio_rw(iocb, from, iomap_ops, &ext4_dio_write_ops,
			   dio_flags, NULL, 0);
	if (ret == -ENOTBLK)
		ret = 0;
	if (extend) {


		WARN_ON_ONCE(ret == -EIOCBQUEUED);
		ext4_inode_extension_cleanup(inode, ret < 0);
	}

out:
	if (ilock_shared)
		inode_unlock_shared(inode);
	else
		inode_unlock(inode);

	if (ret >= 0 && iov_iter_count(from)) {
		ssize_t err;
		loff_t endbyte;

		offset = iocb->ki_pos;
		err = ext4_buffered_write_iter(iocb, from);
		if (err < 0)
			return err;


		ret += err;
		endbyte = offset + err - 1;
		err = filemap_write_and_wait_range(iocb->ki_filp->f_mapping,
						   offset, endbyte);
		if (!err)
			invalidate_mapping_pages(iocb->ki_filp->f_mapping,
						 offset >> PAGE_SHIFT,
						 endbyte >> PAGE_SHIFT);
	}

	return ret;
}

#ifdef CONFIG_FS_DAX
/**
 * ext4_dax_write_iter - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t
ext4_dax_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t ret;
	size_t count;
	loff_t offset;
	handle_t *handle;
	bool extend = false;
	struct inode *inode = file_inode(iocb->ki_filp);

	if (iocb->ki_flags & IOCB_NOWAIT) {
		if (!inode_trylock(inode))
			return -EAGAIN;
	} else {
		inode_lock(inode);
	}

	ret = ext4_write_checks(iocb, from);
	if (ret <= 0)
		goto out;

	offset = iocb->ki_pos;
	count = iov_iter_count(from);

	if (offset + count > EXT4_I(inode)->i_disksize) {
		handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
		if (IS_ERR(handle)) {
			ret = PTR_ERR(handle);
			goto out;
		}

		ret = ext4_orphan_add(handle, inode);
		if (ret) {
			ext4_journal_stop(handle);
			goto out;
		}

		extend = true;
		ext4_journal_stop(handle);
	}

	ret = dax_iomap_rw(iocb, from, &ext4_iomap_ops);

	if (extend) {
		ret = ext4_handle_inode_extension(inode, offset, ret, count);
		ext4_inode_extension_cleanup(inode, ret < (ssize_t)count);
	}
out:
	inode_unlock(inode);
	if (ret > 0)
		ret = generic_write_sync(iocb, ret);
	return ret;
}
#endif

/**
 * ext4_file_write_iter - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static ssize_t
ext4_file_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);

	if (unlikely(ext4_forced_shutdown(inode->i_sb)))
		return -EIO;

#ifdef CONFIG_FS_DAX
	if (IS_DAX(inode))
		return ext4_dax_write_iter(iocb, from);
#endif
	if (iocb->ki_flags & IOCB_DIRECT)
		return ext4_dio_write_iter(iocb, from);
	else
		return ext4_buffered_write_iter(iocb, from);
}

#ifdef CONFIG_FS_DAX
/**
 * ext4_dax_huge_fault - Implements the dax huge fault operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static vm_fault_t ext4_dax_huge_fault(struct vm_fault *vmf, unsigned int order)
{
	int error = 0;
	vm_fault_t result;
	int retries = 0;
	handle_t *handle = NULL;
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct super_block *sb = inode->i_sb;


	bool write = (vmf->flags & FAULT_FLAG_WRITE) &&
		(vmf->vma->vm_flags & VM_SHARED);
	struct address_space *mapping = vmf->vma->vm_file->f_mapping;
	pfn_t pfn;

	if (write) {
		sb_start_pagefault(sb);
		file_update_time(vmf->vma->vm_file);
		filemap_invalidate_lock_shared(mapping);
retry:
		handle = ext4_journal_start_sb(sb, EXT4_HT_WRITE_PAGE,
					       EXT4_DATA_TRANS_BLOCKS(sb));
		if (IS_ERR(handle)) {
			filemap_invalidate_unlock_shared(mapping);
			sb_end_pagefault(sb);
			return VM_FAULT_SIGBUS;
		}
	} else {
		filemap_invalidate_lock_shared(mapping);
	}
	result = dax_iomap_fault(vmf, order, &pfn, &error, &ext4_iomap_ops);
	if (write) {
		ext4_journal_stop(handle);

		if ((result & VM_FAULT_ERROR) && error == -ENOSPC &&
		    ext4_should_retry_alloc(sb, &retries))
			goto retry;

		if (result & VM_FAULT_NEEDDSYNC)
			result = dax_finish_sync_fault(vmf, order, pfn);
		filemap_invalidate_unlock_shared(mapping);
		sb_end_pagefault(sb);
	} else {
		filemap_invalidate_unlock_shared(mapping);
	}

	return result;
}

/**
 * ext4_dax_fault - Implements the dax fault operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static vm_fault_t ext4_dax_fault(struct vm_fault *vmf)
{
	return ext4_dax_huge_fault(vmf, 0);
}

static const struct vm_operations_struct ext4_dax_vm_ops = {
	.fault		= ext4_dax_fault,
	.huge_fault	= ext4_dax_huge_fault,
	.page_mkwrite	= ext4_dax_fault,
	.pfn_mkwrite	= ext4_dax_fault,
};
#else
#define ext4_dax_vm_ops	ext4_file_vm_ops
#endif

static const struct vm_operations_struct ext4_file_vm_ops = {
	.fault		= filemap_fault,
	.map_pages	= filemap_map_pages,
	.page_mkwrite   = ext4_page_mkwrite,
};

/**
 * ext4_file_mmap - Implements the file mmap operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file->f_mapping->host;
	struct dax_device *dax_dev = EXT4_SB(inode->i_sb)->s_daxdev;

	if (unlikely(ext4_forced_shutdown(inode->i_sb)))
		return -EIO;


	if (!daxdev_mapping_supported(vma, dax_dev))
		return -EOPNOTSUPP;

	file_accessed(file);
	if (IS_DAX(file_inode(file))) {
		vma->vm_ops = &ext4_dax_vm_ops;
		vm_flags_set(vma, VM_HUGEPAGE);
	} else {
		vma->vm_ops = &ext4_file_vm_ops;
	}
	return 0;
}

/**
 * ext4_sample_last_mounted - Implements the sample last mounted operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_sample_last_mounted(struct super_block *sb,
				    struct vfsmount *mnt)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct path path;
	char buf[64], *cp;
	handle_t *handle;
	int err;

	if (likely(ext4_test_mount_flag(sb, EXT4_MF_MNTDIR_SAMPLED)))
		return 0;

	if (sb_rdonly(sb) || !sb_start_intwrite_trylock(sb))
		return 0;

	ext4_set_mount_flag(sb, EXT4_MF_MNTDIR_SAMPLED);


	memset(buf, 0, sizeof(buf));
	path.mnt = mnt;
	path.dentry = mnt->mnt_root;
	cp = d_path(&path, buf, sizeof(buf));
	err = 0;
	if (IS_ERR(cp))
		goto out;

	handle = ext4_journal_start_sb(sb, EXT4_HT_MISC, 1);
	err = PTR_ERR(handle);
	if (IS_ERR(handle))
		goto out;
	BUFFER_TRACE(sbi->s_sbh, "get_write_access");
	err = ext4_journal_get_write_access(handle, sb, sbi->s_sbh,
					    EXT4_JTR_NONE);
	if (err)
		goto out_journal;
	lock_buffer(sbi->s_sbh);
	strtomem_pad(sbi->s_es->s_last_mounted, cp, 0);
	ext4_superblock_csum_set(sb);
	unlock_buffer(sbi->s_sbh);
	ext4_handle_dirty_metadata(handle, NULL, sbi->s_sbh);
out_journal:
	ext4_journal_stop(handle);
out:
	sb_end_intwrite(sb);
	return err;
}

/**
 * ext4_file_open - Implements the file open operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_file_open(struct inode *inode, struct file *filp)
{
	int ret;

	if (unlikely(ext4_forced_shutdown(inode->i_sb)))
		return -EIO;

	ret = ext4_sample_last_mounted(inode->i_sb, filp->f_path.mnt);
	if (ret)
		return ret;

	ret = fscrypt_file_open(inode, filp);
	if (ret)
		return ret;

	ret = fsverity_file_open(inode, filp);
	if (ret)
		return ret;


	if (filp->f_mode & FMODE_WRITE) {
		ret = ext4_inode_attach_jinode(inode);
		if (ret < 0)
			return ret;
	}

	filp->f_mode |= FMODE_NOWAIT | FMODE_CAN_ODIRECT;
	return dquot_file_open(inode, filp);
}


/**
 * ext4_llseek - Implements the llseek operation within the regular-file vfs operations subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
loff_t ext4_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	loff_t maxbytes = ext4_get_maxbytes(inode);

	switch (whence) {
	default:
		return generic_file_llseek_size(file, offset, whence,
						maxbytes, i_size_read(inode));
	case SEEK_HOLE:
		inode_lock_shared(inode);
		offset = iomap_seek_hole(inode, offset,
					 &ext4_iomap_report_ops);
		inode_unlock_shared(inode);
		break;
	case SEEK_DATA:
		inode_lock_shared(inode);
		offset = iomap_seek_data(inode, offset,
					 &ext4_iomap_report_ops);
		inode_unlock_shared(inode);
		break;
	}

	if (offset < 0)
		return offset;
	return vfs_setpos(file, offset, maxbytes);
}

const struct file_operations ext4_file_operations = {
	.llseek		= ext4_llseek,
	.read_iter	= ext4_file_read_iter,
	.write_iter	= ext4_file_write_iter,
	.iopoll		= iocb_bio_iopoll,
	.unlocked_ioctl = ext4_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ext4_compat_ioctl,
#endif
	.mmap		= ext4_file_mmap,
	.open		= ext4_file_open,
	.release	= ext4_release_file,
	.fsync		= ext4_sync_file,
	.get_unmapped_area = thp_get_unmapped_area,
	.splice_read	= ext4_file_splice_read,
	.splice_write	= iter_file_splice_write,
	.fallocate	= ext4_fallocate,
	.fop_flags	= FOP_MMAP_SYNC | FOP_BUFFER_RASYNC |
			  FOP_DIO_PARALLEL_WRITE,
};

const struct inode_operations ext4_file_inode_operations = {
	.setattr	= ext4_setattr,
	.getattr	= ext4_file_getattr,
	.listxattr	= ext4_listxattr,
	.get_inode_acl	= ext4_get_acl,
	.set_acl	= ext4_set_acl,
	.fiemap		= ext4_fiemap,
	.fileattr_get	= ext4_fileattr_get,
	.fileattr_set	= ext4_fileattr_set,
};


/**
 * ext4_sync_parent - Drives pending state toward the durability guarantee required by the calling VFS or journal interface.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_sync_parent(struct inode *inode)
{
	struct dentry *dentry, *next;
	int ret = 0;

	if (!ext4_test_inode_state(inode, EXT4_STATE_NEWENTRY))
		return 0;
	dentry = d_find_any_alias(inode);
	if (!dentry)
		return 0;
	while (ext4_test_inode_state(inode, EXT4_STATE_NEWENTRY)) {
		ext4_clear_inode_state(inode, EXT4_STATE_NEWENTRY);

		next = dget_parent(dentry);
		dput(dentry);
		dentry = next;
		inode = dentry->d_inode;


		ret = sync_mapping_buffers(inode->i_mapping);
		if (ret)
			break;
		ret = sync_inode_metadata(inode, 1);
		if (ret)
			break;
	}
	dput(dentry);
	return ret;
}

/**
 * ext4_fsync_nojournal - Drives pending state toward the durability guarantee required by the calling VFS or journal interface.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fsync_nojournal(struct file *file, loff_t start, loff_t end,
				int datasync, bool *needs_barrier)
{
	struct inode *inode = file->f_inode;
	struct writeback_control wbc = {
		.sync_mode = WB_SYNC_ALL,
		.nr_to_write = 0,
	};
	int ret;

	ret = generic_buffers_fsync_noflush(file, start, end, datasync);
	if (ret)
		return ret;


	ret = ext4_write_inode(inode, &wbc);
	if (ret)
		return ret;

	ret = ext4_sync_parent(inode);

	if (test_opt(inode->i_sb, BARRIER))
		*needs_barrier = true;

	return ret;
}

/**
 * ext4_fsync_journal - Drives pending state toward the durability guarantee required by the calling VFS or journal interface.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_fsync_journal(struct inode *inode, bool datasync,
			     bool *needs_barrier)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	journal_t *journal = EXT4_SB(inode->i_sb)->s_journal;
	tid_t commit_tid = datasync ? ei->i_datasync_tid : ei->i_sync_tid;


	if (!S_ISREG(inode->i_mode))
		return ext4_force_commit(inode->i_sb);

	if (journal->j_flags & JBD2_BARRIER &&
	    !jbd2_trans_will_send_data_barrier(journal, commit_tid))
		*needs_barrier = true;

	return ext4_fc_commit(journal, commit_tid);
}


/**
 * ext4_sync_file - Drives pending state toward the durability guarantee required by the calling VFS or journal interface.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_sync_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	int ret = 0, err;
	bool needs_barrier = false;
	struct inode *inode = file->f_mapping->host;

	if (unlikely(ext4_forced_shutdown(inode->i_sb)))
		return -EIO;

	ASSERT(ext4_journal_current_handle() == NULL);

	trace_ext4_sync_file_enter(file, datasync);

	if (sb_rdonly(inode->i_sb)) {

		smp_rmb();
		if (ext4_forced_shutdown(inode->i_sb))
			ret = -EROFS;
		goto out;
	}

	if (!EXT4_SB(inode->i_sb)->s_journal) {
		ret = ext4_fsync_nojournal(file, start, end, datasync,
					   &needs_barrier);
		if (needs_barrier)
			goto issue_flush;
		goto out;
	}

	ret = file_write_and_wait_range(file, start, end);
	if (ret)
		goto out;


	ret = ext4_fsync_journal(inode, datasync, &needs_barrier);

issue_flush:
	if (needs_barrier) {
		err = blkdev_issue_flush(inode->i_sb->s_bdev);
		if (!ret)
			ret = err;
	}
out:
	err = file_check_and_advance_wb_err(file);
	if (ret == 0)
		ret = err;
	trace_ext4_sync_file_exit(inode, ret);
	return ret;
}
