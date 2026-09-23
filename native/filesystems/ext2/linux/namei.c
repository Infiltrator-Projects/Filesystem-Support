/*
 * Infiltrator Filesystem Support — EXT2 Linux namespace adapter.
 *
 * Directory-record semantics live in the EXT2 directory/core implementation.
 * This unit translates Linux VFS namespace operations into those primitives.
 */

#include <linux/pagemap.h>
#include <linux/quotaops.h>

#include "ext2.h"

static int ext2_publish_new_nondir(struct dentry *dentry, struct inode *inode)
{
	int result = ext2_add_link(dentry, inode);

	if (result == 0) {
		d_instantiate_new(dentry, inode);
		return 0;
	}

	inode_dec_link_count(inode);
	discard_new_inode(inode);
	return result;
}

static struct dentry *ext2_lookup(
	struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct inode *inode = NULL;
	ino_t ino;
	int result;

	if (dentry->d_name.len > EXT2_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	result = ext2_inode_by_name(dir, &dentry->d_name, &ino);
	if (result == -ENOENT)
		return d_splice_alias(NULL, dentry);
	if (result != 0)
		return ERR_PTR(result);

	inode = ext2_iget(dir->i_sb, ino);
	if (inode == ERR_PTR(-ESTALE)) {
		ext2_error(dir->i_sb, __func__,
			   "directory references deleted inode %lu",
			   (unsigned long)ino);
		return ERR_PTR(-EIO);
	}

	return d_splice_alias(inode, dentry);
}

struct dentry *ext2_get_parent(struct dentry *child)
{
	ino_t parent_ino;
	int result;

	result = ext2_inode_by_name(d_inode(child), &dotdot_name, &parent_ino);
	if (result != 0)
		return ERR_PTR(result);

	return d_obtain_alias(ext2_iget(child->d_sb, parent_ino));
}

static int ext2_create(
	struct mnt_idmap *idmap, struct inode *dir,
	struct dentry *dentry, umode_t mode, bool exclusive)
{
	struct inode *inode;
	int result;

	result = dquot_initialize(dir);
	if (result != 0)
		return result;

	inode = ext2_new_inode(dir, mode, &dentry->d_name);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	ext2_set_file_ops(inode);
	mark_inode_dirty(inode);
	return ext2_publish_new_nondir(dentry, inode);
}

static int ext2_tmpfile(
	struct mnt_idmap *idmap, struct inode *dir,
	struct file *file, umode_t mode)
{
	struct inode *inode;

	inode = ext2_new_inode(dir, mode, NULL);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	ext2_set_file_ops(inode);
	mark_inode_dirty(inode);
	d_tmpfile(file, inode);
	unlock_new_inode(inode);
	return finish_open_simple(file, 0);
}

static int ext2_mknod(
	struct mnt_idmap *idmap, struct inode *dir,
	struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct inode *inode;
	int result;

	result = dquot_initialize(dir);
	if (result != 0)
		return result;

	inode = ext2_new_inode(dir, mode, &dentry->d_name);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	init_special_inode(inode, inode->i_mode, rdev);
	inode->i_op = &ext2_special_inode_operations;
	mark_inode_dirty(inode);
	return ext2_publish_new_nondir(dentry, inode);
}

static int ext2_symlink(
	struct mnt_idmap *idmap, struct inode *dir,
	struct dentry *dentry, const char *target)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	size_t bytes;
	int result;

	bytes = strlen(target) + 1U;
	if (bytes > sb->s_blocksize)
		return -ENAMETOOLONG;

	result = dquot_initialize(dir);
	if (result != 0)
		return result;

	inode = ext2_new_inode(
		dir, S_IFLNK | S_IRWXUGO, &dentry->d_name);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	if (bytes > sizeof(EXT2_I(inode)->i_data)) {
		inode->i_op = &ext2_symlink_inode_operations;
		inode_nohighmem(inode);
		inode->i_mapping->a_ops = &ext2_aops;
		result = page_symlink(inode, target, bytes);
		if (result != 0)
			goto fail_inode;
	} else {
		inode->i_op = &ext2_fast_symlink_inode_operations;
		inode->i_link = (char *)EXT2_I(inode)->i_data;
		memcpy(inode->i_link, target, bytes);
		inode->i_size = bytes - 1U;
	}

	mark_inode_dirty(inode);
	return ext2_publish_new_nondir(dentry, inode);

fail_inode:
	inode_dec_link_count(inode);
	discard_new_inode(inode);
	return result;
}

static int ext2_link(
	struct dentry *old_dentry, struct inode *dir,
	struct dentry *new_dentry)
{
	struct inode *inode = d_inode(old_dentry);
	int result;

	result = dquot_initialize(dir);
	if (result != 0)
		return result;

	inode_set_ctime_current(inode);
	inode_inc_link_count(inode);
	ihold(inode);

	result = ext2_add_link(new_dentry, inode);
	if (result == 0) {
		d_instantiate(new_dentry, inode);
		return 0;
	}

	inode_dec_link_count(inode);
	iput(inode);
	return result;
}

static int ext2_mkdir(
	struct mnt_idmap *idmap, struct inode *dir,
	struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int result;

	result = dquot_initialize(dir);
	if (result != 0)
		return result;

	inode_inc_link_count(dir);
	inode = ext2_new_inode(
		dir, S_IFDIR | mode, &dentry->d_name);
	if (IS_ERR(inode)) {
		result = PTR_ERR(inode);
		goto fail_parent_link;
	}

	inode->i_op = &ext2_dir_inode_operations;
	inode->i_fop = &ext2_dir_operations;
	inode->i_mapping->a_ops = &ext2_aops;
	inode_inc_link_count(inode);

	result = ext2_make_empty(inode, dir);
	if (result != 0)
		goto fail_child;

	result = ext2_add_link(dentry, inode);
	if (result != 0)
		goto fail_child;

	d_instantiate_new(dentry, inode);
	return 0;

fail_child:
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	discard_new_inode(inode);
fail_parent_link:
	inode_dec_link_count(dir);
	return result;
}

static int ext2_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct ext2_dir_entry_2 *entry;
	struct folio *folio;
	int result;

	result = dquot_initialize(dir);
	if (result != 0)
		return result;

	entry = ext2_find_entry(dir, &dentry->d_name, &folio);
	if (IS_ERR(entry))
		return PTR_ERR(entry);

	result = ext2_delete_entry(entry, folio);
	folio_release_kmap(folio, entry);
	if (result != 0)
		return result;

	inode_set_ctime_to_ts(inode, inode_get_ctime(dir));
	inode_dec_link_count(inode);
	return 0;
}

static int ext2_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int result;

	if (!ext2_empty_dir(inode))
		return -ENOTEMPTY;

	result = ext2_unlink(dir, dentry);
	if (result != 0)
		return result;

	inode->i_size = 0;
	inode_dec_link_count(inode);
	inode_dec_link_count(dir);
	return 0;
}

static int ext2_rename(
	struct mnt_idmap *idmap,
	struct inode *old_dir, struct dentry *old_dentry,
	struct inode *new_dir, struct dentry *new_dentry,
	unsigned int flags)
{
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	const bool directory = S_ISDIR(old_inode->i_mode);
	struct folio *old_folio = NULL;
	struct folio *dotdot_folio = NULL;
	struct ext2_dir_entry_2 *old_entry;
	struct ext2_dir_entry_2 *dotdot_entry = NULL;
	int result;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	result = dquot_initialize(old_dir);
	if (result != 0)
		return result;
	result = dquot_initialize(new_dir);
	if (result != 0)
		return result;

	old_entry = ext2_find_entry(
		old_dir, &old_dentry->d_name, &old_folio);
	if (IS_ERR(old_entry))
		return PTR_ERR(old_entry);

	if (directory && old_dir != new_dir) {
		dotdot_entry = ext2_dotdot(old_inode, &dotdot_folio);
		if (!dotdot_entry) {
			result = -EIO;
			goto out_old;
		}
	}

	if (new_inode) {
		struct folio *new_folio;
		struct ext2_dir_entry_2 *new_entry;

		if (directory && !ext2_empty_dir(new_inode)) {
			result = -ENOTEMPTY;
			goto out_dotdot;
		}

		new_entry = ext2_find_entry(
			new_dir, &new_dentry->d_name, &new_folio);
		if (IS_ERR(new_entry)) {
			result = PTR_ERR(new_entry);
			goto out_dotdot;
		}

		result = ext2_set_link(
			new_dir, new_entry, new_folio, old_inode, true);
		folio_release_kmap(new_folio, new_entry);
		if (result != 0)
			goto out_dotdot;

		inode_set_ctime_current(new_inode);
		if (directory)
			drop_nlink(new_inode);
		inode_dec_link_count(new_inode);
	} else {
		result = ext2_add_link(new_dentry, old_inode);
		if (result != 0)
			goto out_dotdot;
		if (directory)
			inode_inc_link_count(new_dir);
	}

	inode_set_ctime_current(old_inode);
	mark_inode_dirty(old_inode);

	result = ext2_delete_entry(old_entry, old_folio);
	if (result == 0 && directory) {
		if (old_dir != new_dir)
			result = ext2_set_link(
				old_inode, dotdot_entry,
				dotdot_folio, new_dir, false);
		inode_dec_link_count(old_dir);
	}

out_dotdot:
	if (dotdot_entry)
		folio_release_kmap(dotdot_folio, dotdot_entry);
out_old:
	folio_release_kmap(old_folio, old_entry);
	return result;
}

const struct inode_operations ext2_dir_inode_operations = {
	.create = ext2_create,
	.lookup = ext2_lookup,
	.link = ext2_link,
	.unlink = ext2_unlink,
	.symlink = ext2_symlink,
	.mkdir = ext2_mkdir,
	.rmdir = ext2_rmdir,
	.mknod = ext2_mknod,
	.rename = ext2_rename,
	.listxattr = ext2_listxattr,
	.getattr = ext2_getattr,
	.setattr = ext2_setattr,
	.get_inode_acl = ext2_get_acl,
	.set_acl = ext2_set_acl,
	.tmpfile = ext2_tmpfile,
	.fileattr_get = ext2_fileattr_get,
	.fileattr_set = ext2_fileattr_set,
};

const struct inode_operations ext2_special_inode_operations = {
	.listxattr = ext2_listxattr,
	.getattr = ext2_getattr,
	.setattr = ext2_setattr,
	.get_inode_acl = ext2_get_acl,
	.set_acl = ext2_set_acl,
};
