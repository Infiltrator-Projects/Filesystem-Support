/*
 * Filesystem Support EXT3 Linux regular-file adapter.
 *
 * EXT3 filesystem semantics and journal ordering are supplied by the owning
 * EXT3 implementation.  This unit exposes those operations through Linux VFS.
 */

#include <linux/blkdev.h>
#include <linux/quotaops.h>
#include <linux/uaccess.h>

#include "ext3.h"

static int ifs_ext3_release_file(struct inode *inode, struct file *file)
{
	if (ext3_test_inode_state(inode, EXT3_STATE_FLUSH_ON_CLOSE)) {
		filemap_flush(inode->i_mapping);
		ext3_clear_inode_state(inode, EXT3_STATE_FLUSH_ON_CLOSE);
	}

	if ((file->f_mode & FMODE_WRITE) &&
	    atomic_read(&inode->i_writecount) == 1) {
		mutex_lock(&EXT3_I(inode)->truncate_mutex);
		ext3_discard_reservation(inode);
		mutex_unlock(&EXT3_I(inode)->truncate_mutex);
	}

	if (is_dx(inode) && file->private_data) {
		ext3_htree_free_dir_info(file->private_data);
		file->private_data = NULL;
	}

	return 0;
}

int ext3_sync_file(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file_inode(file);
	struct ext3_inode_info *info = EXT3_I(inode);
	journal_t *journal = EXT3_SB(inode->i_sb)->s_journal;
	tid_t commit_tid;
	bool issue_barrier;
	int error;

	if (sb_rdonly(inode->i_sb)) {
		smp_rmb();
		return (EXT3_SB(inode->i_sb)->s_mount_state & EXT3_ERROR_FS) ?
			-EROFS : 0;
	}

	error = filemap_write_and_wait_range(inode->i_mapping, start, end);
	if (error)
		return error;

	if (ext3_journal_current_handle())
		return -EDEADLK;

	if (ext3_should_journal_data(inode))
		return ext3_force_commit(inode->i_sb);

	commit_tid = datasync ?
		atomic_read(&info->i_datasync_tid) :
		atomic_read(&info->i_sync_tid);

	issue_barrier =
		test_opt(inode->i_sb, BARRIER) &&
		!journal_trans_will_send_data_barrier(journal, commit_tid);

	log_start_commit(journal, commit_tid);
	error = log_wait_commit(journal, commit_tid);

	if (issue_barrier) {
		int flush_error = blkdev_issue_flush(inode->i_sb->s_bdev);
		if (!error)
			error = flush_error;
	}

	return error;
}

static int ifs_ext3_set_flags(struct file *file, unsigned long argument)
{
	struct inode *inode = file_inode(file);
	struct ext3_inode_info *info = EXT3_I(inode);
	struct ext3_iloc location;
	handle_t *handle;
	unsigned int requested;
	unsigned int previous;
	unsigned int journal_data;
	int error;

	if (!inode_owner_or_capable(&nop_mnt_idmap, inode))
		return -EACCES;
	if (get_user(requested, (int __user *)argument))
		return -EFAULT;

	error = mnt_want_write_file(file);
	if (error)
		return error;

	requested = ext3_mask_flags(inode->i_mode, requested);
	inode_lock(inode);

	if (IS_NOQUOTA(inode)) {
		error = -EPERM;
		goto out_unlock;
	}

	previous = info->i_flags;
	journal_data = requested & EXT3_JOURNAL_DATA_FL;

	if (((requested ^ previous) &
	     (EXT3_APPEND_FL | EXT3_IMMUTABLE_FL)) &&
	    !capable(CAP_LINUX_IMMUTABLE)) {
		error = -EPERM;
		goto out_unlock;
	}

	if (((journal_data ^ previous) & EXT3_JOURNAL_DATA_FL) &&
	    !capable(CAP_SYS_RESOURCE)) {
		error = -EPERM;
		goto out_unlock;
	}

	handle = ext3_journal_start(inode, 1);
	if (IS_ERR(handle)) {
		error = PTR_ERR(handle);
		goto out_unlock;
	}

	if (IS_SYNC(inode))
		handle->h_sync = 1;

	error = ext3_reserve_inode_write(handle, inode, &location);
	if (!error) {
		requested &= EXT3_FL_USER_MODIFIABLE;
		requested |= previous & ~EXT3_FL_USER_MODIFIABLE;
		info->i_flags = requested;
		ext3_set_inode_flags(inode);
		inode_set_ctime_current(inode);
		error = ext3_mark_iloc_dirty(handle, inode, &location);
	}

	{
		int stop_error = ext3_journal_stop(handle);
		if (!error)
			error = stop_error;
	}

	if (!error &&
	    ((journal_data ^ previous) & EXT3_JOURNAL_DATA_FL))
		error = ext3_change_inode_journal_flag(inode, journal_data);

out_unlock:
	inode_unlock(inode);
	mnt_drop_write_file(file);
	return error;
}

static int ifs_ext3_set_generation(struct file *file, unsigned long argument)
{
	struct inode *inode = file_inode(file);
	struct ext3_iloc location;
	handle_t *handle;
	u32 generation;
	int error;

	if (!inode_owner_or_capable(&nop_mnt_idmap, inode))
		return -EPERM;
	if (get_user(generation, (u32 __user *)argument))
		return -EFAULT;

	error = mnt_want_write_file(file);
	if (error)
		return error;

	inode_lock(inode);
	handle = ext3_journal_start(inode, 1);
	if (IS_ERR(handle)) {
		error = PTR_ERR(handle);
		goto out_unlock;
	}

	error = ext3_reserve_inode_write(handle, inode, &location);
	if (!error) {
		inode->i_generation = generation;
		inode_set_ctime_current(inode);
		error = ext3_mark_iloc_dirty(handle, inode, &location);
	}

	{
		int stop_error = ext3_journal_stop(handle);
		if (!error)
			error = stop_error;
	}

out_unlock:
	inode_unlock(inode);
	mnt_drop_write_file(file);
	return error;
}

static int ifs_ext3_set_reservation(struct file *file, unsigned long argument)
{
	struct inode *inode = file_inode(file);
	struct ext3_inode_info *info = EXT3_I(inode);
	unsigned short requested;
	int error;

	if (!test_opt(inode->i_sb, RESERVATION) || !S_ISREG(inode->i_mode))
		return -ENOTTY;
	if (!inode_owner_or_capable(&nop_mnt_idmap, inode))
		return -EACCES;
	if (get_user(requested, (unsigned short __user *)argument))
		return -EFAULT;

	error = mnt_want_write_file(file);
	if (error)
		return error;

	if (requested > EXT3_MAX_RESERVE_BLOCKS)
		requested = EXT3_MAX_RESERVE_BLOCKS;

	mutex_lock(&info->truncate_mutex);
	if (!info->i_block_alloc_info)
		ext3_init_block_alloc_info(inode);
	if (!info->i_block_alloc_info) {
		error = -ENOMEM;
	} else {
		info->i_block_alloc_info->rsv_window_node.rsv_goal_size =
			requested;
		error = 0;
	}
	mutex_unlock(&info->truncate_mutex);

	mnt_drop_write_file(file);
	return error;
}

static int ifs_ext3_resize_flush(struct super_block *sb)
{
	int error;

	journal_lock_updates(EXT3_SB(sb)->s_journal);
	error = journal_flush(EXT3_SB(sb)->s_journal);
	journal_unlock_updates(EXT3_SB(sb)->s_journal);
	return error;
}

long ext3_ioctl(struct file *file, unsigned int command, unsigned long argument)
{
	struct inode *inode = file_inode(file);
	struct ext3_inode_info *info = EXT3_I(inode);

	switch (command) {
	case EXT3_IOC_GETFLAGS: {
		unsigned int flags;
		ext3_get_inode_flags(info);
		flags = info->i_flags & EXT3_FL_USER_VISIBLE;
		return put_user(flags, (int __user *)argument);
	}
	case EXT3_IOC_SETFLAGS:
		return ifs_ext3_set_flags(file, argument);

	case EXT3_IOC_GETVERSION:
	case EXT3_IOC_GETVERSION_OLD:
		return put_user(inode->i_generation, (int __user *)argument);

	case EXT3_IOC_SETVERSION:
	case EXT3_IOC_SETVERSION_OLD:
		return ifs_ext3_set_generation(file, argument);

	case EXT3_IOC_GETRSVSZ:
		if (!test_opt(inode->i_sb, RESERVATION) ||
		    !S_ISREG(inode->i_mode) ||
		    !info->i_block_alloc_info)
			return -ENOTTY;
		return put_user(
			info->i_block_alloc_info->rsv_window_node.rsv_goal_size,
			(unsigned short __user *)argument);

	case EXT3_IOC_SETRSVSZ:
		return ifs_ext3_set_reservation(file, argument);

	case EXT3_IOC_GROUP_EXTEND: {
		struct super_block *sb = inode->i_sb;
		ext3_fsblk_t blocks;
		int error;
		int flush_error;

		if (!capable(CAP_SYS_RESOURCE))
			return -EPERM;
		if (get_user(blocks, (u32 __user *)argument))
			return -EFAULT;

		error = mnt_want_write_file(file);
		if (error)
			return error;

		error = ext3_group_extend(sb, EXT3_SB(sb)->s_es, blocks);
		flush_error = ifs_ext3_resize_flush(sb);
		if (!error)
			error = flush_error;
		mnt_drop_write_file(file);
		return error;
	}

	case EXT3_IOC_GROUP_ADD: {
		struct ext3_new_group_data input;
		struct super_block *sb = inode->i_sb;
		int error;
		int flush_error;

		if (!capable(CAP_SYS_RESOURCE))
			return -EPERM;
		if (copy_from_user(
			    &input,
			    (struct ext3_new_group_input __user *)argument,
			    sizeof(input)))
			return -EFAULT;

		error = mnt_want_write_file(file);
		if (error)
			return error;

		error = ext3_group_add(sb, &input);
		flush_error = ifs_ext3_resize_flush(sb);
		if (!error)
			error = flush_error;
		mnt_drop_write_file(file);
		return error;
	}

	case FITRIM: {
		struct fstrim_range range;
		int error;

		if (!capable(CAP_SYS_ADMIN))
			return -EPERM;
		if (copy_from_user(
			    &range, (struct fstrim_range __user *)argument,
			    sizeof(range)))
			return -EFAULT;

		error = ext3_trim_fs(inode->i_sb, &range);
		if (error < 0)
			return error;
		if (copy_to_user(
			    (struct fstrim_range __user *)argument,
			    &range, sizeof(range)))
			return -EFAULT;
		return 0;
	}
	default:
		return -ENOTTY;
	}
}

#ifdef CONFIG_COMPAT
long ext3_compat_ioctl(
	struct file *file, unsigned int command, unsigned long argument)
{
	switch (command) {
	case EXT3_IOC32_GETFLAGS:
		command = EXT3_IOC_GETFLAGS;
		break;
	case EXT3_IOC32_SETFLAGS:
		command = EXT3_IOC_SETFLAGS;
		break;
	case EXT3_IOC32_GETVERSION:
		command = EXT3_IOC_GETVERSION;
		break;
	case EXT3_IOC32_SETVERSION:
		command = EXT3_IOC_SETVERSION;
		break;
	case EXT3_IOC32_GROUP_EXTEND:
		command = EXT3_IOC_GROUP_EXTEND;
		break;
	case EXT3_IOC32_GETVERSION_OLD:
		command = EXT3_IOC_GETVERSION_OLD;
		break;
	case EXT3_IOC32_SETVERSION_OLD:
		command = EXT3_IOC_SETVERSION_OLD;
		break;
	case EXT3_IOC32_GETRSVSZ:
		command = EXT3_IOC_GETRSVSZ;
		break;
	case EXT3_IOC32_SETRSVSZ:
		command = EXT3_IOC_SETRSVSZ;
		break;
	case EXT3_IOC_GROUP_ADD:
		break;
	default:
		return -ENOIOCTLCMD;
	}

	return ext3_ioctl(file, command,
			  (unsigned long)compat_ptr(argument));
}
#endif

#ifdef CONFIG_EXT3_FS_POSIX_ACL
static struct posix_acl *ifs_ext3_get_inode_acl(
	struct inode *inode, int type, bool rcu)
{
	if (rcu)
		return ERR_PTR(-ECHILD);
	return ext3_get_acl(inode, type);
}

static int ifs_ext3_set_acl(
	struct mnt_idmap *idmap, struct dentry *dentry,
	struct posix_acl *acl, int type)
{
	(void)idmap;
	return ext3_set_acl(d_inode(dentry), acl, type);
}
#endif

const struct file_operations ext3_file_operations = {
	.llseek = generic_file_llseek,
	.read_iter = generic_file_read_iter,
	.write_iter = generic_file_write_iter,
	.unlocked_ioctl = ext3_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ext3_compat_ioctl,
#endif
	.mmap = generic_file_mmap,
	.open = dquot_file_open,
	.release = ifs_ext3_release_file,
	.fsync = ext3_sync_file,
	.splice_read = filemap_splice_read,
	.splice_write = iter_file_splice_write,
};

const struct inode_operations ext3_file_inode_operations = {
	.setattr = ext3_setattr,
#ifdef CONFIG_EXT3_FS_XATTR
	.listxattr = ext3_listxattr,
#endif
#ifdef CONFIG_EXT3_FS_POSIX_ACL
	.get_inode_acl = ifs_ext3_get_inode_acl,
	.set_acl = ifs_ext3_set_acl,
#endif
	.fiemap = ext3_fiemap,
};
