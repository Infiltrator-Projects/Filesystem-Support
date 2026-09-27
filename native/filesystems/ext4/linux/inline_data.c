/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Infiltrator Filesystem Support — EXT4 inline-data engine.
 *
 * Inline payload is split between the inode's i_block array and the
 * system.data inode-body xattr.  This implementation owns the state machine
 * that grows, shrinks, converts and exposes that representation to the VFS.
 */

// SPDX-License-Identifier: GPL-2.0

#include <linux/buffer_head.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/iomap.h>
#include <linux/iversion.h>
#include <linux/namei.h>
#include <linux/pagemap.h>
#include <linux/slab.h>

#include "ext4.h"
#include "ext4_jbd2.h"
#include "truncate.h"
#include "xattr.h"

#define IFS_EXT4_INLINE_XATTR_NAME "data"
#define IFS_EXT4_INLINE_HEAD_BYTES ((unsigned int)(sizeof(__le32) * EXT4_N_BLOCKS))
#define IFS_EXT4_INLINE_PARENT_BYTES 4U

static unsigned int ifs_ext4_inline_size(const struct inode *inode)
{
	const struct ext4_inode_info *ei =
		container_of(inode, const struct ext4_inode_info, vfs_inode);

	return ei->i_inline_off ? ei->i_inline_size : 0U;
}

static struct ext4_xattr_entry *
ifs_ext4_inline_entry(struct inode *inode, struct ext4_iloc *iloc)
{
	if (!EXT4_I(inode)->i_inline_off)
		return NULL;
	return (struct ext4_xattr_entry *)
		((u8 *)ext4_raw_inode(iloc) + EXT4_I(inode)->i_inline_off);
}

static void *ifs_ext4_inline_tail(struct inode *inode, struct ext4_iloc *iloc)
{
	struct ext4_xattr_entry *entry = ifs_ext4_inline_entry(inode, iloc);
	struct ext4_xattr_ibody_header *header;

	if (!entry)
		return NULL;
	header = IHDR(inode, ext4_raw_inode(iloc));
	return (u8 *)IFIRST(header) + le16_to_cpu(entry->e_value_offs);
}

static int ifs_ext4_inline_tail_capacity(struct inode *inode,
					 struct ext4_iloc *iloc)
{
	struct ext4_inode *raw;
	struct ext4_xattr_ibody_header *header;
	struct ext4_xattr_entry *entry;
	u8 *end;
	int lower;
	int used;

	if (!EXT4_INODE_HAS_XATTR_SPACE(inode))
		return 0;

	raw = ext4_raw_inode(iloc);
	header = IHDR(inode, raw);
	entry = IFIRST(header);
	end = (u8 *)raw + EXT4_SB(inode->i_sb)->s_inode_size;

	lower = EXT4_SB(inode->i_sb)->s_inode_size -
		EXT4_GOOD_OLD_INODE_SIZE -
		EXT4_I(inode)->i_extra_isize -
		sizeof(*header);

	if (!ext4_test_inode_state(inode, EXT4_STATE_XATTR)) {
		used = EXT4_XATTR_LEN(strlen(IFS_EXT4_INLINE_XATTR_NAME)) +
			EXT4_XATTR_ROUND + sizeof(__u32);
		return lower > used ?
			EXT4_XATTR_SIZE(lower - used) : 0;
	}

	while (!IS_LAST_ENTRY(entry)) {
		struct ext4_xattr_entry *next = EXT4_XATTR_NEXT(entry);
		size_t value_off;

		if ((u8 *)next >= end)
			return 0;

		if (!entry->e_value_inum && entry->e_value_size) {
			value_off = le16_to_cpu(entry->e_value_offs);
			if (value_off < lower)
				lower = value_off;
		}
		entry = next;
	}

	used = (u8 *)entry - (u8 *)IFIRST(header) + sizeof(__u32);
	lower -= used;

	if (EXT4_I(inode)->i_inline_off) {
		struct ext4_xattr_entry *inline_entry =
			ifs_ext4_inline_entry(inode, iloc);

		lower += EXT4_XATTR_SIZE(
			le32_to_cpu(inline_entry->e_value_size));
		return max(lower, 0);
	}

	lower -= EXT4_XATTR_LEN(strlen(IFS_EXT4_INLINE_XATTR_NAME));
	if (lower <= EXT4_XATTR_ROUND)
		return 0;
	return EXT4_XATTR_SIZE(lower - EXT4_XATTR_ROUND);
}

int ext4_get_max_inline_size(struct inode *inode)
{
	struct ext4_iloc iloc;
	int tail;
	int err;

	if (!EXT4_I(inode)->i_extra_isize)
		return 0;

	err = ext4_get_inode_loc(inode, &iloc);
	if (err)
		return 0;

	down_read(&EXT4_I(inode)->xattr_sem);
	tail = ifs_ext4_inline_tail_capacity(inode, &iloc);
	up_read(&EXT4_I(inode)->xattr_sem);
	brelse(iloc.bh);

	return tail > 0 ? tail + IFS_EXT4_INLINE_HEAD_BYTES : 0;
}

int ext4_find_inline_data_nolock(struct inode *inode)
{
	struct ext4_xattr_ibody_find find = {
		.s = { .not_found = -ENODATA },
	};
	struct ext4_xattr_info info = {
		.name_index = EXT4_XATTR_INDEX_SYSTEM,
		.name = IFS_EXT4_INLINE_XATTR_NAME,
	};
	int err;

	if (!EXT4_I(inode)->i_extra_isize)
		return 0;

	err = ext4_get_inode_loc(inode, &find.iloc);
	if (err)
		return err;

	err = ext4_xattr_ibody_find(inode, &info, &find);
	if (!err && !find.s.not_found) {
		if (find.s.here->e_value_inum) {
			err = -EFSCORRUPTED;
		} else {
			EXT4_I(inode)->i_inline_off =
				(u16)((u8 *)find.s.here -
				      (u8 *)ext4_raw_inode(&find.iloc));
			EXT4_I(inode)->i_inline_size =
				IFS_EXT4_INLINE_HEAD_BYTES +
				le32_to_cpu(find.s.here->e_value_size);
		}
	}

	brelse(find.iloc.bh);
	return err;
}

static int ifs_ext4_inline_read(struct inode *inode,
				struct ext4_iloc *iloc,
				void *destination,
				unsigned int length)
{
	struct ext4_xattr_entry *entry;
	unsigned int head;
	unsigned int tail;

	if (!length)
		return 0;
	if (!ext4_has_inline_data(inode) ||
	    length > ifs_ext4_inline_size(inode))
		return -EFSCORRUPTED;

	head = min(length, IFS_EXT4_INLINE_HEAD_BYTES);
	memcpy(destination, ext4_raw_inode(iloc)->i_block, head);

	tail = length - head;
	if (tail) {
		void *tail_source;

		entry = ifs_ext4_inline_entry(inode, iloc);
		if (!entry || entry->e_value_inum ||
		    tail > le32_to_cpu(entry->e_value_size))
			return -EFSCORRUPTED;

		tail_source = ifs_ext4_inline_tail(inode, iloc);
		if (!tail_source)
			return -EFSCORRUPTED;
		memcpy((u8 *)destination + head, tail_source, tail);
	}

	return length;
}

static int ifs_ext4_inline_write(struct inode *inode,
				 struct ext4_iloc *iloc,
				 const void *source,
				 loff_t position,
				 unsigned int length)
{
	unsigned int head;
	unsigned int tail;
	unsigned int head_offset;

	if (position < 0 ||
	    (u64)position + length > ifs_ext4_inline_size(inode))
		return -EINVAL;
	if (!length)
		return 0;

	head_offset = min_t(u64, position, IFS_EXT4_INLINE_HEAD_BYTES);
	head = 0;
	if (position < IFS_EXT4_INLINE_HEAD_BYTES) {
		head = min_t(unsigned int, length,
			IFS_EXT4_INLINE_HEAD_BYTES - (unsigned int)position);
		memcpy((u8 *)ext4_raw_inode(iloc)->i_block + position,
		       source, head);
	}

	tail = length - head;
	if (tail) {
		unsigned int tail_offset =
			(unsigned int)(position + head -
				       IFS_EXT4_INLINE_HEAD_BYTES);
		struct ext4_xattr_entry *entry =
			ifs_ext4_inline_entry(inode, iloc);
		void *tail_destination = ifs_ext4_inline_tail(inode, iloc);

		if (!entry || !tail_destination || entry->e_value_inum ||
		    tail_offset + tail > le32_to_cpu(entry->e_value_size))
			return -EFSCORRUPTED;

		memcpy((u8 *)tail_destination + tail_offset,
		       (const u8 *)source + head, tail);
	}

	(void)head_offset;
	return 0;
}

static int ifs_ext4_resize_inline_xattr(handle_t *handle,
					 struct inode *inode,
					 unsigned int total_length)
{
	struct ext4_xattr_ibody_find find = {
		.s = { .not_found = -ENODATA },
	};
	struct ext4_xattr_info info = {
		.name_index = EXT4_XATTR_INDEX_SYSTEM,
		.name = IFS_EXT4_INLINE_XATTR_NAME,
	};
	unsigned int new_tail =
		total_length > IFS_EXT4_INLINE_HEAD_BYTES ?
		total_length - IFS_EXT4_INLINE_HEAD_BYTES : 0U;
	void *preserve = NULL;
	unsigned int preserve_length = 0;
	int err;

	err = ext4_get_inode_loc(inode, &find.iloc);
	if (err)
		return err;

	err = ext4_xattr_ibody_find(inode, &info, &find);
	if (err)
		goto out;

	if (!find.s.not_found) {
		unsigned int old_tail =
			le32_to_cpu(find.s.here->e_value_size);

		preserve_length = min(old_tail, new_tail);
		if (preserve_length) {
			preserve = kmalloc(new_tail, GFP_NOFS);
			if (!preserve) {
				err = -ENOMEM;
				goto out;
			}
			memset(preserve, 0, new_tail);
			err = ext4_xattr_ibody_get(
				inode, EXT4_XATTR_INDEX_SYSTEM,
				IFS_EXT4_INLINE_XATTR_NAME,
				preserve, old_tail);
			if (err < 0)
				goto out;
		}
	}

	info.value_len = new_tail;
	if (!new_tail) {
		/*
		 * Keep a zero-length system.data entry.  Its presence is the
		 * durable discriminator between ordinary i_block contents and
		 * the inline-data representation.
		 */
		info.value = "";
	} else if (preserve) {
		info.value = preserve;
	} else {
		info.value = EXT4_ZERO_XATTR_VALUE;
	}

	err = ext4_journal_get_write_access(
		handle, inode->i_sb, find.iloc.bh, EXT4_JTR_NONE);
	if (err)
		goto out;

	err = ext4_xattr_ibody_set(handle, inode, &info, &find);
	if (err)
		goto out;

	EXT4_I(inode)->i_inline_off =
		(u16)((u8 *)find.s.here -
		      (u8 *)ext4_raw_inode(&find.iloc));
	EXT4_I(inode)->i_inline_size =
		IFS_EXT4_INLINE_HEAD_BYTES + new_tail;
	ext4_set_inode_flag(inode, EXT4_INODE_INLINE_DATA);
	ext4_clear_inode_flag(inode, EXT4_INODE_EXTENTS);
	ext4_set_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);

	get_bh(find.iloc.bh);
	err = ext4_mark_iloc_dirty(handle, inode, &find.iloc);

out:
	kfree(preserve);
	brelse(find.iloc.bh);
	return err;
}

static int ifs_ext4_prepare_inline(handle_t *handle,
				   struct inode *inode,
				   loff_t end)
{
	int saved;
	int capacity;
	int err;

	if (!ext4_test_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA))
		return -ENOSPC;
	if (end < 0 || end > INT_MAX)
		return -ENOSPC;

	capacity = ext4_get_max_inline_size(inode);
	if (capacity < end)
		return -ENOSPC;

	ext4_write_lock_xattr(inode, &saved);
	(void)ext4_find_inline_data_nolock(inode);

	if (!ext4_has_inline_data(inode))
		memset(EXT4_I(inode)->i_data, 0,
		       IFS_EXT4_INLINE_HEAD_BYTES);

	if (ifs_ext4_inline_size(inode) < end)
		err = ifs_ext4_resize_inline_xattr(
			handle, inode, (unsigned int)end);
	else
		err = 0;

	ext4_write_unlock_xattr(inode, &saved);
	return err;
}

static int ifs_ext4_destroy_inline_locked(handle_t *handle,
					  struct inode *inode)
{
	struct ext4_xattr_ibody_find find = {
		.s = { .not_found = -ENODATA },
	};
	struct ext4_xattr_info info = {
		.name_index = EXT4_XATTR_INDEX_SYSTEM,
		.name = IFS_EXT4_INLINE_XATTR_NAME,
		.value = NULL,
		.value_len = 0,
	};
	int err;

	if (!ext4_has_inline_data(inode))
		return 0;

	err = ext4_get_inode_loc(inode, &find.iloc);
	if (err)
		return err;

	err = ext4_xattr_ibody_find(inode, &info, &find);
	if (err)
		goto out;

	if (!find.s.not_found) {
		err = ext4_journal_get_write_access(
			handle, inode->i_sb, find.iloc.bh, EXT4_JTR_NONE);
		if (err)
			goto out;
		err = ext4_xattr_ibody_set(handle, inode, &info, &find);
		if (err)
			goto out;
	}

	memset(ext4_raw_inode(&find.iloc)->i_block, 0,
	       IFS_EXT4_INLINE_HEAD_BYTES);
	memset(EXT4_I(inode)->i_data, 0, IFS_EXT4_INLINE_HEAD_BYTES);

	EXT4_I(inode)->i_inline_off = 0;
	EXT4_I(inode)->i_inline_size = 0;
	ext4_clear_inode_flag(inode, EXT4_INODE_INLINE_DATA);
	ext4_clear_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);

	if (ext4_has_feature_extents(inode->i_sb) &&
	    (S_ISREG(inode->i_mode) ||
	     S_ISDIR(inode->i_mode) ||
	     S_ISLNK(inode->i_mode))) {
		ext4_set_inode_flag(inode, EXT4_INODE_EXTENTS);
		ext4_ext_tree_init(handle, inode);
	}

	get_bh(find.iloc.bh);
	err = ext4_mark_iloc_dirty(handle, inode, &find.iloc);

out:
	brelse(find.iloc.bh);
	return err == -ENODATA ? 0 : err;
}

int ext4_destroy_inline_data(handle_t *handle, struct inode *inode)
{
	int saved;
	int err;

	ext4_write_lock_xattr(inode, &saved);
	down_write(&EXT4_I(inode)->i_data_sem);
	err = ifs_ext4_destroy_inline_locked(handle, inode);
	up_write(&EXT4_I(inode)->i_data_sem);
	ext4_write_unlock_xattr(inode, &saved);
	return err;
}

static int ifs_ext4_fill_inline_folio(struct inode *inode,
				      struct folio *folio)
{
	struct ext4_iloc iloc;
	void *address;
	unsigned int amount;
	int err;

	if (!folio_test_locked(folio) || folio->index != 0)
		return -EINVAL;

	err = ext4_get_inode_loc(inode, &iloc);
	if (err)
		return err;

	amount = min_t(loff_t, ifs_ext4_inline_size(inode),
		       i_size_read(inode));
	if (amount > folio_size(folio)) {
		err = -EFSCORRUPTED;
		goto out;
	}

	address = kmap_local_folio(folio, 0);
	err = ifs_ext4_inline_read(inode, &iloc, address, amount);
	if (err >= 0) {
		memset((u8 *)address + amount, 0,
		       folio_size(folio) - amount);
		folio_mark_uptodate(folio);
		err = 0;
	}
	kunmap_local(address);

out:
	brelse(iloc.bh);
	return err;
}

int ext4_readpage_inline(struct inode *inode, struct folio *folio)
{
	int err = 0;

	down_read(&EXT4_I(inode)->xattr_sem);
	if (!ext4_has_inline_data(inode)) {
		err = -EAGAIN;
	} else if (folio->index == 0) {
		err = ifs_ext4_fill_inline_folio(inode, folio);
	} else if (!folio_test_uptodate(folio)) {
		folio_zero_segment(folio, 0, folio_size(folio));
		folio_mark_uptodate(folio);
	}
	up_read(&EXT4_I(inode)->xattr_sem);

	folio_unlock(folio);
	return err;
}

static int ifs_ext4_inline_to_block(handle_t *handle,
				    struct inode *inode,
				    struct ext4_iloc *iloc)
{
	unsigned int inline_size = ifs_ext4_inline_size(inode);
	struct ext4_map_blocks map = {
		.m_lblk = 0,
		.m_len = 1,
	};
	struct buffer_head *data = NULL;
	void *payload = NULL;
	int err;

	if (!inline_size)
		return 0;

	payload = kmalloc(inline_size, GFP_NOFS);
	if (!payload)
		return -ENOMEM;

	err = ifs_ext4_inline_read(
		inode, iloc, payload, inline_size);
	if (err < 0)
		goto out;

	if (S_ISDIR(inode->i_mode)) {
		err = ext4_check_all_de(
			inode, iloc->bh,
			(u8 *)payload + IFS_EXT4_INLINE_PARENT_BYTES,
			inline_size - IFS_EXT4_INLINE_PARENT_BYTES);
		if (err)
			goto out;
	}

	err = ifs_ext4_destroy_inline_locked(handle, inode);
	if (err)
		goto out;

	err = ext4_map_blocks(
		handle, inode, &map, EXT4_GET_BLOCKS_CREATE);
	if (err < 0)
		goto restore;
	if (!(map.m_flags & EXT4_MAP_MAPPED)) {
		err = -EIO;
		goto restore;
	}

	data = sb_getblk(inode->i_sb, map.m_pblk);
	if (!data) {
		err = -ENOMEM;
		goto restore;
	}

	lock_buffer(data);
	err = ext4_journal_get_create_access(
		handle, inode->i_sb, data, EXT4_JTR_NONE);
	if (err) {
		unlock_buffer(data);
		goto restore;
	}

	memset(data->b_data, 0, inode->i_sb->s_blocksize);

	if (!S_ISDIR(inode->i_mode)) {
		memcpy(data->b_data, payload, inline_size);
		set_buffer_uptodate(data);
		unlock_buffer(data);
		err = ext4_handle_dirty_metadata(handle, inode, data);
		goto out;
	}

	{
		struct ext4_dir_entry_2 *first =
			(struct ext4_dir_entry_2 *)data->b_data;
		struct ext4_dir_entry_2 *after_dotdot;
		unsigned int checksum_bytes = ext4_has_metadata_csum(inode->i_sb) ?
			sizeof(struct ext4_dir_entry_tail) : 0U;
		unsigned int prefix;
		unsigned int source_bytes =
			inline_size - IFS_EXT4_INLINE_PARENT_BYTES;

		after_dotdot = ext4_init_dot_dotdot(
			inode, first, inode->i_sb->s_blocksize, 0,
			le32_to_cpu(
				((struct ext4_dir_entry_2 *)payload)->inode),
			1);
		prefix = (u8 *)after_dotdot - (u8 *)data->b_data;
		memcpy(after_dotdot,
		       (u8 *)payload + IFS_EXT4_INLINE_PARENT_BYTES,
		       source_bytes);

		/* Give the final copied dirent all remaining directory space. */
		if (source_bytes) {
			u8 *cursor = (u8 *)after_dotdot;
			u8 *limit = cursor + source_bytes;
			struct ext4_dir_entry_2 *last = NULL;

			while (cursor < limit) {
				struct ext4_dir_entry_2 *de =
					(struct ext4_dir_entry_2 *)cursor;
				unsigned int len =
					ext4_rec_len_from_disk(
						de->rec_len, source_bytes);

				if (!len || cursor + len > limit) {
					err = -EFSCORRUPTED;
					break;
				}
				last = de;
				cursor += len;
			}
			if (!err && last) {
				unsigned int offset =
					(u8 *)last - (u8 *)data->b_data;
				last->rec_len = ext4_rec_len_to_disk(
					inode->i_sb->s_blocksize -
					checksum_bytes - offset,
					inode->i_sb->s_blocksize);
			}
		}

		if (!err && checksum_bytes)
			ext4_initialize_dirent_tail(
				data, inode->i_sb->s_blocksize);
		if (!err) {
			inode->i_size = inode->i_sb->s_blocksize;
			EXT4_I(inode)->i_disksize = inode->i_sb->s_blocksize;
			set_buffer_uptodate(data);
			unlock_buffer(data);
			err = ext4_handle_dirty_dirblock(
				handle, inode, data);
			if (!err) {
				set_buffer_verified(data);
				err = ext4_mark_inode_dirty(handle, inode);
			}
		} else {
			unlock_buffer(data);
		}
		(void)prefix;
	}
	goto out;

restore:
	/*
	 * Conversion failure must not leave the inode in a half-converted
	 * state. Recreate the inline xattr and restore the captured bytes.
	 */
	if (!ifs_ext4_resize_inline_xattr(handle, inode, inline_size)) {
		struct ext4_iloc restore_iloc;

		if (!ext4_get_inode_loc(inode, &restore_iloc)) {
			(void)ifs_ext4_inline_write(
				inode, &restore_iloc,
				payload, 0, inline_size);
			brelse(restore_iloc.bh);
		}
	}

out:
	brelse(data);
	kfree(payload);
	return err;
}

static int ifs_ext4_convert_inline_mapping(struct address_space *mapping,
					   struct inode *inode)
{
	struct ext4_iloc iloc;
	struct folio *folio;
	handle_t *handle;
	int credits;
	int saved;
	int err;

	if (!ext4_has_inline_data(inode)) {
		ext4_clear_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);
		return 0;
	}

	credits = ext4_writepage_trans_blocks(inode);
	err = ext4_get_inode_loc(inode, &iloc);
	if (err)
		return err;

	handle = ext4_journal_start(inode, EXT4_HT_WRITE_PAGE, credits);
	if (IS_ERR(handle)) {
		brelse(iloc.bh);
		return PTR_ERR(handle);
	}

	folio = __filemap_get_folio(
		mapping, 0, FGP_WRITEBEGIN | FGP_NOFS,
		mapping_gfp_mask(mapping));
	if (IS_ERR(folio)) {
		err = PTR_ERR(folio);
		goto stop;
	}

	ext4_write_lock_xattr(inode, &saved);
	if (!ext4_has_inline_data(inode)) {
		err = 0;
		goto unlock_xattr;
	}

	if (!folio_test_uptodate(folio)) {
		err = ifs_ext4_fill_inline_folio(inode, folio);
		if (err)
			goto unlock_xattr;
	}

	err = ifs_ext4_inline_to_block(handle, inode, &iloc);
	if (!err) {
		folio_mark_uptodate(folio);
		folio_mark_dirty(folio);
	}

unlock_xattr:
	ext4_write_unlock_xattr(inode, &saved);
	folio_unlock(folio);
	folio_put(folio);
stop:
	ext4_journal_stop(handle);
	brelse(iloc.bh);
	return err;
}

int ext4_try_to_write_inline_data(struct address_space *mapping,
				  struct inode *inode,
				  loff_t pos,
				  unsigned int len,
				  struct folio **foliop)
{
	struct ext4_iloc iloc;
	struct folio *folio;
	handle_t *handle;
	int err;

	if ((u64)pos + len > ext4_get_max_inline_size(inode))
		return ifs_ext4_convert_inline_mapping(mapping, inode);

	err = ext4_get_inode_loc(inode, &iloc);
	if (err)
		return err;

	handle = ext4_journal_start(inode, EXT4_HT_INODE, 1);
	if (IS_ERR(handle)) {
		brelse(iloc.bh);
		return PTR_ERR(handle);
	}

	err = ifs_ext4_prepare_inline(handle, inode, pos + len);
	if (err == -ENOSPC) {
		ext4_journal_stop(handle);
		brelse(iloc.bh);
		return ifs_ext4_convert_inline_mapping(mapping, inode);
	}
	if (err)
		goto stop;

	err = ext4_journal_get_write_access(
		handle, inode->i_sb, iloc.bh, EXT4_JTR_NONE);
	if (err)
		goto stop;

	folio = __filemap_get_folio(
		mapping, 0, FGP_WRITEBEGIN | FGP_NOFS,
		mapping_gfp_mask(mapping));
	if (IS_ERR(folio)) {
		err = PTR_ERR(folio);
		goto stop;
	}

	down_read(&EXT4_I(inode)->xattr_sem);
	if (!ext4_has_inline_data(inode)) {
		up_read(&EXT4_I(inode)->xattr_sem);
		folio_unlock(folio);
		folio_put(folio);
		err = 0;
		goto stop;
	}

	if (!folio_test_uptodate(folio)) {
		err = ifs_ext4_fill_inline_folio(inode, folio);
		if (err) {
			up_read(&EXT4_I(inode)->xattr_sem);
			folio_unlock(folio);
			folio_put(folio);
			goto stop;
		}
	}
	up_read(&EXT4_I(inode)->xattr_sem);

	*foliop = folio;
	brelse(iloc.bh);
	return 1;

stop:
	ext4_journal_stop(handle);
	brelse(iloc.bh);
	return err;
}

int ext4_write_inline_data_end(struct inode *inode,
			       loff_t pos,
			       unsigned int len,
			       unsigned int copied,
			       struct folio *folio)
{
	handle_t *handle = ext4_journal_current_handle();
	struct ext4_iloc iloc;
	void *address;
	int saved;
	int err = 0;
	int stop_err;

	if (copied < len && !folio_test_uptodate(folio))
		copied = 0;

	if (copied) {
		err = ext4_get_inode_loc(inode, &iloc);
		if (!err) {
			ext4_write_lock_xattr(inode, &saved);
			if (!ext4_has_inline_data(inode)) {
				err = -EAGAIN;
			} else {
				address = kmap_local_folio(folio, 0);
				err = ifs_ext4_inline_write(
					inode, &iloc, address, pos, copied);
				kunmap_local(address);
				if (!err) {
					folio_mark_uptodate(folio);
					folio_clear_dirty(folio);
					ext4_update_inode_size(
						inode, pos + copied);
				}
			}
			ext4_write_unlock_xattr(inode, &saved);
			brelse(iloc.bh);
		}
	}

	folio_unlock(folio);
	folio_put(folio);

	if (!err && copied)
		mark_inode_dirty(inode);

	if ((u64)pos + len > i_size_read(inode) &&
	    ext4_can_truncate(inode))
		ext4_orphan_add(handle, inode);

	stop_err = ext4_journal_stop(handle);
	if (!err)
		err = stop_err;

	if ((u64)pos + len > i_size_read(inode)) {
		ext4_truncate_failed_write(inode);
		if (inode->i_nlink)
			ext4_orphan_del(NULL, inode);
	}

	return err ? err : copied;
}

static int ifs_ext4_da_materialise_inline(struct address_space *mapping,
					  struct inode *inode,
					  void **fsdata)
{
	struct folio *folio;
	unsigned int bytes;
	int err = 0;

	folio = __filemap_get_folio(
		mapping, 0, FGP_WRITEBEGIN,
		mapping_gfp_mask(mapping));
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	down_read(&EXT4_I(inode)->xattr_sem);
	if (!ext4_has_inline_data(inode)) {
		ext4_clear_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);
		goto out;
	}

	bytes = ifs_ext4_inline_size(inode);
	if (!folio_test_uptodate(folio)) {
		err = ifs_ext4_fill_inline_folio(inode, folio);
		if (err)
			goto out;
	}

	err = ext4_block_write_begin(
		NULL, folio, 0, bytes, ext4_da_get_block_prep);
	if (!err) {
		clear_buffer_new(folio_buffers(folio));
		folio_mark_dirty(folio);
		folio_mark_uptodate(folio);
		ext4_clear_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);
		*fsdata = (void *)CONVERT_INLINE_DATA;
	}

out:
	up_read(&EXT4_I(inode)->xattr_sem);
	folio_unlock(folio);
	folio_put(folio);
	return err;
}

int ext4_da_write_inline_data_begin(struct address_space *mapping,
				    struct inode *inode,
				    loff_t pos,
				    unsigned int len,
				    struct folio **foliop,
				    void **fsdata)
{
	struct ext4_iloc iloc;
	struct folio *folio;
	handle_t *handle;
	int retries = 0;
	int err;

	err = ext4_get_inode_loc(inode, &iloc);
	if (err)
		return err;

retry:
	handle = ext4_journal_start(inode, EXT4_HT_INODE, 1);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out;
	}

	err = ifs_ext4_prepare_inline(handle, inode, pos + len);
	if (err == -ENOSPC) {
		ext4_journal_stop(handle);
		err = ifs_ext4_da_materialise_inline(
			mapping, inode, fsdata);
		if (err == -ENOSPC &&
		    ext4_should_retry_alloc(inode->i_sb, &retries))
			goto retry;
		goto out;
	}
	if (err)
		goto stop;

	folio = __filemap_get_folio(
		mapping, 0, FGP_WRITEBEGIN | FGP_NOFS,
		mapping_gfp_mask(mapping));
	if (IS_ERR(folio)) {
		err = PTR_ERR(folio);
		goto stop;
	}

	down_read(&EXT4_I(inode)->xattr_sem);
	if (!ext4_has_inline_data(inode)) {
		err = 0;
		goto release;
	}
	if (!folio_test_uptodate(folio)) {
		err = ifs_ext4_fill_inline_folio(inode, folio);
		if (err)
			goto release;
	}
	err = ext4_journal_get_write_access(
		handle, inode->i_sb, iloc.bh, EXT4_JTR_NONE);
	if (err)
		goto release;

	up_read(&EXT4_I(inode)->xattr_sem);
	*foliop = folio;
	brelse(iloc.bh);
	return 1;

release:
	up_read(&EXT4_I(inode)->xattr_sem);
	folio_unlock(folio);
	folio_put(folio);
stop:
	ext4_journal_stop(handle);
out:
	brelse(iloc.bh);
	return err;
}

#ifdef INLINE_DIR_DEBUG
static void ext4_show_inline_dir(struct inode *dir,
			  struct buffer_head *bh,
			  void *start,
			  int size)
{
	u8 *cursor = start;
	u8 *limit = cursor + size;

	while (cursor < limit) {
		struct ext4_dir_entry_2 *de =
			(struct ext4_dir_entry_2 *)cursor;
		unsigned int len =
			ext4_rec_len_from_disk(de->rec_len, size);

		if (ext4_check_dir_entry(
			    dir, NULL, de, bh, start, size,
			    cursor - (u8 *)start))
			break;
		if (!len)
			break;
		cursor += len;
	}
}
#else
void ext4_show_inline_dir(struct inode *dir,
			  struct buffer_head *bh,
			  void *start,
			  int size)
{
	(void)dir;
	(void)bh;
	(void)start;
	(void)size;
}
#endif

static int ifs_ext4_inline_add_dirent(handle_t *handle,
				      struct ext4_filename *fname,
				      struct inode *dir,
				      struct inode *inode,
				      struct ext4_iloc *iloc,
				      void *region,
				      unsigned int region_size)
{
	struct ext4_dir_entry_2 *slot;
	int err;

	err = ext4_find_dest_de(
		dir, inode, iloc->bh, region,
		region_size, fname, &slot);
	if (err)
		return err;

	err = ext4_journal_get_write_access(
		handle, dir->i_sb, iloc->bh, EXT4_JTR_NONE);
	if (err)
		return err;

	ext4_insert_dentry(dir, inode, slot, region_size, fname);
	inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	ext4_update_dx_flag(dir);
	inode_inc_iversion(dir);
	return 1;
}

static void ifs_ext4_expand_last_dirent(void *region,
				       unsigned int old_size,
				       unsigned int new_size)
{
	u8 *cursor = region;
	u8 *limit = cursor + old_size;
	struct ext4_dir_entry_2 *last = NULL;

	if (!old_size) {
		last = region;
		last->inode = 0;
		last->rec_len =
			ext4_rec_len_to_disk(new_size, new_size);
		return;
	}

	while (cursor < limit) {
		struct ext4_dir_entry_2 *de =
			(struct ext4_dir_entry_2 *)cursor;
		unsigned int len =
			ext4_rec_len_from_disk(de->rec_len, old_size);

		if (!len || cursor + len > limit)
			return;
		last = de;
		cursor += len;
	}

	if (last) {
		unsigned int offset = (u8 *)last - (u8 *)region;

		last->rec_len = ext4_rec_len_to_disk(
			new_size - offset, new_size);
	}
}

static int ifs_ext4_grow_inline_directory(handle_t *handle,
					   struct inode *dir,
					   struct ext4_iloc *iloc)
{
	unsigned int old_tail =
		ifs_ext4_inline_size(dir) > IFS_EXT4_INLINE_HEAD_BYTES ?
		ifs_ext4_inline_size(dir) - IFS_EXT4_INLINE_HEAD_BYTES : 0U;
	unsigned int capacity =
		ifs_ext4_inline_tail_capacity(dir, iloc);
	int err;

	if (capacity <= old_tail + ext4_dir_rec_len(1, NULL))
		return -ENOSPC;

	err = ifs_ext4_resize_inline_xattr(
		handle, dir, IFS_EXT4_INLINE_HEAD_BYTES + capacity);
	if (err)
		return err;

	ifs_ext4_expand_last_dirent(
		ifs_ext4_inline_tail(dir, iloc),
		old_tail, capacity);
	dir->i_size = EXT4_I(dir)->i_disksize =
		ifs_ext4_inline_size(dir);
	return 0;
}

int ext4_try_add_inline_entry(handle_t *handle,
			      struct ext4_filename *fname,
			      struct inode *dir,
			      struct inode *inode)
{
	struct ext4_iloc iloc;
	void *region;
	unsigned int region_size;
	int saved;
	int err;
	int dirty_err;

	err = ext4_get_inode_loc(dir, &iloc);
	if (err)
		return err;

	ext4_write_lock_xattr(dir, &saved);
	if (!ext4_has_inline_data(dir)) {
		err = 0;
		goto out;
	}

	region = (u8 *)ext4_raw_inode(&iloc)->i_block +
		IFS_EXT4_INLINE_PARENT_BYTES;
	region_size =
		IFS_EXT4_INLINE_HEAD_BYTES - IFS_EXT4_INLINE_PARENT_BYTES;

	err = ifs_ext4_inline_add_dirent(
		handle, fname, dir, inode, &iloc,
		region, region_size);
	if (err != -ENOSPC)
		goto out;

	region_size = ifs_ext4_inline_size(dir) -
		IFS_EXT4_INLINE_HEAD_BYTES;
	if (!region_size) {
		err = ifs_ext4_grow_inline_directory(
			handle, dir, &iloc);
		if (err && err != -ENOSPC)
			goto out;
		region_size = ifs_ext4_inline_size(dir) -
			IFS_EXT4_INLINE_HEAD_BYTES;
	}

	if (region_size) {
		region = ifs_ext4_inline_tail(dir, &iloc);
		err = ifs_ext4_inline_add_dirent(
			handle, fname, dir, inode, &iloc,
			region, region_size);
		if (err != -ENOSPC)
			goto out;
	}

	err = ifs_ext4_inline_to_block(handle, dir, &iloc);

out:
	ext4_write_unlock_xattr(dir, &saved);
	dirty_err = ext4_mark_inode_dirty(handle, dir);
	if (!err)
		err = dirty_err;
	brelse(iloc.bh);
	return err;
}

static int ifs_ext4_copy_inline_directory(struct inode *inode,
					  struct ext4_iloc *iloc,
					  void **buffer,
					  unsigned int *size)
{
	void *copy;
	int err;

	*size = ifs_ext4_inline_size(inode);
	copy = kmalloc(*size, GFP_NOFS);
	if (!copy)
		return -ENOMEM;

	err = ifs_ext4_inline_read(inode, iloc, copy, *size);
	if (err < 0) {
		kfree(copy);
		return err;
	}
	*buffer = copy;
	return 0;
}

int ext4_inlinedir_to_tree(struct file *file,
			   struct inode *dir,
			   ext4_lblk_t block,
			   struct dx_hash_info *hinfo,
			   __u32 start_hash,
			   __u32 start_minor_hash,
			   int *has_inline_data)
{
	struct inode *inode = file_inode(file);
	struct ext4_iloc iloc;
	struct fscrypt_str name;
	struct ext4_dir_entry_2 synthetic = { };
	void *buffer = NULL;
	unsigned int size = 0;
	unsigned int offset = 0;
	unsigned int parent;
	int count = 0;
	int err;

	(void)dir;
	(void)block;

	err = ext4_get_inode_loc(inode, &iloc);
	if (err)
		return err;

	down_read(&EXT4_I(inode)->xattr_sem);
	if (!ext4_has_inline_data(inode)) {
		*has_inline_data = 0;
		err = 0;
		goto unlock;
	}

	err = ifs_ext4_copy_inline_directory(
		inode, &iloc, &buffer, &size);
	if (err)
		goto unlock;

	parent = le32_to_cpu(
		((struct ext4_dir_entry_2 *)buffer)->inode);

	while (offset < size) {
		struct ext4_dir_entry_2 *de;
		unsigned int next;

		if (offset == 0) {
			synthetic.inode = cpu_to_le32(inode->i_ino);
			synthetic.name_len = 1;
			memcpy(synthetic.name, ".", 1);
			ext4_set_de_type(inode->i_sb, &synthetic, S_IFDIR);
			de = &synthetic;
			next = 2;
		} else if (offset == 2) {
			synthetic.inode = cpu_to_le32(parent);
			synthetic.name_len = 2;
			memcpy(synthetic.name, "..", 2);
			ext4_set_de_type(inode->i_sb, &synthetic, S_IFDIR);
			de = &synthetic;
			next = IFS_EXT4_INLINE_PARENT_BYTES;
		} else {
			de = (struct ext4_dir_entry_2 *)
				((u8 *)buffer + offset);
			next = offset + ext4_rec_len_from_disk(
				de->rec_len, size);
			if (next <= offset || next > size) {
				err = -EFSCORRUPTED;
				break;
			}
		}

		if (ext4_hash_in_dirent(inode)) {
			hinfo->hash = EXT4_DIRENT_HASH(de);
			hinfo->minor_hash = EXT4_DIRENT_MINOR_HASH(de);
		} else {
			err = ext4fs_dirhash(
				inode, de->name, de->name_len, hinfo);
			if (err)
				break;
		}

		if (de->inode &&
		    (hinfo->hash > start_hash ||
		     (hinfo->hash == start_hash &&
		      hinfo->minor_hash >= start_minor_hash))) {
			name.name = de->name;
			name.len = de->name_len;
			err = ext4_htree_store_dirent(
				file, hinfo->hash, hinfo->minor_hash,
				de, &name);
			if (err)
				break;
			count++;
		}

		offset = next;
	}

	if (!err)
		err = count;

unlock:
	up_read(&EXT4_I(inode)->xattr_sem);
	kfree(buffer);
	brelse(iloc.bh);
	return err;
}

int ext4_read_inline_dir(struct file *file,
			 struct dir_context *ctx,
			 int *has_inline_data)
{
	struct inode *inode = file_inode(file);
	struct dir_private_info *info = file->private_data;
	struct ext4_iloc iloc;
	void *buffer = NULL;
	unsigned int size = 0;
	unsigned int parent;
	unsigned int dot = ext4_dir_rec_len(1, NULL);
	unsigned int dotdot = dot + ext4_dir_rec_len(2, NULL);
	unsigned int translated_base = dotdot - IFS_EXT4_INLINE_PARENT_BYTES;
	int err;

	err = ext4_get_inode_loc(inode, &iloc);
	if (err)
		return err;

	down_read(&EXT4_I(inode)->xattr_sem);
	if (!ext4_has_inline_data(inode)) {
		*has_inline_data = 0;
		err = 0;
		goto unlock;
	}

	err = ifs_ext4_copy_inline_directory(
		inode, &iloc, &buffer, &size);
	if (err)
		goto unlock;

	parent = le32_to_cpu(
		((struct ext4_dir_entry_2 *)buffer)->inode);

	if (info && !inode_eq_iversion(inode, info->cookie)) {
		ctx->pos = 0;
		info->cookie = inode_query_iversion(inode);
	}

	while (ctx->pos < translated_base + size) {
		if (ctx->pos == 0) {
			if (!dir_emit(
				    ctx, ".", 1, inode->i_ino, DT_DIR))
				break;
			ctx->pos = dot;
			continue;
		}
		if (ctx->pos == dot) {
			if (!dir_emit(ctx, "..", 2, parent, DT_DIR))
				break;
			ctx->pos = dotdot;
			continue;
		}

		{
			unsigned int raw_offset =
				ctx->pos - translated_base;
			struct ext4_dir_entry_2 *de;
			unsigned int len;

			if (raw_offset < IFS_EXT4_INLINE_PARENT_BYTES) {
				ctx->pos = dotdot;
				continue;
			}
			de = (struct ext4_dir_entry_2 *)
				((u8 *)buffer + raw_offset);
			len = ext4_rec_len_from_disk(de->rec_len, size);
			if (!len || raw_offset + len > size) {
				err = -EFSCORRUPTED;
				break;
			}
			if (le32_to_cpu(de->inode) &&
			    !dir_emit(ctx, de->name, de->name_len,
				      le32_to_cpu(de->inode),
				      get_dtype(inode->i_sb, de->file_type)))
				break;
			ctx->pos += len;
		}
	}

unlock:
	up_read(&EXT4_I(inode)->xattr_sem);
	kfree(buffer);
	brelse(iloc.bh);
	return err;
}

void *ext4_read_inline_link(struct inode *inode)
{
	struct ext4_iloc iloc;
	void *buffer;
	unsigned int size;
	int err;

	err = ext4_get_inode_loc(inode, &iloc);
	if (err)
		return ERR_PTR(err);

	size = ifs_ext4_inline_size(inode);
	buffer = kmalloc(size + 1U, GFP_NOFS);
	if (!buffer) {
		brelse(iloc.bh);
		return ERR_PTR(-ENOMEM);
	}

	err = ifs_ext4_inline_read(inode, &iloc, buffer, size);
	brelse(iloc.bh);
	if (err < 0) {
		kfree(buffer);
		return ERR_PTR(err);
	}

	nd_terminate_link(buffer, inode->i_size, size);
	return buffer;
}

struct buffer_head *ext4_get_first_inline_block(
	struct inode *inode,
	struct ext4_dir_entry_2 **parent_de,
	int *retval)
{
	struct ext4_iloc iloc;

	*retval = ext4_get_inode_loc(inode, &iloc);
	if (*retval)
		return NULL;

	*parent_de = (struct ext4_dir_entry_2 *)
		ext4_raw_inode(&iloc)->i_block;
	return iloc.bh;
}

int ext4_try_create_inline_dir(handle_t *handle,
			       struct inode *parent,
			       struct inode *inode)
{
	struct ext4_iloc iloc;
	struct ext4_dir_entry_2 *parent_slot;
	struct ext4_dir_entry_2 *free_slot;
	int err;

	err = ext4_get_inode_loc(inode, &iloc);
	if (err)
		return err;

	err = ifs_ext4_prepare_inline(
		handle, inode, IFS_EXT4_INLINE_HEAD_BYTES);
	if (err)
		goto out;

	parent_slot = (struct ext4_dir_entry_2 *)
		ext4_raw_inode(&iloc)->i_block;
	memset(parent_slot, 0, IFS_EXT4_INLINE_HEAD_BYTES);
	parent_slot->inode = cpu_to_le32(parent->i_ino);

	free_slot = (struct ext4_dir_entry_2 *)
		((u8 *)parent_slot + IFS_EXT4_INLINE_PARENT_BYTES);
	free_slot->rec_len = ext4_rec_len_to_disk(
		IFS_EXT4_INLINE_HEAD_BYTES -
		IFS_EXT4_INLINE_PARENT_BYTES,
		IFS_EXT4_INLINE_HEAD_BYTES);

	set_nlink(inode, 2);
	inode->i_size = EXT4_I(inode)->i_disksize =
		IFS_EXT4_INLINE_HEAD_BYTES;
	err = ext4_mark_inode_dirty(handle, inode);

out:
	brelse(iloc.bh);
	return err;
}

static int ifs_ext4_search_inline_region(
	struct inode *dir,
	struct ext4_filename *fname,
	struct buffer_head *bh,
	void *region,
	unsigned int size,
	struct ext4_dir_entry_2 **result)
{
	if (!size)
		return 0;
	return ext4_search_dir(
		bh, region, size, dir, fname, 0, result);
}

struct buffer_head *ext4_find_inline_entry(
	struct inode *dir,
	struct ext4_filename *fname,
	struct ext4_dir_entry_2 **res_dir,
	int *has_inline_data)
{
	struct ext4_iloc iloc;
	void *region;
	unsigned int size;
	int err;

	err = ext4_get_inode_loc(dir, &iloc);
	if (err)
		return ERR_PTR(err);

	down_read(&EXT4_I(dir)->xattr_sem);
	if (!ext4_has_inline_data(dir)) {
		*has_inline_data = 0;
		err = 0;
		goto miss;
	}

	region = (u8 *)ext4_raw_inode(&iloc)->i_block +
		IFS_EXT4_INLINE_PARENT_BYTES;
	size = IFS_EXT4_INLINE_HEAD_BYTES -
		IFS_EXT4_INLINE_PARENT_BYTES;
	err = ifs_ext4_search_inline_region(
		dir, fname, iloc.bh, region, size, res_dir);
	if (err == 1)
		goto found;
	if (err < 0)
		goto error;

	size = ifs_ext4_inline_size(dir) -
		IFS_EXT4_INLINE_HEAD_BYTES;
	if (size) {
		region = ifs_ext4_inline_tail(dir, &iloc);
		err = ifs_ext4_search_inline_region(
			dir, fname, iloc.bh, region, size, res_dir);
		if (err == 1)
			goto found;
		if (err < 0)
			goto error;
	}

miss:
	up_read(&EXT4_I(dir)->xattr_sem);
	brelse(iloc.bh);
	return NULL;

found:
	up_read(&EXT4_I(dir)->xattr_sem);
	return iloc.bh;

error:
	up_read(&EXT4_I(dir)->xattr_sem);
	brelse(iloc.bh);
	return ERR_PTR(err);
}

int ext4_delete_inline_entry(handle_t *handle,
			     struct inode *dir,
			     struct ext4_dir_entry_2 *entry,
			     struct buffer_head *bh,
			     int *has_inline_data)
{
	struct ext4_iloc iloc;
	void *head;
	void *tail;
	unsigned int head_size =
		IFS_EXT4_INLINE_HEAD_BYTES -
		IFS_EXT4_INLINE_PARENT_BYTES;
	unsigned int tail_size;
	int saved;
	int err;

	err = ext4_get_inode_loc(dir, &iloc);
	if (err)
		return err;

	ext4_write_lock_xattr(dir, &saved);
	if (!ext4_has_inline_data(dir)) {
		*has_inline_data = 0;
		err = 0;
		goto out;
	}

	err = ext4_journal_get_write_access(
		handle, dir->i_sb, bh, EXT4_JTR_NONE);
	if (err)
		goto out;

	head = (u8 *)ext4_raw_inode(&iloc)->i_block +
		IFS_EXT4_INLINE_PARENT_BYTES;
	tail = ifs_ext4_inline_tail(dir, &iloc);
	tail_size = ifs_ext4_inline_size(dir) -
		IFS_EXT4_INLINE_HEAD_BYTES;

	if ((u8 *)entry >= (u8 *)head &&
	    (u8 *)entry < (u8 *)head + head_size) {
		err = ext4_generic_delete_entry(
			dir, entry, bh, head, head_size, 0);
	} else if (tail &&
		   (u8 *)entry >= (u8 *)tail &&
		   (u8 *)entry < (u8 *)tail + tail_size) {
		err = ext4_generic_delete_entry(
			dir, entry, bh, tail, tail_size, 0);
	} else {
		err = -EFSCORRUPTED;
	}

	if (!err)
		err = ext4_mark_inode_dirty(handle, dir);

out:
	ext4_write_unlock_xattr(dir, &saved);
	brelse(iloc.bh);
	if (err && err != -ENOENT)
		ext4_std_error(dir->i_sb, err);
	return err;
}

static bool ifs_ext4_region_is_empty(struct inode *dir,
				     struct buffer_head *bh,
				     void *region,
				     unsigned int size,
				     unsigned int base)
{
	u8 *cursor = region;
	u8 *limit = cursor + size;

	while (cursor < limit) {
		struct ext4_dir_entry_2 *de =
			(struct ext4_dir_entry_2 *)cursor;
		unsigned int len =
			ext4_rec_len_from_disk(de->rec_len, size);

		if (!len || cursor + len > limit)
			return false;
		if (ext4_check_dir_entry(
			    dir, NULL, de, bh, region, size,
			    base + (cursor - (u8 *)region)))
			return false;
		if (le32_to_cpu(de->inode))
			return false;
		cursor += len;
	}
	return true;
}

bool empty_inline_dir(struct inode *dir, int *has_inline_data)
{
	struct ext4_iloc iloc;
	void *head;
	void *tail;
	unsigned int tail_size;
	bool empty = false;
	int err;

	err = ext4_get_inode_loc(dir, &iloc);
	if (err)
		return false;

	down_read(&EXT4_I(dir)->xattr_sem);
	if (!ext4_has_inline_data(dir)) {
		*has_inline_data = 0;
		empty = true;
		goto out;
	}

	if (!le32_to_cpu(
		    ((struct ext4_dir_entry_2 *)
		     ext4_raw_inode(&iloc)->i_block)->inode))
		goto out;

	head = (u8 *)ext4_raw_inode(&iloc)->i_block +
		IFS_EXT4_INLINE_PARENT_BYTES;
	if (!ifs_ext4_region_is_empty(
		    dir, iloc.bh, head,
		    IFS_EXT4_INLINE_HEAD_BYTES -
		    IFS_EXT4_INLINE_PARENT_BYTES,
		    IFS_EXT4_INLINE_PARENT_BYTES))
		goto out;

	tail_size = ifs_ext4_inline_size(dir) -
		IFS_EXT4_INLINE_HEAD_BYTES;
	tail = ifs_ext4_inline_tail(dir, &iloc);
	if (tail_size &&
	    !ifs_ext4_region_is_empty(
		    dir, iloc.bh, tail, tail_size,
		    IFS_EXT4_INLINE_HEAD_BYTES))
		goto out;

	empty = true;

out:
	up_read(&EXT4_I(dir)->xattr_sem);
	brelse(iloc.bh);
	return empty;
}

int ext4_inline_data_iomap(struct inode *inode, struct iomap *iomap)
{
	struct ext4_iloc iloc;
	u64 address;
	int err = -EAGAIN;

	down_read(&EXT4_I(inode)->xattr_sem);
	if (!ext4_has_inline_data(inode))
		goto out;

	err = ext4_get_inode_loc(inode, &iloc);
	if (err)
		goto out;

	address = (u64)iloc.bh->b_blocknr <<
		inode->i_sb->s_blocksize_bits;
	address += (u8 *)ext4_raw_inode(&iloc) -
		(u8 *)iloc.bh->b_data;
	address += offsetof(struct ext4_inode, i_block);

	iomap->addr = address;
	iomap->offset = 0;
	iomap->length = min_t(
		loff_t, ifs_ext4_inline_size(inode),
		i_size_read(inode));
	iomap->type = IOMAP_INLINE;
	iomap->flags = 0;
	brelse(iloc.bh);

out:
	up_read(&EXT4_I(inode)->xattr_sem);
	return err;
}

int ext4_inline_data_truncate(struct inode *inode, int *has_inline)
{
	handle_t *handle;
	struct ext4_iloc iloc;
	unsigned int target;
	unsigned int old_size;
	void *snapshot = NULL;
	int saved;
	int credits;
	int err;

	credits = ext4_writepage_trans_blocks(inode);
	handle = ext4_journal_start(inode, EXT4_HT_INODE, credits);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	ext4_write_lock_xattr(inode, &saved);
	if (!ext4_has_inline_data(inode)) {
		*has_inline = 0;
		err = 0;
		goto unlock;
	}

	err = ext4_get_inode_loc(inode, &iloc);
	if (err)
		goto unlock;

	down_write(&EXT4_I(inode)->i_data_sem);
	old_size = ifs_ext4_inline_size(inode);
	target = min_t(loff_t, i_size_read(inode), old_size);

	snapshot = kmalloc(old_size, GFP_NOFS);
	if (!snapshot) {
		err = -ENOMEM;
		goto data_unlock;
	}
	err = ifs_ext4_inline_read(
		inode, &iloc, snapshot, old_size);
	if (err < 0)
		goto data_unlock;

	EXT4_I(inode)->i_disksize = i_size_read(inode);
	if (target < old_size) {
		unsigned int representation =
			max(target, IFS_EXT4_INLINE_HEAD_BYTES);

		err = ifs_ext4_resize_inline_xattr(
			handle, inode, representation);
		if (err)
			goto data_unlock;

		err = ifs_ext4_inline_write(
			inode, &iloc, snapshot, 0, target);
		if (err)
			goto data_unlock;

		if (target < IFS_EXT4_INLINE_HEAD_BYTES)
			memset((u8 *)ext4_raw_inode(&iloc)->i_block + target,
			       0,
			       IFS_EXT4_INLINE_HEAD_BYTES - target);
	}

	inode_set_mtime_to_ts(
		inode, inode_set_ctime_current(inode));
	err = ext4_mark_inode_dirty(handle, inode);
	if (!err && IS_SYNC(inode))
		ext4_handle_sync(handle);

data_unlock:
	kfree(snapshot);
	up_write(&EXT4_I(inode)->i_data_sem);
	brelse(iloc.bh);
unlock:
	ext4_write_unlock_xattr(inode, &saved);
	if (inode->i_nlink)
		ext4_orphan_del(handle, inode);
	ext4_journal_stop(handle);
	return err;
}

int ext4_convert_inline_data(struct inode *inode)
{
	struct ext4_iloc iloc;
	handle_t *handle;
	int saved;
	int credits;
	int err;

	if (!ext4_has_inline_data(inode)) {
		ext4_clear_inode_state(inode, EXT4_STATE_MAY_INLINE_DATA);
		return 0;
	}

	credits = ext4_writepage_trans_blocks(inode);
	err = ext4_get_inode_loc(inode, &iloc);
	if (err)
		return err;

	handle = ext4_journal_start(
		inode, EXT4_HT_WRITE_PAGE, credits);
	if (IS_ERR(handle)) {
		brelse(iloc.bh);
		return PTR_ERR(handle);
	}

	ext4_write_lock_xattr(inode, &saved);
	if (ext4_has_inline_data(inode))
		err = ifs_ext4_inline_to_block(
			handle, inode, &iloc);
	else
		err = 0;
	ext4_write_unlock_xattr(inode, &saved);

	ext4_journal_stop(handle);
	brelse(iloc.bh);
	return err;
}
