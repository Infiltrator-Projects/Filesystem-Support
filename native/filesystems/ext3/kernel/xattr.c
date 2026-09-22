/*
 * linux/fs/ext3/xattr.c
 *
 * Copyright (C) 2001-2003 Andreas Gruenbacher, <agruen@suse.de>
 *
 * Fix by Harrison Xing <harrison@mountainviewdata.com>.
 * Ext3 code with a lot of help from Eric Jarman <ejarman@acm.org>.
 * Extended attributes for symlinks and special files added per
 *  suggestion of Luka Renko <luka.renko@hermes.si>.
 * xattr consolidation Copyright (c) 2004 James Morris <jmorris@redhat.com>,
 *  Red Hat Inc.
 * ea-in-inode support by Alex Tomas <alex@clusterfs.com> aka bzzz
 *  and Andreas Gruenbacher <agruen@suse.de>.
 */

/*
 * Extended attributes are stored directly in inodes (on file systems with
 * inodes bigger than 128 bytes) and on additional disk blocks. The i_file_acl
 * field contains the block number if an inode uses an additional block. All
 * attributes must fit in the inode and one additional block. Blocks that
 * contain the identical set of attributes may be shared among several inodes.
 * Identical blocks are detected by keeping a cache of blocks that have
 * recently been accessed.
 *
 * The attributes in inodes and on blocks have a different header; the entries
 * are stored in the same format:
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
 * The header is followed by multiple entry descriptors. In disk blocks, the
 * entry descriptors are kept sorted. In inodes, they are unsorted. The
 * attribute values are aligned to the end of the block in no specific order.
 *
 * Locking strategy
 * ----------------
 * EXT3_I(inode)->i_file_acl is protected by EXT3_I(inode)->xattr_sem.
 * EA blocks are only changed if they are exclusive to an inode, so
 * holding xattr_sem also means that nothing but the EA block's reference
 * count can change. Multiple writers to the same block are synchronized
 * by the buffer lock.
 */

#include "ext3.h"
#include <linux/quotaops.h>

#define BHDR(bh) ((struct ext3_xattr_header *)((bh)->b_data))
#define ENTRY(ptr) ((struct ext3_xattr_entry *)(ptr))
#define BFIRST(bh) ENTRY(BHDR(bh)+1)
#define IS_LAST_ENTRY(entry) (*(__u32 *)(entry) == 0)

#define IHDR(inode, raw_inode) \
	((struct ext3_xattr_ibody_header *) \
		((void *)raw_inode + \
		 EXT3_GOOD_OLD_INODE_SIZE + \
		 EXT3_I(inode)->i_extra_isize))
#define IFIRST(hdr) ((struct ext3_xattr_entry *)((hdr)+1))

#ifdef EXT3_XATTR_DEBUG
# define ea_idebug(inode, f...) do { \
		printk(KERN_DEBUG "inode %s:%lu: ", \
			inode->i_sb->s_id, inode->i_ino); \
		printk(f); \
		printk("\n"); \
	} while (0)
# define ea_bdebug(bh, f...) do { \
		char b[BDEVNAME_SIZE]; \
		printk(KERN_DEBUG "block %s:%lu: ", \
			bdevname(bh->b_bdev, b), \
			(unsigned long) bh->b_blocknr); \
		printk(f); \
		printk("\n"); \
	} while (0)
#else
# define ea_idebug(f...)
# define ea_bdebug(f...)
#endif

static void ext3_xattr_cache_insert(struct buffer_head *);
static struct buffer_head *ext3_xattr_cache_find(struct inode *,
						 struct ext3_xattr_header *,
						 struct mb_cache_entry **);
static void ext3_xattr_rehash(struct ext3_xattr_header *,
			      struct ext3_xattr_entry *);
static int ext3_xattr_list(struct dentry *dentry, char *buffer,
			   size_t buffer_size);

static struct mb_cache *ext3_xattr_cache;

static const struct xattr_handler *ext3_xattr_handler_map[] = {
	[EXT3_XATTR_INDEX_USER]		     = &ext3_xattr_user_handler,
#ifdef CONFIG_EXT3_FS_POSIX_ACL
	[EXT3_XATTR_INDEX_POSIX_ACL_ACCESS]  = &posix_acl_access_xattr_handler,
	[EXT3_XATTR_INDEX_POSIX_ACL_DEFAULT] = &posix_acl_default_xattr_handler,
#endif
	[EXT3_XATTR_INDEX_TRUSTED]	     = &ext3_xattr_trusted_handler,
#ifdef CONFIG_EXT3_FS_SECURITY
	[EXT3_XATTR_INDEX_SECURITY]	     = &ext3_xattr_security_handler,
#endif
};

const struct xattr_handler *ext3_xattr_handlers[] = {
	&ext3_xattr_user_handler,
	&ext3_xattr_trusted_handler,
#ifdef CONFIG_EXT3_FS_POSIX_ACL
	&posix_acl_access_xattr_handler,
	&posix_acl_default_xattr_handler,
#endif
#ifdef CONFIG_EXT3_FS_SECURITY
	&ext3_xattr_security_handler,
#endif
	NULL
};

static inline const struct xattr_handler *
ext3_xattr_handler(int name_index)
{
	const struct xattr_handler *handler = NULL;

	if (name_index > 0 && name_index < ARRAY_SIZE(ext3_xattr_handler_map))
		handler = ext3_xattr_handler_map[name_index];
	return handler;
}

/*
 * Inode operation listxattr()
 *
 * d_inode(dentry)->i_mutex: don't care
 */
ssize_t
ext3_listxattr(struct dentry *dentry, char *buffer, size_t size)
{
	return ext3_xattr_list(dentry, buffer, size);
}

static int
ext3_xattr_check_names(struct ext3_xattr_entry *entry, void *end)
{
	while (!IS_LAST_ENTRY(entry)) {
		struct ext3_xattr_entry *next = EXT3_XATTR_NEXT(entry);
		if ((void *)next >= end)
			return -EIO;
		entry = next;
	}
	return 0;
}

static inline int
ext3_xattr_check_block(struct buffer_head *bh)
{
	int error;

	if (BHDR(bh)->h_magic != cpu_to_le32(EXT3_XATTR_MAGIC) ||
	    BHDR(bh)->h_blocks != cpu_to_le32(1))
		return -EIO;
	error = ext3_xattr_check_names(BFIRST(bh), bh->b_data + bh->b_size);
	return error;
}

static inline int
ext3_xattr_check_entry(struct ext3_xattr_entry *entry, size_t size)
{
	size_t value_size = le32_to_cpu(entry->e_value_size);

	if (entry->e_value_block != 0 || value_size > size ||
	    le16_to_cpu(entry->e_value_offs) + value_size > size)
		return -EIO;
	return 0;
}

static int
ext3_xattr_find_entry(struct ext3_xattr_entry **pentry, int name_index,
		      const char *name, size_t size, int sorted)
{
	struct ext3_xattr_entry *entry;
	size_t name_len;
	int cmp = 1;

	if (name == NULL)
		return -EINVAL;
	name_len = strlen(name);
	entry = *pentry;
	for (; !IS_LAST_ENTRY(entry); entry = EXT3_XATTR_NEXT(entry)) {
		cmp = name_index - entry->e_name_index;
		if (!cmp)
			cmp = name_len - entry->e_name_len;
		if (!cmp)
			cmp = memcmp(name, entry->e_name, name_len);
		if (cmp <= 0 && (sorted || cmp == 0))
			break;
	}
	*pentry = entry;
	if (!cmp && ext3_xattr_check_entry(entry, size))
			return -EIO;
	return cmp ? -ENODATA : 0;
}

static int
ext3_xattr_block_get(struct inode *inode, int name_index, const char *name,
		     void *buffer, size_t buffer_size)
{
	struct buffer_head *bh = NULL;
	struct ext3_xattr_entry *entry;
	size_t size;
	int error;

	ea_idebug(inode, "name=%d.%s, buffer=%p, buffer_size=%ld",
		  name_index, name, buffer, (long)buffer_size);

	error = -ENODATA;
	if (!EXT3_I(inode)->i_file_acl)
		goto cleanup;
	ea_idebug(inode, "reading block %u", EXT3_I(inode)->i_file_acl);
	bh = sb_bread(inode->i_sb, EXT3_I(inode)->i_file_acl);
	if (!bh)
		goto cleanup;
	ea_bdebug(bh, "b_count=%d, refcount=%d",
		atomic_read(&(bh->b_count)), le32_to_cpu(BHDR(bh)->h_refcount));
	if (ext3_xattr_check_block(bh)) {
bad_block:	ext3_error(inode->i_sb, __func__,
			   "inode %lu: bad block "E3FSBLK, inode->i_ino,
			   EXT3_I(inode)->i_file_acl);
		error = -EIO;
		goto cleanup;
	}
	ext3_xattr_cache_insert(bh);
	entry = BFIRST(bh);
	error = ext3_xattr_find_entry(&entry, name_index, name, bh->b_size, 1);
	if (error == -EIO)
		goto bad_block;
	if (error)
		goto cleanup;
	size = le32_to_cpu(entry->e_value_size);
	if (buffer) {
		error = -ERANGE;
		if (size > buffer_size)
			goto cleanup;
		memcpy(buffer, bh->b_data + le16_to_cpu(entry->e_value_offs),
		       size);
	}
	error = size;

cleanup:
	brelse(bh);
	return error;
}

static int
ext3_xattr_ibody_get(struct inode *inode, int name_index, const char *name,
		     void *buffer, size_t buffer_size)
{
	struct ext3_xattr_ibody_header *header;
	struct ext3_xattr_entry *entry;
	struct ext3_inode *raw_inode;
	struct ext3_iloc iloc;
	size_t size;
	void *end;
	int error;

	if (!ext3_test_inode_state(inode, EXT3_STATE_XATTR))
		return -ENODATA;
	error = ext3_get_inode_loc(inode, &iloc);
	if (error)
		return error;
	raw_inode = ext3_raw_inode(&iloc);
	header = IHDR(inode, raw_inode);
	entry = IFIRST(header);
	end = (void *)raw_inode + EXT3_SB(inode->i_sb)->s_inode_size;
	error = ext3_xattr_check_names(entry, end);
	if (error)
		goto cleanup;
	error = ext3_xattr_find_entry(&entry, name_index, name,
				      end - (void *)entry, 0);
	if (error)
		goto cleanup;
	size = le32_to_cpu(entry->e_value_size);
	if (buffer) {
		error = -ERANGE;
		if (size > buffer_size)
			goto cleanup;
		memcpy(buffer, (void *)IFIRST(header) +
		       le16_to_cpu(entry->e_value_offs), size);
	}
	error = size;

cleanup:
	brelse(iloc.bh);
	return error;
}

/*
 * ext3_xattr_get()
 *
 * Copy an extended attribute into the buffer
 * provided, or compute the buffer size required.
 * Buffer is NULL to compute the size of the buffer required.
 *
 * Returns a negative error number on failure, or the number of bytes
 * used / required on success.
 */
int
ext3_xattr_get(struct inode *inode, int name_index, const char *name,
	       void *buffer, size_t buffer_size)
{
	int error;

	down_read(&EXT3_I(inode)->xattr_sem);
	error = ext3_xattr_ibody_get(inode, name_index, name, buffer,
				     buffer_size);
	if (error == -ENODATA)
		error = ext3_xattr_block_get(inode, name_index, name, buffer,
					     buffer_size);
	up_read(&EXT3_I(inode)->xattr_sem);
	return error;
}

static int
ext3_xattr_list_entries(struct dentry *dentry, struct ext3_xattr_entry *entry,
			char *buffer, size_t buffer_size)
{
	size_t rest = buffer_size;

	for (; !IS_LAST_ENTRY(entry); entry = EXT3_XATTR_NEXT(entry)) {
		const struct xattr_handler *handler =
			ext3_xattr_handler(entry->e_name_index);

		if (handler) {
			size_t size = handler->list(dentry, buffer, rest,
						    entry->e_name,
						    entry->e_name_len,
						    handler->flags);
			if (buffer) {
				if (size > rest)
					return -ERANGE;
				buffer += size;
			}
			rest -= size;
		}
	}
	return buffer_size - rest;
}

static int
ext3_xattr_block_list(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	struct inode *inode = d_inode(dentry);
	struct buffer_head *bh = NULL;
	int error;

	ea_idebug(inode, "buffer=%p, buffer_size=%ld",
		  buffer, (long)buffer_size);

	error = 0;
	if (!EXT3_I(inode)->i_file_acl)
		goto cleanup;
	ea_idebug(inode, "reading block %u", EXT3_I(inode)->i_file_acl);
	bh = sb_bread(inode->i_sb, EXT3_I(inode)->i_file_acl);
	error = -EIO;
	if (!bh)
		goto cleanup;
	ea_bdebug(bh, "b_count=%d, refcount=%d",
		atomic_read(&(bh->b_count)), le32_to_cpu(BHDR(bh)->h_refcount));
	if (ext3_xattr_check_block(bh)) {
		ext3_error(inode->i_sb, __func__,
			   "inode %lu: bad block "E3FSBLK, inode->i_ino,
			   EXT3_I(inode)->i_file_acl);
		error = -EIO;
		goto cleanup;
	}
	ext3_xattr_cache_insert(bh);
	error = ext3_xattr_list_entries(dentry, BFIRST(bh), buffer, buffer_size);

cleanup:
	brelse(bh);

	return error;
}

static int
ext3_xattr_ibody_list(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	struct inode *inode = d_inode(dentry);
	struct ext3_xattr_ibody_header *header;
	struct ext3_inode *raw_inode;
	struct ext3_iloc iloc;
	void *end;
	int error;

	if (!ext3_test_inode_state(inode, EXT3_STATE_XATTR))
		return 0;
	error = ext3_get_inode_loc(inode, &iloc);
	if (error)
		return error;
	raw_inode = ext3_raw_inode(&iloc);
	header = IHDR(inode, raw_inode);
	end = (void *)raw_inode + EXT3_SB(inode->i_sb)->s_inode_size;
	error = ext3_xattr_check_names(IFIRST(header), end);
	if (error)
		goto cleanup;
	error = ext3_xattr_list_entries(dentry, IFIRST(header),
					buffer, buffer_size);

cleanup:
	brelse(iloc.bh);
	return error;
}

/*
 * ext3_xattr_list()
 *
 * Copy a list of attribute names into the buffer
 * provided, or compute the buffer size required.
 * Buffer is NULL to compute the size of the buffer required.
 *
 * Returns a negative error number on failure, or the number of bytes
 * used / required on success.
 */
static int
ext3_xattr_list(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	int i_error, b_error;

	down_read(&EXT3_I(d_inode(dentry))->xattr_sem);
	i_error = ext3_xattr_ibody_list(dentry, buffer, buffer_size);
	if (i_error < 0) {
		b_error = 0;
	} else {
		if (buffer) {
			buffer += i_error;
			buffer_size -= i_error;
		}
		b_error = ext3_xattr_block_list(dentry, buffer, buffer_size);
		if (b_error < 0)
			i_error = 0;
	}
	up_read(&EXT3_I(d_inode(dentry))->xattr_sem);
	return i_error + b_error;
}

/*
 * If the EXT3_FEATURE_COMPAT_EXT_ATTR feature of this file system is
 * not set, set it.
 */
static void ext3_xattr_update_super_block(handle_t *handle,
					  struct super_block *sb)
{
	if (EXT3_HAS_COMPAT_FEATURE(sb, EXT3_FEATURE_COMPAT_EXT_ATTR))
		return;

	if (ext3_journal_get_write_access(handle, EXT3_SB(sb)->s_sbh) == 0) {
		EXT3_SET_COMPAT_FEATURE(sb, EXT3_FEATURE_COMPAT_EXT_ATTR);
		ext3_journal_dirty_metadata(handle, EXT3_SB(sb)->s_sbh);
	}
}

/*
 * Release the xattr block BH: If the reference count is > 1, decrement
 * it; otherwise free the block.
 */
static void
ext3_xattr_release_block(handle_t *handle, struct inode *inode,
			 struct buffer_head *bh)
{
	struct mb_cache_entry *ce = NULL;
	int error = 0;

	ce = mb_cache_entry_get(ext3_xattr_cache, bh->b_bdev, bh->b_blocknr);
	error = ext3_journal_get_write_access(handle, bh);
	if (error)
		 goto out;

	lock_buffer(bh);

	if (BHDR(bh)->h_refcount == cpu_to_le32(1)) {
		ea_bdebug(bh, "refcount now=0; freeing");
		if (ce)
			mb_cache_entry_free(ce);
		ext3_free_blocks(handle, inode, bh->b_blocknr, 1);
		get_bh(bh);
		ext3_forget(handle, 1, inode, bh, bh->b_blocknr);
	} else {
		le32_add_cpu(&BHDR(bh)->h_refcount, -1);
		error = ext3_journal_dirty_metadata(handle, bh);
		if (IS_SYNC(inode))
			handle->h_sync = 1;
		dquot_free_block(inode, 1);
		ea_bdebug(bh, "refcount now=%d; releasing",
			  le32_to_cpu(BHDR(bh)->h_refcount));
		if (ce)
			mb_cache_entry_release(ce);
	}
	unlock_buffer(bh);
out:
	ext3_std_error(inode->i_sb, error);
	return;
}

struct ext3_xattr_info {
	int name_index;
	const char *name;
	const void *value;
	size_t value_len;
};

struct ext3_xattr_search {
	struct ext3_xattr_entry *first;
	void *base;
	void *end;
	struct ext3_xattr_entry *here;
	int not_found;
};

static int
ext3_xattr_set_entry(struct ext3_xattr_info *i, struct ext3_xattr_search *s)
{
	struct ext3_xattr_entry *last;
	size_t free, min_offs = s->end - s->base, name_len = strlen(i->name);

	/* Compute min_offs and last. */
	last = s->first;
	for (; !IS_LAST_ENTRY(last); last = EXT3_XATTR_NEXT(last)) {
		if (!last->e_value_block && last->e_value_size) {
			size_t offs = le16_to_cpu(last->e_value_offs);
			if (offs < min_offs)
				min_offs = offs;
		}
	}
	free = min_offs - ((void *)last - s->base) - sizeof(__u32);
	if (!s->not_found) {
		if (!s->here->e_value_block && s->here->e_value_size) {
			size_t size = le32_to_cpu(s->here->e_value_size);
			free += EXT3_XATTR_SIZE(size);
		}
		free += EXT3_XATTR_LEN(name_len);
	}
	if (i->value) {
		if (free < EXT3_XATTR_LEN(name_len) +
			   EXT3_XATTR_SIZE(i->value_len))
			return -ENOSPC;
	}

	if (i->value && s->not_found) {
		/* Insert the new name. */
		size_t size = EXT3_XATTR_LEN(name_len);
		size_t rest = (void *)last - (void *)s->here + sizeof(__u32);
		memmove((void *)s->here + size, s->here, rest);
		memset(s->here, 0, size);
		s->here->e_name_index = i->name_index;
		s->here->e_name_len = name_len;
		memcpy(s->here->e_name, i->name, name_len);
	} else {
		if (!s->here->e_value_block && s->here->e_value_size) {
			void *first_val = s->base + min_offs;
			size_t offs = le16_to_cpu(s->here->e_value_offs);
			void *val = s->base + offs;
			size_t size = EXT3_XATTR_SIZE(
				le32_to_cpu(s->here->e_value_size));

			if (i->value && size == EXT3_XATTR_SIZE(i->value_len)) {
				/* The old and the new value have the same
				   size. Just replace. */
				s->here->e_value_size =
					cpu_to_le32(i->value_len);
				memset(val + size - EXT3_XATTR_PAD, 0,
				       EXT3_XATTR_PAD); /* Clear pad bytes. */
				memcpy(val, i->value, i->value_len);
				return 0;
			}

			/* Remove the old value. */
			memmove(first_val + size, first_val, val - first_val);
			memset(first_val, 0, size);
			s->here->e_value_size = 0;
			s->here->e_value_offs = 0;
			min_offs += size;

			/* Adjust all value offsets. */
			last = s->first;
			while (!IS_LAST_ENTRY(last)) {
				size_t o = le16_to_cpu(last->e_value_offs);
				if (!last->e_value_block &&
				    last->e_value_size && o < offs)
					last->e_value_offs =
						cpu_to_le16(o + size);
				last = EXT3_XATTR_NEXT(last);
			}
		}
		if (!i->value) {
			/* Remove the old name. */
			size_t size = EXT3_XATTR_LEN(name_len);
			last = ENTRY((void *)last - size);
			memmove(s->here, (void *)s->here + size,
				(void *)last - (void *)s->here + sizeof(__u32));
			memset(last, 0, size);
		}
	}

	if (i->value) {
		/* Insert the new value. */
		s->here->e_value_size = cpu_to_le32(i->value_len);
		if (i->value_len) {
			size_t size = EXT3_XATTR_SIZE(i->value_len);
			void *val = s->base + min_offs - size;
			s->here->e_value_offs = cpu_to_le16(min_offs - size);
			memset(val + size - EXT3_XATTR_PAD, 0,
			       EXT3_XATTR_PAD); /* Clear the pad bytes. */
			memcpy(val, i->value, i->value_len);
		}
	}
	return 0;
}

struct ext3_xattr_block_find {
	struct ext3_xattr_search s;
	struct buffer_head *bh;
};

static int
ext3_xattr_block_find(struct inode *inode, struct ext3_xattr_info *i,
		      struct ext3_xattr_block_find *bs)
{
	struct super_block *sb = inode->i_sb;
	int error;

	ea_idebug(inode, "name=%d.%s, value=%p, value_len=%ld",
		  i->name_index, i->name, i->value, (long)i->value_len);

	if (EXT3_I(inode)->i_file_acl) {
		/* The inode already has an extended attribute block. */
		bs->bh = sb_bread(sb, EXT3_I(inode)->i_file_acl);
		error = -EIO;
		if (!bs->bh)
			goto cleanup;
		ea_bdebug(bs->bh, "b_count=%d, refcount=%d",
			atomic_read(&(bs->bh->b_count)),
			le32_to_cpu(BHDR(bs->bh)->h_refcount));
		if (ext3_xattr_check_block(bs->bh)) {
			ext3_error(sb, __func__,
				"inode %lu: bad block "E3FSBLK, inode->i_ino,
				EXT3_I(inode)->i_file_acl);
			error = -EIO;
			goto cleanup;
		}
		/* Find the named attribute. */
		bs->s.base = BHDR(bs->bh);
		bs->s.first = BFIRST(bs->bh);
		bs->s.end = bs->bh->b_data + bs->bh->b_size;
		bs->s.here = bs->s.first;
		error = ext3_xattr_find_entry(&bs->s.here, i->name_index,
					      i->name, bs->bh->b_size, 1);
		if (error && error != -ENODATA)
			goto cleanup;
		bs->s.not_found = error;
	}
	error = 0;

cleanup:
	return error;
}

static int
ext3_xattr_block_set(handle_t *handle, struct inode *inode,
		     struct ext3_xattr_info *i,
		     struct ext3_xattr_block_find *bs)
{
	struct super_block *sb = inode->i_sb;
	struct buffer_head *new_bh = NULL;
	struct ext3_xattr_search *s = &bs->s;
	struct mb_cache_entry *ce = NULL;
	int error = 0;

#define header(x) ((struct ext3_xattr_header *)(x))

	if (i->value && i->value_len > sb->s_blocksize)
		return -ENOSPC;
	if (s->base) {
		ce = mb_cache_entry_get(ext3_xattr_cache, bs->bh->b_bdev,
					bs->bh->b_blocknr);
		error = ext3_journal_get_write_access(handle, bs->bh);
		if (error)
			goto cleanup;
		lock_buffer(bs->bh);

		if (header(s->base)->h_refcount == cpu_to_le32(1)) {
			if (ce) {
				mb_cache_entry_free(ce);
				ce = NULL;
			}
			ea_bdebug(bs->bh, "modifying in-place");
			error = ext3_xattr_set_entry(i, s);
			if (!error) {
				if (!IS_LAST_ENTRY(s->first))
					ext3_xattr_rehash(header(s->base),
							  s->here);
				ext3_xattr_cache_insert(bs->bh);
			}
			unlock_buffer(bs->bh);
			if (error == -EIO)
				goto bad_block;
			if (!error)
				error = ext3_journal_dirty_metadata(handle,
								    bs->bh);
			if (error)
				goto cleanup;
			goto inserted;
		} else {
			int offset = (char *)s->here - bs->bh->b_data;

			unlock_buffer(bs->bh);
			journal_release_buffer(handle, bs->bh);

			if (ce) {
				mb_cache_entry_release(ce);
				ce = NULL;
			}
			ea_bdebug(bs->bh, "cloning");
			s->base = kmalloc(bs->bh->b_size, GFP_NOFS);
			error = -ENOMEM;
			if (s->base == NULL)
				goto cleanup;
			memcpy(s->base, BHDR(bs->bh), bs->bh->b_size);
			s->first = ENTRY(header(s->base)+1);
			header(s->base)->h_refcount = cpu_to_le32(1);
			s->here = ENTRY(s->base + offset);
			s->end = s->base + bs->bh->b_size;
		}
	} else {
		/* Allocate a buffer where we construct the new block. */
		s->base = kzalloc(sb->s_blocksize, GFP_NOFS);
		/* assert(header == s->base) */
		error = -ENOMEM;
		if (s->base == NULL)
			goto cleanup;
		header(s->base)->h_magic = cpu_to_le32(EXT3_XATTR_MAGIC);
		header(s->base)->h_blocks = cpu_to_le32(1);
		header(s->base)->h_refcount = cpu_to_le32(1);
		s->first = ENTRY(header(s->base)+1);
		s->here = ENTRY(header(s->base)+1);
		s->end = s->base + sb->s_blocksize;
	}

	error = ext3_xattr_set_entry(i, s);
	if (error == -EIO)
		goto bad_block;
	if (error)
		goto cleanup;
	if (!IS_LAST_ENTRY(s->first))
		ext3_xattr_rehash(header(s->base), s->here);

inserted:
	if (!IS_LAST_ENTRY(s->first)) {
		new_bh = ext3_xattr_cache_find(inode, header(s->base), &ce);
		if (new_bh) {
			/* We found an identical block in the cache. */
			if (new_bh == bs->bh)
				ea_bdebug(new_bh, "keeping");
			else {
				/* The old block is released after updating
				   the inode. */
				error = dquot_alloc_block(inode, 1);
				if (error)
					goto cleanup;
				error = ext3_journal_get_write_access(handle,
								      new_bh);
				if (error)
					goto cleanup_dquot;
				lock_buffer(new_bh);
				le32_add_cpu(&BHDR(new_bh)->h_refcount, 1);
				ea_bdebug(new_bh, "reusing; refcount now=%d",
					le32_to_cpu(BHDR(new_bh)->h_refcount));
				unlock_buffer(new_bh);
				error = ext3_journal_dirty_metadata(handle,
								    new_bh);
				if (error)
					goto cleanup_dquot;
			}
			mb_cache_entry_release(ce);
			ce = NULL;
		} else if (bs->bh && s->base == bs->bh->b_data) {
			/* We were modifying this block in-place. */
			ea_bdebug(bs->bh, "keeping this block");
			new_bh = bs->bh;
			get_bh(new_bh);
		} else {
			/* We need to allocate a new block */
			ext3_fsblk_t goal = ext3_group_first_block_no(sb,
						EXT3_I(inode)->i_block_group);
			ext3_fsblk_t block;

			/*
			 * Protect us agaist concurrent allocations to the
			 * same inode from ext3_..._writepage(). Reservation
			 * code does not expect racing allocations.
			 */
			mutex_lock(&EXT3_I(inode)->truncate_mutex);
			block = ext3_new_block(handle, inode, goal, &error);
			mutex_unlock(&EXT3_I(inode)->truncate_mutex);
			if (error)
				goto cleanup;
			ea_idebug(inode, "creating block %d", block);

			new_bh = sb_getblk(sb, block);
			if (unlikely(!new_bh)) {
getblk_failed:
				ext3_free_blocks(handle, inode, block, 1);
				error = -ENOMEM;
				goto cleanup;
			}
			lock_buffer(new_bh);
			error = ext3_journal_get_create_access(handle, new_bh);
			if (error) {
				unlock_buffer(new_bh);
				goto getblk_failed;
			}
			memcpy(new_bh->b_data, s->base, new_bh->b_size);
			set_buffer_uptodate(new_bh);
			unlock_buffer(new_bh);
			ext3_xattr_cache_insert(new_bh);
			error = ext3_journal_dirty_metadata(handle, new_bh);
			if (error)
				goto cleanup;
		}
	}

	/* Update the inode. */
	EXT3_I(inode)->i_file_acl = new_bh ? new_bh->b_blocknr : 0;

	/* Drop the previous xattr block. */
	if (bs->bh && bs->bh != new_bh)
		ext3_xattr_release_block(handle, inode, bs->bh);
	error = 0;

cleanup:
	if (ce)
		mb_cache_entry_release(ce);
	brelse(new_bh);
	if (!(bs->bh && s->base == bs->bh->b_data))
		kfree(s->base);

	return error;

cleanup_dquot:
	dquot_free_block(inode, 1);
	goto cleanup;

bad_block:
	ext3_error(inode->i_sb, __func__,
		   "inode %lu: bad block "E3FSBLK, inode->i_ino,
		   EXT3_I(inode)->i_file_acl);
	goto cleanup;

#undef header
}

struct ext3_xattr_ibody_find {
	struct ext3_xattr_search s;
	struct ext3_iloc iloc;
};

static int
ext3_xattr_ibody_find(struct inode *inode, struct ext3_xattr_info *i,
		      struct ext3_xattr_ibody_find *is)
{
	struct ext3_xattr_ibody_header *header;
	struct ext3_inode *raw_inode;
	int error;

	if (EXT3_I(inode)->i_extra_isize == 0)
		return 0;
	raw_inode = ext3_raw_inode(&is->iloc);
	header = IHDR(inode, raw_inode);
	is->s.base = is->s.first = IFIRST(header);
	is->s.here = is->s.first;
	is->s.end = (void *)raw_inode + EXT3_SB(inode->i_sb)->s_inode_size;
	if (ext3_test_inode_state(inode, EXT3_STATE_XATTR)) {
		error = ext3_xattr_check_names(IFIRST(header), is->s.end);
		if (error)
			return error;
		/* Find the named attribute. */
		error = ext3_xattr_find_entry(&is->s.here, i->name_index,
					      i->name, is->s.end -
					      (void *)is->s.base, 0);
		if (error && error != -ENODATA)
			return error;
		is->s.not_found = error;
	}
	return 0;
}

static int
ext3_xattr_ibody_set(handle_t *handle, struct inode *inode,
		     struct ext3_xattr_info *i,
		     struct ext3_xattr_ibody_find *is)
{
	struct ext3_xattr_ibody_header *header;
	struct ext3_xattr_search *s = &is->s;
	int error;

	if (EXT3_I(inode)->i_extra_isize == 0)
		return -ENOSPC;
	error = ext3_xattr_set_entry(i, s);
	if (error)
		return error;
	header = IHDR(inode, ext3_raw_inode(&is->iloc));
	if (!IS_LAST_ENTRY(s->first)) {
		header->h_magic = cpu_to_le32(EXT3_XATTR_MAGIC);
		ext3_set_inode_state(inode, EXT3_STATE_XATTR);
	} else {
		header->h_magic = cpu_to_le32(0);
		ext3_clear_inode_state(inode, EXT3_STATE_XATTR);
	}
	return 0;
}

/*
 * ext3_xattr_set_handle()
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
ext3_xattr_set_handle(handle_t *handle, struct inode *inode, int name_index,
		      const char *name, const void *value, size_t value_len,
		      int flags)
{
	struct ext3_xattr_info i = {
		.name_index = name_index,
		.name = name,
		.value = value,
		.value_len = value_len,

	};
	struct ext3_xattr_ibody_find is = {
		.s = { .not_found = -ENODATA, },
	};
	struct ext3_xattr_block_find bs = {
		.s = { .not_found = -ENODATA, },
	};
	int error;

	if (!name)
		return -EINVAL;
	if (strlen(name) > 255)
		return -ERANGE;
	down_write(&EXT3_I(inode)->xattr_sem);
	error = ext3_get_inode_loc(inode, &is.iloc);
	if (error)
		goto cleanup;

	error = ext3_journal_get_write_access(handle, is.iloc.bh);
	if (error)
		goto cleanup;

	if (ext3_test_inode_state(inode, EXT3_STATE_NEW)) {
		struct ext3_inode *raw_inode = ext3_raw_inode(&is.iloc);
		memset(raw_inode, 0, EXT3_SB(inode->i_sb)->s_inode_size);
		ext3_clear_inode_state(inode, EXT3_STATE_NEW);
	}

	error = ext3_xattr_ibody_find(inode, &i, &is);
	if (error)
		goto cleanup;
	if (is.s.not_found)
		error = ext3_xattr_block_find(inode, &i, &bs);
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
			error = ext3_xattr_ibody_set(handle, inode, &i, &is);
		else if (!bs.s.not_found)
			error = ext3_xattr_block_set(handle, inode, &i, &bs);
	} else {
		error = ext3_xattr_ibody_set(handle, inode, &i, &is);
		if (!error && !bs.s.not_found) {
			i.value = NULL;
			error = ext3_xattr_block_set(handle, inode, &i, &bs);
		} else if (error == -ENOSPC) {
			if (EXT3_I(inode)->i_file_acl && !bs.s.base) {
				error = ext3_xattr_block_find(inode, &i, &bs);
				if (error)
					goto cleanup;
			}
			error = ext3_xattr_block_set(handle, inode, &i, &bs);
			if (error)
				goto cleanup;
			if (!is.s.not_found) {
				i.value = NULL;
				error = ext3_xattr_ibody_set(handle, inode, &i,
							     &is);
			}
		}
	}
	if (!error) {
		ext3_xattr_update_super_block(handle, inode->i_sb);
		inode->i_ctime = CURRENT_TIME_SEC;
		error = ext3_mark_iloc_dirty(handle, inode, &is.iloc);
		/*
		 * The bh is consumed by ext3_mark_iloc_dirty, even with
		 * error != 0.
		 */
		is.iloc.bh = NULL;
		if (IS_SYNC(inode))
			handle->h_sync = 1;
	}

cleanup:
	brelse(is.iloc.bh);
	brelse(bs.bh);
	up_write(&EXT3_I(inode)->xattr_sem);
	return error;
}

/*
 * ext3_xattr_set()
 *
 * Like ext3_xattr_set_handle, but start from an inode. This extended
 * attribute modification is a filesystem transaction by itself.
 *
 * Returns 0, or a negative error number on failure.
 */
int
ext3_xattr_set(struct inode *inode, int name_index, const char *name,
	       const void *value, size_t value_len, int flags)
{
	handle_t *handle;
	int error, retries = 0;

retry:
	handle = ext3_journal_start(inode, EXT3_DATA_TRANS_BLOCKS(inode->i_sb));
	if (IS_ERR(handle)) {
		error = PTR_ERR(handle);
	} else {
		int error2;

		error = ext3_xattr_set_handle(handle, inode, name_index, name,
					      value, value_len, flags);
		error2 = ext3_journal_stop(handle);
		if (error == -ENOSPC &&
		    ext3_should_retry_alloc(inode->i_sb, &retries))
			goto retry;
		if (error == 0)
			error = error2;
	}

	return error;
}

/*
 * ext3_xattr_delete_inode()
 *
 * Free extended attribute resources associated with this inode. This
 * is called immediately before an inode is freed. We have exclusive
 * access to the inode.
 */
void
ext3_xattr_delete_inode(handle_t *handle, struct inode *inode)
{
	struct buffer_head *bh = NULL;

	if (!EXT3_I(inode)->i_file_acl)
		goto cleanup;
	bh = sb_bread(inode->i_sb, EXT3_I(inode)->i_file_acl);
	if (!bh) {
		ext3_error(inode->i_sb, __func__,
			"inode %lu: block "E3FSBLK" read error", inode->i_ino,
			EXT3_I(inode)->i_file_acl);
		goto cleanup;
	}
	if (BHDR(bh)->h_magic != cpu_to_le32(EXT3_XATTR_MAGIC) ||
	    BHDR(bh)->h_blocks != cpu_to_le32(1)) {
		ext3_error(inode->i_sb, __func__,
			"inode %lu: bad block "E3FSBLK, inode->i_ino,
			EXT3_I(inode)->i_file_acl);
		goto cleanup;
	}
	ext3_xattr_release_block(handle, inode, bh);
	EXT3_I(inode)->i_file_acl = 0;

cleanup:
	brelse(bh);
}

/*
 * ext3_xattr_put_super()
 *
 * This is called when a file system is unmounted.
 */
void
ext3_xattr_put_super(struct super_block *sb)
{
	mb_cache_shrink(sb->s_bdev);
}

/*
 * ext3_xattr_cache_insert()
 *
 * Create a new entry in the extended attribute cache, and insert
 * it unless such an entry is already in the cache.
 *
 * Returns 0, or a negative error number on failure.
 */
static void
ext3_xattr_cache_insert(struct buffer_head *bh)
{
	__u32 hash = le32_to_cpu(BHDR(bh)->h_hash);
	struct mb_cache_entry *ce;
	int error;

	ce = mb_cache_entry_alloc(ext3_xattr_cache, GFP_NOFS);
	if (!ce) {
		ea_bdebug(bh, "out of memory");
		return;
	}
	error = mb_cache_entry_insert(ce, bh->b_bdev, bh->b_blocknr, hash);
	if (error) {
		mb_cache_entry_free(ce);
		if (error == -EBUSY) {
			ea_bdebug(bh, "already in cache");
			error = 0;
		}
	} else {
		ea_bdebug(bh, "inserting [%x]", (int)hash);
		mb_cache_entry_release(ce);
	}
}

/*
 * ext3_xattr_cmp()
 *
 * Compare two extended attribute blocks for equality.
 *
 * Returns 0 if the blocks are equal, 1 if they differ, and
 * a negative error number on errors.
 */
static int
ext3_xattr_cmp(struct ext3_xattr_header *header1,
	       struct ext3_xattr_header *header2)
{
	struct ext3_xattr_entry *entry1, *entry2;

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

		entry1 = EXT3_XATTR_NEXT(entry1);
		entry2 = EXT3_XATTR_NEXT(entry2);
	}
	if (!IS_LAST_ENTRY(entry2))
		return 1;
	return 0;
}

/*
 * ext3_xattr_cache_find()
 *
 * Find an identical extended attribute block.
 *
 * Returns a pointer to the block found, or NULL if such a block was
 * not found or an error occurred.
 */
static struct buffer_head *
ext3_xattr_cache_find(struct inode *inode, struct ext3_xattr_header *header,
		      struct mb_cache_entry **pce)
{
	__u32 hash = le32_to_cpu(header->h_hash);
	struct mb_cache_entry *ce;

	if (!header->h_hash)
		return NULL;  /* never share */
	ea_idebug(inode, "looking for cached blocks [%x]", (int)hash);
again:
	ce = mb_cache_entry_find_first(ext3_xattr_cache, inode->i_sb->s_bdev,
				       hash);
	while (ce) {
		struct buffer_head *bh;

		if (IS_ERR(ce)) {
			if (PTR_ERR(ce) == -EAGAIN)
				goto again;
			break;
		}
		bh = sb_bread(inode->i_sb, ce->e_block);
		if (!bh) {
			ext3_error(inode->i_sb, __func__,
				"inode %lu: block %lu read error",
				inode->i_ino, (unsigned long) ce->e_block);
		} else if (le32_to_cpu(BHDR(bh)->h_refcount) >=
				EXT3_XATTR_REFCOUNT_MAX) {
			ea_idebug(inode, "block %lu refcount %d>=%d",
				  (unsigned long) ce->e_block,
				  le32_to_cpu(BHDR(bh)->h_refcount),
					  EXT3_XATTR_REFCOUNT_MAX);
		} else if (ext3_xattr_cmp(header, BHDR(bh)) == 0) {
			*pce = ce;
			return bh;
		}
		brelse(bh);
		ce = mb_cache_entry_find_next(ce, inode->i_sb->s_bdev, hash);
	}
	return NULL;
}

#define NAME_HASH_SHIFT 5
#define VALUE_HASH_SHIFT 16

/*
 * ext3_xattr_hash_entry()
 *
 * Compute the hash of an extended attribute.
 */
static inline void ext3_xattr_hash_entry(struct ext3_xattr_header *header,
					 struct ext3_xattr_entry *entry)
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
		     EXT3_XATTR_ROUND) >> EXT3_XATTR_PAD_BITS; n; n--) {
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
 * ext3_xattr_rehash()
 *
 * Re-compute the extended attribute hash value after an entry has changed.
 */
static void ext3_xattr_rehash(struct ext3_xattr_header *header,
			      struct ext3_xattr_entry *entry)
{
	struct ext3_xattr_entry *here;
	__u32 hash = 0;

	ext3_xattr_hash_entry(header, entry);
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
		here = EXT3_XATTR_NEXT(here);
	}
	header->h_hash = cpu_to_le32(hash);
}

#undef BLOCK_HASH_SHIFT

int __init
init_ext3_xattr(void)
{
	ext3_xattr_cache = mb_cache_create("ext3_xattr", 6);
	if (!ext3_xattr_cache)
		return -ENOMEM;
	return 0;
}

void
exit_ext3_xattr(void)
{
	if (ext3_xattr_cache)
		mb_cache_destroy(ext3_xattr_cache);
	ext3_xattr_cache = NULL;
}

/* ---- EXT3 user xattr handler (merged into this translation unit) ---- */
/*
 * linux/fs/ext3/xattr_user.c
 * Handler for extended user attributes.
 *
 * Copyright (C) 2001 by Andreas Gruenbacher, <a.gruenbacher@computer.org>
 */


static size_t
ext3_xattr_user_list(struct dentry *dentry, char *list, size_t list_size,
		const char *name, size_t name_len, int type)
{
	const size_t prefix_len = XATTR_USER_PREFIX_LEN;
	const size_t total_len = prefix_len + name_len + 1;

	if (!test_opt(dentry->d_sb, XATTR_USER))
		return 0;

	if (list && total_len <= list_size) {
		memcpy(list, XATTR_USER_PREFIX, prefix_len);
		memcpy(list+prefix_len, name, name_len);
		list[prefix_len + name_len] = '\0';
	}
	return total_len;
}

static int
ext3_xattr_user_get(struct dentry *dentry, const char *name, void *buffer,
		size_t size, int type)
{
	if (strcmp(name, "") == 0)
		return -EINVAL;
	if (!test_opt(dentry->d_sb, XATTR_USER))
		return -EOPNOTSUPP;
	return ext3_xattr_get(d_inode(dentry), EXT3_XATTR_INDEX_USER,
			      name, buffer, size);
}

static int
ext3_xattr_user_set(struct dentry *dentry, const char *name,
		const void *value, size_t size, int flags, int type)
{
	if (strcmp(name, "") == 0)
		return -EINVAL;
	if (!test_opt(dentry->d_sb, XATTR_USER))
		return -EOPNOTSUPP;
	return ext3_xattr_set(d_inode(dentry), EXT3_XATTR_INDEX_USER,
			      name, value, size, flags);
}

const struct xattr_handler ext3_xattr_user_handler = {
	.prefix	= XATTR_USER_PREFIX,
	.list	= ext3_xattr_user_list,
	.get	= ext3_xattr_user_get,
	.set	= ext3_xattr_user_set,
};

/* ---- EXT3 trusted xattr handler (merged into this translation unit) ---- */
/*
 * linux/fs/ext3/xattr_trusted.c
 * Handler for trusted extended attributes.
 *
 * Copyright (C) 2003 by Andreas Gruenbacher, <a.gruenbacher@computer.org>
 */


static size_t
ext3_xattr_trusted_list(struct dentry *dentry, char *list, size_t list_size,
		const char *name, size_t name_len, int type)
{
	const size_t prefix_len = XATTR_TRUSTED_PREFIX_LEN;
	const size_t total_len = prefix_len + name_len + 1;

	if (!capable(CAP_SYS_ADMIN))
		return 0;

	if (list && total_len <= list_size) {
		memcpy(list, XATTR_TRUSTED_PREFIX, prefix_len);
		memcpy(list+prefix_len, name, name_len);
		list[prefix_len + name_len] = '\0';
	}
	return total_len;
}

static int
ext3_xattr_trusted_get(struct dentry *dentry, const char *name,
		       void *buffer, size_t size, int type)
{
	if (strcmp(name, "") == 0)
		return -EINVAL;
	return ext3_xattr_get(d_inode(dentry), EXT3_XATTR_INDEX_TRUSTED,
			      name, buffer, size);
}

static int
ext3_xattr_trusted_set(struct dentry *dentry, const char *name,
		const void *value, size_t size, int flags, int type)
{
	if (strcmp(name, "") == 0)
		return -EINVAL;
	return ext3_xattr_set(d_inode(dentry), EXT3_XATTR_INDEX_TRUSTED, name,
			      value, size, flags);
}

const struct xattr_handler ext3_xattr_trusted_handler = {
	.prefix	= XATTR_TRUSTED_PREFIX,
	.list	= ext3_xattr_trusted_list,
	.get	= ext3_xattr_trusted_get,
	.set	= ext3_xattr_trusted_set,
};

/* ---- EXT3 security xattr handler (merged into this translation unit) ---- */
/*
 * linux/fs/ext3/xattr_security.c
 * Handler for storing security labels as extended attributes.
 */


static size_t
ext3_xattr_security_list(struct dentry *dentry, char *list, size_t list_size,
			 const char *name, size_t name_len, int type)
{
	const size_t prefix_len = XATTR_SECURITY_PREFIX_LEN;
	const size_t total_len = prefix_len + name_len + 1;


	if (list && total_len <= list_size) {
		memcpy(list, XATTR_SECURITY_PREFIX, prefix_len);
		memcpy(list+prefix_len, name, name_len);
		list[prefix_len + name_len] = '\0';
	}
	return total_len;
}

static int
ext3_xattr_security_get(struct dentry *dentry, const char *name,
		void *buffer, size_t size, int type)
{
	if (strcmp(name, "") == 0)
		return -EINVAL;
	return ext3_xattr_get(d_inode(dentry), EXT3_XATTR_INDEX_SECURITY,
			      name, buffer, size);
}

static int
ext3_xattr_security_set(struct dentry *dentry, const char *name,
		const void *value, size_t size, int flags, int type)
{
	if (strcmp(name, "") == 0)
		return -EINVAL;
	return ext3_xattr_set(d_inode(dentry), EXT3_XATTR_INDEX_SECURITY,
			      name, value, size, flags);
}

static int ext3_initxattrs(struct inode *inode,
			   const struct xattr *xattr_array,
			   void *fs_info)
{
	const struct xattr *xattr;
	handle_t *handle = fs_info;
	int err = 0;

	for (xattr = xattr_array; xattr->name != NULL; xattr++) {
		err = ext3_xattr_set_handle(handle, inode,
					    EXT3_XATTR_INDEX_SECURITY,
					    xattr->name, xattr->value,
					    xattr->value_len, 0);
		if (err < 0)
			break;
	}
	return err;
}

int
ext3_init_security(handle_t *handle, struct inode *inode, struct inode *dir,
		   const struct qstr *qstr)
{
	return security_inode_init_security(inode, dir, qstr,
					    &ext3_initxattrs, handle);
}

const struct xattr_handler ext3_xattr_security_handler = {
	.prefix	= XATTR_SECURITY_PREFIX,
	.list	= ext3_xattr_security_list,
	.get	= ext3_xattr_security_get,
	.set	= ext3_xattr_security_set,
};

/* ---- EXT3 POSIX ACL implementation (merged into this translation unit) ---- */
/*
 * linux/fs/ext3/acl.c
 *
 * Copyright (C) 2001-2003 Andreas Gruenbacher, <agruen@suse.de>
 */


/*
 * Convert from filesystem to in-memory representation.
 */
static struct posix_acl *
ext3_acl_from_disk(const void *value, size_t size)
{
	const char *end = (char *)value + size;
	int n, count;
	struct posix_acl *acl;

	if (!value)
		return NULL;
	if (size < sizeof(ext3_acl_header))
		 return ERR_PTR(-EINVAL);
	if (((ext3_acl_header *)value)->a_version !=
	    cpu_to_le32(EXT3_ACL_VERSION))
		return ERR_PTR(-EINVAL);
	value = (char *)value + sizeof(ext3_acl_header);
	count = ext3_acl_count(size);
	if (count < 0)
		return ERR_PTR(-EINVAL);
	if (count == 0)
		return NULL;
	acl = posix_acl_alloc(count, GFP_NOFS);
	if (!acl)
		return ERR_PTR(-ENOMEM);
	for (n=0; n < count; n++) {
		ext3_acl_entry *entry =
			(ext3_acl_entry *)value;
		if ((char *)value + sizeof(ext3_acl_entry_short) > end)
			goto fail;
		acl->a_entries[n].e_tag  = le16_to_cpu(entry->e_tag);
		acl->a_entries[n].e_perm = le16_to_cpu(entry->e_perm);
		switch(acl->a_entries[n].e_tag) {
			case ACL_USER_OBJ:
			case ACL_GROUP_OBJ:
			case ACL_MASK:
			case ACL_OTHER:
				value = (char *)value +
					sizeof(ext3_acl_entry_short);
				break;

			case ACL_USER:
				value = (char *)value + sizeof(ext3_acl_entry);
				if ((char *)value > end)
					goto fail;
				acl->a_entries[n].e_uid =
					make_kuid(&init_user_ns,
						  le32_to_cpu(entry->e_id));
				break;
			case ACL_GROUP:
				value = (char *)value + sizeof(ext3_acl_entry);
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
ext3_acl_to_disk(const struct posix_acl *acl, size_t *size)
{
	ext3_acl_header *ext_acl;
	char *e;
	size_t n;

	*size = ext3_acl_size(acl->a_count);
	ext_acl = kmalloc(sizeof(ext3_acl_header) + acl->a_count *
			sizeof(ext3_acl_entry), GFP_NOFS);
	if (!ext_acl)
		return ERR_PTR(-ENOMEM);
	ext_acl->a_version = cpu_to_le32(EXT3_ACL_VERSION);
	e = (char *)ext_acl + sizeof(ext3_acl_header);
	for (n=0; n < acl->a_count; n++) {
		const struct posix_acl_entry *acl_e = &acl->a_entries[n];
		ext3_acl_entry *entry = (ext3_acl_entry *)e;
		entry->e_tag  = cpu_to_le16(acl_e->e_tag);
		entry->e_perm = cpu_to_le16(acl_e->e_perm);
		switch(acl_e->e_tag) {
			case ACL_USER:
				entry->e_id = cpu_to_le32(
					from_kuid(&init_user_ns, acl_e->e_uid));
				e += sizeof(ext3_acl_entry);
				break;
			case ACL_GROUP:
				entry->e_id = cpu_to_le32(
					from_kgid(&init_user_ns, acl_e->e_gid));
				e += sizeof(ext3_acl_entry);
				break;

			case ACL_USER_OBJ:
			case ACL_GROUP_OBJ:
			case ACL_MASK:
			case ACL_OTHER:
				e += sizeof(ext3_acl_entry_short);
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
 * Inode operation get_posix_acl().
 *
 * inode->i_mutex: don't care
 */
struct posix_acl *
ext3_get_acl(struct inode *inode, int type)
{
	int name_index;
	char *value = NULL;
	struct posix_acl *acl;
	int retval;

	switch (type) {
	case ACL_TYPE_ACCESS:
		name_index = EXT3_XATTR_INDEX_POSIX_ACL_ACCESS;
		break;
	case ACL_TYPE_DEFAULT:
		name_index = EXT3_XATTR_INDEX_POSIX_ACL_DEFAULT;
		break;
	default:
		BUG();
	}

	retval = ext3_xattr_get(inode, name_index, "", NULL, 0);
	if (retval > 0) {
		value = kmalloc(retval, GFP_NOFS);
		if (!value)
			return ERR_PTR(-ENOMEM);
		retval = ext3_xattr_get(inode, name_index, "", value, retval);
	}
	if (retval > 0)
		acl = ext3_acl_from_disk(value, retval);
	else if (retval == -ENODATA || retval == -ENOSYS)
		acl = NULL;
	else
		acl = ERR_PTR(retval);
	kfree(value);

	if (!IS_ERR(acl))
		set_cached_acl(inode, type, acl);

	return acl;
}

/*
 * Set the access or default ACL of an inode.
 *
 * inode->i_mutex: down unless called from ext3_new_inode
 */
static int
__ext3_set_acl(handle_t *handle, struct inode *inode, int type,
	     struct posix_acl *acl)
{
	int name_index;
	void *value = NULL;
	size_t size = 0;
	int error;

	switch(type) {
		case ACL_TYPE_ACCESS:
			name_index = EXT3_XATTR_INDEX_POSIX_ACL_ACCESS;
			if (acl) {
				error = posix_acl_equiv_mode(acl, &inode->i_mode);
				if (error < 0)
					return error;
				else {
					inode->i_ctime = CURRENT_TIME_SEC;
					ext3_mark_inode_dirty(handle, inode);
					if (error == 0)
						acl = NULL;
				}
			}
			break;

		case ACL_TYPE_DEFAULT:
			name_index = EXT3_XATTR_INDEX_POSIX_ACL_DEFAULT;
			if (!S_ISDIR(inode->i_mode))
				return acl ? -EACCES : 0;
			break;

		default:
			return -EINVAL;
	}
	if (acl) {
		value = ext3_acl_to_disk(acl, &size);
		if (IS_ERR(value))
			return (int)PTR_ERR(value);
	}

	error = ext3_xattr_set_handle(handle, inode, name_index, "",
				      value, size, 0);

	kfree(value);

	if (!error)
		set_cached_acl(inode, type, acl);

	return error;
}

int
ext3_set_acl(struct inode *inode, struct posix_acl *acl, int type)
{
	handle_t *handle;
	int error, retries = 0;

retry:
	handle = ext3_journal_start(inode, EXT3_DATA_TRANS_BLOCKS(inode->i_sb));
	if (IS_ERR(handle))
		return PTR_ERR(handle);
	error = __ext3_set_acl(handle, inode, type, acl);
	ext3_journal_stop(handle);
	if (error == -ENOSPC && ext3_should_retry_alloc(inode->i_sb, &retries))
		goto retry;
	return error;
}

/*
 * Initialize the ACLs of a new inode. Called from ext3_new_inode.
 *
 * dir->i_mutex: down
 * inode->i_mutex: up (access to inode is still exclusive)
 */
int
ext3_init_acl(handle_t *handle, struct inode *inode, struct inode *dir)
{
	struct posix_acl *default_acl, *acl;
	int error;

	error = posix_acl_create(dir, &inode->i_mode, &default_acl, &acl);
	if (error)
		return error;

	if (default_acl) {
		error = __ext3_set_acl(handle, inode, ACL_TYPE_DEFAULT,
				       default_acl);
		posix_acl_release(default_acl);
	}
	if (acl) {
		if (!error)
			error = __ext3_set_acl(handle, inode, ACL_TYPE_ACCESS,
					       acl);
		posix_acl_release(acl);
	}
	return error;
}

/* ---- EXT3 private metadata-block cache (merged into this translation unit) ---- */
/*
 * linux/fs/mbcache.c
 * (C) 2001-2002 Andreas Gruenbacher, <a.gruenbacher@computer.org>
 */

/*
 * Filesystem Meta Information Block Cache (mbcache)
 *
 * The mbcache caches blocks of block devices that need to be located
 * by their device/block number, as well as by other criteria (such
 * as the block's contents).
 *
 * There can only be one cache entry in a cache per device and block number.
 * Additional indexes need not be unique in this sense. The number of
 * additional indexes (=other criteria) can be hardwired at compile time
 * or specified at cache create time.
 *
 * Each cache entry is of fixed size. An entry may be `valid' or `invalid'
 * in the cache. A valid entry is in the main hash tables of the cache,
 * and may also be in the lru list. An invalid entry is not in any hashes
 * or lists.
 *
 * A valid cache entry is only in the lru list if no handles refer to it.
 * Invalid cache entries will be freed when the last handle to the cache
 * entry is released. Entries that cannot be freed immediately are put
 * back on the lru list.
 */

/*
 * Lock descriptions and usage:
 *
 * Each hash chain of both the block and index hash tables now contains
 * a built-in lock used to serialize accesses to the hash chain.
 *
 * Accesses to global data structures mb_cache_list and mb_cache_lru_list
 * are serialized via the global spinlock mb_cache_spinlock.
 *
 * Each mb_cache_entry contains a spinlock, e_entry_lock, to serialize
 * accesses to its local data, such as e_used and e_queued.
 *
 * Lock ordering:
 *
 * Each block hash chain's lock has the highest lock order, followed by an
 * index hash chain's lock, mb_cache_bg_lock (used to implement mb_cache_entry's
 * lock), and mb_cach_spinlock, with the lowest order.  While holding
 * either a block or index hash chain lock, a thread can acquire an
 * mc_cache_bg_lock, which in turn can also acquire mb_cache_spinlock.
 *
 * Synchronization:
 *
 * Since both mb_cache_entry_get and mb_cache_entry_find scan the block and
 * index hash chian, it needs to lock the corresponding hash chain.  For each
 * mb_cache_entry within the chain, it needs to lock the mb_cache_entry to
 * prevent either any simultaneous release or free on the entry and also
 * to serialize accesses to either the e_used or e_queued member of the entry.
 *
 * To avoid having a dangling reference to an already freed
 * mb_cache_entry, an mb_cache_entry is only freed when it is not on a
 * block hash chain and also no longer being referenced, both e_used,
 * and e_queued are 0's.  When an mb_cache_entry is explicitly freed it is
 * first removed from a block hash chain.
 */



#ifdef MB_CACHE_DEBUG
# define mb_debug(f...) do { \
		printk(KERN_DEBUG f); \
		printk("\n"); \
	} while (0)
#define mb_assert(c) do { if (!(c)) \
		printk(KERN_ERR "assertion " #c " failed\n"); \
	} while(0)
#else
# define mb_debug(f...) do { } while(0)
# define mb_assert(c) do { } while(0)
#endif
#define mb_error(f...) do { \
		printk(KERN_ERR f); \
		printk("\n"); \
	} while(0)

#define MB_CACHE_WRITER ((unsigned short)~0U >> 1)

#define MB_CACHE_ENTRY_LOCK_BITS	ilog2(NR_BG_LOCKS)
#define	MB_CACHE_ENTRY_LOCK_INDEX(ce)			\
	(hash_long((unsigned long)ce, MB_CACHE_ENTRY_LOCK_BITS))

static DECLARE_WAIT_QUEUE_HEAD(mb_cache_queue);
static struct blockgroup_lock *mb_cache_bg_lock;
static struct kmem_cache *mb_cache_kmem_cache;

MODULE_AUTHOR("Andreas Gruenbacher <a.gruenbacher@computer.org>");
MODULE_DESCRIPTION("Meta block cache (for extended attributes)");
MODULE_LICENSE("GPL");

#if !defined(MB_CACHE_INDEXES_COUNT) || (MB_CACHE_INDEXES_COUNT > 0)
#endif

/*
 * Global data: list of all mbcache's, lru list, and a spinlock for
 * accessing cache data structures on SMP machines. The lru list is
 * global across all mbcaches.
 */

static LIST_HEAD(mb_cache_list);
static LIST_HEAD(mb_cache_lru_list);
static DEFINE_SPINLOCK(mb_cache_spinlock);

static inline void
__spin_lock_mb_cache_entry(struct mb_cache_entry *ce)
{
	spin_lock(bgl_lock_ptr(mb_cache_bg_lock,
		MB_CACHE_ENTRY_LOCK_INDEX(ce)));
}

static inline void
__spin_unlock_mb_cache_entry(struct mb_cache_entry *ce)
{
	spin_unlock(bgl_lock_ptr(mb_cache_bg_lock,
		MB_CACHE_ENTRY_LOCK_INDEX(ce)));
}

static inline int
__mb_cache_entry_is_block_hashed(struct mb_cache_entry *ce)
{
	return !hlist_bl_unhashed(&ce->e_block_list);
}


static inline void
__mb_cache_entry_unhash_block(struct mb_cache_entry *ce)
{
	if (__mb_cache_entry_is_block_hashed(ce))
		hlist_bl_del_init(&ce->e_block_list);
}

static inline int
__mb_cache_entry_is_index_hashed(struct mb_cache_entry *ce)
{
	return !hlist_bl_unhashed(&ce->e_index.o_list);
}

static inline void
__mb_cache_entry_unhash_index(struct mb_cache_entry *ce)
{
	if (__mb_cache_entry_is_index_hashed(ce))
		hlist_bl_del_init(&ce->e_index.o_list);
}

/*
 * __mb_cache_entry_unhash_unlock()
 *
 * This function is called to unhash both the block and index hash
 * chain.
 * It assumes both the block and index hash chain is locked upon entry.
 * It also unlock both hash chains both exit
 */
static inline void
__mb_cache_entry_unhash_unlock(struct mb_cache_entry *ce)
{
	__mb_cache_entry_unhash_index(ce);
	hlist_bl_unlock(ce->e_index_hash_p);
	__mb_cache_entry_unhash_block(ce);
	hlist_bl_unlock(ce->e_block_hash_p);
}

static void
__mb_cache_entry_forget(struct mb_cache_entry *ce, gfp_t gfp_mask)
{
	struct mb_cache *cache = ce->e_cache;

	mb_assert(!(ce->e_used || ce->e_queued || atomic_read(&ce->e_refcnt)));
	kmem_cache_free(cache->c_entry_cache, ce);
	atomic_dec(&cache->c_entry_count);
}

static void
__mb_cache_entry_release(struct mb_cache_entry *ce)
{
	/* First lock the entry to serialize access to its local data. */
	__spin_lock_mb_cache_entry(ce);
	/* Wake up all processes queuing for this cache entry. */
	if (ce->e_queued)
		wake_up_all(&mb_cache_queue);
	if (ce->e_used >= MB_CACHE_WRITER)
		ce->e_used -= MB_CACHE_WRITER;
	/*
	 * Make sure that all cache entries on lru_list have
	 * both e_used and e_qued of 0s.
	 */
	ce->e_used--;
	if (!(ce->e_used || ce->e_queued || atomic_read(&ce->e_refcnt))) {
		if (!__mb_cache_entry_is_block_hashed(ce)) {
			__spin_unlock_mb_cache_entry(ce);
			goto forget;
		}
		/*
		 * Need access to lru list, first drop entry lock,
		 * then reacquire the lock in the proper order.
		 */
		spin_lock(&mb_cache_spinlock);
		if (list_empty(&ce->e_lru_list))
			list_add_tail(&ce->e_lru_list, &mb_cache_lru_list);
		spin_unlock(&mb_cache_spinlock);
	}
	__spin_unlock_mb_cache_entry(ce);
	return;
forget:
	mb_assert(list_empty(&ce->e_lru_list));
	__mb_cache_entry_forget(ce, GFP_KERNEL);
}

/*
 * mb_cache_shrink_scan()  memory pressure callback
 *
 * This function is called by the kernel memory management when memory
 * gets low.
 *
 * @shrink: (ignored)
 * @sc: shrink_control passed from reclaim
 *
 * Returns the number of objects freed.
 */
static unsigned long
mb_cache_shrink_scan(struct shrinker *shrink, struct shrink_control *sc)
{
	LIST_HEAD(free_list);
	struct mb_cache_entry *entry, *tmp;
	int nr_to_scan = sc->nr_to_scan;
	gfp_t gfp_mask = sc->gfp_mask;
	unsigned long freed = 0;

	mb_debug("trying to free %d entries", nr_to_scan);
	spin_lock(&mb_cache_spinlock);
	while ((nr_to_scan-- > 0) && !list_empty(&mb_cache_lru_list)) {
		struct mb_cache_entry *ce =
			list_entry(mb_cache_lru_list.next,
				struct mb_cache_entry, e_lru_list);
		list_del_init(&ce->e_lru_list);
		if (ce->e_used || ce->e_queued || atomic_read(&ce->e_refcnt))
			continue;
		spin_unlock(&mb_cache_spinlock);
		/* Prevent any find or get operation on the entry */
		hlist_bl_lock(ce->e_block_hash_p);
		hlist_bl_lock(ce->e_index_hash_p);
		/* Ignore if it is touched by a find/get */
		if (ce->e_used || ce->e_queued || atomic_read(&ce->e_refcnt) ||
			!list_empty(&ce->e_lru_list)) {
			hlist_bl_unlock(ce->e_index_hash_p);
			hlist_bl_unlock(ce->e_block_hash_p);
			spin_lock(&mb_cache_spinlock);
			continue;
		}
		__mb_cache_entry_unhash_unlock(ce);
		list_add_tail(&ce->e_lru_list, &free_list);
		spin_lock(&mb_cache_spinlock);
	}
	spin_unlock(&mb_cache_spinlock);

	list_for_each_entry_safe(entry, tmp, &free_list, e_lru_list) {
		__mb_cache_entry_forget(entry, gfp_mask);
		freed++;
	}
	return freed;
}

static unsigned long
mb_cache_shrink_count(struct shrinker *shrink, struct shrink_control *sc)
{
	struct mb_cache *cache;
	unsigned long count = 0;

	spin_lock(&mb_cache_spinlock);
	list_for_each_entry(cache, &mb_cache_list, c_cache_list) {
		mb_debug("cache %s (%d)", cache->c_name,
			  atomic_read(&cache->c_entry_count));
		count += atomic_read(&cache->c_entry_count);
	}
	spin_unlock(&mb_cache_spinlock);

	return vfs_pressure_ratio(count);
}

static struct shrinker mb_cache_shrinker = {
	.count_objects = mb_cache_shrink_count,
	.scan_objects = mb_cache_shrink_scan,
	.seeks = DEFAULT_SEEKS,
};

/*
 * mb_cache_create()  create a new cache
 *
 * All entries in one cache are equal size. Cache entries may be from
 * multiple devices. If this is the first mbcache created, registers
 * the cache with kernel memory management. Returns NULL if no more
 * memory was available.
 *
 * @name: name of the cache (informal)
 * @bucket_bits: log2(number of hash buckets)
 */
struct mb_cache *
mb_cache_create(const char *name, int bucket_bits)
{
	int n, bucket_count = 1 << bucket_bits;
	struct mb_cache *cache = NULL;

	if (!mb_cache_bg_lock) {
		mb_cache_bg_lock = kmalloc(sizeof(struct blockgroup_lock),
			GFP_KERNEL);
		if (!mb_cache_bg_lock)
			return NULL;
		bgl_lock_init(mb_cache_bg_lock);
	}

	cache = kmalloc(sizeof(struct mb_cache), GFP_KERNEL);
	if (!cache)
		return NULL;
	cache->c_name = name;
	atomic_set(&cache->c_entry_count, 0);
	cache->c_bucket_bits = bucket_bits;
	cache->c_block_hash = kmalloc(bucket_count *
		sizeof(struct hlist_bl_head), GFP_KERNEL);
	if (!cache->c_block_hash)
		goto fail;
	for (n=0; n<bucket_count; n++)
		INIT_HLIST_BL_HEAD(&cache->c_block_hash[n]);
	cache->c_index_hash = kmalloc(bucket_count *
		sizeof(struct hlist_bl_head), GFP_KERNEL);
	if (!cache->c_index_hash)
		goto fail;
	for (n=0; n<bucket_count; n++)
		INIT_HLIST_BL_HEAD(&cache->c_index_hash[n]);
	if (!mb_cache_kmem_cache) {
		mb_cache_kmem_cache = kmem_cache_create(name,
			sizeof(struct mb_cache_entry), 0,
			SLAB_RECLAIM_ACCOUNT|SLAB_MEM_SPREAD, NULL);
		if (!mb_cache_kmem_cache)
			goto fail2;
	}
	cache->c_entry_cache = mb_cache_kmem_cache;

	/*
	 * Set an upper limit on the number of cache entries so that the hash
	 * chains won't grow too long.
	 */
	cache->c_max_entries = bucket_count << 4;

	spin_lock(&mb_cache_spinlock);
	list_add(&cache->c_cache_list, &mb_cache_list);
	spin_unlock(&mb_cache_spinlock);
	return cache;

fail2:
	kfree(cache->c_index_hash);

fail:
	kfree(cache->c_block_hash);
	kfree(cache);
	return NULL;
}


/*
 * mb_cache_shrink()
 *
 * Removes all cache entries of a device from the cache. All cache entries
 * currently in use cannot be freed, and thus remain in the cache. All others
 * are freed.
 *
 * @bdev: which device's cache entries to shrink
 */
void
mb_cache_shrink(struct block_device *bdev)
{
	LIST_HEAD(free_list);
	struct list_head *l;
	struct mb_cache_entry *ce, *tmp;

	l = &mb_cache_lru_list;
	spin_lock(&mb_cache_spinlock);
	while (!list_is_last(l, &mb_cache_lru_list)) {
		l = l->next;
		ce = list_entry(l, struct mb_cache_entry, e_lru_list);
		if (ce->e_bdev == bdev) {
			list_del_init(&ce->e_lru_list);
			if (ce->e_used || ce->e_queued ||
				atomic_read(&ce->e_refcnt))
				continue;
			spin_unlock(&mb_cache_spinlock);
			/*
			 * Prevent any find or get operation on the entry.
			 */
			hlist_bl_lock(ce->e_block_hash_p);
			hlist_bl_lock(ce->e_index_hash_p);
			/* Ignore if it is touched by a find/get */
			if (ce->e_used || ce->e_queued ||
				atomic_read(&ce->e_refcnt) ||
				!list_empty(&ce->e_lru_list)) {
				hlist_bl_unlock(ce->e_index_hash_p);
				hlist_bl_unlock(ce->e_block_hash_p);
				l = &mb_cache_lru_list;
				spin_lock(&mb_cache_spinlock);
				continue;
			}
			__mb_cache_entry_unhash_unlock(ce);
			mb_assert(!(ce->e_used || ce->e_queued ||
				atomic_read(&ce->e_refcnt)));
			list_add_tail(&ce->e_lru_list, &free_list);
			l = &mb_cache_lru_list;
			spin_lock(&mb_cache_spinlock);
		}
	}
	spin_unlock(&mb_cache_spinlock);

	list_for_each_entry_safe(ce, tmp, &free_list, e_lru_list) {
		__mb_cache_entry_forget(ce, GFP_KERNEL);
	}
}


/*
 * mb_cache_destroy()
 *
 * Shrinks the cache to its minimum possible size (hopefully 0 entries),
 * and then destroys it. If this was the last mbcache, un-registers the
 * mbcache from kernel memory management.
 */
void
mb_cache_destroy(struct mb_cache *cache)
{
	LIST_HEAD(free_list);
	struct mb_cache_entry *ce, *tmp;

	spin_lock(&mb_cache_spinlock);
	list_for_each_entry_safe(ce, tmp, &mb_cache_lru_list, e_lru_list) {
		if (ce->e_cache == cache)
			list_move_tail(&ce->e_lru_list, &free_list);
	}
	list_del(&cache->c_cache_list);
	spin_unlock(&mb_cache_spinlock);

	list_for_each_entry_safe(ce, tmp, &free_list, e_lru_list) {
		list_del_init(&ce->e_lru_list);
		/*
		 * Prevent any find or get operation on the entry.
		 */
		hlist_bl_lock(ce->e_block_hash_p);
		hlist_bl_lock(ce->e_index_hash_p);
		mb_assert(!(ce->e_used || ce->e_queued ||
			atomic_read(&ce->e_refcnt)));
		__mb_cache_entry_unhash_unlock(ce);
		__mb_cache_entry_forget(ce, GFP_KERNEL);
	}

	if (atomic_read(&cache->c_entry_count) > 0) {
		mb_error("cache %s: %d orphaned entries",
			  cache->c_name,
			  atomic_read(&cache->c_entry_count));
	}

	if (list_empty(&mb_cache_list)) {
		kmem_cache_destroy(mb_cache_kmem_cache);
		mb_cache_kmem_cache = NULL;
	}
	kfree(cache->c_index_hash);
	kfree(cache->c_block_hash);
	kfree(cache);
}

/*
 * mb_cache_entry_alloc()
 *
 * Allocates a new cache entry. The new entry will not be valid initially,
 * and thus cannot be looked up yet. It should be filled with data, and
 * then inserted into the cache using mb_cache_entry_insert(). Returns NULL
 * if no more memory was available.
 */
struct mb_cache_entry *
mb_cache_entry_alloc(struct mb_cache *cache, gfp_t gfp_flags)
{
	struct mb_cache_entry *ce;

	if (atomic_read(&cache->c_entry_count) >= cache->c_max_entries) {
		struct list_head *l;

		l = &mb_cache_lru_list;
		spin_lock(&mb_cache_spinlock);
		while (!list_is_last(l, &mb_cache_lru_list)) {
			l = l->next;
			ce = list_entry(l, struct mb_cache_entry, e_lru_list);
			if (ce->e_cache == cache) {
				list_del_init(&ce->e_lru_list);
				if (ce->e_used || ce->e_queued ||
					atomic_read(&ce->e_refcnt))
					continue;
				spin_unlock(&mb_cache_spinlock);
				/*
				 * Prevent any find or get operation on the
				 * entry.
				 */
				hlist_bl_lock(ce->e_block_hash_p);
				hlist_bl_lock(ce->e_index_hash_p);
				/* Ignore if it is touched by a find/get */
				if (ce->e_used || ce->e_queued ||
					atomic_read(&ce->e_refcnt) ||
					!list_empty(&ce->e_lru_list)) {
					hlist_bl_unlock(ce->e_index_hash_p);
					hlist_bl_unlock(ce->e_block_hash_p);
					l = &mb_cache_lru_list;
					spin_lock(&mb_cache_spinlock);
					continue;
				}
				mb_assert(list_empty(&ce->e_lru_list));
				mb_assert(!(ce->e_used || ce->e_queued ||
					atomic_read(&ce->e_refcnt)));
				__mb_cache_entry_unhash_unlock(ce);
				goto found;
			}
		}
		spin_unlock(&mb_cache_spinlock);
	}

	ce = kmem_cache_alloc(cache->c_entry_cache, gfp_flags);
	if (!ce)
		return NULL;
	atomic_inc(&cache->c_entry_count);
	INIT_LIST_HEAD(&ce->e_lru_list);
	INIT_HLIST_BL_NODE(&ce->e_block_list);
	INIT_HLIST_BL_NODE(&ce->e_index.o_list);
	ce->e_cache = cache;
	ce->e_queued = 0;
	atomic_set(&ce->e_refcnt, 0);
found:
	ce->e_block_hash_p = &cache->c_block_hash[0];
	ce->e_index_hash_p = &cache->c_index_hash[0];
	ce->e_used = 1 + MB_CACHE_WRITER;
	return ce;
}


/*
 * mb_cache_entry_insert()
 *
 * Inserts an entry that was allocated using mb_cache_entry_alloc() into
 * the cache. After this, the cache entry can be looked up, but is not yet
 * in the lru list as the caller still holds a handle to it. Returns 0 on
 * success, or -EBUSY if a cache entry for that device + inode exists
 * already (this may happen after a failed lookup, but when another process
 * has inserted the same cache entry in the meantime).
 *
 * @bdev: device the cache entry belongs to
 * @block: block number
 * @key: lookup key
 */
int
mb_cache_entry_insert(struct mb_cache_entry *ce, struct block_device *bdev,
		      sector_t block, unsigned int key)
{
	struct mb_cache *cache = ce->e_cache;
	unsigned int bucket;
	struct hlist_bl_node *l;
	struct hlist_bl_head *block_hash_p;
	struct hlist_bl_head *index_hash_p;
	struct mb_cache_entry *lce;

	mb_assert(ce);
	bucket = hash_long((unsigned long)bdev + (block & 0xffffffff), 
			   cache->c_bucket_bits);
	block_hash_p = &cache->c_block_hash[bucket];
	hlist_bl_lock(block_hash_p);
	hlist_bl_for_each_entry(lce, l, block_hash_p, e_block_list) {
		if (lce->e_bdev == bdev && lce->e_block == block) {
			hlist_bl_unlock(block_hash_p);
			return -EBUSY;
		}
	}
	mb_assert(!__mb_cache_entry_is_block_hashed(ce));
	__mb_cache_entry_unhash_block(ce);
	__mb_cache_entry_unhash_index(ce);
	ce->e_bdev = bdev;
	ce->e_block = block;
	ce->e_block_hash_p = block_hash_p;
	ce->e_index.o_key = key;
	hlist_bl_add_head(&ce->e_block_list, block_hash_p);
	hlist_bl_unlock(block_hash_p);
	bucket = hash_long(key, cache->c_bucket_bits);
	index_hash_p = &cache->c_index_hash[bucket];
	hlist_bl_lock(index_hash_p);
	ce->e_index_hash_p = index_hash_p;
	hlist_bl_add_head(&ce->e_index.o_list, index_hash_p);
	hlist_bl_unlock(index_hash_p);
	return 0;
}


/*
 * mb_cache_entry_release()
 *
 * Release a handle to a cache entry. When the last handle to a cache entry
 * is released it is either freed (if it is invalid) or otherwise inserted
 * in to the lru list.
 */
void
mb_cache_entry_release(struct mb_cache_entry *ce)
{
	__mb_cache_entry_release(ce);
}


/*
 * mb_cache_entry_free()
 *
 */
void
mb_cache_entry_free(struct mb_cache_entry *ce)
{
	mb_assert(ce);
	mb_assert(list_empty(&ce->e_lru_list));
	hlist_bl_lock(ce->e_index_hash_p);
	__mb_cache_entry_unhash_index(ce);
	hlist_bl_unlock(ce->e_index_hash_p);
	hlist_bl_lock(ce->e_block_hash_p);
	__mb_cache_entry_unhash_block(ce);
	hlist_bl_unlock(ce->e_block_hash_p);
	__mb_cache_entry_release(ce);
}


/*
 * mb_cache_entry_get()
 *
 * Get a cache entry  by device / block number. (There can only be one entry
 * in the cache per device and block.) Returns NULL if no such cache entry
 * exists. The returned cache entry is locked for exclusive access ("single
 * writer").
 */
struct mb_cache_entry *
mb_cache_entry_get(struct mb_cache *cache, struct block_device *bdev,
		   sector_t block)
{
	unsigned int bucket;
	struct hlist_bl_node *l;
	struct mb_cache_entry *ce;
	struct hlist_bl_head *block_hash_p;

	bucket = hash_long((unsigned long)bdev + (block & 0xffffffff),
			   cache->c_bucket_bits);
	block_hash_p = &cache->c_block_hash[bucket];
	/* First serialize access to the block corresponding hash chain. */
	hlist_bl_lock(block_hash_p);
	hlist_bl_for_each_entry(ce, l, block_hash_p, e_block_list) {
		mb_assert(ce->e_block_hash_p == block_hash_p);
		if (ce->e_bdev == bdev && ce->e_block == block) {
			/*
			 * Prevent a free from removing the entry.
			 */
			atomic_inc(&ce->e_refcnt);
			hlist_bl_unlock(block_hash_p);
			__spin_lock_mb_cache_entry(ce);
			atomic_dec(&ce->e_refcnt);
			if (ce->e_used > 0) {
				DEFINE_WAIT(wait);
				while (ce->e_used > 0) {
					ce->e_queued++;
					prepare_to_wait(&mb_cache_queue, &wait,
							TASK_UNINTERRUPTIBLE);
					__spin_unlock_mb_cache_entry(ce);
					schedule();
					__spin_lock_mb_cache_entry(ce);
					ce->e_queued--;
				}
				finish_wait(&mb_cache_queue, &wait);
			}
			ce->e_used += 1 + MB_CACHE_WRITER;
			__spin_unlock_mb_cache_entry(ce);

			if (!list_empty(&ce->e_lru_list)) {
				spin_lock(&mb_cache_spinlock);
				list_del_init(&ce->e_lru_list);
				spin_unlock(&mb_cache_spinlock);
			}
			if (!__mb_cache_entry_is_block_hashed(ce)) {
				__mb_cache_entry_release(ce);
				return NULL;
			}
			return ce;
		}
	}
	hlist_bl_unlock(block_hash_p);
	return NULL;
}

#if !defined(MB_CACHE_INDEXES_COUNT) || (MB_CACHE_INDEXES_COUNT > 0)

static struct mb_cache_entry *
__mb_cache_entry_find(struct hlist_bl_node *l, struct hlist_bl_head *head,
		      struct block_device *bdev, unsigned int key)
{

	/* The index hash chain is alredy acquire by caller. */
	while (l != NULL) {
		struct mb_cache_entry *ce =
			hlist_bl_entry(l, struct mb_cache_entry,
				e_index.o_list);
		mb_assert(ce->e_index_hash_p == head);
		if (ce->e_bdev == bdev && ce->e_index.o_key == key) {
			/*
			 * Prevent a free from removing the entry.
			 */
			atomic_inc(&ce->e_refcnt);
			hlist_bl_unlock(head);
			__spin_lock_mb_cache_entry(ce);
			atomic_dec(&ce->e_refcnt);
			ce->e_used++;
			/* Incrementing before holding the lock gives readers
			   priority over writers. */
			if (ce->e_used >= MB_CACHE_WRITER) {
				DEFINE_WAIT(wait);

				while (ce->e_used >= MB_CACHE_WRITER) {
					ce->e_queued++;
					prepare_to_wait(&mb_cache_queue, &wait,
							TASK_UNINTERRUPTIBLE);
					__spin_unlock_mb_cache_entry(ce);
					schedule();
					__spin_lock_mb_cache_entry(ce);
					ce->e_queued--;
				}
				finish_wait(&mb_cache_queue, &wait);
			}
			__spin_unlock_mb_cache_entry(ce);
			if (!list_empty(&ce->e_lru_list)) {
				spin_lock(&mb_cache_spinlock);
				list_del_init(&ce->e_lru_list);
				spin_unlock(&mb_cache_spinlock);
			}
			if (!__mb_cache_entry_is_block_hashed(ce)) {
				__mb_cache_entry_release(ce);
				return ERR_PTR(-EAGAIN);
			}
			return ce;
		}
		l = l->next;
	}
	hlist_bl_unlock(head);
	return NULL;
}


/*
 * mb_cache_entry_find_first()
 *
 * Find the first cache entry on a given device with a certain key in
 * an additional index. Additional matches can be found with
 * mb_cache_entry_find_next(). Returns NULL if no match was found. The
 * returned cache entry is locked for shared access ("multiple readers").
 *
 * @cache: the cache to search
 * @bdev: the device the cache entry should belong to
 * @key: the key in the index
 */
struct mb_cache_entry *
mb_cache_entry_find_first(struct mb_cache *cache, struct block_device *bdev,
			  unsigned int key)
{
	unsigned int bucket = hash_long(key, cache->c_bucket_bits);
	struct hlist_bl_node *l;
	struct mb_cache_entry *ce = NULL;
	struct hlist_bl_head *index_hash_p;

	index_hash_p = &cache->c_index_hash[bucket];
	hlist_bl_lock(index_hash_p);
	if (!hlist_bl_empty(index_hash_p)) {
		l = hlist_bl_first(index_hash_p);
		ce = __mb_cache_entry_find(l, index_hash_p, bdev, key);
	} else
		hlist_bl_unlock(index_hash_p);
	return ce;
}


/*
 * mb_cache_entry_find_next()
 *
 * Find the next cache entry on a given device with a certain key in an
 * additional index. Returns NULL if no match could be found. The previous
 * entry is atomatically released, so that mb_cache_entry_find_next() can
 * be called like this:
 *
 * entry = mb_cache_entry_find_first();
 * while (entry) {
 * 	...
 *	entry = mb_cache_entry_find_next(entry, ...);
 * }
 *
 * @prev: The previous match
 * @bdev: the device the cache entry should belong to
 * @key: the key in the index
 */
struct mb_cache_entry *
mb_cache_entry_find_next(struct mb_cache_entry *prev,
			 struct block_device *bdev, unsigned int key)
{
	struct mb_cache *cache = prev->e_cache;
	unsigned int bucket = hash_long(key, cache->c_bucket_bits);
	struct hlist_bl_node *l;
	struct mb_cache_entry *ce;
	struct hlist_bl_head *index_hash_p;

	index_hash_p = &cache->c_index_hash[bucket];
	mb_assert(prev->e_index_hash_p == index_hash_p);
	hlist_bl_lock(index_hash_p);
	mb_assert(!hlist_bl_empty(index_hash_p));
	l = prev->e_index.o_list.next;
	ce = __mb_cache_entry_find(l, index_hash_p, bdev, key);
	__mb_cache_entry_release(prev);
	return ce;
}

#endif  /* !defined(MB_CACHE_INDEXES_COUNT) || (MB_CACHE_INDEXES_COUNT > 0) */

int __init infiltratr_ext3_mbcache_init(void)
{
	register_shrinker(&mb_cache_shrinker);
	return 0;
}

void __exit infiltratr_ext3_mbcache_exit(void)
{
	unregister_shrinker(&mb_cache_shrinker);
}


