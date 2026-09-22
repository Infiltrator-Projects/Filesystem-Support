/*
 *  linux/fs/ext3/file.c
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
 *  ext3 fs regular file handling primitives
 *
 *  64-bit file support on 64-bit platforms by Jakub Jelinek
 *	(jj@sunsite.ms.mff.cuni.cz)
 */

/*
 * EXT3 — Regular-file VFS operations
 *
 * Purpose:
 *   Implements the regular-file interface, including open/read/write/mmap/direct-I/O related paths and any small file-facing operations consolidated into this unit.
 *
 * Filesystem model:
 *   This file belongs to a standalone EXT3 VFS implementation with its historical JBD engine embedded in ext3.ko.
 *
 * Correctness focus:
 *   I/O ordering, size visibility, writeback and error propagation must agree with the filesystem's allocation and journaling rules.
 *
 * Project rules:
 *   - EXT3 requires its journal semantics; it is not an EXT4 compatibility registration.
 *   - Preserve the journal, recovery, ordered/writeback/journal data modes and EXT3 on-disk limits.
 *   - JBD and the metadata cache are private implementation code, not separately deployed modules.
 *
 * Commentary policy:
 *   Comments explain invariants, ownership, persistence ordering and
 *   non-obvious design intent. They deliberately avoid restating C syntax.
 */

#include <linux/quotaops.h>
#include "ext3.h"


/**
 * ext3_release_file - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext3_release_file (struct inode * inode, struct file * filp)
{
	if (ext3_test_inode_state(inode, EXT3_STATE_FLUSH_ON_CLOSE)) {
		filemap_flush(inode->i_mapping);
		ext3_clear_inode_state(inode, EXT3_STATE_FLUSH_ON_CLOSE);
	}

	if ((filp->f_mode & FMODE_WRITE) &&
			(atomic_read(&inode->i_writecount) == 1))
	{
		mutex_lock(&EXT3_I(inode)->truncate_mutex);
		ext3_discard_reservation(inode);
		mutex_unlock(&EXT3_I(inode)->truncate_mutex);
	}
	if (is_dx(inode) && filp->private_data)
		ext3_htree_free_dir_info(filp->private_data);

	return 0;
}

const struct file_operations ext3_file_operations = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= generic_file_write_iter,
	.unlocked_ioctl	= ext3_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ext3_compat_ioctl,
#endif
	.mmap		= generic_file_mmap,
	.open		= dquot_file_open,
	.release	= ext3_release_file,
	.fsync		= ext3_sync_file,
	.splice_read	= generic_file_splice_read,
	.splice_write	= iter_file_splice_write,
};

const struct inode_operations ext3_file_inode_operations = {
	.setattr	= ext3_setattr,
#ifdef CONFIG_EXT3_FS_XATTR
	.setxattr	= generic_setxattr,
	.getxattr	= generic_getxattr,
	.listxattr	= ext3_listxattr,
	.removexattr	= generic_removexattr,
#endif
	.get_acl	= ext3_get_acl,
	.set_acl	= ext3_set_acl,
	.fiemap		= ext3_fiemap,
};


/**
 * ext3_sync_file - Drives pending state toward the durability guarantee required by the calling VFS or journal interface.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext3_sync_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file->f_mapping->host;
	struct ext3_inode_info *ei = EXT3_I(inode);
	journal_t *journal = EXT3_SB(inode->i_sb)->s_journal;
	int ret, needs_barrier = 0;
	tid_t commit_tid;

	trace_ext3_sync_file_enter(file, datasync);

	if (inode->i_sb->s_flags & MS_RDONLY) {

		smp_rmb();
		if (EXT3_SB(inode->i_sb)->s_mount_state & EXT3_ERROR_FS)
			return -EROFS;
		return 0;
	}
	ret = filemap_write_and_wait_range(inode->i_mapping, start, end);
	if (ret)
		goto out;

	J_ASSERT(ext3_journal_current_handle() == NULL);


	if (ext3_should_journal_data(inode)) {
		ret = ext3_force_commit(inode->i_sb);
		goto out;
	}

	if (datasync)
		commit_tid = atomic_read(&ei->i_datasync_tid);
	else
		commit_tid = atomic_read(&ei->i_sync_tid);

	if (test_opt(inode->i_sb, BARRIER) &&
	    !journal_trans_will_send_data_barrier(journal, commit_tid))
		needs_barrier = 1;
	log_start_commit(journal, commit_tid);
	ret = log_wait_commit(journal, commit_tid);


	if (needs_barrier) {
		int err;

		err = blkdev_issue_flush(inode->i_sb->s_bdev, GFP_KERNEL, NULL);
		if (!ret)
			ret = err;
	}
out:
	trace_ext3_sync_file_exit(inode, ret);
	return ret;
}


/**
 * ext3_ioctl - Handles a filesystem-specific control operation exposed through the file API.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
long ext3_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(filp);
	struct ext3_inode_info *ei = EXT3_I(inode);
	unsigned int flags;
	unsigned short rsv_window_size;

	ext3_debug ("cmd = %u, arg = %lu\n", cmd, arg);

	switch (cmd) {
	case EXT3_IOC_GETFLAGS:
		ext3_get_inode_flags(ei);
		flags = ei->i_flags & EXT3_FL_USER_VISIBLE;
		return put_user(flags, (int __user *) arg);
	case EXT3_IOC_SETFLAGS: {
		handle_t *handle = NULL;
		int err;
		struct ext3_iloc iloc;
		unsigned int oldflags;
		unsigned int jflag;

		if (!inode_owner_or_capable(inode))
			return -EACCES;

		if (get_user(flags, (int __user *) arg))
			return -EFAULT;

		err = mnt_want_write_file(filp);
		if (err)
			return err;

		flags = ext3_mask_flags(inode->i_mode, flags);

		mutex_lock(&inode->i_mutex);


		err = -EPERM;
		if (IS_NOQUOTA(inode))
			goto flags_out;

		oldflags = ei->i_flags;


		jflag = flags & EXT3_JOURNAL_DATA_FL;


		if ((flags ^ oldflags) & (EXT3_APPEND_FL | EXT3_IMMUTABLE_FL)) {
			if (!capable(CAP_LINUX_IMMUTABLE))
				goto flags_out;
		}


		if ((jflag ^ oldflags) & (EXT3_JOURNAL_DATA_FL)) {
			if (!capable(CAP_SYS_RESOURCE))
				goto flags_out;
		}

		handle = ext3_journal_start(inode, 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto flags_out;
		}
		if (IS_SYNC(inode))
			handle->h_sync = 1;
		err = ext3_reserve_inode_write(handle, inode, &iloc);
		if (err)
			goto flags_err;

		flags = flags & EXT3_FL_USER_MODIFIABLE;
		flags |= oldflags & ~EXT3_FL_USER_MODIFIABLE;
		ei->i_flags = flags;

		ext3_set_inode_flags(inode);
		inode->i_ctime = CURRENT_TIME_SEC;

		err = ext3_mark_iloc_dirty(handle, inode, &iloc);
flags_err:
		ext3_journal_stop(handle);
		if (err)
			goto flags_out;

		if ((jflag ^ oldflags) & (EXT3_JOURNAL_DATA_FL))
			err = ext3_change_inode_journal_flag(inode, jflag);
flags_out:
		mutex_unlock(&inode->i_mutex);
		mnt_drop_write_file(filp);
		return err;
	}
	case EXT3_IOC_GETVERSION:
	case EXT3_IOC_GETVERSION_OLD:
		return put_user(inode->i_generation, (int __user *) arg);
	case EXT3_IOC_SETVERSION:
	case EXT3_IOC_SETVERSION_OLD: {
		handle_t *handle;
		struct ext3_iloc iloc;
		__u32 generation;
		int err;

		if (!inode_owner_or_capable(inode))
			return -EPERM;

		err = mnt_want_write_file(filp);
		if (err)
			return err;
		if (get_user(generation, (int __user *) arg)) {
			err = -EFAULT;
			goto setversion_out;
		}

		mutex_lock(&inode->i_mutex);
		handle = ext3_journal_start(inode, 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto unlock_out;
		}
		err = ext3_reserve_inode_write(handle, inode, &iloc);
		if (err == 0) {
			inode->i_ctime = CURRENT_TIME_SEC;
			inode->i_generation = generation;
			err = ext3_mark_iloc_dirty(handle, inode, &iloc);
		}
		ext3_journal_stop(handle);

unlock_out:
		mutex_unlock(&inode->i_mutex);
setversion_out:
		mnt_drop_write_file(filp);
		return err;
	}
	case EXT3_IOC_GETRSVSZ:
		if (test_opt(inode->i_sb, RESERVATION)
			&& S_ISREG(inode->i_mode)
			&& ei->i_block_alloc_info) {
			rsv_window_size = ei->i_block_alloc_info->rsv_window_node.rsv_goal_size;
			return put_user(rsv_window_size, (int __user *)arg);
		}
		return -ENOTTY;
	case EXT3_IOC_SETRSVSZ: {
		int err;

		if (!test_opt(inode->i_sb, RESERVATION) ||!S_ISREG(inode->i_mode))
			return -ENOTTY;

		err = mnt_want_write_file(filp);
		if (err)
			return err;

		if (!inode_owner_or_capable(inode)) {
			err = -EACCES;
			goto setrsvsz_out;
		}

		if (get_user(rsv_window_size, (int __user *)arg)) {
			err = -EFAULT;
			goto setrsvsz_out;
		}

		if (rsv_window_size > EXT3_MAX_RESERVE_BLOCKS)
			rsv_window_size = EXT3_MAX_RESERVE_BLOCKS;


		mutex_lock(&ei->truncate_mutex);
		if (!ei->i_block_alloc_info)
			ext3_init_block_alloc_info(inode);

		if (ei->i_block_alloc_info){
			struct ext3_reserve_window_node *rsv = &ei->i_block_alloc_info->rsv_window_node;
			rsv->rsv_goal_size = rsv_window_size;
		}
		mutex_unlock(&ei->truncate_mutex);
setrsvsz_out:
		mnt_drop_write_file(filp);
		return err;
	}
	case EXT3_IOC_GROUP_EXTEND: {
		ext3_fsblk_t n_blocks_count;
		struct super_block *sb = inode->i_sb;
		int err, err2;

		if (!capable(CAP_SYS_RESOURCE))
			return -EPERM;

		err = mnt_want_write_file(filp);
		if (err)
			return err;

		if (get_user(n_blocks_count, (__u32 __user *)arg)) {
			err = -EFAULT;
			goto group_extend_out;
		}
		err = ext3_group_extend(sb, EXT3_SB(sb)->s_es, n_blocks_count);
		journal_lock_updates(EXT3_SB(sb)->s_journal);
		err2 = journal_flush(EXT3_SB(sb)->s_journal);
		journal_unlock_updates(EXT3_SB(sb)->s_journal);
		if (err == 0)
			err = err2;
group_extend_out:
		mnt_drop_write_file(filp);
		return err;
	}
	case EXT3_IOC_GROUP_ADD: {
		struct ext3_new_group_data input;
		struct super_block *sb = inode->i_sb;
		int err, err2;

		if (!capable(CAP_SYS_RESOURCE))
			return -EPERM;

		err = mnt_want_write_file(filp);
		if (err)
			return err;

		if (copy_from_user(&input, (struct ext3_new_group_input __user *)arg,
				sizeof(input))) {
			err = -EFAULT;
			goto group_add_out;
		}

		err = ext3_group_add(sb, &input);
		journal_lock_updates(EXT3_SB(sb)->s_journal);
		err2 = journal_flush(EXT3_SB(sb)->s_journal);
		journal_unlock_updates(EXT3_SB(sb)->s_journal);
		if (err == 0)
			err = err2;
group_add_out:
		mnt_drop_write_file(filp);
		return err;
	}
	case FITRIM: {

		struct super_block *sb = inode->i_sb;
		struct fstrim_range range;
		int ret = 0;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;

		if (copy_from_user(&range, (struct fstrim_range __user *)arg,
				   sizeof(range)))
			return -EFAULT;

		ret = ext3_trim_fs(sb, &range);
		if (ret < 0)
			return ret;

		if (copy_to_user((struct fstrim_range __user *)arg, &range,
				 sizeof(range)))
			return -EFAULT;

		return 0;
	}

	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
/**
 * ext3_compat_ioctl - Handles a filesystem-specific control operation exposed through the file API.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
long ext3_compat_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{

	switch (cmd) {
	case EXT3_IOC32_GETFLAGS:
		cmd = EXT3_IOC_GETFLAGS;
		break;
	case EXT3_IOC32_SETFLAGS:
		cmd = EXT3_IOC_SETFLAGS;
		break;
	case EXT3_IOC32_GETVERSION:
		cmd = EXT3_IOC_GETVERSION;
		break;
	case EXT3_IOC32_SETVERSION:
		cmd = EXT3_IOC_SETVERSION;
		break;
	case EXT3_IOC32_GROUP_EXTEND:
		cmd = EXT3_IOC_GROUP_EXTEND;
		break;
	case EXT3_IOC32_GETVERSION_OLD:
		cmd = EXT3_IOC_GETVERSION_OLD;
		break;
	case EXT3_IOC32_SETVERSION_OLD:
		cmd = EXT3_IOC_SETVERSION_OLD;
		break;
#ifdef CONFIG_JBD_DEBUG
	case EXT3_IOC32_WAIT_FOR_READONLY:
		cmd = EXT3_IOC_WAIT_FOR_READONLY;
		break;
#endif
	case EXT3_IOC32_GETRSVSZ:
		cmd = EXT3_IOC_GETRSVSZ;
		break;
	case EXT3_IOC32_SETRSVSZ:
		cmd = EXT3_IOC_SETRSVSZ;
		break;
	case EXT3_IOC_GROUP_ADD:
		break;
	default:
		return -ENOIOCTLCMD;
	}
	return ext3_ioctl(file, cmd, (unsigned long) compat_ptr(arg));
}
#endif
