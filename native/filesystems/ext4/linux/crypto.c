#include <linux/quotaops.h>
#include <linux/uuid.h>

#include "ext4.h"
#include "xattr.h"
#include "ext4_jbd2.h"

static void ext4_copy_fscrypt_name(struct ext4_filename *dst,
				   const struct fscrypt_name *src)
{
	memset(dst, 0, sizeof(*dst));
	dst->usr_fname = src->usr_fname;
	dst->disk_name = src->disk_name;
	dst->hinfo.hash = src->hash;
	dst->hinfo.minor_hash = src->minor_hash;
	dst->crypto_buf = src->crypto_buf;
}

int ext4_fname_setup_filename(struct inode *dir, const struct qstr *iname,
			      int lookup, struct ext4_filename *fname)
{
	struct fscrypt_name fsname;
	int err;

	err = fscrypt_setup_filename(dir, iname, lookup, &fsname);
	if (err)
		return err;

	ext4_copy_fscrypt_name(fname, &fsname);
	err = ext4_fname_setup_ci_filename(dir, iname, fname);
	if (err)
		ext4_fname_free_filename(fname);
	return err;
}

int ext4_fname_prepare_lookup(struct inode *dir, struct dentry *dentry,
			      struct ext4_filename *fname)
{
	struct fscrypt_name fsname;
	int err;

	err = fscrypt_prepare_lookup(dir, dentry, &fsname);
	if (err)
		return err;

	ext4_copy_fscrypt_name(fname, &fsname);
	err = ext4_fname_setup_ci_filename(dir, &dentry->d_name, fname);
	if (err)
		ext4_fname_free_filename(fname);
	return err;
}

void ext4_fname_free_filename(struct ext4_filename *fname)
{
	struct fscrypt_name fsname = {
		.crypto_buf = fname->crypto_buf,
	};

	fscrypt_free_filename(&fsname);
	fname->crypto_buf.name = NULL;
	fname->usr_fname = NULL;
	fname->disk_name.name = NULL;
	ext4_fname_free_ci_filename(fname);
}

static bool ext4_uuid_all_zero(const __u8 value[16])
{
	unsigned int i;

	for (i = 0; i < 16; ++i)
		if (value[i] != 0)
			return false;
	return true;
}

int ext4_ioctl_get_encryption_pwsalt(struct file *file, void __user *arg)
{
	struct super_block *sb = file_inode(file)->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	handle_t *handle;
	int err;
	int stop_err;

	if (!ext4_has_feature_encrypt(sb))
		return -EOPNOTSUPP;

	if (ext4_uuid_all_zero(sbi->s_es->s_encrypt_pw_salt)) {
		err = mnt_want_write_file(file);
		if (err)
			return err;

		handle = ext4_journal_start_sb(sb, EXT4_HT_MISC, 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto out_drop_write;
		}

		err = ext4_journal_get_write_access(
			handle, sb, sbi->s_sbh, EXT4_JTR_NONE);
		if (!err) {
			lock_buffer(sbi->s_sbh);
			generate_random_uuid(sbi->s_es->s_encrypt_pw_salt);
			ext4_superblock_csum_set(sb);
			unlock_buffer(sbi->s_sbh);
			err = ext4_handle_dirty_metadata(
				handle, NULL, sbi->s_sbh);
		}

		stop_err = ext4_journal_stop(handle);
		if (!err)
			err = stop_err;

out_drop_write:
		mnt_drop_write_file(file);
		if (err)
			return err;
	}

	if (copy_to_user(arg, sbi->s_es->s_encrypt_pw_salt, 16))
		return -EFAULT;
	return 0;
}

static int ext4_fscrypt_get_context(struct inode *inode, void *ctx, size_t len)
{
	return ext4_xattr_get(inode, EXT4_XATTR_INDEX_ENCRYPTION,
			      EXT4_XATTR_NAME_ENCRYPTION_CONTEXT, ctx, len);
}

static int ext4_fscrypt_set_context(struct inode *inode, const void *ctx,
				    size_t len, void *fs_data)
{
	handle_t *handle = fs_data;
	int credits;
	int retries = 0;
	int err;
	int stop_err;

	if (inode->i_ino == EXT4_ROOT_INO)
		return -EPERM;
	if (WARN_ON_ONCE(IS_DAX(inode) && i_size_read(inode)))
		return -EINVAL;
	if (ext4_test_inode_flag(inode, EXT4_INODE_DAX))
		return -EOPNOTSUPP;

	err = ext4_convert_inline_data(inode);
	if (err)
		return err;

	if (handle) {
		err = ext4_xattr_set_handle(
			handle, inode, EXT4_XATTR_INDEX_ENCRYPTION,
			EXT4_XATTR_NAME_ENCRYPTION_CONTEXT,
			ctx, len, XATTR_CREATE);
		if (!err) {
			ext4_set_inode_flag(inode, EXT4_INODE_ENCRYPT);
			ext4_clear_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);
			ext4_set_inode_flags(inode, false);
		}
		return err;
	}

	err = dquot_initialize(inode);
	if (err)
		return err;

retry:
	err = ext4_xattr_set_credits(inode, len, false, &credits);
	if (err)
		return err;

	handle = ext4_journal_start(inode, EXT4_HT_MISC, credits);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	err = ext4_xattr_set_handle(
		handle, inode, EXT4_XATTR_INDEX_ENCRYPTION,
		EXT4_XATTR_NAME_ENCRYPTION_CONTEXT, ctx, len, 0);
	if (!err) {
		ext4_set_inode_flag(inode, EXT4_INODE_ENCRYPT);
		ext4_set_inode_flags(inode, false);
		err = ext4_mark_inode_dirty(handle, inode);
		if (err)
			EXT4_ERROR_INODE(inode, "failed to persist encryption flag");
	}

	stop_err = ext4_journal_stop(handle);
	if (err == -ENOSPC && ext4_should_retry_alloc(inode->i_sb, &retries))
		goto retry;
	return err ? err : stop_err;
}

static const union fscrypt_policy *
ext4_fscrypt_dummy_policy(struct super_block *sb)
{
	return EXT4_SB(sb)->s_dummy_enc_policy.policy;
}

static bool ext4_fscrypt_has_stable_inodes(struct super_block *sb)
{
	return ext4_has_feature_stable_inodes(sb);
}

const struct fscrypt_operations ext4_cryptops = {
	.needs_bounce_pages = 1,
	.has_32bit_inodes = 1,
	.supports_subblock_data_units = 1,
	.legacy_key_prefix = "ext4:",
	.get_context = ext4_fscrypt_get_context,
	.set_context = ext4_fscrypt_set_context,
	.get_dummy_policy = ext4_fscrypt_dummy_policy,
	.empty_dir = ext4_empty_dir,
	.has_stable_inodes = ext4_fscrypt_has_stable_inodes,
};
