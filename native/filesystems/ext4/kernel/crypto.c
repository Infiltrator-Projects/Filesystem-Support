// SPDX-License-Identifier: GPL-2.0

/*
 * EXT4 — Filesystem encryption integration
 *
 * Purpose:
 *   Connects EXT4 inode and filename operations to the kernel fscrypt framework.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   The filesystem owns persistence and policy plumbing; cryptographic primitives and key management remain responsibilities of the generic kernel framework.
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

#include <linux/quotaops.h>
#include <linux/uuid.h>

#include "ext4.h"
#include "xattr.h"
#include "ext4_jbd2.h"

/**
 * ext4_fname_from_fscrypt_name - Implements the fname from fscrypt name operation within the filesystem encryption integration subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_fname_from_fscrypt_name(struct ext4_filename *dst,
					 const struct fscrypt_name *src)
{
	memset(dst, 0, sizeof(*dst));

	dst->usr_fname = src->usr_fname;
	dst->disk_name = src->disk_name;
	dst->hinfo.hash = src->hash;
	dst->hinfo.minor_hash = src->minor_hash;
	dst->crypto_buf = src->crypto_buf;
}

/**
 * ext4_fname_setup_filename - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_fname_setup_filename(struct inode *dir, const struct qstr *iname,
			      int lookup, struct ext4_filename *fname)
{
	struct fscrypt_name name;
	int err;

	err = fscrypt_setup_filename(dir, iname, lookup, &name);
	if (err)
		return err;

	ext4_fname_from_fscrypt_name(fname, &name);

	err = ext4_fname_setup_ci_filename(dir, iname, fname);
	if (err)
		ext4_fname_free_filename(fname);

	return err;
}

/**
 * ext4_fname_prepare_lookup - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_fname_prepare_lookup(struct inode *dir, struct dentry *dentry,
			      struct ext4_filename *fname)
{
	struct fscrypt_name name;
	int err;

	err = fscrypt_prepare_lookup(dir, dentry, &name);
	if (err)
		return err;

	ext4_fname_from_fscrypt_name(fname, &name);

	err = ext4_fname_setup_ci_filename(dir, &dentry->d_name, fname);
	if (err)
		ext4_fname_free_filename(fname);
	return err;
}

/**
 * ext4_fname_free_filename - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_fname_free_filename(struct ext4_filename *fname)
{
	struct fscrypt_name name;

	name.crypto_buf = fname->crypto_buf;
	fscrypt_free_filename(&name);

	fname->crypto_buf.name = NULL;
	fname->usr_fname = NULL;
	fname->disk_name.name = NULL;

	ext4_fname_free_ci_filename(fname);
}

/**
 * uuid_is_zero - Implements the uuid is zero operation within the filesystem encryption integration subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool uuid_is_zero(__u8 u[16])
{
	int i;

	for (i = 0; i < 16; i++)
		if (u[i])
			return false;
	return true;
}

/**
 * ext4_ioctl_get_encryption_pwsalt - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_ioctl_get_encryption_pwsalt(struct file *filp, void __user *arg)
{
	struct super_block *sb = file_inode(filp)->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int err, err2;
	handle_t *handle;

	if (!ext4_has_feature_encrypt(sb))
		return -EOPNOTSUPP;

	if (uuid_is_zero(sbi->s_es->s_encrypt_pw_salt)) {
		err = mnt_want_write_file(filp);
		if (err)
			return err;
		handle = ext4_journal_start_sb(sb, EXT4_HT_MISC, 1);
		if (IS_ERR(handle)) {
			err = PTR_ERR(handle);
			goto pwsalt_err_exit;
		}
		err = ext4_journal_get_write_access(handle, sb, sbi->s_sbh,
						    EXT4_JTR_NONE);
		if (err)
			goto pwsalt_err_journal;
		lock_buffer(sbi->s_sbh);
		generate_random_uuid(sbi->s_es->s_encrypt_pw_salt);
		ext4_superblock_csum_set(sb);
		unlock_buffer(sbi->s_sbh);
		err = ext4_handle_dirty_metadata(handle, NULL, sbi->s_sbh);
pwsalt_err_journal:
		err2 = ext4_journal_stop(handle);
		if (err2 && !err)
			err = err2;
pwsalt_err_exit:
		mnt_drop_write_file(filp);
		if (err)
			return err;
	}

	if (copy_to_user(arg, sbi->s_es->s_encrypt_pw_salt, 16))
		return -EFAULT;
	return 0;
}

/**
 * ext4_get_context - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_get_context(struct inode *inode, void *ctx, size_t len)
{
	return ext4_xattr_get(inode, EXT4_XATTR_INDEX_ENCRYPTION,
				 EXT4_XATTR_NAME_ENCRYPTION_CONTEXT, ctx, len);
}

/**
 * ext4_set_context - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_set_context(struct inode *inode, const void *ctx, size_t len,
							void *fs_data)
{
	handle_t *handle = fs_data;
	int res, res2, credits, retries = 0;


	if (inode->i_ino == EXT4_ROOT_INO)
		return -EPERM;

	if (WARN_ON_ONCE(IS_DAX(inode) && i_size_read(inode)))
		return -EINVAL;

	if (ext4_test_inode_flag(inode, EXT4_INODE_DAX))
		return -EOPNOTSUPP;

	res = ext4_convert_inline_data(inode);
	if (res)
		return res;


	if (handle) {


		res = ext4_xattr_set_handle(handle, inode,
					    EXT4_XATTR_INDEX_ENCRYPTION,
					    EXT4_XATTR_NAME_ENCRYPTION_CONTEXT,
					    ctx, len, XATTR_CREATE);
		if (!res) {
			ext4_set_inode_flag(inode, EXT4_INODE_ENCRYPT);
			ext4_clear_inode_state(inode,
					EXT4_STATE_MAY_INLINE_DATA);


			ext4_set_inode_flags(inode, false);
		}
		return res;
	}

	res = dquot_initialize(inode);
	if (res)
		return res;
retry:
	res = ext4_xattr_set_credits(inode, len, false                ,
				     &credits);
	if (res)
		return res;

	handle = ext4_journal_start(inode, EXT4_HT_MISC, credits);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	res = ext4_xattr_set_handle(handle, inode, EXT4_XATTR_INDEX_ENCRYPTION,
				    EXT4_XATTR_NAME_ENCRYPTION_CONTEXT,
				    ctx, len, 0);
	if (!res) {
		ext4_set_inode_flag(inode, EXT4_INODE_ENCRYPT);


		ext4_set_inode_flags(inode, false);
		res = ext4_mark_inode_dirty(handle, inode);
		if (res)
			EXT4_ERROR_INODE(inode, "Failed to mark inode dirty");
	}
	res2 = ext4_journal_stop(handle);

	if (res == -ENOSPC && ext4_should_retry_alloc(inode->i_sb, &retries))
		goto retry;
	if (!res)
		res = res2;
	return res;
}

/**
 * ext4_get_dummy_policy - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static const union fscrypt_policy *ext4_get_dummy_policy(struct super_block *sb)
{
	return EXT4_SB(sb)->s_dummy_enc_policy.policy;
}

/**
 * ext4_has_stable_inodes - Implements the has stable inodes operation within the filesystem encryption integration subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool ext4_has_stable_inodes(struct super_block *sb)
{
	return ext4_has_feature_stable_inodes(sb);
}

const struct fscrypt_operations ext4_cryptops = {
	.needs_bounce_pages	= 1,
	.has_32bit_inodes	= 1,
	.supports_subblock_data_units = 1,
	.legacy_key_prefix	= "ext4:",
	.get_context		= ext4_get_context,
	.set_context		= ext4_set_context,
	.get_dummy_policy	= ext4_get_dummy_policy,
	.empty_dir		= ext4_empty_dir,
	.has_stable_inodes	= ext4_has_stable_inodes,
};
