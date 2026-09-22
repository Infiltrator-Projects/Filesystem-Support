// SPDX-License-Identifier: GPL-2.0
#include <linux/capability.h>
#include <linux/list.h>
#include <linux/list_bl.h>
#include <linux/module.h>
#include <linux/posix_acl_xattr.h>
#include <linux/sched.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/workqueue.h>
/*
 * linux/fs/ext2/xattr.c
 *
 * Copyright (C) 2001-2003 Andreas Gruenbacher <agruen@suse.de>
 *
 * Fix by Harrison Xing <harrison@mountainviewdata.com>.
 * Extended attributes for symlinks and special files added per
 *  suggestion of Luka Renko <luka.renko@hermes.si>.
 * xattr consolidation Copyright (c) 2004 James Morris <jmorris@redhat.com>,
 *  Red Hat Inc.
 *
 */

/*
 * Extended attributes are stored on disk blocks allocated outside of
 * any inode. The i_file_acl field is then made to point to this allocated
 * block. If all extended attributes of an inode are identical, these
 * inodes may share the same extended attribute block. Such situations
 * are automatically detected by keeping a cache of recent attribute block
 * numbers and hashes over the block's contents in memory.
 *
 *
 * Extended attribute block layout:
 *
 *   +------------------+
 *   | header           |
 *   | entry 1          | |
 *   | entry 2          | | growing downwards
 *   | entry 3          | v
 *   | four null bytes  |
 *   | . . .            |
 *   | value 1          | ^
 *   | value 3          | | growing upwards
 *   | value 2          | |
 *   +------------------+
 *
 * The block header is followed by multiple entry descriptors. These entry
 * descriptors are variable in size, and aligned to EXT2_XATTR_PAD
 * byte boundaries. The entry descriptors are sorted by attribute name,
 * so that two extended attribute blocks can be compared efficiently.
 *
 * Attribute values are aligned to the end of the block, stored in
 * no specific order. They are also padded to EXT2_XATTR_PAD byte
 * boundaries. No additional gaps are left between them.
 *
 * Locking strategy
 * ----------------
 * EXT2_I(inode)->i_file_acl is protected by EXT2_I(inode)->xattr_sem.
 * EA blocks are only changed if they are exclusive to an inode, so
 * holding xattr_sem also means that nothing but the EA block's reference
 * count will change. Multiple writers to an EA block are synchronized
 * by the bh lock. No more than a single bh lock is held at any time
 * to avoid deadlocks.
 */

#include <linux/buffer_head.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/slab.h>
#include <linux/quotaops.h>
#include <linux/rwsem.h>
#include <linux/security.h>
#include "ext2.h"

#define HDR(bh) ((struct ext2_xattr_header *)((bh)->b_data))
#define ENTRY(ptr) ((struct ext2_xattr_entry *)(ptr))
#define FIRST_ENTRY(bh) ENTRY(HDR(bh)+1)
#define IS_LAST_ENTRY(entry) (*(__u32 *)(entry) == 0)

#ifdef EXT2_XATTR_DEBUG
# define ea_idebug(inode, f...) do { \
		printk(KERN_DEBUG "inode %s:%ld: ", \
			inode->i_sb->s_id, inode->i_ino); \
		printk(f); \
		printk("\n"); \
	} while (0)
# define ea_bdebug(bh, f...) do { \
		printk(KERN_DEBUG "block %pg:%lu: ", \
			bh->b_bdev, (unsigned long) bh->b_blocknr); \
		printk(f); \
		printk("\n"); \
	} while (0)
#else
# define ea_idebug(inode, f...)	no_printk(f)
# define ea_bdebug(bh, f...)	no_printk(f)
#endif

static int ext2_xattr_set2(struct inode *, struct buffer_head *,
			   struct ext2_xattr_header *);

static int ext2_xattr_cache_insert(struct mb_cache *, struct buffer_head *);
static struct buffer_head *ext2_xattr_cache_find(struct inode *,
						 struct ext2_xattr_header *);
static void ext2_xattr_rehash(struct ext2_xattr_header *,
			      struct ext2_xattr_entry *);

static const struct xattr_handler * const ext2_xattr_handler_map[] = {
	[EXT2_XATTR_INDEX_USER]		     = &ext2_xattr_user_handler,
#ifdef CONFIG_EXT2_FS_POSIX_ACL
	[EXT2_XATTR_INDEX_POSIX_ACL_ACCESS]  = &nop_posix_acl_access,
	[EXT2_XATTR_INDEX_POSIX_ACL_DEFAULT] = &nop_posix_acl_default,
#endif
	[EXT2_XATTR_INDEX_TRUSTED]	     = &ext2_xattr_trusted_handler,
#ifdef CONFIG_EXT2_FS_SECURITY
	[EXT2_XATTR_INDEX_SECURITY]	     = &ext2_xattr_security_handler,
#endif
};

const struct xattr_handler * const ext2_xattr_handlers[] = {
	&ext2_xattr_user_handler,
	&ext2_xattr_trusted_handler,
#ifdef CONFIG_EXT2_FS_SECURITY
	&ext2_xattr_security_handler,
#endif
	NULL
};

#define EA_BLOCK_CACHE(inode)	(EXT2_SB(inode->i_sb)->s_ea_block_cache)

static inline const char *ext2_xattr_prefix(int name_index,
					    struct dentry *dentry)
{
	const struct xattr_handler *handler = NULL;

	if (name_index > 0 && name_index < ARRAY_SIZE(ext2_xattr_handler_map))
		handler = ext2_xattr_handler_map[name_index];

	if (!xattr_handler_can_list(handler, dentry))
		return NULL;

	return xattr_prefix(handler);
}

static bool
ext2_xattr_header_valid(struct ext2_xattr_header *header)
{
	if (header->h_magic != cpu_to_le32(EXT2_XATTR_MAGIC) ||
	    header->h_blocks != cpu_to_le32(1))
		return false;

	return true;
}

static bool
ext2_xattr_entry_valid(struct ext2_xattr_entry *entry,
		       char *end, size_t end_offs)
{
	struct ext2_xattr_entry *next;
	size_t size;

	next = EXT2_XATTR_NEXT(entry);
	if ((char *)next >= end)
		return false;

	if (entry->e_value_block != 0)
		return false;

	size = le32_to_cpu(entry->e_value_size);
	if (size > end_offs ||
	    le16_to_cpu(entry->e_value_offs) + size > end_offs)
		return false;

	return true;
}

static int
ext2_xattr_cmp_entry(int name_index, size_t name_len, const char *name,
		     struct ext2_xattr_entry *entry)
{
	int cmp;

	cmp = name_index - entry->e_name_index;
	if (!cmp)
		cmp = name_len - entry->e_name_len;
	if (!cmp)
		cmp = memcmp(name, entry->e_name, name_len);

	return cmp;
}

/*
 * ext2_xattr_get()
 *
 * Copy an extended attribute into the buffer
 * provided, or compute the buffer size required.
 * Buffer is NULL to compute the size of the buffer required.
 *
 * Returns a negative error number on failure, or the number of bytes
 * used / required on success.
 */
int
ext2_xattr_get(struct inode *inode, int name_index, const char *name,
	       void *buffer, size_t buffer_size)
{
	struct buffer_head *bh = NULL;
	struct ext2_xattr_entry *entry;
	size_t name_len, size;
	char *end;
	int error, not_found;
	struct mb_cache *ea_block_cache = EA_BLOCK_CACHE(inode);

	ea_idebug(inode, "name=%d.%s, buffer=%p, buffer_size=%ld",
		  name_index, name, buffer, (long)buffer_size);

	if (name == NULL)
		return -EINVAL;
	name_len = strlen(name);
	if (name_len > 255)
		return -ERANGE;

	down_read(&EXT2_I(inode)->xattr_sem);
	error = -ENODATA;
	if (!EXT2_I(inode)->i_file_acl)
		goto cleanup;
	ea_idebug(inode, "reading block %d", EXT2_I(inode)->i_file_acl);
	bh = sb_bread(inode->i_sb, EXT2_I(inode)->i_file_acl);
	error = -EIO;
	if (!bh)
		goto cleanup;
	ea_bdebug(bh, "b_count=%d, refcount=%d",
		atomic_read(&(bh->b_count)), le32_to_cpu(HDR(bh)->h_refcount));
	end = bh->b_data + bh->b_size;
	if (!ext2_xattr_header_valid(HDR(bh))) {
bad_block:
		ext2_error(inode->i_sb, "ext2_xattr_get",
			"inode %ld: bad block %d", inode->i_ino,
			EXT2_I(inode)->i_file_acl);
		error = -EIO;
		goto cleanup;
	}

	/* find named attribute */
	entry = FIRST_ENTRY(bh);
	while (!IS_LAST_ENTRY(entry)) {
		if (!ext2_xattr_entry_valid(entry, end,
		    inode->i_sb->s_blocksize))
			goto bad_block;

		not_found = ext2_xattr_cmp_entry(name_index, name_len, name,
						 entry);
		if (!not_found)
			goto found;
		if (not_found < 0)
			break;

		entry = EXT2_XATTR_NEXT(entry);
	}
	if (ext2_xattr_cache_insert(ea_block_cache, bh))
		ea_idebug(inode, "cache insert failed");
	error = -ENODATA;
	goto cleanup;
found:
	size = le32_to_cpu(entry->e_value_size);
	if (ext2_xattr_cache_insert(ea_block_cache, bh))
		ea_idebug(inode, "cache insert failed");
	if (buffer) {
		error = -ERANGE;
		if (size > buffer_size)
			goto cleanup;
		/* return value of attribute */
		memcpy(buffer, bh->b_data + le16_to_cpu(entry->e_value_offs),
			size);
	}
	error = size;

cleanup:
	brelse(bh);
	up_read(&EXT2_I(inode)->xattr_sem);

	return error;
}

/*
 * ext2_xattr_list()
 *
 * Copy a list of attribute names into the buffer
 * provided, or compute the buffer size required.
 * Buffer is NULL to compute the size of the buffer required.
 *
 * Returns a negative error number on failure, or the number of bytes
 * used / required on success.
 */
static int
ext2_xattr_list(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	struct inode *inode = d_inode(dentry);
	struct buffer_head *bh = NULL;
	struct ext2_xattr_entry *entry;
	char *end;
	size_t rest = buffer_size;
	int error;
	struct mb_cache *ea_block_cache = EA_BLOCK_CACHE(inode);

	ea_idebug(inode, "buffer=%p, buffer_size=%ld",
		  buffer, (long)buffer_size);

	down_read(&EXT2_I(inode)->xattr_sem);
	error = 0;
	if (!EXT2_I(inode)->i_file_acl)
		goto cleanup;
	ea_idebug(inode, "reading block %d", EXT2_I(inode)->i_file_acl);
	bh = sb_bread(inode->i_sb, EXT2_I(inode)->i_file_acl);
	error = -EIO;
	if (!bh)
		goto cleanup;
	ea_bdebug(bh, "b_count=%d, refcount=%d",
		atomic_read(&(bh->b_count)), le32_to_cpu(HDR(bh)->h_refcount));
	end = bh->b_data + bh->b_size;
	if (!ext2_xattr_header_valid(HDR(bh))) {
bad_block:
		ext2_error(inode->i_sb, "ext2_xattr_list",
			"inode %ld: bad block %d", inode->i_ino,
			EXT2_I(inode)->i_file_acl);
		error = -EIO;
		goto cleanup;
	}

	/* check the on-disk data structure */
	entry = FIRST_ENTRY(bh);
	while (!IS_LAST_ENTRY(entry)) {
		if (!ext2_xattr_entry_valid(entry, end,
		    inode->i_sb->s_blocksize))
			goto bad_block;
		entry = EXT2_XATTR_NEXT(entry);
	}
	if (ext2_xattr_cache_insert(ea_block_cache, bh))
		ea_idebug(inode, "cache insert failed");

	/* list the attribute names */
	for (entry = FIRST_ENTRY(bh); !IS_LAST_ENTRY(entry);
	     entry = EXT2_XATTR_NEXT(entry)) {
		const char *prefix;

		prefix = ext2_xattr_prefix(entry->e_name_index, dentry);
		if (prefix) {
			size_t prefix_len = strlen(prefix);
			size_t size = prefix_len + entry->e_name_len + 1;

			if (buffer) {
				if (size > rest) {
					error = -ERANGE;
					goto cleanup;
				}
				memcpy(buffer, prefix, prefix_len);
				buffer += prefix_len;
				memcpy(buffer, entry->e_name, entry->e_name_len);
				buffer += entry->e_name_len;
				*buffer++ = 0;
			}
			rest -= size;
		}
	}
	error = buffer_size - rest;  /* total size */

cleanup:
	brelse(bh);
	up_read(&EXT2_I(inode)->xattr_sem);

	return error;
}

/*
 * Inode operation listxattr()
 *
 * d_inode(dentry)->i_mutex: don't care
 */
ssize_t
ext2_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	return ext2_xattr_list(dentry, buffer, size);
}

/*
 * If the EXT2_FEATURE_COMPAT_EXT_ATTR feature of this file system is
 * not set, set it.
 */
static void ext2_xattr_update_super_block(struct super_block *sb)
{
	if (EXT2_HAS_COMPAT_FEATURE(sb, EXT2_FEATURE_COMPAT_EXT_ATTR))
		return;

	spin_lock(&EXT2_SB(sb)->s_lock);
	ext2_update_dynamic_rev(sb);
	EXT2_SET_COMPAT_FEATURE(sb, EXT2_FEATURE_COMPAT_EXT_ATTR);
	spin_unlock(&EXT2_SB(sb)->s_lock);
	mark_buffer_dirty(EXT2_SB(sb)->s_sbh);
}

/*
 * ext2_xattr_set()
 *
 * Create, replace or remove an extended attribute for this inode.  Value
 * is NULL to remove an existing extended attribute, and non-NULL to
 * either replace an existing extended attribute, or create a new extended
 * attribute. The flags XATTR_REPLACE and XATTR_CREATE
 * specify that an extended attribute must exist and must not exist
 * previous to the call, respectively.
 *
 * Returns 0, or a negative error number on failure.
 */
int
ext2_xattr_set(struct inode *inode, int name_index, const char *name,
	       const void *value, size_t value_len, int flags)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh = NULL;
	struct ext2_xattr_header *header = NULL;
	struct ext2_xattr_entry *here = NULL, *last = NULL;
	size_t name_len, free, min_offs = sb->s_blocksize;
	int not_found = 1, error;
	char *end;
	
	/*
	 * header -- Points either into bh, or to a temporarily
	 *           allocated buffer.
	 * here -- The named entry found, or the place for inserting, within
	 *         the block pointed to by header.
	 * last -- Points right after the last named entry within the block
	 *         pointed to by header.
	 * min_offs -- The offset of the first value (values are aligned
	 *             towards the end of the block).
	 * end -- Points right after the block pointed to by header.
	 */
	
	ea_idebug(inode, "name=%d.%s, value=%p, value_len=%ld",
		  name_index, name, value, (long)value_len);

	if (value == NULL)
		value_len = 0;
	if (name == NULL)
		return -EINVAL;
	name_len = strlen(name);
	if (name_len > 255 || value_len > sb->s_blocksize)
		return -ERANGE;
	error = dquot_initialize(inode);
	if (error)
		return error;
	down_write(&EXT2_I(inode)->xattr_sem);
	if (EXT2_I(inode)->i_file_acl) {
		/* The inode already has an extended attribute block. */
		bh = sb_bread(sb, EXT2_I(inode)->i_file_acl);
		error = -EIO;
		if (!bh)
			goto cleanup;
		ea_bdebug(bh, "b_count=%d, refcount=%d",
			atomic_read(&(bh->b_count)),
			le32_to_cpu(HDR(bh)->h_refcount));
		header = HDR(bh);
		end = bh->b_data + bh->b_size;
		if (!ext2_xattr_header_valid(header)) {
bad_block:
			ext2_error(sb, "ext2_xattr_set",
				"inode %ld: bad block %d", inode->i_ino, 
				   EXT2_I(inode)->i_file_acl);
			error = -EIO;
			goto cleanup;
		}
		/*
		 * Find the named attribute. If not found, 'here' will point
		 * to entry where the new attribute should be inserted to
		 * maintain sorting.
		 */
		last = FIRST_ENTRY(bh);
		while (!IS_LAST_ENTRY(last)) {
			if (!ext2_xattr_entry_valid(last, end, sb->s_blocksize))
				goto bad_block;
			if (last->e_value_size) {
				size_t offs = le16_to_cpu(last->e_value_offs);
				if (offs < min_offs)
					min_offs = offs;
			}
			if (not_found > 0) {
				not_found = ext2_xattr_cmp_entry(name_index,
								 name_len,
								 name, last);
				if (not_found <= 0)
					here = last;
			}
			last = EXT2_XATTR_NEXT(last);
		}
		if (not_found > 0)
			here = last;

		/* Check whether we have enough space left. */
		free = min_offs - ((char*)last - (char*)header) - sizeof(__u32);
	} else {
		/* We will use a new extended attribute block. */
		free = sb->s_blocksize -
			sizeof(struct ext2_xattr_header) - sizeof(__u32);
	}

	if (not_found) {
		/* Request to remove a nonexistent attribute? */
		error = -ENODATA;
		if (flags & XATTR_REPLACE)
			goto cleanup;
		error = 0;
		if (value == NULL)
			goto cleanup;
	} else {
		/* Request to create an existing attribute? */
		error = -EEXIST;
		if (flags & XATTR_CREATE)
			goto cleanup;
		free += EXT2_XATTR_SIZE(le32_to_cpu(here->e_value_size));
		free += EXT2_XATTR_LEN(name_len);
	}
	error = -ENOSPC;
	if (free < EXT2_XATTR_LEN(name_len) + EXT2_XATTR_SIZE(value_len))
		goto cleanup;

	/* Here we know that we can set the new attribute. */

	if (header) {
		int offset;

		lock_buffer(bh);
		if (header->h_refcount == cpu_to_le32(1)) {
			__u32 hash = le32_to_cpu(header->h_hash);
			struct mb_cache_entry *oe;

			oe = mb_cache_entry_delete_or_get(EA_BLOCK_CACHE(inode),
					hash, bh->b_blocknr);
			if (!oe) {
				ea_bdebug(bh, "modifying in-place");
				goto update_block;
			}
			/*
			 * Someone is trying to reuse the block, leave it alone
			 */
			mb_cache_entry_put(EA_BLOCK_CACHE(inode), oe);
		}
		unlock_buffer(bh);
		ea_bdebug(bh, "cloning");
		header = kmemdup(HDR(bh), bh->b_size, GFP_KERNEL);
		error = -ENOMEM;
		if (header == NULL)
			goto cleanup;
		header->h_refcount = cpu_to_le32(1);

		offset = (char *)here - bh->b_data;
		here = ENTRY((char *)header + offset);
		offset = (char *)last - bh->b_data;
		last = ENTRY((char *)header + offset);
	} else {
		/* Allocate a buffer where we construct the new block. */
		header = kzalloc(sb->s_blocksize, GFP_KERNEL);
		error = -ENOMEM;
		if (header == NULL)
			goto cleanup;
		header->h_magic = cpu_to_le32(EXT2_XATTR_MAGIC);
		header->h_blocks = header->h_refcount = cpu_to_le32(1);
		last = here = ENTRY(header+1);
	}

update_block:
	/* Iff we are modifying the block in-place, bh is locked here. */

	if (not_found) {
		/* Insert the new name. */
		size_t size = EXT2_XATTR_LEN(name_len);
		size_t rest = (char *)last - (char *)here;
		memmove((char *)here + size, here, rest);
		memset(here, 0, size);
		here->e_name_index = name_index;
		here->e_name_len = name_len;
		memcpy(here->e_name, name, name_len);
	} else {
		if (here->e_value_size) {
			char *first_val = (char *)header + min_offs;
			size_t offs = le16_to_cpu(here->e_value_offs);
			char *val = (char *)header + offs;
			size_t size = EXT2_XATTR_SIZE(
				le32_to_cpu(here->e_value_size));

			if (size == EXT2_XATTR_SIZE(value_len)) {
				/* The old and the new value have the same
				   size. Just replace. */
				here->e_value_size = cpu_to_le32(value_len);
				memset(val + size - EXT2_XATTR_PAD, 0,
				       EXT2_XATTR_PAD); /* Clear pad bytes. */
				memcpy(val, value, value_len);
				goto skip_replace;
			}

			/* Remove the old value. */
			memmove(first_val + size, first_val, val - first_val);
			memset(first_val, 0, size);
			min_offs += size;

			/* Adjust all value offsets. */
			last = ENTRY(header+1);
			while (!IS_LAST_ENTRY(last)) {
				size_t o = le16_to_cpu(last->e_value_offs);
				if (o < offs)
					last->e_value_offs =
						cpu_to_le16(o + size);
				last = EXT2_XATTR_NEXT(last);
			}

			here->e_value_offs = 0;
		}
		if (value == NULL) {
			/* Remove the old name. */
			size_t size = EXT2_XATTR_LEN(name_len);
			last = ENTRY((char *)last - size);
			memmove(here, (char*)here + size,
				(char*)last - (char*)here);
			memset(last, 0, size);
		}
	}

	if (value != NULL) {
		/* Insert the new value. */
		here->e_value_size = cpu_to_le32(value_len);
		if (value_len) {
			size_t size = EXT2_XATTR_SIZE(value_len);
			char *val = (char *)header + min_offs - size;
			here->e_value_offs =
				cpu_to_le16((char *)val - (char *)header);
			memset(val + size - EXT2_XATTR_PAD, 0,
			       EXT2_XATTR_PAD); /* Clear the pad bytes. */
			memcpy(val, value, value_len);
		}
	}

skip_replace:
	if (IS_LAST_ENTRY(ENTRY(header+1))) {
		/* This block is now empty. */
		if (bh && header == HDR(bh))
			unlock_buffer(bh);  /* we were modifying in-place. */
		error = ext2_xattr_set2(inode, bh, NULL);
	} else {
		ext2_xattr_rehash(header, here);
		if (bh && header == HDR(bh))
			unlock_buffer(bh);  /* we were modifying in-place. */
		error = ext2_xattr_set2(inode, bh, header);
	}

cleanup:
	if (!(bh && header == HDR(bh)))
		kfree(header);
	brelse(bh);
	up_write(&EXT2_I(inode)->xattr_sem);

	return error;
}

static void ext2_xattr_release_block(struct inode *inode,
				     struct buffer_head *bh)
{
	struct mb_cache *ea_block_cache = EA_BLOCK_CACHE(inode);

retry_ref:
	lock_buffer(bh);
	if (HDR(bh)->h_refcount == cpu_to_le32(1)) {
		__u32 hash = le32_to_cpu(HDR(bh)->h_hash);
		struct mb_cache_entry *oe;

		/*
		 * This must happen under buffer lock to properly
		 * serialize with ext2_xattr_set() reusing the block.
		 */
		oe = mb_cache_entry_delete_or_get(ea_block_cache, hash,
						  bh->b_blocknr);
		if (oe) {
			/*
			 * Someone is trying to reuse the block. Wait
			 * and retry.
			 */
			unlock_buffer(bh);
			mb_cache_entry_wait_unused(oe);
			mb_cache_entry_put(ea_block_cache, oe);
			goto retry_ref;
		}

		/* Free the old block. */
		ea_bdebug(bh, "freeing");
		ext2_free_blocks(inode, bh->b_blocknr, 1);
		/* We let our caller release bh, so we
		 * need to duplicate the buffer before. */
		get_bh(bh);
		bforget(bh);
		unlock_buffer(bh);
	} else {
		/* Decrement the refcount only. */
		le32_add_cpu(&HDR(bh)->h_refcount, -1);
		dquot_free_block(inode, 1);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		ea_bdebug(bh, "refcount now=%d",
			le32_to_cpu(HDR(bh)->h_refcount));
		if (IS_SYNC(inode))
			sync_dirty_buffer(bh);
	}
}

/*
 * Second half of ext2_xattr_set(): Update the file system.
 */
static int
ext2_xattr_set2(struct inode *inode, struct buffer_head *old_bh,
		struct ext2_xattr_header *header)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *new_bh = NULL;
	int error;
	struct mb_cache *ea_block_cache = EA_BLOCK_CACHE(inode);

	if (header) {
		new_bh = ext2_xattr_cache_find(inode, header);
		if (new_bh) {
			/* We found an identical block in the cache. */
			if (new_bh == old_bh) {
				ea_bdebug(new_bh, "keeping this block");
			} else {
				/* The old block is released after updating
				   the inode.  */
				ea_bdebug(new_bh, "reusing block");

				error = dquot_alloc_block(inode, 1);
				if (error) {
					unlock_buffer(new_bh);
					goto cleanup;
				}
				le32_add_cpu(&HDR(new_bh)->h_refcount, 1);
				ea_bdebug(new_bh, "refcount now=%d",
					le32_to_cpu(HDR(new_bh)->h_refcount));
			}
			unlock_buffer(new_bh);
		} else if (old_bh && header == HDR(old_bh)) {
			/* Keep this block. No need to lock the block as we
			   don't need to change the reference count. */
			new_bh = old_bh;
			get_bh(new_bh);
			ext2_xattr_cache_insert(ea_block_cache, new_bh);
		} else {
			/* We need to allocate a new block */
			ext2_fsblk_t goal = ext2_group_first_block_no(sb,
						EXT2_I(inode)->i_block_group);
			unsigned long count = 1;
			ext2_fsblk_t block = ext2_new_blocks(inode, goal,
						&count, &error,
						EXT2_ALLOC_NORESERVE);
			if (error)
				goto cleanup;
			ea_idebug(inode, "creating block %lu", block);

			new_bh = sb_getblk(sb, block);
			if (unlikely(!new_bh)) {
				ext2_free_blocks(inode, block, 1);
				mark_inode_dirty(inode);
				error = -ENOMEM;
				goto cleanup;
			}
			lock_buffer(new_bh);
			memcpy(new_bh->b_data, header, new_bh->b_size);
			set_buffer_uptodate(new_bh);
			unlock_buffer(new_bh);
			ext2_xattr_cache_insert(ea_block_cache, new_bh);
			
			ext2_xattr_update_super_block(sb);
		}
		mark_buffer_dirty(new_bh);
		if (IS_SYNC(inode)) {
			sync_dirty_buffer(new_bh);
			error = -EIO;
			if (buffer_req(new_bh) && !buffer_uptodate(new_bh))
				goto cleanup;
		}
	}

	/* Update the inode. */
	EXT2_I(inode)->i_file_acl = new_bh ? new_bh->b_blocknr : 0;
	inode_set_ctime_current(inode);
	if (IS_SYNC(inode)) {
		error = sync_inode_metadata(inode, 1);
		/* In case sync failed due to ENOSPC the inode was actually
		 * written (only some dirty data were not) so we just proceed
		 * as if nothing happened and cleanup the unused block */
		if (error && error != -ENOSPC) {
			if (new_bh && new_bh != old_bh) {
				dquot_free_block_nodirty(inode, 1);
				mark_inode_dirty(inode);
			}
			goto cleanup;
		}
	} else
		mark_inode_dirty(inode);

	error = 0;
	if (old_bh && old_bh != new_bh) {
		/*
		 * If there was an old block and we are no longer using it,
		 * release the old block.
		 */
		ext2_xattr_release_block(inode, old_bh);
	}

cleanup:
	brelse(new_bh);

	return error;
}

/*
 * ext2_xattr_delete_inode()
 *
 * Free extended attribute resources associated with this inode. This
 * is called immediately before an inode is freed.
 */
void
ext2_xattr_delete_inode(struct inode *inode)
{
	struct buffer_head *bh = NULL;
	struct ext2_sb_info *sbi = EXT2_SB(inode->i_sb);

	/*
	 * We are the only ones holding inode reference. The xattr_sem should
	 * better be unlocked! We could as well just not acquire xattr_sem at
	 * all but this makes the code more futureproof. OTOH we need trylock
	 * here to avoid false-positive warning from lockdep about reclaim
	 * circular dependency.
	 */
	if (WARN_ON_ONCE(!down_write_trylock(&EXT2_I(inode)->xattr_sem)))
		return;
	if (!EXT2_I(inode)->i_file_acl)
		goto cleanup;

	if (!ext2_data_block_valid(sbi, EXT2_I(inode)->i_file_acl, 1)) {
		ext2_error(inode->i_sb, "ext2_xattr_delete_inode",
			"inode %ld: xattr block %d is out of data blocks range",
			inode->i_ino, EXT2_I(inode)->i_file_acl);
		goto cleanup;
	}

	bh = sb_bread(inode->i_sb, EXT2_I(inode)->i_file_acl);
	if (!bh) {
		ext2_error(inode->i_sb, "ext2_xattr_delete_inode",
			"inode %ld: block %d read error", inode->i_ino,
			EXT2_I(inode)->i_file_acl);
		goto cleanup;
	}
	ea_bdebug(bh, "b_count=%d", atomic_read(&(bh->b_count)));
	if (!ext2_xattr_header_valid(HDR(bh))) {
		ext2_error(inode->i_sb, "ext2_xattr_delete_inode",
			"inode %ld: bad block %d", inode->i_ino,
			EXT2_I(inode)->i_file_acl);
		goto cleanup;
	}
	ext2_xattr_release_block(inode, bh);
	EXT2_I(inode)->i_file_acl = 0;

cleanup:
	brelse(bh);
	up_write(&EXT2_I(inode)->xattr_sem);
}

/*
 * ext2_xattr_cache_insert()
 *
 * Create a new entry in the extended attribute cache, and insert
 * it unless such an entry is already in the cache.
 *
 * Returns 0, or a negative error number on failure.
 */
static int
ext2_xattr_cache_insert(struct mb_cache *cache, struct buffer_head *bh)
{
	__u32 hash = le32_to_cpu(HDR(bh)->h_hash);
	int error;

	error = mb_cache_entry_create(cache, GFP_KERNEL, hash, bh->b_blocknr,
				      true);
	if (error) {
		if (error == -EBUSY) {
			ea_bdebug(bh, "already in cache");
			error = 0;
		}
	} else
		ea_bdebug(bh, "inserting [%x]", (int)hash);
	return error;
}

/*
 * ext2_xattr_cmp()
 *
 * Compare two extended attribute blocks for equality.
 *
 * Returns 0 if the blocks are equal, 1 if they differ, and
 * a negative error number on errors.
 */
static int
ext2_xattr_cmp(struct ext2_xattr_header *header1,
	       struct ext2_xattr_header *header2)
{
	struct ext2_xattr_entry *entry1, *entry2;

	entry1 = ENTRY(header1+1);
	entry2 = ENTRY(header2+1);
	while (!IS_LAST_ENTRY(entry1)) {
		if (IS_LAST_ENTRY(entry2))
			return 1;
		if (entry1->e_hash != entry2->e_hash ||
		    entry1->e_name_index != entry2->e_name_index ||
		    entry1->e_name_len != entry2->e_name_len ||
		    entry1->e_value_size != entry2->e_value_size ||
		    memcmp(entry1->e_name, entry2->e_name, entry1->e_name_len))
			return 1;
		if (entry1->e_value_block != 0 || entry2->e_value_block != 0)
			return -EIO;
		if (memcmp((char *)header1 + le16_to_cpu(entry1->e_value_offs),
			   (char *)header2 + le16_to_cpu(entry2->e_value_offs),
			   le32_to_cpu(entry1->e_value_size)))
			return 1;

		entry1 = EXT2_XATTR_NEXT(entry1);
		entry2 = EXT2_XATTR_NEXT(entry2);
	}
	if (!IS_LAST_ENTRY(entry2))
		return 1;
	return 0;
}

/*
 * ext2_xattr_cache_find()
 *
 * Find an identical extended attribute block.
 *
 * Returns a locked buffer head to the block found, or NULL if such
 * a block was not found or an error occurred.
 */
static struct buffer_head *
ext2_xattr_cache_find(struct inode *inode, struct ext2_xattr_header *header)
{
	__u32 hash = le32_to_cpu(header->h_hash);
	struct mb_cache_entry *ce;
	struct mb_cache *ea_block_cache = EA_BLOCK_CACHE(inode);

	if (!header->h_hash)
		return NULL;  /* never share */
	ea_idebug(inode, "looking for cached blocks [%x]", (int)hash);

	ce = mb_cache_entry_find_first(ea_block_cache, hash);
	while (ce) {
		struct buffer_head *bh;

		bh = sb_bread(inode->i_sb, ce->e_value);
		if (!bh) {
			ext2_error(inode->i_sb, "ext2_xattr_cache_find",
				"inode %ld: block %ld read error",
				inode->i_ino, (unsigned long) ce->e_value);
		} else {
			lock_buffer(bh);
			if (le32_to_cpu(HDR(bh)->h_refcount) >
			    EXT2_XATTR_REFCOUNT_MAX) {
				ea_idebug(inode, "block %ld refcount %d>%d",
					  (unsigned long) ce->e_value,
					  le32_to_cpu(HDR(bh)->h_refcount),
					  EXT2_XATTR_REFCOUNT_MAX);
			} else if (!ext2_xattr_cmp(header, HDR(bh))) {
				ea_bdebug(bh, "b_count=%d",
					  atomic_read(&(bh->b_count)));
				mb_cache_entry_touch(ea_block_cache, ce);
				mb_cache_entry_put(ea_block_cache, ce);
				return bh;
			}
			unlock_buffer(bh);
			brelse(bh);
		}
		ce = mb_cache_entry_find_next(ea_block_cache, ce);
	}
	return NULL;
}

#define NAME_HASH_SHIFT 5
#define VALUE_HASH_SHIFT 16

/*
 * ext2_xattr_hash_entry()
 *
 * Compute the hash of an extended attribute.
 */
static inline void ext2_xattr_hash_entry(struct ext2_xattr_header *header,
					 struct ext2_xattr_entry *entry)
{
	__u32 hash = 0;
	char *name = entry->e_name;
	int n;

	for (n=0; n < entry->e_name_len; n++) {
		hash = (hash << NAME_HASH_SHIFT) ^
		       (hash >> (8*sizeof(hash) - NAME_HASH_SHIFT)) ^
		       *name++;
	}

	if (entry->e_value_block == 0 && entry->e_value_size != 0) {
		__le32 *value = (__le32 *)((char *)header +
			le16_to_cpu(entry->e_value_offs));
		for (n = (le32_to_cpu(entry->e_value_size) +
		     EXT2_XATTR_ROUND) >> EXT2_XATTR_PAD_BITS; n; n--) {
			hash = (hash << VALUE_HASH_SHIFT) ^
			       (hash >> (8*sizeof(hash) - VALUE_HASH_SHIFT)) ^
			       le32_to_cpu(*value++);
		}
	}
	entry->e_hash = cpu_to_le32(hash);
}

#undef NAME_HASH_SHIFT
#undef VALUE_HASH_SHIFT

#define BLOCK_HASH_SHIFT 16

/*
 * ext2_xattr_rehash()
 *
 * Re-compute the extended attribute hash value after an entry has changed.
 */
static void ext2_xattr_rehash(struct ext2_xattr_header *header,
			      struct ext2_xattr_entry *entry)
{
	struct ext2_xattr_entry *here;
	__u32 hash = 0;
	
	ext2_xattr_hash_entry(header, entry);
	here = ENTRY(header+1);
	while (!IS_LAST_ENTRY(here)) {
		if (!here->e_hash) {
			/* Block is not shared if an entry's hash value == 0 */
			hash = 0;
			break;
		}
		hash = (hash << BLOCK_HASH_SHIFT) ^
		       (hash >> (8*sizeof(hash) - BLOCK_HASH_SHIFT)) ^
		       le32_to_cpu(here->e_hash);
		here = EXT2_XATTR_NEXT(here);
	}
	header->h_hash = cpu_to_le32(hash);
}

#undef BLOCK_HASH_SHIFT

#define HASH_BUCKET_BITS 10

struct mb_cache *ext2_xattr_create_cache(void)
{
	return mb_cache_create(HASH_BUCKET_BITS);
}

void ext2_xattr_destroy_cache(struct mb_cache *cache)
{
	if (cache)
		mb_cache_destroy(cache);
}

/* ---- EXT2 user xattr handler (merged into this translation unit) ---- */
// SPDX-License-Identifier: GPL-2.0
/*
 * linux/fs/ext2/xattr_user.c
 * Handler for extended user attributes.
 *
 * Copyright (C) 2001 by Andreas Gruenbacher, <a.gruenbacher@computer.org>
 */


static bool
ext2_xattr_user_list(struct dentry *dentry)
{
	return test_opt(dentry->d_sb, XATTR_USER);
}

static int
ext2_xattr_user_get(const struct xattr_handler *handler,
		    struct dentry *unused, struct inode *inode,
		    const char *name, void *buffer, size_t size)
{
	if (!test_opt(inode->i_sb, XATTR_USER))
		return -EOPNOTSUPP;
	return ext2_xattr_get(inode, EXT2_XATTR_INDEX_USER,
			      name, buffer, size);
}

static int
ext2_xattr_user_set(const struct xattr_handler *handler,
		    struct mnt_idmap *idmap,
		    struct dentry *unused, struct inode *inode,
		    const char *name, const void *value,
		    size_t size, int flags)
{
	if (!test_opt(inode->i_sb, XATTR_USER))
		return -EOPNOTSUPP;

	return ext2_xattr_set(inode, EXT2_XATTR_INDEX_USER,
			      name, value, size, flags);
}

const struct xattr_handler ext2_xattr_user_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.list	= ext2_xattr_user_list,
	.get	= ext2_xattr_user_get,
	.set	= ext2_xattr_user_set,
};

/* ---- EXT2 trusted xattr handler (merged into this translation unit) ---- */
// SPDX-License-Identifier: GPL-2.0
/*
 * linux/fs/ext2/xattr_trusted.c
 * Handler for trusted extended attributes.
 *
 * Copyright (C) 2003 by Andreas Gruenbacher, <a.gruenbacher@computer.org>
 */


static bool
ext2_xattr_trusted_list(struct dentry *dentry)
{
	return capable(CAP_SYS_ADMIN);
}

static int
ext2_xattr_trusted_get(const struct xattr_handler *handler,
		       struct dentry *unused, struct inode *inode,
		       const char *name, void *buffer, size_t size)
{
	return ext2_xattr_get(inode, EXT2_XATTR_INDEX_TRUSTED, name,
			      buffer, size);
}

static int
ext2_xattr_trusted_set(const struct xattr_handler *handler,
		       struct mnt_idmap *idmap,
		       struct dentry *unused, struct inode *inode,
		       const char *name, const void *value,
		       size_t size, int flags)
{
	return ext2_xattr_set(inode, EXT2_XATTR_INDEX_TRUSTED, name,
			      value, size, flags);
}

const struct xattr_handler ext2_xattr_trusted_handler = {
	.prefix	= XATTR_TRUSTED_PREFIX,
	.list	= ext2_xattr_trusted_list,
	.get	= ext2_xattr_trusted_get,
	.set	= ext2_xattr_trusted_set,
};

#ifdef CONFIG_EXT2_FS_SECURITY

/* ---- EXT2 security xattr handler (merged into this translation unit) ---- */
// SPDX-License-Identifier: GPL-2.0
/*
 * linux/fs/ext2/xattr_security.c
 * Handler for storing security labels as extended attributes.
 */


static int
ext2_xattr_security_get(const struct xattr_handler *handler,
			struct dentry *unused, struct inode *inode,
			const char *name, void *buffer, size_t size)
{
	return ext2_xattr_get(inode, EXT2_XATTR_INDEX_SECURITY, name,
			      buffer, size);
}

static int
ext2_xattr_security_set(const struct xattr_handler *handler,
			struct mnt_idmap *idmap,
			struct dentry *unused, struct inode *inode,
			const char *name, const void *value,
			size_t size, int flags)
{
	return ext2_xattr_set(inode, EXT2_XATTR_INDEX_SECURITY, name,
			      value, size, flags);
}

static int ext2_initxattrs(struct inode *inode, const struct xattr *xattr_array,
			   void *fs_info)
{
	const struct xattr *xattr;
	int err = 0;

	for (xattr = xattr_array; xattr->name != NULL; xattr++) {
		err = ext2_xattr_set(inode, EXT2_XATTR_INDEX_SECURITY,
				     xattr->name, xattr->value,
				     xattr->value_len, 0);
		if (err < 0)
			break;
	}
	return err;
}

int
ext2_init_security(struct inode *inode, struct inode *dir,
		   const struct qstr *qstr)
{
	return security_inode_init_security(inode, dir, qstr,
					    &ext2_initxattrs, NULL);
}

const struct xattr_handler ext2_xattr_security_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.get	= ext2_xattr_security_get,
	.set	= ext2_xattr_security_set,
};
#endif /* CONFIG_EXT2_FS_SECURITY */

#ifdef CONFIG_EXT2_FS_POSIX_ACL

/* ---- EXT2 POSIX ACL implementation (merged into this translation unit) ---- */
// SPDX-License-Identifier: GPL-2.0
/*
 * linux/fs/ext2/acl.c
 *
 * Copyright (C) 2001-2003 Andreas Gruenbacher, <agruen@suse.de>
 */


/*
 * Convert from filesystem to in-memory representation.
 */
static struct posix_acl *
ext2_acl_from_disk(const void *value, size_t size)
{
	const char *end = (char *)value + size;
	int n, count;
	struct posix_acl *acl;

	if (!value)
		return NULL;
	if (size < sizeof(ext2_acl_header))
		 return ERR_PTR(-EINVAL);
	if (((ext2_acl_header *)value)->a_version !=
	    cpu_to_le32(EXT2_ACL_VERSION))
		return ERR_PTR(-EINVAL);
	value = (char *)value + sizeof(ext2_acl_header);
	count = ext2_acl_count(size);
	if (count < 0)
		return ERR_PTR(-EINVAL);
	if (count == 0)
		return NULL;
	acl = posix_acl_alloc(count, GFP_KERNEL);
	if (!acl)
		return ERR_PTR(-ENOMEM);
	for (n=0; n < count; n++) {
		ext2_acl_entry *entry =
			(ext2_acl_entry *)value;
		if ((char *)value + sizeof(ext2_acl_entry_short) > end)
			goto fail;
		acl->a_entries[n].e_tag  = le16_to_cpu(entry->e_tag);
		acl->a_entries[n].e_perm = le16_to_cpu(entry->e_perm);
		switch(acl->a_entries[n].e_tag) {
			case ACL_USER_OBJ:
			case ACL_GROUP_OBJ:
			case ACL_MASK:
			case ACL_OTHER:
				value = (char *)value +
					sizeof(ext2_acl_entry_short);
				break;

			case ACL_USER:
				value = (char *)value + sizeof(ext2_acl_entry);
				if ((char *)value > end)
					goto fail;
				acl->a_entries[n].e_uid =
					make_kuid(&init_user_ns,
						  le32_to_cpu(entry->e_id));
				break;
			case ACL_GROUP:
				value = (char *)value + sizeof(ext2_acl_entry);
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

/*
 * Convert from in-memory to filesystem representation.
 */
static void *
ext2_acl_to_disk(const struct posix_acl *acl, size_t *size)
{
	ext2_acl_header *ext_acl;
	char *e;
	size_t n;

	*size = ext2_acl_size(acl->a_count);
	ext_acl = kmalloc(sizeof(ext2_acl_header) + acl->a_count *
			sizeof(ext2_acl_entry), GFP_KERNEL);
	if (!ext_acl)
		return ERR_PTR(-ENOMEM);
	ext_acl->a_version = cpu_to_le32(EXT2_ACL_VERSION);
	e = (char *)ext_acl + sizeof(ext2_acl_header);
	for (n=0; n < acl->a_count; n++) {
		const struct posix_acl_entry *acl_e = &acl->a_entries[n];
		ext2_acl_entry *entry = (ext2_acl_entry *)e;
		entry->e_tag  = cpu_to_le16(acl_e->e_tag);
		entry->e_perm = cpu_to_le16(acl_e->e_perm);
		switch(acl_e->e_tag) {
			case ACL_USER:
				entry->e_id = cpu_to_le32(
					from_kuid(&init_user_ns, acl_e->e_uid));
				e += sizeof(ext2_acl_entry);
				break;
			case ACL_GROUP:
				entry->e_id = cpu_to_le32(
					from_kgid(&init_user_ns, acl_e->e_gid));
				e += sizeof(ext2_acl_entry);
				break;

			case ACL_USER_OBJ:
			case ACL_GROUP_OBJ:
			case ACL_MASK:
			case ACL_OTHER:
				e += sizeof(ext2_acl_entry_short);
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

/*
 * inode->i_mutex: don't care
 */
struct posix_acl *
ext2_get_acl(struct inode *inode, int type, bool rcu)
{
	int name_index;
	char *value = NULL;
	struct posix_acl *acl;
	int retval;

	if (rcu)
		return ERR_PTR(-ECHILD);

	switch (type) {
	case ACL_TYPE_ACCESS:
		name_index = EXT2_XATTR_INDEX_POSIX_ACL_ACCESS;
		break;
	case ACL_TYPE_DEFAULT:
		name_index = EXT2_XATTR_INDEX_POSIX_ACL_DEFAULT;
		break;
	default:
		BUG();
	}
	retval = ext2_xattr_get(inode, name_index, "", NULL, 0);
	if (retval > 0) {
		value = kmalloc(retval, GFP_KERNEL);
		if (!value)
			return ERR_PTR(-ENOMEM);
		retval = ext2_xattr_get(inode, name_index, "", value, retval);
	}
	if (retval > 0)
		acl = ext2_acl_from_disk(value, retval);
	else if (retval == -ENODATA || retval == -ENOSYS)
		acl = NULL;
	else
		acl = ERR_PTR(retval);
	kfree(value);

	return acl;
}

static int
__ext2_set_acl(struct inode *inode, struct posix_acl *acl, int type)
{
	int name_index;
	void *value = NULL;
	size_t size = 0;
	int error;

	switch(type) {
		case ACL_TYPE_ACCESS:
			name_index = EXT2_XATTR_INDEX_POSIX_ACL_ACCESS;
			break;

		case ACL_TYPE_DEFAULT:
			name_index = EXT2_XATTR_INDEX_POSIX_ACL_DEFAULT;
			if (!S_ISDIR(inode->i_mode))
				return acl ? -EACCES : 0;
			break;

		default:
			return -EINVAL;
	}
 	if (acl) {
		value = ext2_acl_to_disk(acl, &size);
		if (IS_ERR(value))
			return (int)PTR_ERR(value);
	}

	error = ext2_xattr_set(inode, name_index, "", value, size, 0);

	kfree(value);
	if (!error)
		set_cached_acl(inode, type, acl);
	return error;
}

/*
 * inode->i_mutex: down
 */
int
ext2_set_acl(struct mnt_idmap *idmap, struct dentry *dentry,
	     struct posix_acl *acl, int type)
{
	int error;
	int update_mode = 0;
	struct inode *inode = d_inode(dentry);
	umode_t mode = inode->i_mode;

	if (type == ACL_TYPE_ACCESS && acl) {
		error = posix_acl_update_mode(&nop_mnt_idmap, inode, &mode,
					      &acl);
		if (error)
			return error;
		update_mode = 1;
	}
	error = __ext2_set_acl(inode, acl, type);
	if (!error && update_mode) {
		inode->i_mode = mode;
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
	}
	return error;
}

/*
 * Initialize the ACLs of a new inode. Called from ext2_new_inode.
 *
 * dir->i_mutex: down
 * inode->i_mutex: up (access to inode is still exclusive)
 */
int
ext2_init_acl(struct inode *inode, struct inode *dir)
{
	struct posix_acl *default_acl, *acl;
	int error;

	error = posix_acl_create(dir, &inode->i_mode, &default_acl, &acl);
	if (error)
		return error;

	if (default_acl) {
		error = __ext2_set_acl(inode, default_acl, ACL_TYPE_DEFAULT);
		posix_acl_release(default_acl);
	} else {
		inode->i_default_acl = NULL;
	}
	if (acl) {
		if (!error)
			error = __ext2_set_acl(inode, acl, ACL_TYPE_ACCESS);
		posix_acl_release(acl);
	} else {
		inode->i_acl = NULL;
	}
	return error;
}
#endif /* CONFIG_EXT2_FS_POSIX_ACL */

/* ---- EXT2 private metadata-block cache (merged into this translation unit) ---- */
// SPDX-License-Identifier: GPL-2.0-only

/*
 * Mbcache is a simple key-value store. Keys need not be unique, however
 * key-value pairs are expected to be unique (we use this fact in
 * mb_cache_entry_delete_or_get()).
 *
 * Ext2 and ext4 use this cache for deduplication of extended attribute blocks.
 * Ext4 also uses it for deduplication of xattr values stored in inodes.
 * They use hash of data as a key and provide a value that may represent a
 * block or inode number. That's why keys need not be unique (hash of different
 * data may be the same). However user provided value always uniquely
 * identifies a cache entry.
 *
 * We provide functions for creation and removal of entries, search by key,
 * and a special "delete entry with given key-value pair" operation. Fixed
 * size hash table is used for fast key lookups.
 */

struct mb_cache {
	/* Hash table of entries */
	struct hlist_bl_head	*c_hash;
	/* log2 of hash table size */
	int			c_bucket_bits;
	/* Maximum entries in cache to avoid degrading hash too much */
	unsigned long		c_max_entries;
	/* Protects c_list, c_entry_count */
	spinlock_t		c_list_lock;
	struct list_head	c_list;
	/* Number of entries in cache */
	unsigned long		c_entry_count;
	struct shrinker		*c_shrink;
	/* Work for shrinking when the cache has too many entries */
	struct work_struct	c_shrink_work;
};

static struct kmem_cache *mb_entry_cache;

static unsigned long mb_cache_shrink(struct mb_cache *cache,
				     unsigned long nr_to_scan);

static inline struct hlist_bl_head *mb_cache_entry_head(struct mb_cache *cache,
							u32 key)
{
	return &cache->c_hash[hash_32(key, cache->c_bucket_bits)];
}

/*
 * Number of entries to reclaim synchronously when there are too many entries
 * in cache
 */
#define SYNC_SHRINK_BATCH 64

/*
 * mb_cache_entry_create - create entry in cache
 * @cache - cache where the entry should be created
 * @mask - gfp mask with which the entry should be allocated
 * @key - key of the entry
 * @value - value of the entry
 * @reusable - is the entry reusable by others?
 *
 * Creates entry in @cache with key @key and value @value. The function returns
 * -EBUSY if entry with the same key and value already exists in cache.
 * Otherwise 0 is returned.
 */
int mb_cache_entry_create(struct mb_cache *cache, gfp_t mask, u32 key,
			  u64 value, bool reusable)
{
	struct mb_cache_entry *entry, *dup;
	struct hlist_bl_node *dup_node;
	struct hlist_bl_head *head;

	/* Schedule background reclaim if there are too many entries */
	if (cache->c_entry_count >= cache->c_max_entries)
		schedule_work(&cache->c_shrink_work);
	/* Do some sync reclaim if background reclaim cannot keep up */
	if (cache->c_entry_count >= 2*cache->c_max_entries)
		mb_cache_shrink(cache, SYNC_SHRINK_BATCH);

	entry = kmem_cache_alloc(mb_entry_cache, mask);
	if (!entry)
		return -ENOMEM;

	INIT_LIST_HEAD(&entry->e_list);
	/*
	 * We create entry with two references. One reference is kept by the
	 * hash table, the other reference is used to protect us from
	 * mb_cache_entry_delete_or_get() until the entry is fully setup. This
	 * avoids nesting of cache->c_list_lock into hash table bit locks which
	 * is problematic for RT.
	 */
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

void __mb_cache_entry_free(struct mb_cache *cache, struct mb_cache_entry *entry)
{
	struct hlist_bl_head *head;

	head = mb_cache_entry_head(cache, entry->e_key);
	hlist_bl_lock(head);
	hlist_bl_del(&entry->e_hash_list);
	hlist_bl_unlock(head);
	kmem_cache_free(mb_entry_cache, entry);
}

/*
 * mb_cache_entry_wait_unused - wait to be the last user of the entry
 *
 * @entry - entry to work on
 *
 * Wait to be the last user of the entry.
 */
void mb_cache_entry_wait_unused(struct mb_cache_entry *entry)
{
	wait_var_event(&entry->e_refcnt, atomic_read(&entry->e_refcnt) <= 2);
}

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

/*
 * mb_cache_entry_find_first - find the first reusable entry with the given key
 * @cache: cache where we should search
 * @key: key to look for
 *
 * Search in @cache for a reusable entry with key @key. Grabs reference to the
 * first reusable entry found and returns the entry.
 */
struct mb_cache_entry *mb_cache_entry_find_first(struct mb_cache *cache,
						 u32 key)
{
	return __entry_find(cache, NULL, key);
}

/*
 * mb_cache_entry_find_next - find next reusable entry with the same key
 * @cache: cache where we should search
 * @entry: entry to start search from
 *
 * Finds next reusable entry in the hash chain which has the same key as @entry.
 * If @entry is unhashed (which can happen when deletion of entry races with the
 * search), finds the first reusable entry in the hash chain. The function drops
 * reference to @entry and returns with a reference to the found entry.
 */
struct mb_cache_entry *mb_cache_entry_find_next(struct mb_cache *cache,
						struct mb_cache_entry *entry)
{
	return __entry_find(cache, entry, entry->e_key);
}

/*
 * mb_cache_entry_get - get a cache entry by value (and key)
 * @cache - cache we work with
 * @key - key
 * @value - value
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

/* mb_cache_entry_delete_or_get - remove a cache entry if it has no users
 * @cache - cache we work with
 * @key - key
 * @value - value
 *
 * Remove entry from cache @cache with key @key and value @value. The removal
 * happens only if the entry is unused. The function returns NULL in case the
 * entry was successfully removed or there's no entry in cache. Otherwise the
 * function grabs reference of the entry that we failed to delete because it
 * still has users and return it.
 */
struct mb_cache_entry *mb_cache_entry_delete_or_get(struct mb_cache *cache,
						    u32 key, u64 value)
{
	struct mb_cache_entry *entry;

	entry = mb_cache_entry_get(cache, key, value);
	if (!entry)
		return NULL;

	/*
	 * Drop the ref we got from mb_cache_entry_get() and the initial hash
	 * ref if we are the last user
	 */
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

/* mb_cache_entry_touch - cache entry got used
 * @cache - cache the entry belongs to
 * @entry - entry that got used
 *
 * Marks entry as used to give hit higher chances of surviving in cache.
 */
void mb_cache_entry_touch(struct mb_cache *cache,
			  struct mb_cache_entry *entry)
{
	set_bit(MBE_REFERENCED_B, &entry->e_flags);
}

static unsigned long mb_cache_count(struct shrinker *shrink,
				    struct shrink_control *sc)
{
	struct mb_cache *cache = shrink->private_data;

	return cache->c_entry_count;
}

/* Shrink number of entries in cache */
static unsigned long mb_cache_shrink(struct mb_cache *cache,
				     unsigned long nr_to_scan)
{
	struct mb_cache_entry *entry;
	unsigned long shrunk = 0;

	spin_lock(&cache->c_list_lock);
	while (nr_to_scan-- && !list_empty(&cache->c_list)) {
		entry = list_first_entry(&cache->c_list,
					 struct mb_cache_entry, e_list);
		/* Drop initial hash reference if there is no user */
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

static unsigned long mb_cache_scan(struct shrinker *shrink,
				   struct shrink_control *sc)
{
	struct mb_cache *cache = shrink->private_data;
	return mb_cache_shrink(cache, sc->nr_to_scan);
}

/* We shrink 1/X of the cache when we have too many entries in it */
#define SHRINK_DIVISOR 16

static void mb_cache_shrink_worker(struct work_struct *work)
{
	struct mb_cache *cache = container_of(work, struct mb_cache,
					      c_shrink_work);
	mb_cache_shrink(cache, cache->c_max_entries / SHRINK_DIVISOR);
}

/*
 * mb_cache_create - create cache
 * @bucket_bits: log2 of the hash table size
 *
 * Create cache for keys with 2^bucket_bits hash entries.
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

/*
 * mb_cache_destroy - destroy cache
 * @cache: the cache to destroy
 *
 * Free all entries in cache and cache itself. Caller must make sure nobody
 * (except shrinker) can reach @cache when calling this.
 */
void mb_cache_destroy(struct mb_cache *cache)
{
	struct mb_cache_entry *entry, *next;

	cancel_work_sync(&cache->c_shrink_work);
	shrinker_free(cache->c_shrink);

	/*
	 * We don't bother with any locking. Cache must not be used at this
	 * point.
	 */
	list_for_each_entry_safe(entry, next, &cache->c_list, e_list) {
		list_del(&entry->e_list);
		WARN_ON(atomic_read(&entry->e_refcnt) != 1);
		mb_cache_entry_put(cache, entry);
	}
	kfree(cache->c_hash);
	kfree(cache);
}

int __init infiltratr_mbcache_init(void)
{
	mb_entry_cache = KMEM_CACHE(mb_cache_entry, SLAB_RECLAIM_ACCOUNT);
	if (!mb_entry_cache)
		return -ENOMEM;
	return 0;
}

void __exit infiltratr_mbcache_exit(void)
{
	kmem_cache_destroy(mb_entry_cache);
}


