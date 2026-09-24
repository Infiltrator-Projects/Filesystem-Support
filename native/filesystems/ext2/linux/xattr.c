/*
 * Filesystem Support EXT2 extended metadata adapter.
 *
 * EXT2 xattrs occupy one filesystem block referenced by i_file_acl.  This
 * implementation validates every disk offset before use, serialises entries
 * deterministically, and uses copy-on-write whenever an existing xattr block
 * is shared by more than one inode.
 */

#include <linux/buffer_head.h>
#include <linux/capability.h>
#include <linux/init.h>
#include <linux/posix_acl_xattr.h>
#include <linux/quotaops.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/string.h>

#include "ext2.h"

#define IFS_EXT2_XATTR_SENTINEL_SIZE 4U

struct mb_cache {
	u32 marker;
};

struct ifs_ext2_xattr_item {
	u8 name_index;
	u8 name_length;
	const char *name;
	u32 value_length;
	const void *value;
};

static const struct xattr_handler * const ifs_ext2_xattr_handler_map[] = {
	[EXT2_XATTR_INDEX_USER] = &ext2_xattr_user_handler,
#ifdef CONFIG_EXT2_FS_POSIX_ACL
	[EXT2_XATTR_INDEX_POSIX_ACL_ACCESS] = &nop_posix_acl_access,
	[EXT2_XATTR_INDEX_POSIX_ACL_DEFAULT] = &nop_posix_acl_default,
#endif
	[EXT2_XATTR_INDEX_TRUSTED] = &ext2_xattr_trusted_handler,
#ifdef CONFIG_EXT2_FS_SECURITY
	[EXT2_XATTR_INDEX_SECURITY] = &ext2_xattr_security_handler,
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

static unsigned long ifs_ext2_xattr_sectors(const struct inode *inode)
{
	return inode->i_sb->s_blocksize >> 9;
}

static void ifs_ext2_xattr_account_alloc(struct inode *inode)
{
	inode->i_blocks += ifs_ext2_xattr_sectors(inode);
}

static void ifs_ext2_xattr_account_free(struct inode *inode)
{
	unsigned long sectors = ifs_ext2_xattr_sectors(inode);

	if (inode->i_blocks >= sectors)
		inode->i_blocks -= sectors;
	else
		inode->i_blocks = 0;
}

static bool ifs_ext2_xattr_is_last(const struct ext2_xattr_entry *entry)
{
	return *(__le32 *)entry == 0;
}

static struct ext2_xattr_entry *ifs_ext2_xattr_first(void *block)
{
	return (struct ext2_xattr_entry *)
		((char *)block + sizeof(struct ext2_xattr_header));
}

static int ifs_ext2_xattr_validate_block(struct super_block *sb,
					 const void *block)
{
	const struct ext2_xattr_header *header = block;
	const char *base = block;
	const char *end = base + sb->s_blocksize;
	const struct ext2_xattr_entry *entry;
	size_t value_floor = sb->s_blocksize;
	u32 refcount;

	if (sb->s_blocksize <
	    sizeof(*header) + IFS_EXT2_XATTR_SENTINEL_SIZE)
		return -EFSCORRUPTED;

	if (header->h_magic != cpu_to_le32(EXT2_XATTR_MAGIC) ||
	    header->h_blocks != cpu_to_le32(1))
		return -EFSCORRUPTED;

	refcount = le32_to_cpu(header->h_refcount);
	if (refcount == 0 || refcount > EXT2_XATTR_REFCOUNT_MAX)
		return -EFSCORRUPTED;

	entry = ifs_ext2_xattr_first((void *)block);
	for (;;) {
		const char *entry_ptr = (const char *)entry;
		const char *next;
		size_t value_size;
		size_t value_offset;
		size_t padded_size;

		if (entry_ptr + IFS_EXT2_XATTR_SENTINEL_SIZE > end)
			return -EFSCORRUPTED;
		if (ifs_ext2_xattr_is_last(entry)) {
			if ((size_t)(entry_ptr - base) +
			    IFS_EXT2_XATTR_SENTINEL_SIZE > value_floor)
				return -EFSCORRUPTED;
			return 0;
		}

		if (entry_ptr + sizeof(*entry) > end)
			return -EFSCORRUPTED;

		next = entry_ptr + EXT2_XATTR_LEN(entry->e_name_len);
		if (next > end ||
		    (size_t)(next - base) + IFS_EXT2_XATTR_SENTINEL_SIZE >
		    value_floor)
			return -EFSCORRUPTED;

		if (entry->e_value_block != 0)
			return -EFSCORRUPTED;

		value_size = le32_to_cpu(entry->e_value_size);
		value_offset = le16_to_cpu(entry->e_value_offs);
		padded_size = EXT2_XATTR_SIZE(value_size);

		if (value_size != 0) {
			if ((value_offset & EXT2_XATTR_ROUND) != 0 ||
			    value_offset > sb->s_blocksize ||
			    padded_size > sb->s_blocksize - value_offset)
				return -EFSCORRUPTED;
			if (value_offset < value_floor)
				value_floor = value_offset;
			if ((size_t)(next - base) +
			    IFS_EXT2_XATTR_SENTINEL_SIZE > value_floor)
				return -EFSCORRUPTED;
		} else if (value_offset != 0) {
			return -EFSCORRUPTED;
		}

		entry = (const struct ext2_xattr_entry *)next;
	}
}

static struct buffer_head *ifs_ext2_xattr_read(struct inode *inode, int *error)
{
	struct ext2_sb_info *sbi = EXT2_SB(inode->i_sb);
	struct buffer_head *bh;
	u32 block = EXT2_I(inode)->i_file_acl;

	*error = 0;
	if (!block)
		return NULL;

	if (!ext2_data_block_valid(sbi, block, 1)) {
		*error = -EFSCORRUPTED;
		return NULL;
	}

	bh = sb_bread(inode->i_sb, block);
	if (!bh) {
		*error = -EIO;
		return NULL;
	}

	*error = ifs_ext2_xattr_validate_block(inode->i_sb, bh->b_data);
	if (*error) {
		brelse(bh);
		return NULL;
	}

	return bh;
}

static int ifs_ext2_xattr_item_compare(const void *left, const void *right)
{
	const struct ifs_ext2_xattr_item *a = left;
	const struct ifs_ext2_xattr_item *b = right;
	int result;
	size_t common;

	result = (int)a->name_index - (int)b->name_index;
	if (result)
		return result;

	result = (int)a->name_length - (int)b->name_length;
	if (result)
		return result;

	common = min_t(size_t, a->name_length, b->name_length);
	return memcmp(a->name, b->name, common);
}

static int ifs_ext2_xattr_collect(
	struct super_block *sb,
	const void *block,
	struct ifs_ext2_xattr_item *items,
	size_t capacity,
	size_t *count)
{
	const struct ext2_xattr_entry *entry;
	size_t used = 0;

	*count = 0;
	if (!block)
		return 0;

	if (ifs_ext2_xattr_validate_block(sb, block))
		return -EFSCORRUPTED;

	entry = ifs_ext2_xattr_first((void *)block);
	while (!ifs_ext2_xattr_is_last(entry)) {
		if (used >= capacity)
			return -E2BIG;

		items[used].name_index = entry->e_name_index;
		items[used].name_length = entry->e_name_len;
		items[used].name = entry->e_name;
		items[used].value_length = le32_to_cpu(entry->e_value_size);
		items[used].value = items[used].value_length ?
			(char *)block + le16_to_cpu(entry->e_value_offs) :
			NULL;
		used++;
		entry = EXT2_XATTR_NEXT(entry);
	}

	*count = used;
	return 0;
}

static int ifs_ext2_xattr_find(
	const struct ifs_ext2_xattr_item *items,
	size_t count,
	int name_index,
	const char *name,
	size_t name_length)
{
	size_t index;

	for (index = 0; index < count; index++) {
		if (items[index].name_index != name_index ||
		    items[index].name_length != name_length)
			continue;
		if (!memcmp(items[index].name, name, name_length))
			return (int)index;
	}

	return -1;
}

static u32 ifs_ext2_xattr_entry_hash(
	const char *name, size_t name_length,
	const void *value, size_t value_length)
{
	const unsigned char *name_bytes = (const unsigned char *)name;
	u32 hash = 0;
	size_t index;
	size_t padded = EXT2_XATTR_SIZE(value_length);

	for (index = 0; index < name_length; index++)
		hash = (hash << 5) ^ (hash >> 27) ^ name_bytes[index];

	for (index = 0; index < padded; index += sizeof(__le32)) {
		u32 word = 0;
		size_t remaining;

		if (index < value_length) {
			remaining = min_t(size_t, sizeof(word),
					  value_length - index);
			memcpy(&word, (const char *)value + index, remaining);
			word = le32_to_cpu((__force __le32)word);
		}

		hash = (hash << 16) ^ (hash >> 16) ^ word;
	}

	return hash;
}

static void ifs_ext2_xattr_block_hash(struct ext2_xattr_header *header)
{
	struct ext2_xattr_entry *entry =
		(struct ext2_xattr_entry *)(header + 1);
	u32 hash = 0;

	while (!ifs_ext2_xattr_is_last(entry)) {
		u32 entry_hash = le32_to_cpu(entry->e_hash);

		if (!entry_hash) {
			hash = 0;
			break;
		}
		hash = (hash << 16) ^ (hash >> 16) ^ entry_hash;
		entry = EXT2_XATTR_NEXT(entry);
	}

	header->h_hash = cpu_to_le32(hash);
}

static int ifs_ext2_xattr_serialize(
	struct super_block *sb,
	struct ifs_ext2_xattr_item *items,
	size_t count,
	void **block_out)
{
	struct ext2_xattr_header *header;
	struct ext2_xattr_entry *entry;
	char *block;
	size_t value_cursor = sb->s_blocksize;
	size_t index;

	*block_out = NULL;
	if (!count)
		return 0;

	sort(items, count, sizeof(*items), ifs_ext2_xattr_item_compare, NULL);

	block = kzalloc(sb->s_blocksize, GFP_NOFS);
	if (!block)
		return -ENOMEM;

	header = (struct ext2_xattr_header *)block;
	header->h_magic = cpu_to_le32(EXT2_XATTR_MAGIC);
	header->h_refcount = cpu_to_le32(1);
	header->h_blocks = cpu_to_le32(1);

	entry = (struct ext2_xattr_entry *)(header + 1);
	for (index = 0; index < count; index++) {
		size_t entry_size = EXT2_XATTR_LEN(items[index].name_length);
		size_t value_size = EXT2_XATTR_SIZE(items[index].value_length);
		size_t entry_end =
			(size_t)((char *)entry - block) + entry_size +
			IFS_EXT2_XATTR_SENTINEL_SIZE;

		if (value_size > value_cursor) {
			kfree(block);
			return -ENOSPC;
		}
		value_cursor -= value_size;

		if (entry_end > value_cursor) {
			kfree(block);
			return -ENOSPC;
		}

		entry->e_name_len = items[index].name_length;
		entry->e_name_index = items[index].name_index;
		entry->e_value_block = 0;
		entry->e_value_size =
			cpu_to_le32(items[index].value_length);

		memcpy(entry->e_name, items[index].name,
		       items[index].name_length);

		if (items[index].value_length) {
			entry->e_value_offs = cpu_to_le16(value_cursor);
			memcpy(block + value_cursor, items[index].value,
			       items[index].value_length);
		} else {
			entry->e_value_offs = 0;
		}

		entry->e_hash = cpu_to_le32(
			ifs_ext2_xattr_entry_hash(
				items[index].name,
				items[index].name_length,
				items[index].value,
				items[index].value_length));

		entry = (struct ext2_xattr_entry *)
			((char *)entry + entry_size);
	}

	ifs_ext2_xattr_block_hash(header);
	*block_out = block;
	return 0;
}

static void ifs_ext2_xattr_enable_feature(struct super_block *sb)
{
	if (EXT2_HAS_COMPAT_FEATURE(sb, EXT2_FEATURE_COMPAT_EXT_ATTR))
		return;

	spin_lock(&EXT2_SB(sb)->s_lock);
	ext2_update_dynamic_rev(sb);
	EXT2_SET_COMPAT_FEATURE(sb, EXT2_FEATURE_COMPAT_EXT_ATTR);
	spin_unlock(&EXT2_SB(sb)->s_lock);
	mark_buffer_dirty(EXT2_SB(sb)->s_sbh);
}

static int ifs_ext2_xattr_allocate_block(
	struct inode *inode, const void *contents,
	struct buffer_head **bh_out)
{
	struct super_block *sb = inode->i_sb;
	ext2_fsblk_t goal;
	ext2_fsblk_t block;
	unsigned long count = 1;
	struct buffer_head *bh;
	int error = 0;

	goal = ext2_group_first_block_no(
		sb, EXT2_I(inode)->i_block_group);
	block = ext2_new_blocks(
		inode, goal, &count, &error, EXT2_ALLOC_NORESERVE);
	if (error)
		return error;
	if (!block || count != 1) {
		if (block && count)
			ext2_free_blocks(inode, block, count);
		return -ENOSPC;
	}

	bh = sb_getblk(sb, block);
	if (!bh) {
		ext2_free_blocks(inode, block, 1);
		return -ENOMEM;
	}

	lock_buffer(bh);
	memcpy(bh->b_data, contents, sb->s_blocksize);
	set_buffer_uptodate(bh);
	mark_buffer_dirty(bh);
	unlock_buffer(bh);

	sync_dirty_buffer(bh);
	if (!buffer_uptodate(bh)) {
		bforget(bh);
		ext2_free_blocks(inode, block, 1);
		return -EIO;
	}

	ifs_ext2_xattr_account_alloc(inode);
	*bh_out = bh;
	return 0;
}

static int ifs_ext2_xattr_release_block(
	struct inode *inode, struct buffer_head **bhp)
{
	struct buffer_head *bh = *bhp;
	struct ext2_xattr_header *header;
	u32 refcount;

	if (!bh)
		return 0;

	header = (struct ext2_xattr_header *)bh->b_data;
	if (ifs_ext2_xattr_validate_block(inode->i_sb, header))
		return -EFSCORRUPTED;

	refcount = le32_to_cpu(header->h_refcount);
	if (refcount > 1) {
		lock_buffer(bh);
		header->h_refcount = cpu_to_le32(refcount - 1);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		sync_dirty_buffer(bh);
		dquot_free_block(inode, 1);
		ifs_ext2_xattr_account_free(inode);
		return 0;
	}

	ext2_free_blocks(inode, bh->b_blocknr, 1);
	ifs_ext2_xattr_account_free(inode);
	clear_buffer_dirty(bh);
	bforget(bh);
	*bhp = NULL;
	return 0;
}

static int ifs_ext2_xattr_publish(
	struct inode *inode,
	struct buffer_head *old_bh,
	const void *new_contents)
{
	struct ext2_inode_info *info = EXT2_I(inode);
	struct buffer_head *new_bh = NULL;
	u32 old_block = info->i_file_acl;
	int error;

	if (!new_contents) {
		if (!old_bh)
			return 0;

		info->i_file_acl = 0;
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);

		error = sync_inode_metadata(inode, 1);
		if (error) {
			info->i_file_acl = old_block;
			mark_inode_dirty(inode);
			return error;
		}

		error = ifs_ext2_xattr_release_block(inode, &old_bh);
		brelse(old_bh);
		return error;
	}

	if (old_bh &&
	    le32_to_cpu(((struct ext2_xattr_header *)
			 old_bh->b_data)->h_refcount) == 1) {
		lock_buffer(old_bh);
		memcpy(old_bh->b_data, new_contents, inode->i_sb->s_blocksize);
		mark_buffer_dirty(old_bh);
		unlock_buffer(old_bh);
		if (IS_SYNC(inode))
			sync_dirty_buffer(old_bh);

		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
		return IS_SYNC(inode) ? sync_inode_metadata(inode, 1) : 0;
	}

	error = ifs_ext2_xattr_allocate_block(
		inode, new_contents, &new_bh);
	if (error)
		return error;

	ifs_ext2_xattr_enable_feature(inode->i_sb);

	info->i_file_acl = new_bh->b_blocknr;
	inode_set_ctime_current(inode);
	mark_inode_dirty(inode);

	error = sync_inode_metadata(inode, 1);
	if (error) {
		info->i_file_acl = old_block;
		mark_inode_dirty(inode);
		sync_inode_metadata(inode, 1);
		ext2_free_blocks(inode, new_bh->b_blocknr, 1);
		ifs_ext2_xattr_account_free(inode);
		bforget(new_bh);
		return error;
	}

	if (old_bh) {
		error = ifs_ext2_xattr_release_block(inode, &old_bh);
		if (error) {
			brelse(old_bh);
			brelse(new_bh);
			return error;
		}
	}

	brelse(old_bh);
	brelse(new_bh);
	return 0;
}

static const char *ifs_ext2_xattr_prefix(
	int name_index, struct dentry *dentry)
{
	const struct xattr_handler *handler = NULL;

	if (name_index > 0 &&
	    name_index < ARRAY_SIZE(ifs_ext2_xattr_handler_map))
		handler = ifs_ext2_xattr_handler_map[name_index];

	if (!xattr_handler_can_list(handler, dentry))
		return NULL;
	return xattr_prefix(handler);
}

int ext2_xattr_get(struct inode *inode, int name_index, const char *name,
		   void *buffer, size_t buffer_size)
{
	struct buffer_head *bh;
	struct ext2_xattr_entry *entry;
	size_t name_length;
	int error;

	if (!name)
		return -EINVAL;
	name_length = strlen(name);
	if (name_length > 255)
		return -ERANGE;

	down_read(&EXT2_I(inode)->xattr_sem);
	bh = ifs_ext2_xattr_read(inode, &error);
	if (error)
		goto out_unlock;
	if (!bh) {
		error = -ENODATA;
		goto out_unlock;
	}

	entry = ifs_ext2_xattr_first(bh->b_data);
	error = -ENODATA;
	while (!ifs_ext2_xattr_is_last(entry)) {
		if (entry->e_name_index == name_index &&
		    entry->e_name_len == name_length &&
		    !memcmp(entry->e_name, name, name_length)) {
			size_t value_length = le32_to_cpu(entry->e_value_size);

			if (!buffer) {
				error = value_length;
				break;
			}
			if (buffer_size < value_length) {
				error = -ERANGE;
				break;
			}
			if (value_length)
				memcpy(buffer,
				       bh->b_data +
				       le16_to_cpu(entry->e_value_offs),
				       value_length);
			error = value_length;
			break;
		}
		entry = EXT2_XATTR_NEXT(entry);
	}

	brelse(bh);
out_unlock:
	up_read(&EXT2_I(inode)->xattr_sem);
	return error;
}

ssize_t ext2_listxattr(struct dentry *dentry, char *buffer, size_t buffer_size)
{
	struct inode *inode = d_inode(dentry);
	struct buffer_head *bh;
	struct ext2_xattr_entry *entry;
	size_t total = 0;
	int error;

	down_read(&EXT2_I(inode)->xattr_sem);
	bh = ifs_ext2_xattr_read(inode, &error);
	if (error)
		goto out;
	if (!bh) {
		error = 0;
		goto out;
	}

	entry = ifs_ext2_xattr_first(bh->b_data);
	while (!ifs_ext2_xattr_is_last(entry)) {
		const char *prefix =
			ifs_ext2_xattr_prefix(entry->e_name_index, dentry);

		if (prefix) {
			size_t prefix_length = strlen(prefix);
			size_t item_length =
				prefix_length + entry->e_name_len + 1;

			if (buffer) {
				if (total + item_length > buffer_size) {
					error = -ERANGE;
					goto out_brelse;
				}
				memcpy(buffer + total, prefix, prefix_length);
				memcpy(buffer + total + prefix_length,
				       entry->e_name, entry->e_name_len);
				buffer[total + item_length - 1] = '\0';
			}
			total += item_length;
		}

		entry = EXT2_XATTR_NEXT(entry);
	}

	error = total;

out_brelse:
	brelse(bh);
out:
	up_read(&EXT2_I(inode)->xattr_sem);
	return error;
}

int ext2_xattr_set(struct inode *inode, int name_index, const char *name,
		   const void *value, size_t value_length, int flags)
{
	struct buffer_head *old_bh = NULL;
	struct ifs_ext2_xattr_item *items = NULL;
	void *serialised = NULL;
	size_t name_length;
	size_t capacity;
	size_t count = 0;
	int position;
	int error;

	if (!name)
		return -EINVAL;
	name_length = strlen(name);
	if (name_length > 255)
		return -ERANGE;
	if (value_length > inode->i_sb->s_blocksize)
		return -ERANGE;

	capacity = inode->i_sb->s_blocksize /
		   sizeof(struct ext2_xattr_entry) + 1;
	items = kcalloc(capacity, sizeof(*items), GFP_NOFS);
	if (!items)
		return -ENOMEM;

	down_write(&EXT2_I(inode)->xattr_sem);

	old_bh = ifs_ext2_xattr_read(inode, &error);
	if (error)
		goto out;

	error = ifs_ext2_xattr_collect(
		inode->i_sb,
		old_bh ? old_bh->b_data : NULL,
		items, capacity, &count);
	if (error)
		goto out;

	position = ifs_ext2_xattr_find(
		items, count, name_index, name, name_length);

	if ((flags & XATTR_CREATE) && position >= 0) {
		error = -EEXIST;
		goto out;
	}
	if ((flags & XATTR_REPLACE) && position < 0) {
		error = -ENODATA;
		goto out;
	}
	if (!value && position < 0) {
		error = -ENODATA;
		goto out;
	}

	if (position >= 0) {
		if (!value) {
			memmove(&items[position], &items[position + 1],
				(count - position - 1) * sizeof(*items));
			count--;
		} else {
			items[position].value = value;
			items[position].value_length = value_length;
		}
	} else {
		if (count >= capacity) {
			error = -ENOSPC;
			goto out;
		}
		items[count].name_index = name_index;
		items[count].name_length = name_length;
		items[count].name = name;
		items[count].value = value;
		items[count].value_length = value_length;
		count++;
	}

	error = ifs_ext2_xattr_serialize(
		inode->i_sb, items, count, &serialised);
	if (error)
		goto out;

	error = ifs_ext2_xattr_publish(inode, old_bh, serialised);
	old_bh = NULL;

out:
	brelse(old_bh);
	kfree(serialised);
	up_write(&EXT2_I(inode)->xattr_sem);
	kfree(items);
	return error;
}

void ext2_xattr_delete_inode(struct inode *inode)
{
	struct buffer_head *bh;
	int error;

	if (!down_write_trylock(&EXT2_I(inode)->xattr_sem))
		return;

	bh = ifs_ext2_xattr_read(inode, &error);
	if (!error && bh) {
		EXT2_I(inode)->i_file_acl = 0;
		ifs_ext2_xattr_release_block(inode, &bh);
	}
	brelse(bh);
	up_write(&EXT2_I(inode)->xattr_sem);
}

struct mb_cache *ext2_xattr_create_cache(void)
{
	return kzalloc(sizeof(struct mb_cache), GFP_KERNEL);
}

void ext2_xattr_destroy_cache(struct mb_cache *cache)
{
	kfree(cache);
}

int __init infiltratr_mbcache_init(void)
{
	return 0;
}

void infiltratr_mbcache_exit(void)
{
}

static bool ifs_ext2_user_list(struct dentry *dentry)
{
	return test_opt(dentry->d_sb, XATTR_USER);
}

static int ifs_ext2_user_get(
	const struct xattr_handler *handler,
	struct dentry *dentry, struct inode *inode,
	const char *name, void *buffer, size_t size)
{
	if (!test_opt(inode->i_sb, XATTR_USER))
		return -EOPNOTSUPP;
	return ext2_xattr_get(
		inode, EXT2_XATTR_INDEX_USER, name, buffer, size);
}

static int ifs_ext2_user_set(
	const struct xattr_handler *handler,
	struct mnt_idmap *idmap, struct dentry *dentry,
	struct inode *inode, const char *name,
	const void *value, size_t size, int flags)
{
	if (!test_opt(inode->i_sb, XATTR_USER))
		return -EOPNOTSUPP;
	return ext2_xattr_set(
		inode, EXT2_XATTR_INDEX_USER, name, value, size, flags);
}

const struct xattr_handler ext2_xattr_user_handler = {
	.prefix = XATTR_USER_PREFIX,
	.list = ifs_ext2_user_list,
	.get = ifs_ext2_user_get,
	.set = ifs_ext2_user_set,
};

static bool ifs_ext2_trusted_list(struct dentry *dentry)
{
	return capable(CAP_SYS_ADMIN);
}

static int ifs_ext2_trusted_get(
	const struct xattr_handler *handler,
	struct dentry *dentry, struct inode *inode,
	const char *name, void *buffer, size_t size)
{
	return ext2_xattr_get(
		inode, EXT2_XATTR_INDEX_TRUSTED, name, buffer, size);
}

static int ifs_ext2_trusted_set(
	const struct xattr_handler *handler,
	struct mnt_idmap *idmap, struct dentry *dentry,
	struct inode *inode, const char *name,
	const void *value, size_t size, int flags)
{
	return ext2_xattr_set(
		inode, EXT2_XATTR_INDEX_TRUSTED, name, value, size, flags);
}

const struct xattr_handler ext2_xattr_trusted_handler = {
	.prefix = XATTR_TRUSTED_PREFIX,
	.list = ifs_ext2_trusted_list,
	.get = ifs_ext2_trusted_get,
	.set = ifs_ext2_trusted_set,
};

#ifdef CONFIG_EXT2_FS_SECURITY
static int ifs_ext2_security_get(
	const struct xattr_handler *handler,
	struct dentry *dentry, struct inode *inode,
	const char *name, void *buffer, size_t size)
{
	return ext2_xattr_get(
		inode, EXT2_XATTR_INDEX_SECURITY, name, buffer, size);
}

static int ifs_ext2_security_set(
	const struct xattr_handler *handler,
	struct mnt_idmap *idmap, struct dentry *dentry,
	struct inode *inode, const char *name,
	const void *value, size_t size, int flags)
{
	return ext2_xattr_set(
		inode, EXT2_XATTR_INDEX_SECURITY, name, value, size, flags);
}

static int ifs_ext2_init_security_xattrs(
	struct inode *inode, const struct xattr *xattrs, void *fs_info)
{
	const struct xattr *item;
	int error = 0;

	for (item = xattrs; item->name; item++) {
		error = ext2_xattr_set(
			inode, EXT2_XATTR_INDEX_SECURITY,
			item->name, item->value, item->value_len, 0);
		if (error)
			break;
	}

	return error;
}

int ext2_init_security(
	struct inode *inode, struct inode *dir, const struct qstr *qstr)
{
	return security_inode_init_security(
		inode, dir, qstr, ifs_ext2_init_security_xattrs, NULL);
}

const struct xattr_handler ext2_xattr_security_handler = {
	.prefix = XATTR_SECURITY_PREFIX,
	.get = ifs_ext2_security_get,
	.set = ifs_ext2_security_set,
};
#endif

#ifdef CONFIG_EXT2_FS_POSIX_ACL
static struct posix_acl *ifs_ext2_acl_decode(
	const void *value, size_t size)
{
	const char *cursor;
	const char *end;
	struct posix_acl *acl;
	int count;
	int index;

	if (!value)
		return NULL;
	if (size < sizeof(ext2_acl_header))
		return ERR_PTR(-EINVAL);
	if (((const ext2_acl_header *)value)->a_version !=
	    cpu_to_le32(EXT2_ACL_VERSION))
		return ERR_PTR(-EINVAL);

	count = ext2_acl_count(size);
	if (count < 0)
		return ERR_PTR(-EINVAL);
	if (!count)
		return NULL;

	acl = posix_acl_alloc(count, GFP_KERNEL);
	if (!acl)
		return ERR_PTR(-ENOMEM);

	cursor = (const char *)value + sizeof(ext2_acl_header);
	end = (const char *)value + size;

	for (index = 0; index < count; index++) {
		const ext2_acl_entry *disk =
			(const ext2_acl_entry *)cursor;
		struct posix_acl_entry *memory = &acl->a_entries[index];

		if (cursor + sizeof(ext2_acl_entry_short) > end)
			goto invalid;

		memory->e_tag = le16_to_cpu(disk->e_tag);
		memory->e_perm = le16_to_cpu(disk->e_perm);

		switch (memory->e_tag) {
		case ACL_USER_OBJ:
		case ACL_GROUP_OBJ:
		case ACL_MASK:
		case ACL_OTHER:
			cursor += sizeof(ext2_acl_entry_short);
			break;
		case ACL_USER:
			if (cursor + sizeof(*disk) > end)
				goto invalid;
			memory->e_uid = make_kuid(
				&init_user_ns, le32_to_cpu(disk->e_id));
			if (!uid_valid(memory->e_uid))
				goto invalid;
			cursor += sizeof(*disk);
			break;
		case ACL_GROUP:
			if (cursor + sizeof(*disk) > end)
				goto invalid;
			memory->e_gid = make_kgid(
				&init_user_ns, le32_to_cpu(disk->e_id));
			if (!gid_valid(memory->e_gid))
				goto invalid;
			cursor += sizeof(*disk);
			break;
		default:
			goto invalid;
		}
	}

	if (cursor != end)
		goto invalid;
	return acl;

invalid:
	posix_acl_release(acl);
	return ERR_PTR(-EINVAL);
}

static void *ifs_ext2_acl_encode(
	const struct posix_acl *acl, size_t *size)
{
	ext2_acl_header *header;
	char *cursor;
	int index;

	*size = ext2_acl_size(acl->a_count);
	header = kzalloc(*size, GFP_KERNEL);
	if (!header)
		return ERR_PTR(-ENOMEM);

	header->a_version = cpu_to_le32(EXT2_ACL_VERSION);
	cursor = (char *)(header + 1);

	for (index = 0; index < acl->a_count; index++) {
		const struct posix_acl_entry *memory =
			&acl->a_entries[index];
		ext2_acl_entry *disk = (ext2_acl_entry *)cursor;

		disk->e_tag = cpu_to_le16(memory->e_tag);
		disk->e_perm = cpu_to_le16(memory->e_perm);

		switch (memory->e_tag) {
		case ACL_USER_OBJ:
		case ACL_GROUP_OBJ:
		case ACL_MASK:
		case ACL_OTHER:
			cursor += sizeof(ext2_acl_entry_short);
			break;
		case ACL_USER:
			disk->e_id = cpu_to_le32(
				from_kuid(&init_user_ns, memory->e_uid));
			cursor += sizeof(*disk);
			break;
		case ACL_GROUP:
			disk->e_id = cpu_to_le32(
				from_kgid(&init_user_ns, memory->e_gid));
			cursor += sizeof(*disk);
			break;
		default:
			kfree(header);
			return ERR_PTR(-EINVAL);
		}
	}

	return header;
}

struct posix_acl *ext2_get_acl(
	struct inode *inode, int type, bool rcu)
{
	int name_index;
	int size;
	void *value = NULL;
	struct posix_acl *acl;

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
		return ERR_PTR(-EINVAL);
	}

	size = ext2_xattr_get(inode, name_index, "", NULL, 0);
	if (size == -ENODATA)
		return NULL;
	if (size < 0)
		return ERR_PTR(size);

	if (size) {
		value = kmalloc(size, GFP_KERNEL);
		if (!value)
			return ERR_PTR(-ENOMEM);
		size = ext2_xattr_get(
			inode, name_index, "", value, size);
		if (size < 0) {
			kfree(value);
			return ERR_PTR(size);
		}
	}

	acl = ifs_ext2_acl_decode(value, size);
	kfree(value);
	return acl;
}

static int ifs_ext2_set_acl_value(
	struct inode *inode, struct posix_acl *acl, int type)
{
	int name_index;
	void *value = NULL;
	size_t size = 0;
	int error;

	switch (type) {
	case ACL_TYPE_ACCESS:
		name_index = EXT2_XATTR_INDEX_POSIX_ACL_ACCESS;
		break;
	case ACL_TYPE_DEFAULT:
		if (!S_ISDIR(inode->i_mode))
			return acl ? -EACCES : 0;
		name_index = EXT2_XATTR_INDEX_POSIX_ACL_DEFAULT;
		break;
	default:
		return -EINVAL;
	}

	if (acl) {
		value = ifs_ext2_acl_encode(acl, &size);
		if (IS_ERR(value))
			return PTR_ERR(value);
	}

	error = ext2_xattr_set(
		inode, name_index, "", value, size, 0);
	kfree(value);

	if (!error)
		set_cached_acl(inode, type, acl);
	return error;
}

int ext2_set_acl(
	struct mnt_idmap *idmap, struct dentry *dentry,
	struct posix_acl *acl, int type)
{
	struct inode *inode = d_inode(dentry);
	umode_t mode = inode->i_mode;
	bool update_mode = false;
	int error;

	if (type == ACL_TYPE_ACCESS && acl) {
		error = posix_acl_update_mode(
			&nop_mnt_idmap, inode, &mode, &acl);
		if (error)
			return error;
		update_mode = true;
	}

	error = ifs_ext2_set_acl_value(inode, acl, type);
	if (!error && update_mode) {
		inode->i_mode = mode;
		inode_set_ctime_current(inode);
		mark_inode_dirty(inode);
	}

	return error;
}

int ext2_init_acl(struct inode *inode, struct inode *dir)
{
	struct posix_acl *default_acl;
	struct posix_acl *access_acl;
	int error;

	error = posix_acl_create(
		dir, &inode->i_mode, &default_acl, &access_acl);
	if (error)
		return error;

	if (default_acl) {
		error = ifs_ext2_set_acl_value(
			inode, default_acl, ACL_TYPE_DEFAULT);
		posix_acl_release(default_acl);
	} else {
		inode->i_default_acl = NULL;
	}

	if (access_acl) {
		if (!error)
			error = ifs_ext2_set_acl_value(
				inode, access_acl, ACL_TYPE_ACCESS);
		posix_acl_release(access_acl);
	} else {
		inode->i_acl = NULL;
	}

	return error;
}
#endif
