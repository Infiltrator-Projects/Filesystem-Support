// SPDX-License-Identifier: GPL-2.0
/*
 * linux/fs/ext4/xattr.c
 *
 * Copyright (C) 2001-2003 Andreas Gruenbacher, <agruen@suse.de>
 *
 * Fix by Harrison Xing <harrison@mountainviewdata.com>.
 * Ext4 code with a lot of help from Eric Jarman <ejarman@acm.org>.
 * Extended attributes for symlinks and special files added per
 *  suggestion of Luka Renko <luka.renko@hermes.si>.
 * xattr consolidation Copyright (c) 2004 James Morris <jmorris@redhat.com>,
 *  Red Hat Inc.
 * ea-in-inode support by Alex Tomas <alex@clusterfs.com> aka bzzz
 *  and Andreas Gruenbacher <agruen@suse.de>.
 */

/*
 * EXT4 — Extended metadata
 *
 * Purpose:
 *   Implements extended attributes, ACL/security metadata and the private metadata-block cache used by the owning filesystem.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Shared xattr blocks require exact reference/accounting rules; cache state is advisory, while on-disk reference counts and transaction ordering are authoritative.
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

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/quotaops.h>
#include <linux/iversion.h>
#include "ext4_jbd2.h"
#include "ext4.h"
#include "xattr.h"

#ifdef EXT4_XATTR_DEBUG
# define ea_idebug(inode, fmt, ...)					\
	printk(KERN_DEBUG "inode %s:%lu: " fmt "\n",			\
	       inode->i_sb->s_id, inode->i_ino, ##__VA_ARGS__)
# define ea_bdebug(bh, fmt, ...)					\
	printk(KERN_DEBUG "block %pg:%lu: " fmt "\n",			\
	       bh->b_bdev, (unsigned long)bh->b_blocknr, ##__VA_ARGS__)
#else
# define ea_idebug(inode, fmt, ...)	no_printk(fmt, ##__VA_ARGS__)
# define ea_bdebug(bh, fmt, ...)	no_printk(fmt, ##__VA_ARGS__)
#endif

static void ext4_xattr_block_cache_insert(struct mb_cache *,
					  struct buffer_head *);
static struct buffer_head *
ext4_xattr_block_cache_find(struct inode *, struct ext4_xattr_header *,
			    struct mb_cache_entry **);
static __le32 ext4_xattr_hash_entry(char *name, size_t name_len, __le32 *value,
				    size_t value_count);
static __le32 ext4_xattr_hash_entry_signed(char *name, size_t name_len, __le32 *value,
				    size_t value_count);
static void ext4_xattr_rehash(struct ext4_xattr_header *);

static const struct xattr_handler * const ext4_xattr_handler_map[] = {
	[EXT4_XATTR_INDEX_USER]		     = &ext4_xattr_user_handler,
#ifdef CONFIG_EXT4_FS_POSIX_ACL
	[EXT4_XATTR_INDEX_POSIX_ACL_ACCESS]  = &nop_posix_acl_access,
	[EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT] = &nop_posix_acl_default,
#endif
	[EXT4_XATTR_INDEX_TRUSTED]	     = &ext4_xattr_trusted_handler,
#ifdef CONFIG_EXT4_FS_SECURITY
	[EXT4_XATTR_INDEX_SECURITY]	     = &ext4_xattr_security_handler,
#endif
	[EXT4_XATTR_INDEX_HURD]		     = &ext4_xattr_hurd_handler,
};

const struct xattr_handler * const ext4_xattr_handlers[] = {
	&ext4_xattr_user_handler,
	&ext4_xattr_trusted_handler,
#ifdef CONFIG_EXT4_FS_SECURITY
	&ext4_xattr_security_handler,
#endif
	&ext4_xattr_hurd_handler,
	NULL
};

#define EA_BLOCK_CACHE(inode)	(((struct ext4_sb_info *) \
				inode->i_sb->s_fs_info)->s_ea_block_cache)

#define EA_INODE_CACHE(inode)	(((struct ext4_sb_info *) \
				inode->i_sb->s_fs_info)->s_ea_inode_cache)

static int
ext4_expand_inode_array(struct ext4_xattr_inode_array **ea_inode_array,
			struct inode *inode);

#ifdef CONFIG_LOCKDEP


/**
 * ext4_xattr_inode_set_class - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_xattr_inode_set_class(struct inode *ea_inode)
{
	struct ext4_inode_info *ei = EXT4_I(ea_inode);

	lockdep_set_subclass(&ea_inode->i_rwsem, 1);
	(void) ei;
	lockdep_set_subclass(&ei->i_data_sem, I_DATA_SEM_EA);
}
#endif


/**
 * ext4_xattr_block_csum - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static __le32 ext4_xattr_block_csum(struct inode *inode,
				    sector_t block_nr,
				    struct ext4_xattr_header *hdr)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	__u32 csum;
	__le64 dsk_block_nr = cpu_to_le64(block_nr);
	__u32 dummy_csum = 0;
	int offset = offsetof(struct ext4_xattr_header, h_checksum);

	csum = ext4_chksum(sbi, sbi->s_csum_seed, (__u8 *)&dsk_block_nr,
			   sizeof(dsk_block_nr));
	csum = ext4_chksum(sbi, csum, (__u8 *)hdr, offset);
	csum = ext4_chksum(sbi, csum, (__u8 *)&dummy_csum, sizeof(dummy_csum));
	offset += sizeof(dummy_csum);
	csum = ext4_chksum(sbi, csum, (__u8 *)hdr + offset,
			   EXT4_BLOCK_SIZE(inode->i_sb) - offset);

	return cpu_to_le32(csum);
}


/**
 * ext4_xattr_block_csum_verify - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_block_csum_verify(struct inode *inode,
					struct buffer_head *bh)
{
	struct ext4_xattr_header *hdr = BHDR(bh);
	int ret = 1;

	if (ext4_has_metadata_csum(inode->i_sb)) {
		lock_buffer(bh);
		ret = (hdr->h_checksum == ext4_xattr_block_csum(inode,
							bh->b_blocknr, hdr));
		unlock_buffer(bh);
	}
	return ret;
}


/**
 * ext4_xattr_block_csum_set - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_xattr_block_csum_set(struct inode *inode,
				      struct buffer_head *bh)
{
	if (ext4_has_metadata_csum(inode->i_sb))
		BHDR(bh)->h_checksum = ext4_xattr_block_csum(inode,
						bh->b_blocknr, BHDR(bh));
}


/**
 * ext4_xattr_prefix - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline const char *ext4_xattr_prefix(int name_index,
					    struct dentry *dentry)
{
	const struct xattr_handler *handler = NULL;

	if (name_index > 0 && name_index < ARRAY_SIZE(ext4_xattr_handler_map))
		handler = ext4_xattr_handler_map[name_index];

	if (!xattr_handler_can_list(handler, dentry))
		return NULL;

	return xattr_prefix(handler);
}


/**
 * check_xattrs - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
check_xattrs(struct inode *inode, struct buffer_head *bh,
	     struct ext4_xattr_entry *entry, void *end, void *value_start,
	     const char *function, unsigned int line)
{
	struct ext4_xattr_entry *e = entry;
	int err = -EFSCORRUPTED;
	char *err_str;

	if (bh) {
		if (BHDR(bh)->h_magic != cpu_to_le32(EXT4_XATTR_MAGIC) ||
		    BHDR(bh)->h_blocks != cpu_to_le32(1)) {
			err_str = "invalid header";
			goto errout;
		}
		if (buffer_verified(bh))
			return 0;
		if (!ext4_xattr_block_csum_verify(inode, bh)) {
			err = -EFSBADCRC;
			err_str = "invalid checksum";
			goto errout;
		}
	} else {
		struct ext4_xattr_ibody_header *header = value_start;

		header -= 1;
		if (end - (void *)header < sizeof(*header) + sizeof(u32)) {
			err_str = "in-inode xattr block too small";
			goto errout;
		}
		if (header->h_magic != cpu_to_le32(EXT4_XATTR_MAGIC)) {
			err_str = "bad magic number in in-inode xattr";
			goto errout;
		}
	}


	while (!IS_LAST_ENTRY(e)) {
		struct ext4_xattr_entry *next = EXT4_XATTR_NEXT(e);
		if ((void *)next + sizeof(u32) > end) {
			err_str = "e_name out of bounds";
			goto errout;
		}
		if (strnlen(e->e_name, e->e_name_len) != e->e_name_len) {
			err_str = "bad e_name length";
			goto errout;
		}
		e = next;
	}


	while (!IS_LAST_ENTRY(entry)) {
		u32 size = le32_to_cpu(entry->e_value_size);
		unsigned long ea_ino = le32_to_cpu(entry->e_value_inum);

		if (!ext4_has_feature_ea_inode(inode->i_sb) && ea_ino) {
			err_str = "ea_inode specified without ea_inode feature enabled";
			goto errout;
		}
		if (ea_ino && ((ea_ino == EXT4_ROOT_INO) ||
			       !ext4_valid_inum(inode->i_sb, ea_ino))) {
			err_str = "invalid ea_ino";
			goto errout;
		}
		if (ea_ino && !size) {
			err_str = "invalid size in ea xattr";
			goto errout;
		}
		if (size > EXT4_XATTR_SIZE_MAX) {
			err_str = "e_value size too large";
			goto errout;
		}

		if (size != 0 && entry->e_value_inum == 0) {
			u16 offs = le16_to_cpu(entry->e_value_offs);
			void *value;


			if (offs > end - value_start) {
				err_str = "e_value out of bounds";
				goto errout;
			}
			value = value_start + offs;
			if (value < (void *)e + sizeof(u32) ||
			    size > end - value ||
			    EXT4_XATTR_SIZE(size) > end - value) {
				err_str = "overlapping e_value ";
				goto errout;
			}
		}
		entry = EXT4_XATTR_NEXT(entry);
	}
	if (bh)
		set_buffer_verified(bh);
	return 0;

errout:
	if (bh)
		__ext4_error_inode(inode, function, line, 0, -err,
				   "corrupted xattr block %llu: %s",
				   (unsigned long long) bh->b_blocknr,
				   err_str);
	else
		__ext4_error_inode(inode, function, line, 0, -err,
				   "corrupted in-inode xattr: %s", err_str);
	return err;
}


/**
 * __ext4_xattr_check_block - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int
__ext4_xattr_check_block(struct inode *inode, struct buffer_head *bh,
			 const char *function, unsigned int line)
{
	return check_xattrs(inode, bh, BFIRST(bh), bh->b_data + bh->b_size,
			    bh->b_data, function, line);
}

#define ext4_xattr_check_block(inode, bh) \
	__ext4_xattr_check_block((inode), (bh),  __func__, __LINE__)


/**
 * __xattr_check_inode - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int
__xattr_check_inode(struct inode *inode, struct ext4_xattr_ibody_header *header,
			 void *end, const char *function, unsigned int line)
{
	return check_xattrs(inode, NULL, IFIRST(header), end, IFIRST(header),
			    function, line);
}


/**
 * xattr_find_entry - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
xattr_find_entry(struct inode *inode, struct ext4_xattr_entry **pentry,
		 void *end, int name_index, const char *name, int sorted)
{
	struct ext4_xattr_entry *entry, *next;
	size_t name_len;
	int cmp = 1;

	if (name == NULL)
		return -EINVAL;
	name_len = strlen(name);
	for (entry = *pentry; !IS_LAST_ENTRY(entry); entry = next) {
		next = EXT4_XATTR_NEXT(entry);
		if ((void *) next >= end) {
			EXT4_ERROR_INODE(inode, "corrupted xattr entries");
			return -EFSCORRUPTED;
		}
		cmp = name_index - entry->e_name_index;
		if (!cmp)
			cmp = name_len - entry->e_name_len;
		if (!cmp)
			cmp = memcmp(name, entry->e_name, name_len);
		if (cmp <= 0 && (sorted || cmp == 0))
			break;
	}
	*pentry = entry;
	return cmp ? -ENODATA : 0;
}


/**
 * ext4_xattr_inode_hash - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static u32
ext4_xattr_inode_hash(struct ext4_sb_info *sbi, const void *buffer, size_t size)
{
	return ext4_chksum(sbi, sbi->s_csum_seed, buffer, size);
}


/**
 * ext4_xattr_inode_get_ref - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static u64 ext4_xattr_inode_get_ref(struct inode *ea_inode)
{
	return ((u64) inode_get_ctime_sec(ea_inode) << 32) |
		(u32) inode_peek_iversion_raw(ea_inode);
}


/**
 * ext4_xattr_inode_set_ref - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_xattr_inode_set_ref(struct inode *ea_inode, u64 ref_count)
{
	inode_set_ctime(ea_inode, (u32)(ref_count >> 32), 0);
	inode_set_iversion_raw(ea_inode, ref_count & 0xffffffff);
}


/**
 * ext4_xattr_inode_get_hash - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static u32 ext4_xattr_inode_get_hash(struct inode *ea_inode)
{
	return (u32) inode_get_atime_sec(ea_inode);
}


/**
 * ext4_xattr_inode_set_hash - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_xattr_inode_set_hash(struct inode *ea_inode, u32 hash)
{
	inode_set_atime(ea_inode, hash, 0);
}


/**
 * ext4_xattr_inode_read - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_inode_read(struct inode *ea_inode, void *buf, size_t size)
{
	int blocksize = 1 << ea_inode->i_blkbits;
	int bh_count = (size + blocksize - 1) >> ea_inode->i_blkbits;
	int tail_size = (size % blocksize) ?: blocksize;
	struct buffer_head *bhs_inline[8];
	struct buffer_head **bhs = bhs_inline;
	int i, ret;

	if (bh_count > ARRAY_SIZE(bhs_inline)) {
		bhs = kmalloc_array(bh_count, sizeof(*bhs), GFP_NOFS);
		if (!bhs)
			return -ENOMEM;
	}

	ret = ext4_bread_batch(ea_inode, 0            , bh_count,
			       true           , bhs);
	if (ret)
		goto free_bhs;

	for (i = 0; i < bh_count; i++) {

		if (!bhs[i]) {
			ret = -EFSCORRUPTED;
			goto put_bhs;
		}
		memcpy((char *)buf + blocksize * i, bhs[i]->b_data,
		       i < bh_count - 1 ? blocksize : tail_size);
	}
	ret = 0;
put_bhs:
	for (i = 0; i < bh_count; i++)
		brelse(bhs[i]);
free_bhs:
	if (bhs != bhs_inline)
		kfree(bhs);
	return ret;
}

#define EXT4_XATTR_INODE_GET_PARENT(inode) ((__u32)(inode_get_mtime_sec(inode)))


/**
 * ext4_xattr_inode_iget - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_inode_iget(struct inode *parent, unsigned long ea_ino,
				 u32 ea_inode_hash, struct inode **ea_inode)
{
	struct inode *inode;
	int err;


	if (parent->i_ino == ea_ino) {
		ext4_error(parent->i_sb,
			   "Parent and EA inode have the same ino %lu", ea_ino);
		return -EFSCORRUPTED;
	}

	inode = ext4_iget(parent->i_sb, ea_ino, EXT4_IGET_EA_INODE);
	if (IS_ERR(inode)) {
		err = PTR_ERR(inode);
		ext4_error(parent->i_sb,
			   "error while reading EA inode %lu err=%d", ea_ino,
			   err);
		return err;
	}
	ext4_xattr_inode_set_class(inode);


	if (ea_inode_hash != ext4_xattr_inode_get_hash(inode) &&
	    EXT4_XATTR_INODE_GET_PARENT(inode) == parent->i_ino &&
	    inode->i_generation == parent->i_generation) {
		ext4_set_inode_state(inode, EXT4_STATE_LUSTRE_EA_INODE);
		ext4_xattr_inode_set_ref(inode, 1);
	} else {
		inode_lock_nested(inode, I_MUTEX_XATTR);
		inode->i_flags |= S_NOQUOTA;
		inode_unlock(inode);
	}

	*ea_inode = inode;
	return 0;
}


/**
 * ext4_evict_ea_inode - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_evict_ea_inode(struct inode *inode)
{
	struct mb_cache_entry *oe;

	if (!EA_INODE_CACHE(inode))
		return;

	while ((oe = mb_cache_entry_delete_or_get(EA_INODE_CACHE(inode),
			ext4_xattr_inode_get_hash(inode), inode->i_ino))) {
		mb_cache_entry_wait_unused(oe);
		mb_cache_entry_put(EA_INODE_CACHE(inode), oe);
	}
}


/**
 * ext4_xattr_inode_verify_hashes - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_inode_verify_hashes(struct inode *ea_inode,
			       struct ext4_xattr_entry *entry, void *buffer,
			       size_t size)
{
	u32 hash;


	hash = ext4_xattr_inode_hash(EXT4_SB(ea_inode->i_sb), buffer, size);
	if (hash != ext4_xattr_inode_get_hash(ea_inode))
		return -EFSCORRUPTED;

	if (entry) {
		__le32 e_hash, tmp_data;


		tmp_data = cpu_to_le32(hash);
		e_hash = ext4_xattr_hash_entry(entry->e_name, entry->e_name_len,
					       &tmp_data, 1);

		if (e_hash == entry->e_hash)
			return 0;


		e_hash = ext4_xattr_hash_entry_signed(entry->e_name, entry->e_name_len,
							&tmp_data, 1);

		if (e_hash != entry->e_hash)
			return -EFSCORRUPTED;


		pr_warn_once("ext4: filesystem with signed xattr name hash");
	}
	return 0;
}


/**
 * ext4_xattr_inode_get - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_inode_get(struct inode *inode, struct ext4_xattr_entry *entry,
		     void *buffer, size_t size)
{
	struct mb_cache *ea_inode_cache = EA_INODE_CACHE(inode);
	struct inode *ea_inode;
	int err;

	err = ext4_xattr_inode_iget(inode, le32_to_cpu(entry->e_value_inum),
				    le32_to_cpu(entry->e_hash), &ea_inode);
	if (err) {
		ea_inode = NULL;
		goto out;
	}

	if (i_size_read(ea_inode) != size) {
		ext4_warning_inode(ea_inode,
				   "ea_inode file size=%llu entry size=%zu",
				   i_size_read(ea_inode), size);
		err = -EFSCORRUPTED;
		goto out;
	}

	err = ext4_xattr_inode_read(ea_inode, buffer, size);
	if (err)
		goto out;

	if (!ext4_test_inode_state(ea_inode, EXT4_STATE_LUSTRE_EA_INODE)) {
		err = ext4_xattr_inode_verify_hashes(ea_inode, entry, buffer,
						     size);
		if (err) {
			ext4_warning_inode(ea_inode,
					   "EA inode hash validation failed");
			goto out;
		}

		if (ea_inode_cache)
			mb_cache_entry_create(ea_inode_cache, GFP_NOFS,
					ext4_xattr_inode_get_hash(ea_inode),
					ea_inode->i_ino, true               );
	}
out:
	iput(ea_inode);
	return err;
}


/**
 * ext4_xattr_block_get - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_block_get(struct inode *inode, int name_index, const char *name,
		     void *buffer, size_t buffer_size)
{
	struct buffer_head *bh = NULL;
	struct ext4_xattr_entry *entry;
	size_t size;
	void *end;
	int error;
	struct mb_cache *ea_block_cache = EA_BLOCK_CACHE(inode);

	ea_idebug(inode, "name=%d.%s, buffer=%p, buffer_size=%ld",
		  name_index, name, buffer, (long)buffer_size);

	if (!EXT4_I(inode)->i_file_acl)
		return -ENODATA;
	ea_idebug(inode, "reading block %llu",
		  (unsigned long long)EXT4_I(inode)->i_file_acl);
	bh = ext4_sb_bread(inode->i_sb, EXT4_I(inode)->i_file_acl, REQ_PRIO);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	ea_bdebug(bh, "b_count=%d, refcount=%d",
		atomic_read(&(bh->b_count)), le32_to_cpu(BHDR(bh)->h_refcount));
	error = ext4_xattr_check_block(inode, bh);
	if (error)
		goto cleanup;
	ext4_xattr_block_cache_insert(ea_block_cache, bh);
	entry = BFIRST(bh);
	end = bh->b_data + bh->b_size;
	error = xattr_find_entry(inode, &entry, end, name_index, name, 1);
	if (error)
		goto cleanup;
	size = le32_to_cpu(entry->e_value_size);
	error = -ERANGE;
	if (unlikely(size > EXT4_XATTR_SIZE_MAX))
		goto cleanup;
	if (buffer) {
		if (size > buffer_size)
			goto cleanup;
		if (entry->e_value_inum) {
			error = ext4_xattr_inode_get(inode, entry, buffer,
						     size);
			if (error)
				goto cleanup;
		} else {
			u16 offset = le16_to_cpu(entry->e_value_offs);
			void *p = bh->b_data + offset;

			if (unlikely(p + size > end))
				goto cleanup;
			memcpy(buffer, p, size);
		}
	}
	error = size;

cleanup:
	brelse(bh);
	return error;
}


/**
 * ext4_xattr_ibody_get - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int
ext4_xattr_ibody_get(struct inode *inode, int name_index, const char *name,
		     void *buffer, size_t buffer_size)
{
	struct ext4_xattr_ibody_header *header;
	struct ext4_xattr_entry *entry;
	struct ext4_inode *raw_inode;
	struct ext4_iloc iloc;
	size_t size;
	void *end;
	int error;

	if (!ext4_test_inode_state(inode, EXT4_STATE_XATTR))
		return -ENODATA;
	error = ext4_get_inode_loc(inode, &iloc);
	if (error)
		return error;
	raw_inode = ext4_raw_inode(&iloc);
	header = IHDR(inode, raw_inode);
	end = ITAIL(inode, raw_inode);
	entry = IFIRST(header);
	error = xattr_find_entry(inode, &entry, end, name_index, name, 0);
	if (error)
		goto cleanup;
	size = le32_to_cpu(entry->e_value_size);
	error = -ERANGE;
	if (unlikely(size > EXT4_XATTR_SIZE_MAX))
		goto cleanup;
	if (buffer) {
		if (size > buffer_size)
			goto cleanup;
		if (entry->e_value_inum) {
			error = ext4_xattr_inode_get(inode, entry, buffer,
						     size);
			if (error)
				goto cleanup;
		} else {
			u16 offset = le16_to_cpu(entry->e_value_offs);
			void *p = (void *)IFIRST(header) + offset;

			if (unlikely(p + size > end))
				goto cleanup;
			memcpy(buffer, p, size);
		}
	}
	error = size;

cleanup:
	brelse(iloc.bh);
	return error;
}


/**
 * ext4_xattr_get - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int
ext4_xattr_get(struct inode *inode, int name_index, const char *name,
	       void *buffer, size_t buffer_size)
{
	int error;

	if (unlikely(ext4_forced_shutdown(inode->i_sb)))
		return -EIO;

	if (strlen(name) > 255)
		return -ERANGE;

	down_read(&EXT4_I(inode)->xattr_sem);
	error = ext4_xattr_ibody_get(inode, name_index, name, buffer,
				     buffer_size);
	if (error == -ENODATA)
		error = ext4_xattr_block_get(inode, name_index, name, buffer,
					     buffer_size);
	up_read(&EXT4_I(inode)->xattr_sem);
	return error;
}


/**
 * ext4_xattr_list_entries - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_list_entries(struct dentry *dentry, struct ext4_xattr_entry *entry,
			char *buffer, size_t buffer_size)
{
	size_t rest = buffer_size;

	for (; !IS_LAST_ENTRY(entry); entry = EXT4_XATTR_NEXT(entry)) {
		const char *prefix;

		prefix = ext4_xattr_prefix(entry->e_name_index, dentry);
		if (prefix) {
			size_t prefix_len = strlen(prefix);
			size_t size = prefix_len + entry->e_name_len + 1;

			if (buffer) {
				if (size > rest)
					return -ERANGE;
				memcpy(buffer, prefix, prefix_len);
				buffer += prefix_len;
				memcpy(buffer, entry->e_name, entry->e_name_len);
				buffer += entry->e_name_len;
				*buffer++ = 0;
			}
			rest -= size;
		}
	}
	return buffer_size - rest;
}


/**
 * ext4_xattr_block_list - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_block_list(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	struct inode *inode = d_inode(dentry);
	struct buffer_head *bh = NULL;
	int error;

	ea_idebug(inode, "buffer=%p, buffer_size=%ld",
		  buffer, (long)buffer_size);

	if (!EXT4_I(inode)->i_file_acl)
		return 0;
	ea_idebug(inode, "reading block %llu",
		  (unsigned long long)EXT4_I(inode)->i_file_acl);
	bh = ext4_sb_bread(inode->i_sb, EXT4_I(inode)->i_file_acl, REQ_PRIO);
	if (IS_ERR(bh))
		return PTR_ERR(bh);
	ea_bdebug(bh, "b_count=%d, refcount=%d",
		atomic_read(&(bh->b_count)), le32_to_cpu(BHDR(bh)->h_refcount));
	error = ext4_xattr_check_block(inode, bh);
	if (error)
		goto cleanup;
	ext4_xattr_block_cache_insert(EA_BLOCK_CACHE(inode), bh);
	error = ext4_xattr_list_entries(dentry, BFIRST(bh), buffer,
					buffer_size);
cleanup:
	brelse(bh);
	return error;
}


/**
 * ext4_xattr_ibody_list - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_ibody_list(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	struct inode *inode = d_inode(dentry);
	struct ext4_xattr_ibody_header *header;
	struct ext4_inode *raw_inode;
	struct ext4_iloc iloc;
	int error;

	if (!ext4_test_inode_state(inode, EXT4_STATE_XATTR))
		return 0;
	error = ext4_get_inode_loc(inode, &iloc);
	if (error)
		return error;
	raw_inode = ext4_raw_inode(&iloc);
	header = IHDR(inode, raw_inode);
	error = ext4_xattr_list_entries(dentry, IFIRST(header),
					buffer, buffer_size);

	brelse(iloc.bh);
	return error;
}


/**
 * ext4_listxattr - Implements the listxattr operation within the extended metadata subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
ssize_t
ext4_listxattr(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	int ret, ret2;

	down_read(&EXT4_I(d_inode(dentry))->xattr_sem);
	ret = ret2 = ext4_xattr_ibody_list(dentry, buffer, buffer_size);
	if (ret < 0)
		goto errout;
	if (buffer) {
		buffer += ret;
		buffer_size -= ret;
	}
	ret = ext4_xattr_block_list(dentry, buffer, buffer_size);
	if (ret < 0)
		goto errout;
	ret += ret2;
errout:
	up_read(&EXT4_I(d_inode(dentry))->xattr_sem);
	return ret;
}


/**
 * ext4_xattr_update_super_block - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_xattr_update_super_block(handle_t *handle,
					  struct super_block *sb)
{
	if (ext4_has_feature_xattr(sb))
		return;

	BUFFER_TRACE(EXT4_SB(sb)->s_sbh, "get_write_access");
	if (ext4_journal_get_write_access(handle, sb, EXT4_SB(sb)->s_sbh,
					  EXT4_JTR_NONE) == 0) {
		lock_buffer(EXT4_SB(sb)->s_sbh);
		ext4_set_feature_xattr(sb);
		ext4_superblock_csum_set(sb);
		unlock_buffer(EXT4_SB(sb)->s_sbh);
		ext4_handle_dirty_metadata(handle, NULL, EXT4_SB(sb)->s_sbh);
	}
}


/**
 * ext4_get_inode_usage - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_get_inode_usage(struct inode *inode, qsize_t *usage)
{
	struct ext4_iloc iloc = { .bh = NULL };
	struct buffer_head *bh = NULL;
	struct ext4_inode *raw_inode;
	struct ext4_xattr_ibody_header *header;
	struct ext4_xattr_entry *entry;
	qsize_t ea_inode_refs = 0;
	int ret;

	lockdep_assert_held_read(&EXT4_I(inode)->xattr_sem);

	if (ext4_test_inode_state(inode, EXT4_STATE_XATTR)) {
		ret = ext4_get_inode_loc(inode, &iloc);
		if (ret)
			goto out;
		raw_inode = ext4_raw_inode(&iloc);
		header = IHDR(inode, raw_inode);

		for (entry = IFIRST(header); !IS_LAST_ENTRY(entry);
		     entry = EXT4_XATTR_NEXT(entry))
			if (entry->e_value_inum)
				ea_inode_refs++;
	}

	if (EXT4_I(inode)->i_file_acl) {
		bh = ext4_sb_bread(inode->i_sb, EXT4_I(inode)->i_file_acl, REQ_PRIO);
		if (IS_ERR(bh)) {
			ret = PTR_ERR(bh);
			bh = NULL;
			goto out;
		}

		ret = ext4_xattr_check_block(inode, bh);
		if (ret)
			goto out;

		for (entry = BFIRST(bh); !IS_LAST_ENTRY(entry);
		     entry = EXT4_XATTR_NEXT(entry))
			if (entry->e_value_inum)
				ea_inode_refs++;
	}
	*usage = ea_inode_refs + 1;
	ret = 0;
out:
	brelse(iloc.bh);
	brelse(bh);
	return ret;
}


/**
 * round_up_cluster - Implements the round up cluster operation within the extended metadata subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline size_t round_up_cluster(struct inode *inode, size_t length)
{
	struct super_block *sb = inode->i_sb;
	size_t cluster_size = 1 << (EXT4_SB(sb)->s_cluster_bits +
				    inode->i_blkbits);
	size_t mask = ~(cluster_size - 1);

	return (length + cluster_size - 1) & mask;
}


/**
 * ext4_xattr_inode_alloc_quota - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_inode_alloc_quota(struct inode *inode, size_t len)
{
	int err;

	err = dquot_alloc_inode(inode);
	if (err)
		return err;
	err = dquot_alloc_space_nodirty(inode, round_up_cluster(inode, len));
	if (err)
		dquot_free_inode(inode);
	return err;
}


/**
 * ext4_xattr_inode_free_quota - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_xattr_inode_free_quota(struct inode *parent,
					struct inode *ea_inode,
					size_t len)
{
	if (ea_inode &&
	    ext4_test_inode_state(ea_inode, EXT4_STATE_LUSTRE_EA_INODE))
		return;
	dquot_free_space_nodirty(parent, round_up_cluster(parent, len));
	dquot_free_inode(parent);
}


/**
 * __ext4_xattr_set_credits - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int __ext4_xattr_set_credits(struct super_block *sb, struct inode *inode,
			     struct buffer_head *block_bh, size_t value_len,
			     bool is_create)
{
	int credits;
	int blocks;


	credits = 7;


	credits += EXT4_MAXQUOTAS_TRANS_BLOCKS(sb);


	if (inode && ext4_has_inline_data(inode))
		credits += ext4_writepage_trans_blocks(inode) + 1;


	if (!ext4_has_feature_ea_inode(sb))
		return credits;


	credits += 4;


	blocks = (value_len + sb->s_blocksize - 1) >> sb->s_blocksize_bits;


	blocks += 1;


	credits += blocks * 2;


	credits += blocks;

	if (!is_create) {


		credits += 4;


		blocks = XATTR_SIZE_MAX >> sb->s_blocksize_bits;


		blocks += 1;


		credits += blocks * 2;
	}


	if (block_bh) {
		struct ext4_xattr_entry *entry = BFIRST(block_bh);

		for (; !IS_LAST_ENTRY(entry); entry = EXT4_XATTR_NEXT(entry))
			if (entry->e_value_inum)

				credits += 1;
	}
	return credits;
}


/**
 * ext4_xattr_inode_update_ref - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_inode_update_ref(handle_t *handle, struct inode *ea_inode,
				       int ref_change)
{
	struct ext4_iloc iloc;
	u64 ref_count;
	int ret;

	inode_lock_nested(ea_inode, I_MUTEX_XATTR);

	ret = ext4_reserve_inode_write(handle, ea_inode, &iloc);
	if (ret)
		goto out;

	ref_count = ext4_xattr_inode_get_ref(ea_inode);
	if ((ref_count == 0 && ref_change < 0) || (ref_count == U64_MAX && ref_change > 0)) {
		ext4_error_inode(ea_inode, __func__, __LINE__, 0,
			"EA inode %lu ref wraparound: ref_count=%lld ref_change=%d",
			ea_inode->i_ino, ref_count, ref_change);
		brelse(iloc.bh);
		ret = -EFSCORRUPTED;
		goto out;
	}
	ref_count += ref_change;
	ext4_xattr_inode_set_ref(ea_inode, ref_count);

	if (ref_change > 0) {
		if (ref_count == 1) {
			WARN_ONCE(ea_inode->i_nlink, "EA inode %lu i_nlink=%u",
				  ea_inode->i_ino, ea_inode->i_nlink);

			set_nlink(ea_inode, 1);
			ext4_orphan_del(handle, ea_inode);
		}
	} else {
		if (ref_count == 0) {
			WARN_ONCE(ea_inode->i_nlink != 1,
				  "EA inode %lu i_nlink=%u",
				  ea_inode->i_ino, ea_inode->i_nlink);

			clear_nlink(ea_inode);
			ext4_orphan_add(handle, ea_inode);
		}
	}

	ret = ext4_mark_iloc_dirty(handle, ea_inode, &iloc);
	if (ret)
		ext4_warning_inode(ea_inode,
				   "ext4_mark_iloc_dirty() failed ret=%d", ret);
out:
	inode_unlock(ea_inode);
	return ret;
}


/**
 * ext4_xattr_inode_inc_ref - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_inode_inc_ref(handle_t *handle, struct inode *ea_inode)
{
	return ext4_xattr_inode_update_ref(handle, ea_inode, 1);
}


/**
 * ext4_xattr_inode_dec_ref - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_inode_dec_ref(handle_t *handle, struct inode *ea_inode)
{
	return ext4_xattr_inode_update_ref(handle, ea_inode, -1);
}


/**
 * ext4_xattr_inode_inc_ref_all - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_inode_inc_ref_all(handle_t *handle, struct inode *parent,
					struct ext4_xattr_entry *first)
{
	struct inode *ea_inode;
	struct ext4_xattr_entry *entry;
	struct ext4_xattr_entry *failed_entry;
	unsigned int ea_ino;
	int err, saved_err;

	for (entry = first; !IS_LAST_ENTRY(entry);
	     entry = EXT4_XATTR_NEXT(entry)) {
		if (!entry->e_value_inum)
			continue;
		ea_ino = le32_to_cpu(entry->e_value_inum);
		err = ext4_xattr_inode_iget(parent, ea_ino,
					    le32_to_cpu(entry->e_hash),
					    &ea_inode);
		if (err)
			goto cleanup;
		err = ext4_xattr_inode_inc_ref(handle, ea_inode);
		if (err) {
			ext4_warning_inode(ea_inode, "inc ref error %d", err);
			iput(ea_inode);
			goto cleanup;
		}
		iput(ea_inode);
	}
	return 0;

cleanup:
	saved_err = err;
	failed_entry = entry;

	for (entry = first; entry != failed_entry;
	     entry = EXT4_XATTR_NEXT(entry)) {
		if (!entry->e_value_inum)
			continue;
		ea_ino = le32_to_cpu(entry->e_value_inum);
		err = ext4_xattr_inode_iget(parent, ea_ino,
					    le32_to_cpu(entry->e_hash),
					    &ea_inode);
		if (err) {
			ext4_warning(parent->i_sb,
				     "cleanup ea_ino %u iget error %d", ea_ino,
				     err);
			continue;
		}
		err = ext4_xattr_inode_dec_ref(handle, ea_inode);
		if (err)
			ext4_warning_inode(ea_inode, "cleanup dec ref error %d",
					   err);
		iput(ea_inode);
	}
	return saved_err;
}


/**
 * ext4_xattr_restart_fn - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_restart_fn(handle_t *handle, struct inode *inode,
			struct buffer_head *bh, bool block_csum, bool dirty)
{
	int error;

	if (bh && dirty) {
		if (block_csum)
			ext4_xattr_block_csum_set(inode, bh);
		error = ext4_handle_dirty_metadata(handle, NULL, bh);
		if (error) {
			ext4_warning(inode->i_sb, "Handle metadata (error %d)",
				     error);
			return error;
		}
	}
	return 0;
}


/**
 * ext4_xattr_inode_dec_ref_all - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void
ext4_xattr_inode_dec_ref_all(handle_t *handle, struct inode *parent,
			     struct buffer_head *bh,
			     struct ext4_xattr_entry *first, bool block_csum,
			     struct ext4_xattr_inode_array **ea_inode_array,
			     int extra_credits, bool skip_quota)
{
	struct inode *ea_inode;
	struct ext4_xattr_entry *entry;
	struct ext4_iloc iloc = { .bh = NULL };
	bool dirty = false;
	unsigned int ea_ino;
	int err;
	int credits;
	void *end;

	if (block_csum)
		end = (void *)bh->b_data + bh->b_size;
	else {
		err = ext4_get_inode_loc(parent, &iloc);
		if (err) {
			EXT4_ERROR_INODE(parent, "parent inode loc (error %d)", err);
			return;
		}
		end = (void *)ext4_raw_inode(&iloc) + EXT4_SB(parent->i_sb)->s_inode_size;
	}


	credits = 2 + extra_credits;

	for (entry = first; (void *)entry < end && !IS_LAST_ENTRY(entry);
	     entry = EXT4_XATTR_NEXT(entry)) {
		if (!entry->e_value_inum)
			continue;
		ea_ino = le32_to_cpu(entry->e_value_inum);
		err = ext4_xattr_inode_iget(parent, ea_ino,
					    le32_to_cpu(entry->e_hash),
					    &ea_inode);
		if (err)
			continue;

		err = ext4_expand_inode_array(ea_inode_array, ea_inode);
		if (err) {
			ext4_warning_inode(ea_inode,
					   "Expand inode array err=%d", err);
			iput(ea_inode);
			continue;
		}

		err = ext4_journal_ensure_credits_fn(handle, credits, credits,
			ext4_free_metadata_revoke_credits(parent->i_sb, 1),
			ext4_xattr_restart_fn(handle, parent, bh, block_csum,
					      dirty));
		if (err < 0) {
			ext4_warning_inode(ea_inode, "Ensure credits err=%d",
					   err);
			continue;
		}
		if (err > 0) {
			err = ext4_journal_get_write_access(handle,
					parent->i_sb, bh, EXT4_JTR_NONE);
			if (err) {
				ext4_warning_inode(ea_inode,
						"Re-get write access err=%d",
						err);
				continue;
			}
		}

		err = ext4_xattr_inode_dec_ref(handle, ea_inode);
		if (err) {
			ext4_warning_inode(ea_inode, "ea_inode dec ref err=%d",
					   err);
			continue;
		}

		if (!skip_quota)
			ext4_xattr_inode_free_quota(parent, ea_inode,
					      le32_to_cpu(entry->e_value_size));


		entry->e_value_inum = 0;
		entry->e_value_size = 0;

		dirty = true;
	}

	if (dirty) {


		err = ext4_handle_dirty_metadata(handle, NULL, bh);
		if (err)
			ext4_warning_inode(parent,
					   "handle dirty metadata err=%d", err);
	}

	brelse(iloc.bh);
}


/**
 * ext4_xattr_release_block - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void
ext4_xattr_release_block(handle_t *handle, struct inode *inode,
			 struct buffer_head *bh,
			 struct ext4_xattr_inode_array **ea_inode_array,
			 int extra_credits)
{
	struct mb_cache *ea_block_cache = EA_BLOCK_CACHE(inode);
	u32 hash, ref;
	int error = 0;

	BUFFER_TRACE(bh, "get_write_access");
	error = ext4_journal_get_write_access(handle, inode->i_sb, bh,
					      EXT4_JTR_NONE);
	if (error)
		goto out;

retry_ref:
	lock_buffer(bh);
	hash = le32_to_cpu(BHDR(bh)->h_hash);
	ref = le32_to_cpu(BHDR(bh)->h_refcount);
	if (ref == 1) {
		ea_bdebug(bh, "refcount now=0; freeing");


		if (ea_block_cache) {
			struct mb_cache_entry *oe;

			oe = mb_cache_entry_delete_or_get(ea_block_cache, hash,
							  bh->b_blocknr);
			if (oe) {
				unlock_buffer(bh);
				mb_cache_entry_wait_unused(oe);
				mb_cache_entry_put(ea_block_cache, oe);
				goto retry_ref;
			}
		}
		get_bh(bh);
		unlock_buffer(bh);

		if (ext4_has_feature_ea_inode(inode->i_sb))
			ext4_xattr_inode_dec_ref_all(handle, inode, bh,
						     BFIRST(bh),
						     true                 ,
						     ea_inode_array,
						     extra_credits,
						     true                 );
		ext4_free_blocks(handle, inode, bh, 0, 1,
				 EXT4_FREE_BLOCKS_METADATA |
				 EXT4_FREE_BLOCKS_FORGET);
	} else {
		ref--;
		BHDR(bh)->h_refcount = cpu_to_le32(ref);
		if (ref == EXT4_XATTR_REFCOUNT_MAX - 1) {
			struct mb_cache_entry *ce;

			if (ea_block_cache) {
				ce = mb_cache_entry_get(ea_block_cache, hash,
							bh->b_blocknr);
				if (ce) {
					set_bit(MBE_REUSABLE_B, &ce->e_flags);
					mb_cache_entry_put(ea_block_cache, ce);
				}
			}
		}

		ext4_xattr_block_csum_set(inode, bh);


		if (ext4_handle_valid(handle))
			error = ext4_handle_dirty_metadata(handle, inode, bh);
		unlock_buffer(bh);
		if (!ext4_handle_valid(handle))
			error = ext4_handle_dirty_metadata(handle, inode, bh);
		if (IS_SYNC(inode))
			ext4_handle_sync(handle);
		dquot_free_block(inode, EXT4_C2B(EXT4_SB(inode->i_sb), 1));
		ea_bdebug(bh, "refcount now=%d; releasing",
			  le32_to_cpu(BHDR(bh)->h_refcount));
	}
out:
	ext4_std_error(inode->i_sb, error);
	return;
}


/**
 * ext4_xattr_free_space - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static size_t ext4_xattr_free_space(struct ext4_xattr_entry *last,
				    size_t *min_offs, void *base, int *total)
{
	for (; !IS_LAST_ENTRY(last); last = EXT4_XATTR_NEXT(last)) {
		if (!last->e_value_inum && last->e_value_size) {
			size_t offs = le16_to_cpu(last->e_value_offs);
			if (offs < *min_offs)
				*min_offs = offs;
		}
		if (total)
			*total += EXT4_XATTR_LEN(last->e_name_len);
	}
	return (*min_offs - ((void *)last - base) - sizeof(__u32));
}


/**
 * ext4_xattr_inode_write - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_inode_write(handle_t *handle, struct inode *ea_inode,
				  const void *buf, int bufsize)
{
	struct buffer_head *bh = NULL;
	unsigned long block = 0;
	int blocksize = ea_inode->i_sb->s_blocksize;
	int max_blocks = (bufsize + blocksize - 1) >> ea_inode->i_blkbits;
	int csize, wsize = 0;
	int ret = 0, ret2 = 0;
	int retries = 0;

retry:
	while (ret >= 0 && ret < max_blocks) {
		struct ext4_map_blocks map;
		map.m_lblk = block += ret;
		map.m_len = max_blocks -= ret;

		ret = ext4_map_blocks(handle, ea_inode, &map,
				      EXT4_GET_BLOCKS_CREATE);
		if (ret <= 0) {
			ext4_mark_inode_dirty(handle, ea_inode);
			if (ret == -ENOSPC &&
			    ext4_should_retry_alloc(ea_inode->i_sb, &retries)) {
				ret = 0;
				goto retry;
			}
			break;
		}
	}

	if (ret < 0)
		return ret;

	block = 0;
	while (wsize < bufsize) {
		brelse(bh);
		csize = (bufsize - wsize) > blocksize ? blocksize :
								bufsize - wsize;
		bh = ext4_getblk(handle, ea_inode, block, 0);
		if (IS_ERR(bh))
			return PTR_ERR(bh);
		if (!bh) {
			WARN_ON_ONCE(1);
			EXT4_ERROR_INODE(ea_inode,
					 "ext4_getblk() return bh = NULL");
			return -EFSCORRUPTED;
		}
		ret = ext4_journal_get_write_access(handle, ea_inode->i_sb, bh,
						   EXT4_JTR_NONE);
		if (ret)
			goto out;

		memcpy(bh->b_data, buf, csize);


		if (csize < blocksize)
			memset(bh->b_data + csize, 0, blocksize - csize);
		set_buffer_uptodate(bh);
		ext4_handle_dirty_metadata(handle, ea_inode, bh);

		buf += csize;
		wsize += csize;
		block += 1;
	}

	inode_lock(ea_inode);
	i_size_write(ea_inode, wsize);
	ext4_update_i_disksize(ea_inode, wsize);
	inode_unlock(ea_inode);

	ret2 = ext4_mark_inode_dirty(handle, ea_inode);
	if (unlikely(ret2 && !ret))
		ret = ret2;

out:
	brelse(bh);

	return ret;
}


/**
 * ext4_xattr_inode_create - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct inode *ext4_xattr_inode_create(handle_t *handle,
					     struct inode *inode, u32 hash)
{
	struct inode *ea_inode = NULL;
	uid_t owner[2] = { i_uid_read(inode), i_gid_read(inode) };
	int err;

	if (inode->i_sb->s_root == NULL) {
		ext4_warning(inode->i_sb,
			     "refuse to create EA inode when umounting");
		WARN_ON(1);
		return ERR_PTR(-EINVAL);
	}


	ea_inode = ext4_new_inode(handle, inode->i_sb->s_root->d_inode,
				  S_IFREG | 0600, NULL, inode->i_ino + 1, owner,
				  EXT4_EA_INODE_FL);
	if (!IS_ERR(ea_inode)) {
		ea_inode->i_op = &ext4_file_inode_operations;
		ea_inode->i_fop = &ext4_file_operations;
		ext4_set_aops(ea_inode);
		ext4_xattr_inode_set_class(ea_inode);
		unlock_new_inode(ea_inode);
		ext4_xattr_inode_set_ref(ea_inode, 1);
		ext4_xattr_inode_set_hash(ea_inode, hash);
		err = ext4_mark_inode_dirty(handle, ea_inode);
		if (!err)
			err = ext4_inode_attach_jinode(ea_inode);
		if (err) {
			if (ext4_xattr_inode_dec_ref(handle, ea_inode))
				ext4_warning_inode(ea_inode,
					"cleanup dec ref error %d", err);
			iput(ea_inode);
			return ERR_PTR(err);
		}


		dquot_free_inode(ea_inode);
		dquot_drop(ea_inode);
		inode_lock(ea_inode);
		ea_inode->i_flags |= S_NOQUOTA;
		inode_unlock(ea_inode);
	}

	return ea_inode;
}


/**
 * ext4_xattr_inode_cache_find - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct inode *
ext4_xattr_inode_cache_find(struct inode *inode, const void *value,
			    size_t value_len, u32 hash)
{
	struct inode *ea_inode;
	struct mb_cache_entry *ce;
	struct mb_cache *ea_inode_cache = EA_INODE_CACHE(inode);
	void *ea_data;

	if (!ea_inode_cache)
		return NULL;

	ce = mb_cache_entry_find_first(ea_inode_cache, hash);
	if (!ce)
		return NULL;

	WARN_ON_ONCE(ext4_handle_valid(journal_current_handle()) &&
		     !(current->flags & PF_MEMALLOC_NOFS));

	ea_data = kvmalloc(value_len, GFP_NOFS);
	if (!ea_data) {
		mb_cache_entry_put(ea_inode_cache, ce);
		return NULL;
	}

	while (ce) {
		ea_inode = ext4_iget(inode->i_sb, ce->e_value,
				     EXT4_IGET_EA_INODE);
		if (IS_ERR(ea_inode))
			goto next_entry;
		ext4_xattr_inode_set_class(ea_inode);
		if (i_size_read(ea_inode) == value_len &&
		    !ext4_xattr_inode_read(ea_inode, ea_data, value_len) &&
		    !ext4_xattr_inode_verify_hashes(ea_inode, NULL, ea_data,
						    value_len) &&
		    !memcmp(value, ea_data, value_len)) {
			mb_cache_entry_touch(ea_inode_cache, ce);
			mb_cache_entry_put(ea_inode_cache, ce);
			kvfree(ea_data);
			return ea_inode;
		}
		iput(ea_inode);
	next_entry:
		ce = mb_cache_entry_find_next(ea_inode_cache, ce);
	}
	kvfree(ea_data);
	return NULL;
}


/**
 * ext4_xattr_inode_lookup_create - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct inode *ext4_xattr_inode_lookup_create(handle_t *handle,
		struct inode *inode, const void *value, size_t value_len)
{
	struct inode *ea_inode;
	u32 hash;
	int err;


	err = ext4_xattr_inode_alloc_quota(inode, value_len);
	if (err)
		return ERR_PTR(err);

	hash = ext4_xattr_inode_hash(EXT4_SB(inode->i_sb), value, value_len);
	ea_inode = ext4_xattr_inode_cache_find(inode, value, value_len, hash);
	if (ea_inode) {
		err = ext4_xattr_inode_inc_ref(handle, ea_inode);
		if (err)
			goto out_err;
		return ea_inode;
	}


	ea_inode = ext4_xattr_inode_create(handle, inode, hash);
	if (IS_ERR(ea_inode)) {
		ext4_xattr_inode_free_quota(inode, NULL, value_len);
		return ea_inode;
	}

	err = ext4_xattr_inode_write(handle, ea_inode, value, value_len);
	if (err) {
		if (ext4_xattr_inode_dec_ref(handle, ea_inode))
			ext4_warning_inode(ea_inode, "cleanup dec ref error %d", err);
		goto out_err;
	}

	if (EA_INODE_CACHE(inode))
		mb_cache_entry_create(EA_INODE_CACHE(inode), GFP_NOFS, hash,
				      ea_inode->i_ino, true               );
	return ea_inode;
out_err:
	iput(ea_inode);
	ext4_xattr_inode_free_quota(inode, NULL, value_len);
	return ERR_PTR(err);
}


#define EXT4_XATTR_BLOCK_RESERVE(inode)	min(i_blocksize(inode)/8, 1024U)


/**
 * ext4_xattr_set_entry - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_set_entry(struct ext4_xattr_info *i,
				struct ext4_xattr_search *s,
				handle_t *handle, struct inode *inode,
				struct inode *new_ea_inode,
				bool is_block)
{
	struct ext4_xattr_entry *last, *next;
	struct ext4_xattr_entry *here = s->here;
	size_t min_offs = s->end - s->base, name_len = strlen(i->name);
	int in_inode = i->in_inode;
	struct inode *old_ea_inode = NULL;
	size_t old_size, new_size;
	int ret;


	old_size = (!s->not_found && !here->e_value_inum) ?
			EXT4_XATTR_SIZE(le32_to_cpu(here->e_value_size)) : 0;
	new_size = (i->value && !in_inode) ? EXT4_XATTR_SIZE(i->value_len) : 0;


	if (new_size && new_size == old_size) {
		size_t offs = le16_to_cpu(here->e_value_offs);
		void *val = s->base + offs;

		here->e_value_size = cpu_to_le32(i->value_len);
		if (i->value == EXT4_ZERO_XATTR_VALUE) {
			memset(val, 0, new_size);
		} else {
			memcpy(val, i->value, i->value_len);

			memset(val + i->value_len, 0, new_size - i->value_len);
		}
		goto update_hash;
	}


	last = s->first;
	for (; !IS_LAST_ENTRY(last); last = next) {
		next = EXT4_XATTR_NEXT(last);
		if ((void *)next >= s->end) {
			EXT4_ERROR_INODE(inode, "corrupted xattr entries");
			ret = -EFSCORRUPTED;
			goto out;
		}
		if (!last->e_value_inum && last->e_value_size) {
			size_t offs = le16_to_cpu(last->e_value_offs);
			if (offs < min_offs)
				min_offs = offs;
		}
	}


	if (i->value) {
		size_t free;

		free = min_offs - ((void *)last - s->base) - sizeof(__u32);
		if (!s->not_found)
			free += EXT4_XATTR_LEN(name_len) + old_size;

		if (free < EXT4_XATTR_LEN(name_len) + new_size) {
			ret = -ENOSPC;
			goto out;
		}


		if (ext4_has_feature_ea_inode(inode->i_sb) &&
		    new_size && is_block &&
		    (min_offs + old_size - new_size) <
					EXT4_XATTR_BLOCK_RESERVE(inode)) {
			ret = -ENOSPC;
			goto out;
		}
	}


	if (!s->not_found && here->e_value_inum) {
		ret = ext4_xattr_inode_iget(inode,
					    le32_to_cpu(here->e_value_inum),
					    le32_to_cpu(here->e_hash),
					    &old_ea_inode);
		if (ret) {
			old_ea_inode = NULL;
			goto out;
		}


		ret = ext4_xattr_inode_dec_ref(handle, old_ea_inode);
		if (ret)
			goto out;

		ext4_xattr_inode_free_quota(inode, old_ea_inode,
					    le32_to_cpu(here->e_value_size));
	}


	if (!s->not_found && here->e_value_size && !here->e_value_inum) {

		void *first_val = s->base + min_offs;
		size_t offs = le16_to_cpu(here->e_value_offs);
		void *val = s->base + offs;

		memmove(first_val + old_size, first_val, val - first_val);
		memset(first_val, 0, old_size);
		min_offs += old_size;


		last = s->first;
		while (!IS_LAST_ENTRY(last)) {
			size_t o = le16_to_cpu(last->e_value_offs);

			if (!last->e_value_inum &&
			    last->e_value_size && o < offs)
				last->e_value_offs = cpu_to_le16(o + old_size);
			last = EXT4_XATTR_NEXT(last);
		}
	}

	if (!i->value) {

		size_t size = EXT4_XATTR_LEN(name_len);

		last = ENTRY((void *)last - size);
		memmove(here, (void *)here + size,
			(void *)last - (void *)here + sizeof(__u32));
		memset(last, 0, size);


		if (!is_block && ext4_has_inline_data(inode)) {
			ret = ext4_find_inline_data_nolock(inode);
			if (ret) {
				ext4_warning_inode(inode,
					"unable to update i_inline_off");
				goto out;
			}
		}
	} else if (s->not_found) {

		size_t size = EXT4_XATTR_LEN(name_len);
		size_t rest = (void *)last - (void *)here + sizeof(__u32);

		memmove((void *)here + size, here, rest);
		memset(here, 0, size);
		here->e_name_index = i->name_index;
		here->e_name_len = name_len;
		memcpy(here->e_name, i->name, name_len);
	} else {

		here->e_value_inum = 0;
		here->e_value_offs = 0;
		here->e_value_size = 0;
	}

	if (i->value) {

		if (in_inode) {
			here->e_value_inum = cpu_to_le32(new_ea_inode->i_ino);
		} else if (i->value_len) {
			void *val = s->base + min_offs - new_size;

			here->e_value_offs = cpu_to_le16(min_offs - new_size);
			if (i->value == EXT4_ZERO_XATTR_VALUE) {
				memset(val, 0, new_size);
			} else {
				memcpy(val, i->value, i->value_len);

				memset(val + i->value_len, 0,
				       new_size - i->value_len);
			}
		}
		here->e_value_size = cpu_to_le32(i->value_len);
	}

update_hash:
	if (i->value) {
		__le32 hash = 0;


		if (in_inode) {
			__le32 crc32c_hash;


			crc32c_hash = cpu_to_le32(
				       ext4_xattr_inode_get_hash(new_ea_inode));
			hash = ext4_xattr_hash_entry(here->e_name,
						     here->e_name_len,
						     &crc32c_hash, 1);
		} else if (is_block) {
			__le32 *value = s->base + le16_to_cpu(
							here->e_value_offs);

			hash = ext4_xattr_hash_entry(here->e_name,
						     here->e_name_len, value,
						     new_size >> 2);
		}
		here->e_hash = hash;
	}

	if (is_block)
		ext4_xattr_rehash((struct ext4_xattr_header *)s->base);

	ret = 0;
out:
	iput(old_ea_inode);
	return ret;
}


/**
 * struct ext4_xattr_block_find - Private EXT4 state/data structure used by extended metadata.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_xattr_block_find {
	struct ext4_xattr_search s;
	struct buffer_head *bh;
};


/**
 * ext4_xattr_block_find - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_block_find(struct inode *inode, struct ext4_xattr_info *i,
		      struct ext4_xattr_block_find *bs)
{
	struct super_block *sb = inode->i_sb;
	int error;

	ea_idebug(inode, "name=%d.%s, value=%p, value_len=%ld",
		  i->name_index, i->name, i->value, (long)i->value_len);

	if (EXT4_I(inode)->i_file_acl) {

		bs->bh = ext4_sb_bread(sb, EXT4_I(inode)->i_file_acl, REQ_PRIO);
		if (IS_ERR(bs->bh)) {
			error = PTR_ERR(bs->bh);
			bs->bh = NULL;
			return error;
		}
		ea_bdebug(bs->bh, "b_count=%d, refcount=%d",
			atomic_read(&(bs->bh->b_count)),
			le32_to_cpu(BHDR(bs->bh)->h_refcount));
		error = ext4_xattr_check_block(inode, bs->bh);
		if (error)
			return error;

		bs->s.base = BHDR(bs->bh);
		bs->s.first = BFIRST(bs->bh);
		bs->s.end = bs->bh->b_data + bs->bh->b_size;
		bs->s.here = bs->s.first;
		error = xattr_find_entry(inode, &bs->s.here, bs->s.end,
					 i->name_index, i->name, 1);
		if (error && error != -ENODATA)
			return error;
		bs->s.not_found = error;
	}
	return 0;
}


/**
 * ext4_xattr_block_set - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_block_set(handle_t *handle, struct inode *inode,
		     struct ext4_xattr_info *i,
		     struct ext4_xattr_block_find *bs)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *new_bh = NULL;
	struct ext4_xattr_search s_copy = bs->s;
	struct ext4_xattr_search *s = &s_copy;
	struct mb_cache_entry *ce = NULL;
	int error = 0;
	struct mb_cache *ea_block_cache = EA_BLOCK_CACHE(inode);
	struct inode *ea_inode = NULL, *tmp_inode;
	size_t old_ea_inode_quota = 0;
	unsigned int ea_ino;

#define header(x) ((struct ext4_xattr_header *)(x))


	if (i->value && i->in_inode) {
		WARN_ON_ONCE(!i->value_len);

		ea_inode = ext4_xattr_inode_lookup_create(handle, inode,
					i->value, i->value_len);
		if (IS_ERR(ea_inode)) {
			error = PTR_ERR(ea_inode);
			ea_inode = NULL;
			goto cleanup;
		}
	}

	if (s->base) {
		int offset = (char *)s->here - bs->bh->b_data;

		BUFFER_TRACE(bs->bh, "get_write_access");
		error = ext4_journal_get_write_access(handle, sb, bs->bh,
						      EXT4_JTR_NONE);
		if (error)
			goto cleanup;

		lock_buffer(bs->bh);

		if (header(s->base)->h_refcount == cpu_to_le32(1)) {
			__u32 hash = le32_to_cpu(BHDR(bs->bh)->h_hash);


			if (ea_block_cache) {
				struct mb_cache_entry *oe;

				oe = mb_cache_entry_delete_or_get(ea_block_cache,
					hash, bs->bh->b_blocknr);
				if (oe) {


					mb_cache_entry_put(ea_block_cache, oe);
					goto clone_block;
				}
			}
			ea_bdebug(bs->bh, "modifying in-place");
			error = ext4_xattr_set_entry(i, s, handle, inode,
					     ea_inode, true               );
			ext4_xattr_block_csum_set(inode, bs->bh);
			unlock_buffer(bs->bh);
			if (error == -EFSCORRUPTED)
				goto bad_block;
			if (!error)
				error = ext4_handle_dirty_metadata(handle,
								   inode,
								   bs->bh);
			if (error)
				goto cleanup;
			goto inserted;
		}
clone_block:
		unlock_buffer(bs->bh);
		ea_bdebug(bs->bh, "cloning");
		s->base = kmemdup(BHDR(bs->bh), bs->bh->b_size, GFP_NOFS);
		error = -ENOMEM;
		if (s->base == NULL)
			goto cleanup;
		s->first = ENTRY(header(s->base)+1);
		header(s->base)->h_refcount = cpu_to_le32(1);
		s->here = ENTRY(s->base + offset);
		s->end = s->base + bs->bh->b_size;


		if (!s->not_found && s->here->e_value_inum) {
			ea_ino = le32_to_cpu(s->here->e_value_inum);
			error = ext4_xattr_inode_iget(inode, ea_ino,
				      le32_to_cpu(s->here->e_hash),
				      &tmp_inode);
			if (error)
				goto cleanup;

			if (!ext4_test_inode_state(tmp_inode,
					EXT4_STATE_LUSTRE_EA_INODE)) {


				old_ea_inode_quota = le32_to_cpu(
						s->here->e_value_size);
			}
			iput(tmp_inode);

			s->here->e_value_inum = 0;
			s->here->e_value_size = 0;
		}
	} else {

		s->base = kzalloc(sb->s_blocksize, GFP_NOFS);
		error = -ENOMEM;
		if (s->base == NULL)
			goto cleanup;
		header(s->base)->h_magic = cpu_to_le32(EXT4_XATTR_MAGIC);
		header(s->base)->h_blocks = cpu_to_le32(1);
		header(s->base)->h_refcount = cpu_to_le32(1);
		s->first = ENTRY(header(s->base)+1);
		s->here = ENTRY(header(s->base)+1);
		s->end = s->base + sb->s_blocksize;
	}

	error = ext4_xattr_set_entry(i, s, handle, inode, ea_inode,
				     true               );
	if (error == -EFSCORRUPTED)
		goto bad_block;
	if (error)
		goto cleanup;

inserted:
	if (!IS_LAST_ENTRY(s->first)) {
		new_bh = ext4_xattr_block_cache_find(inode, header(s->base), &ce);
		if (IS_ERR(new_bh)) {
			error = PTR_ERR(new_bh);
			new_bh = NULL;
			goto cleanup;
		}

		if (new_bh) {

			if (new_bh == bs->bh)
				ea_bdebug(new_bh, "keeping");
			else {
				u32 ref;

#ifdef EXT4_XATTR_DEBUG
				WARN_ON_ONCE(dquot_initialize_needed(inode));
#endif


				error = dquot_alloc_block(inode,
						EXT4_C2B(EXT4_SB(sb), 1));
				if (error)
					goto cleanup;
				BUFFER_TRACE(new_bh, "get_write_access");
				error = ext4_journal_get_write_access(
						handle, sb, new_bh,
						EXT4_JTR_NONE);
				if (error)
					goto cleanup_dquot;
				lock_buffer(new_bh);


				ref = le32_to_cpu(BHDR(new_bh)->h_refcount);
				if (ref >= EXT4_XATTR_REFCOUNT_MAX) {


					clear_bit(MBE_REUSABLE_B, &ce->e_flags);
					unlock_buffer(new_bh);
					dquot_free_block(inode,
							 EXT4_C2B(EXT4_SB(sb),
								  1));
					brelse(new_bh);
					mb_cache_entry_put(ea_block_cache, ce);
					ce = NULL;
					new_bh = NULL;
					goto inserted;
				}
				ref++;
				BHDR(new_bh)->h_refcount = cpu_to_le32(ref);
				if (ref == EXT4_XATTR_REFCOUNT_MAX)
					clear_bit(MBE_REUSABLE_B, &ce->e_flags);
				ea_bdebug(new_bh, "reusing; refcount now=%d",
					  ref);
				ext4_xattr_block_csum_set(inode, new_bh);
				unlock_buffer(new_bh);
				error = ext4_handle_dirty_metadata(handle,
								   inode,
								   new_bh);
				if (error)
					goto cleanup_dquot;
			}
			mb_cache_entry_touch(ea_block_cache, ce);
			mb_cache_entry_put(ea_block_cache, ce);
			ce = NULL;
		} else if (bs->bh && s->base == bs->bh->b_data) {

			ea_bdebug(bs->bh, "keeping this block");
			ext4_xattr_block_cache_insert(ea_block_cache, bs->bh);
			new_bh = bs->bh;
			get_bh(new_bh);
		} else {

			ext4_fsblk_t goal, block;

#ifdef EXT4_XATTR_DEBUG
			WARN_ON_ONCE(dquot_initialize_needed(inode));
#endif
			goal = ext4_group_first_block_no(sb,
						EXT4_I(inode)->i_block_group);
			block = ext4_new_meta_blocks(handle, inode, goal, 0,
						     NULL, &error);
			if (error)
				goto cleanup;

			ea_idebug(inode, "creating block %llu",
				  (unsigned long long)block);

			new_bh = sb_getblk(sb, block);
			if (unlikely(!new_bh)) {
				error = -ENOMEM;
getblk_failed:
				ext4_free_blocks(handle, inode, NULL, block, 1,
						 EXT4_FREE_BLOCKS_METADATA);
				goto cleanup;
			}
			error = ext4_xattr_inode_inc_ref_all(handle, inode,
						      ENTRY(header(s->base)+1));
			if (error)
				goto getblk_failed;
			if (ea_inode) {

				error = ext4_xattr_inode_dec_ref(handle,
								 ea_inode);
				if (error)
					ext4_warning_inode(ea_inode,
							   "dec ref error=%d",
							   error);
				iput(ea_inode);
				ea_inode = NULL;
			}

			lock_buffer(new_bh);
			error = ext4_journal_get_create_access(handle, sb,
							new_bh, EXT4_JTR_NONE);
			if (error) {
				unlock_buffer(new_bh);
				error = -EIO;
				goto getblk_failed;
			}
			memcpy(new_bh->b_data, s->base, new_bh->b_size);
			ext4_xattr_block_csum_set(inode, new_bh);
			set_buffer_uptodate(new_bh);
			unlock_buffer(new_bh);
			ext4_xattr_block_cache_insert(ea_block_cache, new_bh);
			error = ext4_handle_dirty_metadata(handle, inode,
							   new_bh);
			if (error)
				goto cleanup;
		}
	}

	if (old_ea_inode_quota)
		ext4_xattr_inode_free_quota(inode, NULL, old_ea_inode_quota);


	EXT4_I(inode)->i_file_acl = new_bh ? new_bh->b_blocknr : 0;


	if (bs->bh && bs->bh != new_bh) {
		struct ext4_xattr_inode_array *ea_inode_array = NULL;

		ext4_xattr_release_block(handle, inode, bs->bh,
					 &ea_inode_array,
					 0                    );
		ext4_xattr_inode_array_free(ea_inode_array);
	}
	error = 0;

cleanup:
	if (ea_inode) {
		if (error) {
			int error2;

			error2 = ext4_xattr_inode_dec_ref(handle, ea_inode);
			if (error2)
				ext4_warning_inode(ea_inode, "dec ref error=%d",
						   error2);
			ext4_xattr_inode_free_quota(inode, ea_inode,
						    i_size_read(ea_inode));
		}
		iput(ea_inode);
	}
	if (ce)
		mb_cache_entry_put(ea_block_cache, ce);
	brelse(new_bh);
	if (!(bs->bh && s->base == bs->bh->b_data))
		kfree(s->base);

	return error;

cleanup_dquot:
	dquot_free_block(inode, EXT4_C2B(EXT4_SB(sb), 1));
	goto cleanup;

bad_block:
	EXT4_ERROR_INODE(inode, "bad block %llu",
			 EXT4_I(inode)->i_file_acl);
	goto cleanup;

#undef header
}


/**
 * ext4_xattr_ibody_find - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_xattr_ibody_find(struct inode *inode, struct ext4_xattr_info *i,
			  struct ext4_xattr_ibody_find *is)
{
	struct ext4_xattr_ibody_header *header;
	struct ext4_inode *raw_inode;
	int error;

	if (!EXT4_INODE_HAS_XATTR_SPACE(inode))
		return 0;

	raw_inode = ext4_raw_inode(&is->iloc);
	header = IHDR(inode, raw_inode);
	is->s.base = is->s.first = IFIRST(header);
	is->s.here = is->s.first;
	is->s.end = ITAIL(inode, raw_inode);
	if (ext4_test_inode_state(inode, EXT4_STATE_XATTR)) {

		error = xattr_find_entry(inode, &is->s.here, is->s.end,
					 i->name_index, i->name, 0);
		if (error && error != -ENODATA)
			return error;
		is->s.not_found = error;
	}
	return 0;
}


/**
 * ext4_xattr_ibody_set - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_xattr_ibody_set(handle_t *handle, struct inode *inode,
				struct ext4_xattr_info *i,
				struct ext4_xattr_ibody_find *is)
{
	struct ext4_xattr_ibody_header *header;
	struct ext4_xattr_search *s = &is->s;
	struct inode *ea_inode = NULL;
	int error;

	if (!EXT4_INODE_HAS_XATTR_SPACE(inode))
		return -ENOSPC;


	if (i->value && i->in_inode) {
		WARN_ON_ONCE(!i->value_len);

		ea_inode = ext4_xattr_inode_lookup_create(handle, inode,
					i->value, i->value_len);
		if (IS_ERR(ea_inode))
			return PTR_ERR(ea_inode);
	}
	error = ext4_xattr_set_entry(i, s, handle, inode, ea_inode,
				     false               );
	if (error) {
		if (ea_inode) {
			int error2;

			error2 = ext4_xattr_inode_dec_ref(handle, ea_inode);
			if (error2)
				ext4_warning_inode(ea_inode, "dec ref error=%d",
						   error2);

			ext4_xattr_inode_free_quota(inode, ea_inode,
						    i_size_read(ea_inode));
			iput(ea_inode);
		}
		return error;
	}
	header = IHDR(inode, ext4_raw_inode(&is->iloc));
	if (!IS_LAST_ENTRY(s->first)) {
		header->h_magic = cpu_to_le32(EXT4_XATTR_MAGIC);
		ext4_set_inode_state(inode, EXT4_STATE_XATTR);
	} else {
		header->h_magic = cpu_to_le32(0);
		ext4_clear_inode_state(inode, EXT4_STATE_XATTR);
	}
	iput(ea_inode);
	return 0;
}


/**
 * ext4_xattr_value_same - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_value_same(struct ext4_xattr_search *s,
				 struct ext4_xattr_info *i)
{
	void *value;


	if (s->here->e_value_inum)
		return 0;
	if (le32_to_cpu(s->here->e_value_size) != i->value_len)
		return 0;
	value = ((void *)s->base) + le16_to_cpu(s->here->e_value_offs);
	return !memcmp(value, i->value, i->value_len);
}


/**
 * ext4_xattr_get_block - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct buffer_head *ext4_xattr_get_block(struct inode *inode)
{
	struct buffer_head *bh;
	int error;

	if (!EXT4_I(inode)->i_file_acl)
		return NULL;
	bh = ext4_sb_bread(inode->i_sb, EXT4_I(inode)->i_file_acl, REQ_PRIO);
	if (IS_ERR(bh))
		return bh;
	error = ext4_xattr_check_block(inode, bh);
	if (error) {
		brelse(bh);
		return ERR_PTR(error);
	}
	return bh;
}


/**
 * ext4_xattr_set_handle - Coordinates a journal transaction or journal-owned buffer/state transition.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int
ext4_xattr_set_handle(handle_t *handle, struct inode *inode, int name_index,
		      const char *name, const void *value, size_t value_len,
		      int flags)
{
	struct ext4_xattr_info i = {
		.name_index = name_index,
		.name = name,
		.value = value,
		.value_len = value_len,
		.in_inode = 0,
	};
	struct ext4_xattr_ibody_find is = {
		.s = { .not_found = -ENODATA, },
	};
	struct ext4_xattr_block_find bs = {
		.s = { .not_found = -ENODATA, },
	};
	int no_expand;
	int error;

	if (!name)
		return -EINVAL;
	if (strlen(name) > 255)
		return -ERANGE;

	ext4_write_lock_xattr(inode, &no_expand);


	if (ext4_handle_valid(handle)) {
		struct buffer_head *bh;
		int credits;

		bh = ext4_xattr_get_block(inode);
		if (IS_ERR(bh)) {
			error = PTR_ERR(bh);
			goto cleanup;
		}

		credits = __ext4_xattr_set_credits(inode->i_sb, inode, bh,
						   value_len,
						   flags & XATTR_CREATE);
		brelse(bh);

		if (jbd2_handle_buffer_credits(handle) < credits) {
			error = -ENOSPC;
			goto cleanup;
		}
		WARN_ON_ONCE(!(current->flags & PF_MEMALLOC_NOFS));
	}

	error = ext4_reserve_inode_write(handle, inode, &is.iloc);
	if (error)
		goto cleanup;

	if (ext4_test_inode_state(inode, EXT4_STATE_NEW)) {
		struct ext4_inode *raw_inode = ext4_raw_inode(&is.iloc);
		memset(raw_inode, 0, EXT4_SB(inode->i_sb)->s_inode_size);
		ext4_clear_inode_state(inode, EXT4_STATE_NEW);
	}

	error = ext4_xattr_ibody_find(inode, &i, &is);
	if (error)
		goto cleanup;
	if (is.s.not_found)
		error = ext4_xattr_block_find(inode, &i, &bs);
	if (error)
		goto cleanup;
	if (is.s.not_found && bs.s.not_found) {
		error = -ENODATA;
		if (flags & XATTR_REPLACE)
			goto cleanup;
		error = 0;
		if (!value)
			goto cleanup;
	} else {
		error = -EEXIST;
		if (flags & XATTR_CREATE)
			goto cleanup;
	}

	if (!value) {
		if (!is.s.not_found)
			error = ext4_xattr_ibody_set(handle, inode, &i, &is);
		else if (!bs.s.not_found)
			error = ext4_xattr_block_set(handle, inode, &i, &bs);
	} else {
		error = 0;

		if (!is.s.not_found && ext4_xattr_value_same(&is.s, &i))
			goto cleanup;
		if (!bs.s.not_found && ext4_xattr_value_same(&bs.s, &i))
			goto cleanup;

		if (ext4_has_feature_ea_inode(inode->i_sb) &&
		    (EXT4_XATTR_SIZE(i.value_len) >
			EXT4_XATTR_MIN_LARGE_EA_SIZE(inode->i_sb->s_blocksize)))
			i.in_inode = 1;
retry_inode:
		error = ext4_xattr_ibody_set(handle, inode, &i, &is);
		if (!error && !bs.s.not_found) {
			i.value = NULL;
			error = ext4_xattr_block_set(handle, inode, &i, &bs);
		} else if (error == -ENOSPC) {
			if (EXT4_I(inode)->i_file_acl && !bs.s.base) {
				brelse(bs.bh);
				bs.bh = NULL;
				error = ext4_xattr_block_find(inode, &i, &bs);
				if (error)
					goto cleanup;
			}
			error = ext4_xattr_block_set(handle, inode, &i, &bs);
			if (!error && !is.s.not_found) {
				i.value = NULL;
				error = ext4_xattr_ibody_set(handle, inode, &i,
							     &is);
			} else if (error == -ENOSPC) {


				if (ext4_has_feature_ea_inode(inode->i_sb) &&
				    i.value_len && !i.in_inode) {
					i.in_inode = 1;
					goto retry_inode;
				}
			}
		}
	}
	if (!error) {
		ext4_xattr_update_super_block(handle, inode->i_sb);
		inode_set_ctime_current(inode);
		inode_inc_iversion(inode);
		if (!value)
			no_expand = 0;
		error = ext4_mark_iloc_dirty(handle, inode, &is.iloc);


		is.iloc.bh = NULL;
		if (IS_SYNC(inode))
			ext4_handle_sync(handle);
	}
	ext4_fc_mark_ineligible(inode->i_sb, EXT4_FC_REASON_XATTR, handle);

cleanup:
	brelse(is.iloc.bh);
	brelse(bs.bh);
	ext4_write_unlock_xattr(inode, &no_expand);
	return error;
}


/**
 * ext4_xattr_set_credits - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_xattr_set_credits(struct inode *inode, size_t value_len,
			   bool is_create, int *credits)
{
	struct buffer_head *bh;
	int err;

	*credits = 0;

	if (!EXT4_SB(inode->i_sb)->s_journal)
		return 0;

	down_read(&EXT4_I(inode)->xattr_sem);

	bh = ext4_xattr_get_block(inode);
	if (IS_ERR(bh)) {
		err = PTR_ERR(bh);
	} else {
		*credits = __ext4_xattr_set_credits(inode->i_sb, inode, bh,
						    value_len, is_create);
		brelse(bh);
		err = 0;
	}

	up_read(&EXT4_I(inode)->xattr_sem);
	return err;
}


/**
 * ext4_xattr_set - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int
ext4_xattr_set(struct inode *inode, int name_index, const char *name,
	       const void *value, size_t value_len, int flags)
{
	handle_t *handle;
	struct super_block *sb = inode->i_sb;
	int error, retries = 0;
	int credits;

	error = dquot_initialize(inode);
	if (error)
		return error;

retry:
	error = ext4_xattr_set_credits(inode, value_len, flags & XATTR_CREATE,
				       &credits);
	if (error)
		return error;

	handle = ext4_journal_start(inode, EXT4_HT_XATTR, credits);
	if (IS_ERR(handle)) {
		error = PTR_ERR(handle);
	} else {
		int error2;

		error = ext4_xattr_set_handle(handle, inode, name_index, name,
					      value, value_len, flags);
		ext4_fc_mark_ineligible(inode->i_sb, EXT4_FC_REASON_XATTR,
					handle);
		error2 = ext4_journal_stop(handle);
		if (error == -ENOSPC &&
		    ext4_should_retry_alloc(sb, &retries))
			goto retry;
		if (error == 0)
			error = error2;
	}

	return error;
}


/**
 * ext4_xattr_shift_entries - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_xattr_shift_entries(struct ext4_xattr_entry *entry,
				     int value_offs_shift, void *to,
				     void *from, size_t n)
{
	struct ext4_xattr_entry *last = entry;
	int new_offs;


	BUG_ON(value_offs_shift > 0);


	for (; !IS_LAST_ENTRY(last); last = EXT4_XATTR_NEXT(last)) {
		if (!last->e_value_inum && last->e_value_size) {
			new_offs = le16_to_cpu(last->e_value_offs) +
							value_offs_shift;
			last->e_value_offs = cpu_to_le16(new_offs);
		}
	}

	memmove(to, from, n);
}


/**
 * ext4_xattr_move_to_block - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_move_to_block(handle_t *handle, struct inode *inode,
				    struct ext4_inode *raw_inode,
				    struct ext4_xattr_entry *entry)
{
	struct ext4_xattr_ibody_find *is = NULL;
	struct ext4_xattr_block_find *bs = NULL;
	char *buffer = NULL, *b_entry_name = NULL;
	size_t value_size = le32_to_cpu(entry->e_value_size);
	struct ext4_xattr_info i = {
		.value = NULL,
		.value_len = 0,
		.name_index = entry->e_name_index,
		.in_inode = !!entry->e_value_inum,
	};
	struct ext4_xattr_ibody_header *header = IHDR(inode, raw_inode);
	int needs_kvfree = 0;
	int error;

	is = kzalloc(sizeof(struct ext4_xattr_ibody_find), GFP_NOFS);
	bs = kzalloc(sizeof(struct ext4_xattr_block_find), GFP_NOFS);
	b_entry_name = kmalloc(entry->e_name_len + 1, GFP_NOFS);
	if (!is || !bs || !b_entry_name) {
		error = -ENOMEM;
		goto out;
	}

	is->s.not_found = -ENODATA;
	bs->s.not_found = -ENODATA;
	is->iloc.bh = NULL;
	bs->bh = NULL;


	if (entry->e_value_inum) {
		buffer = kvmalloc(value_size, GFP_NOFS);
		if (!buffer) {
			error = -ENOMEM;
			goto out;
		}
		needs_kvfree = 1;
		error = ext4_xattr_inode_get(inode, entry, buffer, value_size);
		if (error)
			goto out;
	} else {
		size_t value_offs = le16_to_cpu(entry->e_value_offs);
		buffer = (void *)IFIRST(header) + value_offs;
	}

	memcpy(b_entry_name, entry->e_name, entry->e_name_len);
	b_entry_name[entry->e_name_len] = '\0';
	i.name = b_entry_name;

	error = ext4_get_inode_loc(inode, &is->iloc);
	if (error)
		goto out;

	error = ext4_xattr_ibody_find(inode, &i, is);
	if (error)
		goto out;

	i.value = buffer;
	i.value_len = value_size;
	error = ext4_xattr_block_find(inode, &i, bs);
	if (error)
		goto out;


	error = ext4_xattr_block_set(handle, inode, &i, bs);
	if (error)
		goto out;


	i.value = NULL;
	i.value_len = 0;
	error = ext4_xattr_ibody_set(handle, inode, &i, is);

out:
	kfree(b_entry_name);
	if (needs_kvfree && buffer)
		kvfree(buffer);
	if (is)
		brelse(is->iloc.bh);
	if (bs)
		brelse(bs->bh);
	kfree(is);
	kfree(bs);

	return error;
}


/**
 * ext4_xattr_make_inode_space - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_xattr_make_inode_space(handle_t *handle, struct inode *inode,
				       struct ext4_inode *raw_inode,
				       int isize_diff, size_t ifree,
				       size_t bfree, int *total_ino)
{
	struct ext4_xattr_ibody_header *header = IHDR(inode, raw_inode);
	struct ext4_xattr_entry *small_entry;
	struct ext4_xattr_entry *entry;
	struct ext4_xattr_entry *last;
	unsigned int entry_size;
	unsigned int total_size;
	unsigned int min_total_size;
	int error;

	while (isize_diff > ifree) {
		entry = NULL;
		small_entry = NULL;
		min_total_size = ~0U;
		last = IFIRST(header);

		for (; !IS_LAST_ENTRY(last); last = EXT4_XATTR_NEXT(last)) {

			if ((last->e_name_len == 4) &&
			    (last->e_name_index == EXT4_XATTR_INDEX_SYSTEM) &&
			    !memcmp(last->e_name, "data", 4))
				continue;
			total_size = EXT4_XATTR_LEN(last->e_name_len);
			if (!last->e_value_inum)
				total_size += EXT4_XATTR_SIZE(
					       le32_to_cpu(last->e_value_size));
			if (total_size <= bfree &&
			    total_size < min_total_size) {
				if (total_size + ifree < isize_diff) {
					small_entry = last;
				} else {
					entry = last;
					min_total_size = total_size;
				}
			}
		}

		if (entry == NULL) {
			if (small_entry == NULL)
				return -ENOSPC;
			entry = small_entry;
		}

		entry_size = EXT4_XATTR_LEN(entry->e_name_len);
		total_size = entry_size;
		if (!entry->e_value_inum)
			total_size += EXT4_XATTR_SIZE(
					      le32_to_cpu(entry->e_value_size));
		error = ext4_xattr_move_to_block(handle, inode, raw_inode,
						 entry);
		if (error)
			return error;

		*total_ino -= entry_size;
		ifree += total_size;
		bfree -= total_size;
	}

	return 0;
}


/**
 * ext4_expand_extra_isize_ea - Implements the expand extra isize ea operation within the extended metadata subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_expand_extra_isize_ea(struct inode *inode, int new_extra_isize,
			       struct ext4_inode *raw_inode, handle_t *handle)
{
	struct ext4_xattr_ibody_header *header;
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	static unsigned int mnt_count;
	size_t min_offs;
	size_t ifree, bfree;
	int total_ino;
	void *base, *end;
	int error = 0, tried_min_extra_isize = 0;
	int s_min_extra_isize = le16_to_cpu(sbi->s_es->s_min_extra_isize);
	int isize_diff;

retry:
	isize_diff = new_extra_isize - EXT4_I(inode)->i_extra_isize;
	if (EXT4_I(inode)->i_extra_isize >= new_extra_isize)
		return 0;

	header = IHDR(inode, raw_inode);


	base = IFIRST(header);
	end = ITAIL(inode, raw_inode);
	min_offs = end - base;
	total_ino = sizeof(struct ext4_xattr_ibody_header) + sizeof(u32);

	ifree = ext4_xattr_free_space(base, &min_offs, base, &total_ino);
	if (ifree >= isize_diff)
		goto shift;


	if (EXT4_I(inode)->i_file_acl) {
		struct buffer_head *bh;

		bh = ext4_sb_bread(inode->i_sb, EXT4_I(inode)->i_file_acl, REQ_PRIO);
		if (IS_ERR(bh)) {
			error = PTR_ERR(bh);
			goto cleanup;
		}
		error = ext4_xattr_check_block(inode, bh);
		if (error) {
			brelse(bh);
			goto cleanup;
		}
		base = BHDR(bh);
		end = bh->b_data + bh->b_size;
		min_offs = end - base;
		bfree = ext4_xattr_free_space(BFIRST(bh), &min_offs, base,
					      NULL);
		brelse(bh);
		if (bfree + ifree < isize_diff) {
			if (!tried_min_extra_isize && s_min_extra_isize) {
				tried_min_extra_isize++;
				new_extra_isize = s_min_extra_isize;
				goto retry;
			}
			error = -ENOSPC;
			goto cleanup;
		}
	} else {
		bfree = inode->i_sb->s_blocksize;
	}

	error = ext4_xattr_make_inode_space(handle, inode, raw_inode,
					    isize_diff, ifree, bfree,
					    &total_ino);
	if (error) {
		if (error == -ENOSPC && !tried_min_extra_isize &&
		    s_min_extra_isize) {
			tried_min_extra_isize++;
			new_extra_isize = s_min_extra_isize;
			error = 0;
			goto retry;
		}
		goto cleanup;
	}
shift:

	ext4_xattr_shift_entries(IFIRST(header), EXT4_I(inode)->i_extra_isize
			- new_extra_isize, (void *)raw_inode +
			EXT4_GOOD_OLD_INODE_SIZE + new_extra_isize,
			(void *)header, total_ino);
	EXT4_I(inode)->i_extra_isize = new_extra_isize;

	if (ext4_has_inline_data(inode))
		error = ext4_find_inline_data_nolock(inode);

cleanup:
	if (error && (mnt_count != le16_to_cpu(sbi->s_es->s_mnt_count))) {
		ext4_warning(inode->i_sb, "Unable to expand inode %lu. Delete some EAs or run e2fsck.",
			     inode->i_ino);
		mnt_count = le16_to_cpu(sbi->s_es->s_mnt_count);
	}
	return error;
}

#define EIA_INCR 16
#define EIA_MASK (EIA_INCR - 1)


/**
 * ext4_expand_inode_array - Implements an inode operation at the boundary between VFS state and the filesystem's persistent representation.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_expand_inode_array(struct ext4_xattr_inode_array **ea_inode_array,
			struct inode *inode)
{
	if (*ea_inode_array == NULL) {


		(*ea_inode_array) = kmalloc(
			struct_size(*ea_inode_array, inodes, EIA_MASK),
			GFP_NOFS);
		if (*ea_inode_array == NULL)
			return -ENOMEM;
		(*ea_inode_array)->count = 0;
	} else if (((*ea_inode_array)->count & EIA_MASK) == EIA_MASK) {

		struct ext4_xattr_inode_array *new_array = NULL;

		new_array = kmalloc(
			struct_size(*ea_inode_array, inodes,
				    (*ea_inode_array)->count + EIA_INCR),
			GFP_NOFS);
		if (new_array == NULL)
			return -ENOMEM;
		memcpy(new_array, *ea_inode_array,
		       struct_size(*ea_inode_array, inodes,
				   (*ea_inode_array)->count));
		kfree(*ea_inode_array);
		*ea_inode_array = new_array;
	}
	(*ea_inode_array)->count++;
	(*ea_inode_array)->inodes[(*ea_inode_array)->count - 1] = inode;
	return 0;
}


/**
 * ext4_xattr_delete_inode - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_xattr_delete_inode(handle_t *handle, struct inode *inode,
			    struct ext4_xattr_inode_array **ea_inode_array,
			    int extra_credits)
{
	struct buffer_head *bh = NULL;
	struct ext4_xattr_ibody_header *header;
	struct ext4_iloc iloc = { .bh = NULL };
	struct ext4_xattr_entry *entry;
	struct inode *ea_inode;
	int error;

	error = ext4_journal_ensure_credits(handle, extra_credits,
			ext4_free_metadata_revoke_credits(inode->i_sb, 1));
	if (error < 0) {
		EXT4_ERROR_INODE(inode, "ensure credits (error %d)", error);
		goto cleanup;
	}

	if (ext4_has_feature_ea_inode(inode->i_sb) &&
	    ext4_test_inode_state(inode, EXT4_STATE_XATTR)) {

		error = ext4_get_inode_loc(inode, &iloc);
		if (error) {
			EXT4_ERROR_INODE(inode, "inode loc (error %d)", error);
			goto cleanup;
		}

		error = ext4_journal_get_write_access(handle, inode->i_sb,
						iloc.bh, EXT4_JTR_NONE);
		if (error) {
			EXT4_ERROR_INODE(inode, "write access (error %d)",
					 error);
			goto cleanup;
		}

		header = IHDR(inode, ext4_raw_inode(&iloc));
		if (header->h_magic == cpu_to_le32(EXT4_XATTR_MAGIC))
			ext4_xattr_inode_dec_ref_all(handle, inode, iloc.bh,
						     IFIRST(header),
						     false                 ,
						     ea_inode_array,
						     extra_credits,
						     false                 );
	}

	if (EXT4_I(inode)->i_file_acl) {
		bh = ext4_sb_bread(inode->i_sb, EXT4_I(inode)->i_file_acl, REQ_PRIO);
		if (IS_ERR(bh)) {
			error = PTR_ERR(bh);
			if (error == -EIO) {
				EXT4_ERROR_INODE_ERR(inode, EIO,
						     "block %llu read error",
						     EXT4_I(inode)->i_file_acl);
			}
			bh = NULL;
			goto cleanup;
		}
		error = ext4_xattr_check_block(inode, bh);
		if (error)
			goto cleanup;

		if (ext4_has_feature_ea_inode(inode->i_sb)) {
			for (entry = BFIRST(bh); !IS_LAST_ENTRY(entry);
			     entry = EXT4_XATTR_NEXT(entry)) {
				if (!entry->e_value_inum)
					continue;
				error = ext4_xattr_inode_iget(inode,
					      le32_to_cpu(entry->e_value_inum),
					      le32_to_cpu(entry->e_hash),
					      &ea_inode);
				if (error)
					continue;
				ext4_xattr_inode_free_quota(inode, ea_inode,
					      le32_to_cpu(entry->e_value_size));
				iput(ea_inode);
			}

		}

		ext4_xattr_release_block(handle, inode, bh, ea_inode_array,
					 extra_credits);


		EXT4_I(inode)->i_file_acl = 0;
		error = ext4_mark_inode_dirty(handle, inode);
		if (error) {
			EXT4_ERROR_INODE(inode, "mark inode dirty (error %d)",
					 error);
			goto cleanup;
		}
		ext4_fc_mark_ineligible(inode->i_sb, EXT4_FC_REASON_XATTR, handle);
	}
	error = 0;
cleanup:
	brelse(iloc.bh);
	brelse(bh);
	return error;
}


/**
 * ext4_xattr_inode_array_free - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_xattr_inode_array_free(struct ext4_xattr_inode_array *ea_inode_array)
{
	int idx;

	if (ea_inode_array == NULL)
		return;

	for (idx = 0; idx < ea_inode_array->count; ++idx)
		iput(ea_inode_array->inodes[idx]);
	kfree(ea_inode_array);
}


/**
 * ext4_xattr_block_cache_insert - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void
ext4_xattr_block_cache_insert(struct mb_cache *ea_block_cache,
			      struct buffer_head *bh)
{
	struct ext4_xattr_header *header = BHDR(bh);
	__u32 hash = le32_to_cpu(header->h_hash);
	int reusable = le32_to_cpu(header->h_refcount) <
		       EXT4_XATTR_REFCOUNT_MAX;
	int error;

	if (!ea_block_cache)
		return;
	error = mb_cache_entry_create(ea_block_cache, GFP_NOFS, hash,
				      bh->b_blocknr, reusable);
	if (error) {
		if (error == -EBUSY)
			ea_bdebug(bh, "already in cache");
	} else
		ea_bdebug(bh, "inserting [%x]", (int)hash);
}


/**
 * ext4_xattr_cmp - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_cmp(struct ext4_xattr_header *header1,
	       struct ext4_xattr_header *header2)
{
	struct ext4_xattr_entry *entry1, *entry2;

	entry1 = ENTRY(header1+1);
	entry2 = ENTRY(header2+1);
	while (!IS_LAST_ENTRY(entry1)) {
		if (IS_LAST_ENTRY(entry2))
			return 1;
		if (entry1->e_hash != entry2->e_hash ||
		    entry1->e_name_index != entry2->e_name_index ||
		    entry1->e_name_len != entry2->e_name_len ||
		    entry1->e_value_size != entry2->e_value_size ||
		    entry1->e_value_inum != entry2->e_value_inum ||
		    memcmp(entry1->e_name, entry2->e_name, entry1->e_name_len))
			return 1;
		if (!entry1->e_value_inum &&
		    memcmp((char *)header1 + le16_to_cpu(entry1->e_value_offs),
			   (char *)header2 + le16_to_cpu(entry2->e_value_offs),
			   le32_to_cpu(entry1->e_value_size)))
			return 1;

		entry1 = EXT4_XATTR_NEXT(entry1);
		entry2 = EXT4_XATTR_NEXT(entry2);
	}
	if (!IS_LAST_ENTRY(entry2))
		return 1;
	return 0;
}


/**
 * ext4_xattr_block_cache_find - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct buffer_head *
ext4_xattr_block_cache_find(struct inode *inode,
			    struct ext4_xattr_header *header,
			    struct mb_cache_entry **pce)
{
	__u32 hash = le32_to_cpu(header->h_hash);
	struct mb_cache_entry *ce;
	struct mb_cache *ea_block_cache = EA_BLOCK_CACHE(inode);

	if (!ea_block_cache)
		return NULL;
	if (!header->h_hash)
		return NULL;
	ea_idebug(inode, "looking for cached blocks [%x]", (int)hash);
	ce = mb_cache_entry_find_first(ea_block_cache, hash);
	while (ce) {
		struct buffer_head *bh;

		bh = ext4_sb_bread(inode->i_sb, ce->e_value, REQ_PRIO);
		if (IS_ERR(bh)) {
			if (PTR_ERR(bh) != -ENOMEM)
				EXT4_ERROR_INODE(inode, "block %lu read error",
						 (unsigned long)ce->e_value);
			mb_cache_entry_put(ea_block_cache, ce);
			return bh;
		} else if (ext4_xattr_cmp(header, BHDR(bh)) == 0) {
			*pce = ce;
			return bh;
		}
		brelse(bh);
		ce = mb_cache_entry_find_next(ea_block_cache, ce);
	}
	return NULL;
}

#define NAME_HASH_SHIFT 5
#define VALUE_HASH_SHIFT 16


/**
 * ext4_xattr_hash_entry - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static __le32 ext4_xattr_hash_entry(char *name, size_t name_len, __le32 *value,
				    size_t value_count)
{
	__u32 hash = 0;

	while (name_len--) {
		hash = (hash << NAME_HASH_SHIFT) ^
		       (hash >> (8*sizeof(hash) - NAME_HASH_SHIFT)) ^
		       (unsigned char)*name++;
	}
	while (value_count--) {
		hash = (hash << VALUE_HASH_SHIFT) ^
		       (hash >> (8*sizeof(hash) - VALUE_HASH_SHIFT)) ^
		       le32_to_cpu(*value++);
	}
	return cpu_to_le32(hash);
}


/**
 * ext4_xattr_hash_entry_signed - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static __le32 ext4_xattr_hash_entry_signed(char *name, size_t name_len, __le32 *value, size_t value_count)
{
	__u32 hash = 0;

	while (name_len--) {
		hash = (hash << NAME_HASH_SHIFT) ^
		       (hash >> (8*sizeof(hash) - NAME_HASH_SHIFT)) ^
		       (signed char)*name++;
	}
	while (value_count--) {
		hash = (hash << VALUE_HASH_SHIFT) ^
		       (hash >> (8*sizeof(hash) - VALUE_HASH_SHIFT)) ^
		       le32_to_cpu(*value++);
	}
	return cpu_to_le32(hash);
}

#undef NAME_HASH_SHIFT
#undef VALUE_HASH_SHIFT

#define BLOCK_HASH_SHIFT 16


/**
 * ext4_xattr_rehash - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_xattr_rehash(struct ext4_xattr_header *header)
{
	struct ext4_xattr_entry *here;
	__u32 hash = 0;

	here = ENTRY(header+1);
	while (!IS_LAST_ENTRY(here)) {
		if (!here->e_hash) {

			hash = 0;
			break;
		}
		hash = (hash << BLOCK_HASH_SHIFT) ^
		       (hash >> (8*sizeof(hash) - BLOCK_HASH_SHIFT)) ^
		       le32_to_cpu(here->e_hash);
		here = EXT4_XATTR_NEXT(here);
	}
	header->h_hash = cpu_to_le32(hash);
}

#undef BLOCK_HASH_SHIFT

#define	HASH_BUCKET_BITS	10


/**
 * ext4_xattr_create_cache - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct mb_cache *
ext4_xattr_create_cache(void)
{
	return mb_cache_create(HASH_BUCKET_BITS);
}


/**
 * ext4_xattr_destroy_cache - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_xattr_destroy_cache(struct mb_cache *cache)
{
	if (cache)
		mb_cache_destroy(cache);
}


/**
 * ext4_xattr_hurd_list - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool
ext4_xattr_hurd_list(struct dentry *dentry)
{
	return test_opt(dentry->d_sb, XATTR_USER);
}


/**
 * ext4_xattr_hurd_get - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_hurd_get(const struct xattr_handler *handler,
		    struct dentry *unused, struct inode *inode,
		    const char *name, void *buffer, size_t size)
{
	if (!test_opt(inode->i_sb, XATTR_USER))
		return -EOPNOTSUPP;

	return ext4_xattr_get(inode, EXT4_XATTR_INDEX_HURD,
			      name, buffer, size);
}


/**
 * ext4_xattr_hurd_set - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_hurd_set(const struct xattr_handler *handler,
		    struct mnt_idmap *idmap,
		    struct dentry *unused, struct inode *inode,
		    const char *name, const void *value,
		    size_t size, int flags)
{
	if (!test_opt(inode->i_sb, XATTR_USER))
		return -EOPNOTSUPP;

	return ext4_xattr_set(inode, EXT4_XATTR_INDEX_HURD,
			      name, value, size, flags);
}

const struct xattr_handler ext4_xattr_hurd_handler = {
	.prefix	= XATTR_HURD_PREFIX,
	.list	= ext4_xattr_hurd_list,
	.get	= ext4_xattr_hurd_get,
	.set	= ext4_xattr_hurd_set,
};


/**
 * ext4_xattr_trusted_list - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool
ext4_xattr_trusted_list(struct dentry *dentry)
{
	return capable(CAP_SYS_ADMIN);
}


/**
 * ext4_xattr_trusted_get - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_trusted_get(const struct xattr_handler *handler,
		       struct dentry *unused, struct inode *inode,
		       const char *name, void *buffer, size_t size)
{
	return ext4_xattr_get(inode, EXT4_XATTR_INDEX_TRUSTED,
			      name, buffer, size);
}


/**
 * ext4_xattr_trusted_set - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_trusted_set(const struct xattr_handler *handler,
		       struct mnt_idmap *idmap,
		       struct dentry *unused, struct inode *inode,
		       const char *name, const void *value,
		       size_t size, int flags)
{
	return ext4_xattr_set(inode, EXT4_XATTR_INDEX_TRUSTED,
			      name, value, size, flags);
}

const struct xattr_handler ext4_xattr_trusted_handler = {
	.prefix	= XATTR_TRUSTED_PREFIX,
	.list	= ext4_xattr_trusted_list,
	.get	= ext4_xattr_trusted_get,
	.set	= ext4_xattr_trusted_set,
};


/**
 * ext4_xattr_user_list - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool
ext4_xattr_user_list(struct dentry *dentry)
{
	return test_opt(dentry->d_sb, XATTR_USER);
}


/**
 * ext4_xattr_user_get - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_user_get(const struct xattr_handler *handler,
		    struct dentry *unused, struct inode *inode,
		    const char *name, void *buffer, size_t size)
{
	if (!test_opt(inode->i_sb, XATTR_USER))
		return -EOPNOTSUPP;
	return ext4_xattr_get(inode, EXT4_XATTR_INDEX_USER,
			      name, buffer, size);
}


/**
 * ext4_xattr_user_set - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_user_set(const struct xattr_handler *handler,
		    struct mnt_idmap *idmap,
		    struct dentry *unused, struct inode *inode,
		    const char *name, const void *value,
		    size_t size, int flags)
{
	if (!test_opt(inode->i_sb, XATTR_USER))
		return -EOPNOTSUPP;
	return ext4_xattr_set(inode, EXT4_XATTR_INDEX_USER,
			      name, value, size, flags);
}

const struct xattr_handler ext4_xattr_user_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.list	= ext4_xattr_user_list,
	.get	= ext4_xattr_user_get,
	.set	= ext4_xattr_user_set,
};

#ifdef CONFIG_EXT4_FS_SECURITY


/**
 * ext4_xattr_security_get - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_security_get(const struct xattr_handler *handler,
			struct dentry *unused, struct inode *inode,
			const char *name, void *buffer, size_t size)
{
	return ext4_xattr_get(inode, EXT4_XATTR_INDEX_SECURITY,
			      name, buffer, size);
}


/**
 * ext4_xattr_security_set - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_xattr_security_set(const struct xattr_handler *handler,
			struct mnt_idmap *idmap,
			struct dentry *unused, struct inode *inode,
			const char *name, const void *value,
			size_t size, int flags)
{
	return ext4_xattr_set(inode, EXT4_XATTR_INDEX_SECURITY,
			      name, value, size, flags);
}


/**
 * ext4_initxattrs - Implements the initxattrs operation within the extended metadata subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
ext4_initxattrs(struct inode *inode, const struct xattr *xattr_array,
		void *fs_info)
{
	const struct xattr *xattr;
	handle_t *handle = fs_info;
	int err = 0;

	for (xattr = xattr_array; xattr->name != NULL; xattr++) {
		err = ext4_xattr_set_handle(handle, inode,
					    EXT4_XATTR_INDEX_SECURITY,
					    xattr->name, xattr->value,
					    xattr->value_len, XATTR_CREATE);
		if (err < 0)
			break;
	}
	return err;
}


/**
 * ext4_init_security - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int
ext4_init_security(handle_t *handle, struct inode *inode, struct inode *dir,
		   const struct qstr *qstr)
{
	return security_inode_init_security(inode, dir, qstr,
					    &ext4_initxattrs, handle);
}

const struct xattr_handler ext4_xattr_security_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.get	= ext4_xattr_security_get,
	.set	= ext4_xattr_security_set,
};
#endif

#ifdef CONFIG_EXT4_FS_POSIX_ACL


/**
 * ext4_acl_from_disk - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct posix_acl *
ext4_acl_from_disk(const void *value, size_t size)
{
	const char *end = (char *)value + size;
	int n, count;
	struct posix_acl *acl;

	if (!value)
		return NULL;
	if (size < sizeof(ext4_acl_header))
		 return ERR_PTR(-EINVAL);
	if (((ext4_acl_header *)value)->a_version !=
	    cpu_to_le32(EXT4_ACL_VERSION))
		return ERR_PTR(-EINVAL);
	value = (char *)value + sizeof(ext4_acl_header);
	count = ext4_acl_count(size);
	if (count < 0)
		return ERR_PTR(-EINVAL);
	if (count == 0)
		return NULL;
	acl = posix_acl_alloc(count, GFP_NOFS);
	if (!acl)
		return ERR_PTR(-ENOMEM);
	for (n = 0; n < count; n++) {
		ext4_acl_entry *entry =
			(ext4_acl_entry *)value;
		if ((char *)value + sizeof(ext4_acl_entry_short) > end)
			goto fail;
		acl->a_entries[n].e_tag  = le16_to_cpu(entry->e_tag);
		acl->a_entries[n].e_perm = le16_to_cpu(entry->e_perm);

		switch (acl->a_entries[n].e_tag) {
		case ACL_USER_OBJ:
		case ACL_GROUP_OBJ:
		case ACL_MASK:
		case ACL_OTHER:
			value = (char *)value +
				sizeof(ext4_acl_entry_short);
			break;

		case ACL_USER:
			value = (char *)value + sizeof(ext4_acl_entry);
			if ((char *)value > end)
				goto fail;
			acl->a_entries[n].e_uid =
				make_kuid(&init_user_ns,
					  le32_to_cpu(entry->e_id));
			break;
		case ACL_GROUP:
			value = (char *)value + sizeof(ext4_acl_entry);
			if ((char *)value > end)
				goto fail;
			acl->a_entries[n].e_gid =
				make_kgid(&init_user_ns,
					  le32_to_cpu(entry->e_id));
			break;

		default:
			goto fail;
		}
	}
	if (value != end)
		goto fail;
	return acl;

fail:
	posix_acl_release(acl);
	return ERR_PTR(-EINVAL);
}


/**
 * ext4_acl_to_disk - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void *
ext4_acl_to_disk(const struct posix_acl *acl, size_t *size)
{
	ext4_acl_header *ext_acl;
	char *e;
	size_t n;

	*size = ext4_acl_size(acl->a_count);
	ext_acl = kmalloc(sizeof(ext4_acl_header) + acl->a_count *
			sizeof(ext4_acl_entry), GFP_NOFS);
	if (!ext_acl)
		return ERR_PTR(-ENOMEM);
	ext_acl->a_version = cpu_to_le32(EXT4_ACL_VERSION);
	e = (char *)ext_acl + sizeof(ext4_acl_header);
	for (n = 0; n < acl->a_count; n++) {
		const struct posix_acl_entry *acl_e = &acl->a_entries[n];
		ext4_acl_entry *entry = (ext4_acl_entry *)e;
		entry->e_tag  = cpu_to_le16(acl_e->e_tag);
		entry->e_perm = cpu_to_le16(acl_e->e_perm);
		switch (acl_e->e_tag) {
		case ACL_USER:
			entry->e_id = cpu_to_le32(
				from_kuid(&init_user_ns, acl_e->e_uid));
			e += sizeof(ext4_acl_entry);
			break;
		case ACL_GROUP:
			entry->e_id = cpu_to_le32(
				from_kgid(&init_user_ns, acl_e->e_gid));
			e += sizeof(ext4_acl_entry);
			break;

		case ACL_USER_OBJ:
		case ACL_GROUP_OBJ:
		case ACL_MASK:
		case ACL_OTHER:
			e += sizeof(ext4_acl_entry_short);
			break;

		default:
			goto fail;
		}
	}
	return (char *)ext_acl;

fail:
	kfree(ext_acl);
	return ERR_PTR(-EINVAL);
}


/**
 * ext4_get_acl - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct posix_acl *
ext4_get_acl(struct inode *inode, int type, bool rcu)
{
	int name_index;
	char *value = NULL;
	struct posix_acl *acl;
	int retval;

	if (rcu)
		return ERR_PTR(-ECHILD);

	switch (type) {
	case ACL_TYPE_ACCESS:
		name_index = EXT4_XATTR_INDEX_POSIX_ACL_ACCESS;
		break;
	case ACL_TYPE_DEFAULT:
		name_index = EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT;
		break;
	default:
		BUG();
	}
	retval = ext4_xattr_get(inode, name_index, "", NULL, 0);
	if (retval > 0) {
		value = kmalloc(retval, GFP_NOFS);
		if (!value)
			return ERR_PTR(-ENOMEM);
		retval = ext4_xattr_get(inode, name_index, "", value, retval);
	}
	if (retval > 0)
		acl = ext4_acl_from_disk(value, retval);
	else if (retval == -ENODATA || retval == -ENOSYS)
		acl = NULL;
	else
		acl = ERR_PTR(retval);
	kfree(value);

	return acl;
}


/**
 * __ext4_set_acl - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
__ext4_set_acl(handle_t *handle, struct inode *inode, int type,
	     struct posix_acl *acl, int xattr_flags)
{
	int name_index;
	void *value = NULL;
	size_t size = 0;
	int error;

	switch (type) {
	case ACL_TYPE_ACCESS:
		name_index = EXT4_XATTR_INDEX_POSIX_ACL_ACCESS;
		break;

	case ACL_TYPE_DEFAULT:
		name_index = EXT4_XATTR_INDEX_POSIX_ACL_DEFAULT;
		if (!S_ISDIR(inode->i_mode))
			return acl ? -EACCES : 0;
		break;

	default:
		return -EINVAL;
	}
	if (acl) {
		value = ext4_acl_to_disk(acl, &size);
		if (IS_ERR(value))
			return (int)PTR_ERR(value);
	}

	error = ext4_xattr_set_handle(handle, inode, name_index, "",
				      value, size, xattr_flags);

	kfree(value);
	if (!error)
		set_cached_acl(inode, type, acl);

	return error;
}


/**
 * ext4_set_acl - Implements an extended-metadata operation in the filesystem's xattr/ACL subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int
ext4_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
	     struct posix_acl *acl, int type)
{
	handle_t *handle;
	int error, credits, retries = 0;
	size_t acl_size = acl ? ext4_acl_size(acl->a_count) : 0;
	struct inode *inode = d_inode(dentry);
	umode_t mode = inode->i_mode;
	int update_mode = 0;

	error = dquot_initialize(inode);
	if (error)
		return error;
retry:
	error = ext4_xattr_set_credits(inode, acl_size, false                ,
				       &credits);
	if (error)
		return error;

	handle = ext4_journal_start(inode, EXT4_HT_XATTR, credits);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	if ((type == ACL_TYPE_ACCESS) && acl) {
		error = posix_acl_update_mode(idmap, inode, &mode, &acl);
		if (error)
			goto out_stop;
		if (mode != inode->i_mode)
			update_mode = 1;
	}

	error = __ext4_set_acl(handle, inode, type, acl, 0                  );
	if (!error && update_mode) {
		inode->i_mode = mode;
		inode_set_ctime_current(inode);
		error = ext4_mark_inode_dirty(handle, inode);
	}
out_stop:
	ext4_journal_stop(handle);
	if (error == -ENOSPC && ext4_should_retry_alloc(inode->i_sb, &retries))
		goto retry;
	return error;
}


/**
 * ext4_init_acl - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int
ext4_init_acl(handle_t *handle, struct inode *inode, struct inode *dir)
{
	struct posix_acl *default_acl, *acl;
	int error;

	error = posix_acl_create(dir, &inode->i_mode, &default_acl, &acl);
	if (error)
		return error;

	if (default_acl) {
		error = __ext4_set_acl(handle, inode, ACL_TYPE_DEFAULT,
				       default_acl, XATTR_CREATE);
		posix_acl_release(default_acl);
	} else {
		inode->i_default_acl = NULL;
	}
	if (acl) {
		if (!error)
			error = __ext4_set_acl(handle, inode, ACL_TYPE_ACCESS,
					       acl, XATTR_CREATE);
		posix_acl_release(acl);
	} else {
		inode->i_acl = NULL;
	}
	return error;
}
#endif


/**
 * struct mb_cache - Private EXT4 state/data structure used by extended metadata.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct mb_cache {

	struct hlist_bl_head	*c_hash;

	int			c_bucket_bits;

	unsigned long		c_max_entries;

	spinlock_t		c_list_lock;
	struct list_head	c_list;

	unsigned long		c_entry_count;
	struct shrinker		*c_shrink;

	struct work_struct	c_shrink_work;
};

static struct kmem_cache *mb_entry_cache;

static unsigned long mb_cache_shrink(struct mb_cache *cache,
				     unsigned long nr_to_scan);


/**
 * mb_cache_entry_head - Implements the mb cache entry head operation within the extended metadata subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline struct hlist_bl_head *mb_cache_entry_head(struct mb_cache *cache,
							u32 key)
{
	return &cache->c_hash[hash_32(key, cache->c_bucket_bits)];
}


#define SYNC_SHRINK_BATCH 64


/**
 * mb_cache_entry_create - Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int mb_cache_entry_create(struct mb_cache *cache, gfp_t mask, u32 key,
			  u64 value, bool reusable)
{
	struct mb_cache_entry *entry, *dup;
	struct hlist_bl_node *dup_node;
	struct hlist_bl_head *head;


	if (cache->c_entry_count >= cache->c_max_entries)
		schedule_work(&cache->c_shrink_work);

	if (cache->c_entry_count >= 2*cache->c_max_entries)
		mb_cache_shrink(cache, SYNC_SHRINK_BATCH);

	entry = kmem_cache_alloc(mb_entry_cache, mask);
	if (!entry)
		return -ENOMEM;

	INIT_LIST_HEAD(&entry->e_list);


	atomic_set(&entry->e_refcnt, 2);
	entry->e_key = key;
	entry->e_value = value;
	entry->e_flags = 0;
	if (reusable)
		set_bit(MBE_REUSABLE_B, &entry->e_flags);
	head = mb_cache_entry_head(cache, key);
	hlist_bl_lock(head);
	hlist_bl_for_each_entry(dup, dup_node, head, e_hash_list) {
		if (dup->e_key == key && dup->e_value == value) {
			hlist_bl_unlock(head);
			kmem_cache_free(mb_entry_cache, entry);
			return -EBUSY;
		}
	}
	hlist_bl_add_head(&entry->e_hash_list, head);
	hlist_bl_unlock(head);
	spin_lock(&cache->c_list_lock);
	list_add_tail(&entry->e_list, &cache->c_list);
	cache->c_entry_count++;
	spin_unlock(&cache->c_list_lock);
	mb_cache_entry_put(cache, entry);

	return 0;
}


/**
 * __mb_cache_entry_free - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void __mb_cache_entry_free(struct mb_cache *cache, struct mb_cache_entry *entry)
{
	struct hlist_bl_head *head;

	head = mb_cache_entry_head(cache, entry->e_key);
	hlist_bl_lock(head);
	hlist_bl_del(&entry->e_hash_list);
	hlist_bl_unlock(head);
	kmem_cache_free(mb_entry_cache, entry);
}


/**
 * mb_cache_entry_wait_unused - Implements the mb cache entry wait unused operation within the extended metadata subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void mb_cache_entry_wait_unused(struct mb_cache_entry *entry)
{
	wait_var_event(&entry->e_refcnt, atomic_read(&entry->e_refcnt) <= 2);
}


/**
 * __entry_find - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct mb_cache_entry *__entry_find(struct mb_cache *cache,
					   struct mb_cache_entry *entry,
					   u32 key)
{
	struct mb_cache_entry *old_entry = entry;
	struct hlist_bl_node *node;
	struct hlist_bl_head *head;

	head = mb_cache_entry_head(cache, key);
	hlist_bl_lock(head);
	if (entry && !hlist_bl_unhashed(&entry->e_hash_list))
		node = entry->e_hash_list.next;
	else
		node = hlist_bl_first(head);
	while (node) {
		entry = hlist_bl_entry(node, struct mb_cache_entry,
				       e_hash_list);
		if (entry->e_key == key &&
		    test_bit(MBE_REUSABLE_B, &entry->e_flags) &&
		    atomic_inc_not_zero(&entry->e_refcnt))
			goto out;
		node = node->next;
	}
	entry = NULL;
out:
	hlist_bl_unlock(head);
	if (old_entry)
		mb_cache_entry_put(cache, old_entry);

	return entry;
}


/**
 * mb_cache_entry_find_first - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct mb_cache_entry *mb_cache_entry_find_first(struct mb_cache *cache,
						 u32 key)
{
	return __entry_find(cache, NULL, key);
}


/**
 * mb_cache_entry_find_next - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct mb_cache_entry *mb_cache_entry_find_next(struct mb_cache *cache,
						struct mb_cache_entry *entry)
{
	return __entry_find(cache, entry, entry->e_key);
}


/**
 * mb_cache_entry_get - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct mb_cache_entry *mb_cache_entry_get(struct mb_cache *cache, u32 key,
					  u64 value)
{
	struct hlist_bl_node *node;
	struct hlist_bl_head *head;
	struct mb_cache_entry *entry;

	head = mb_cache_entry_head(cache, key);
	hlist_bl_lock(head);
	hlist_bl_for_each_entry(entry, node, head, e_hash_list) {
		if (entry->e_key == key && entry->e_value == value &&
		    atomic_inc_not_zero(&entry->e_refcnt))
			goto out;
	}
	entry = NULL;
out:
	hlist_bl_unlock(head);
	return entry;
}


/**
 * mb_cache_entry_delete_or_get - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct mb_cache_entry *mb_cache_entry_delete_or_get(struct mb_cache *cache,
						    u32 key, u64 value)
{
	struct mb_cache_entry *entry;

	entry = mb_cache_entry_get(cache, key, value);
	if (!entry)
		return NULL;


	if (atomic_cmpxchg(&entry->e_refcnt, 2, 0) != 2)
		return entry;

	spin_lock(&cache->c_list_lock);
	if (!list_empty(&entry->e_list))
		list_del_init(&entry->e_list);
	cache->c_entry_count--;
	spin_unlock(&cache->c_list_lock);
	__mb_cache_entry_free(cache, entry);
	return NULL;
}


/**
 * mb_cache_entry_touch - Implements the mb cache entry touch operation within the extended metadata subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void mb_cache_entry_touch(struct mb_cache *cache,
			  struct mb_cache_entry *entry)
{
	set_bit(MBE_REFERENCED_B, &entry->e_flags);
}


/**
 * mb_cache_count - Computes derived filesystem state used for validation, accounting or policy decisions.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static unsigned long mb_cache_count(struct shrinker *shrink,
				    struct shrink_control *sc)
{
	struct mb_cache *cache = shrink->private_data;

	return cache->c_entry_count;
}


/**
 * mb_cache_shrink - Implements the mb cache shrink operation within the extended metadata subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static unsigned long mb_cache_shrink(struct mb_cache *cache,
				     unsigned long nr_to_scan)
{
	struct mb_cache_entry *entry;
	unsigned long shrunk = 0;

	spin_lock(&cache->c_list_lock);
	while (nr_to_scan-- && !list_empty(&cache->c_list)) {
		entry = list_first_entry(&cache->c_list,
					 struct mb_cache_entry, e_list);

		if (test_bit(MBE_REFERENCED_B, &entry->e_flags) ||
		    atomic_cmpxchg(&entry->e_refcnt, 1, 0) != 1) {
			clear_bit(MBE_REFERENCED_B, &entry->e_flags);
			list_move_tail(&entry->e_list, &cache->c_list);
			continue;
		}
		list_del_init(&entry->e_list);
		cache->c_entry_count--;
		spin_unlock(&cache->c_list_lock);
		__mb_cache_entry_free(cache, entry);
		shrunk++;
		cond_resched();
		spin_lock(&cache->c_list_lock);
	}
	spin_unlock(&cache->c_list_lock);

	return shrunk;
}


/**
 * mb_cache_scan - Implements the mb cache scan operation within the extended metadata subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static unsigned long mb_cache_scan(struct shrinker *shrink,
				   struct shrink_control *sc)
{
	struct mb_cache *cache = shrink->private_data;
	return mb_cache_shrink(cache, sc->nr_to_scan);
}


#define SHRINK_DIVISOR 16


/**
 * mb_cache_shrink_worker - Implements the mb cache shrink worker operation within the extended metadata subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void mb_cache_shrink_worker(struct work_struct *work)
{
	struct mb_cache *cache = container_of(work, struct mb_cache,
					      c_shrink_work);
	mb_cache_shrink(cache, cache->c_max_entries / SHRINK_DIVISOR);
}


/**
 * mb_cache_create - Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
struct mb_cache *mb_cache_create(int bucket_bits)
{
	struct mb_cache *cache;
	unsigned long bucket_count = 1UL << bucket_bits;
	unsigned long i;

	cache = kzalloc(sizeof(struct mb_cache), GFP_KERNEL);
	if (!cache)
		goto err_out;
	cache->c_bucket_bits = bucket_bits;
	cache->c_max_entries = bucket_count << 4;
	INIT_LIST_HEAD(&cache->c_list);
	spin_lock_init(&cache->c_list_lock);
	cache->c_hash = kmalloc_array(bucket_count,
				      sizeof(struct hlist_bl_head),
				      GFP_KERNEL);
	if (!cache->c_hash) {
		kfree(cache);
		goto err_out;
	}
	for (i = 0; i < bucket_count; i++)
		INIT_HLIST_BL_HEAD(&cache->c_hash[i]);

	cache->c_shrink = shrinker_alloc(0, "mbcache-shrinker");
	if (!cache->c_shrink) {
		kfree(cache->c_hash);
		kfree(cache);
		goto err_out;
	}

	cache->c_shrink->count_objects = mb_cache_count;
	cache->c_shrink->scan_objects = mb_cache_scan;
	cache->c_shrink->private_data = cache;

	shrinker_register(cache->c_shrink);

	INIT_WORK(&cache->c_shrink_work, mb_cache_shrink_worker);

	return cache;

err_out:
	return NULL;
}


/**
 * mb_cache_destroy - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void mb_cache_destroy(struct mb_cache *cache)
{
	struct mb_cache_entry *entry, *next;

	cancel_work_sync(&cache->c_shrink_work);
	shrinker_free(cache->c_shrink);


	list_for_each_entry_safe(entry, next, &cache->c_list, e_list) {
		list_del(&entry->e_list);
		WARN_ON(atomic_read(&entry->e_refcnt) != 1);
		mb_cache_entry_put(cache, entry);
	}
	kfree(cache->c_hash);
	kfree(cache);
}


/**
 * infiltratr_mbcache_init - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int __init infiltratr_mbcache_init(void)
{
	mb_entry_cache = KMEM_CACHE(mb_cache_entry, SLAB_RECLAIM_ACCOUNT);
	if (!mb_entry_cache)
		return -ENOMEM;
	return 0;
}


/**
 * infiltratr_mbcache_exit - Tears down subsystem state after users have been quiesced.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void __exit infiltratr_mbcache_exit(void)
{
	kmem_cache_destroy(mb_entry_cache);
}
