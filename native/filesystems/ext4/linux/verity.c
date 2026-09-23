// SPDX-License-Identifier: GPL-2.0
/*
 * fs/ext4/verity.c: fs-verity support for ext4
 *
 * Copyright 2019 Google LLC
 */

/*
 * EXT4 — fs-verity integration
 *
 * Purpose:
 *   Connects EXT4 metadata and file operations to the kernel fs-verity framework.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   EXT4 persists verity metadata while the generic framework owns Merkle-tree verification semantics and cryptographic policy.
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

#include "ext4.h"
#include "ext4_extents.h"
#include "ext4_jbd2.h"


/**
 * ext4_verity_metadata_pos - Implements the verity metadata pos operation within the fs-verity integration subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline loff_t ext4_verity_metadata_pos(const struct inode *inode)
{
	return round_up(inode->i_size, 65536);
}


/**
 * pagecache_read - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int pagecache_read(struct inode *inode, void *buf, size_t count,
			  loff_t pos)
{
	while (count) {
		struct folio *folio;
		size_t n;

		folio = read_mapping_folio(inode->i_mapping, pos >> PAGE_SHIFT,
					 NULL);
		if (IS_ERR(folio))
			return PTR_ERR(folio);

		n = memcpy_from_file_folio(buf, folio, pos, count);
		folio_put(folio);

		buf += n;
		pos += n;
		count -= n;
	}
	return 0;
}


/**
 * pagecache_write - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int pagecache_write(struct inode *inode, const void *buf, size_t count,
			   loff_t pos)
{
	struct address_space *mapping = inode->i_mapping;
	const struct address_space_operations *aops = mapping->a_ops;

	if (pos + count > inode->i_sb->s_maxbytes)
		return -EFBIG;

	while (count) {
		size_t n = min_t(size_t, count,
				 PAGE_SIZE - offset_in_page(pos));
		struct folio *folio;
		void *fsdata = NULL;
		int res;

		res = aops->write_begin(NULL, mapping, pos, n, &folio, &fsdata);
		if (res)
			return res;

		memcpy_to_folio(folio, offset_in_folio(folio, pos), buf, n);

		res = aops->write_end(NULL, mapping, pos, n, n, folio, fsdata);
		if (res < 0)
			return res;
		if (res != n)
			return -EIO;

		buf += n;
		pos += n;
		count -= n;
	}
	return 0;
}


/**
 * ext4_begin_enable_verity - Implements the begin enable verity operation within the fs-verity integration subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_begin_enable_verity(struct file *filp)
{
	struct inode *inode = file_inode(filp);
	const int credits = 2;
	handle_t *handle;
	int err;

	if (IS_DAX(inode) || ext4_test_inode_flag(inode, EXT4_INODE_DAX))
		return -EINVAL;

	if (ext4_verity_in_progress(inode))
		return -EBUSY;


	err = ext4_inode_attach_jinode(inode);
	if (err)
		return err;

	err = dquot_initialize(inode);
	if (err)
		return err;

	err = ext4_convert_inline_data(inode);
	if (err)
		return err;

	if (!ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)) {
		ext4_warning_inode(inode,
				   "verity is only allowed on extent-based files");
		return -EOPNOTSUPP;
	}


	err = ext4_truncate(inode);
	if (err)
		return err;

	handle = ext4_journal_start(inode, EXT4_HT_INODE, credits);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	err = ext4_orphan_add(handle, inode);
	if (err == 0)
		ext4_set_inode_state(inode, EXT4_STATE_VERITY_IN_PROGRESS);

	ext4_journal_stop(handle);
	return err;
}


/**
 * ext4_write_verity_descriptor - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_write_verity_descriptor(struct inode *inode, const void *desc,
					size_t desc_size, u64 merkle_tree_size)
{
	const u64 desc_pos = round_up(ext4_verity_metadata_pos(inode) +
				      merkle_tree_size, i_blocksize(inode));
	const u64 desc_end = desc_pos + desc_size;
	const __le32 desc_size_disk = cpu_to_le32(desc_size);
	const u64 desc_size_pos = round_up(desc_end + sizeof(desc_size_disk),
					   i_blocksize(inode)) -
				  sizeof(desc_size_disk);
	int err;

	err = pagecache_write(inode, desc, desc_size, desc_pos);
	if (err)
		return err;

	return pagecache_write(inode, &desc_size_disk, sizeof(desc_size_disk),
			       desc_size_pos);
}


/**
 * ext4_end_enable_verity - Implements the end enable verity operation within the fs-verity integration subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_end_enable_verity(struct file *filp, const void *desc,
				  size_t desc_size, u64 merkle_tree_size)
{
	struct inode *inode = file_inode(filp);
	const int credits = 2;
	handle_t *handle;
	struct ext4_iloc iloc;
	int err = 0;


	if (desc == NULL)
		goto cleanup;


	err = ext4_write_verity_descriptor(inode, desc, desc_size,
					   merkle_tree_size);
	if (err)
		goto cleanup;


	err = filemap_write_and_wait(inode->i_mapping);
	if (err)
		goto cleanup;


	handle = ext4_journal_start(inode, EXT4_HT_INODE, credits);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto cleanup;
	}

	err = ext4_orphan_del(handle, inode);
	if (err)
		goto stop_and_cleanup;

	err = ext4_reserve_inode_write(handle, inode, &iloc);
	if (err)
		goto stop_and_cleanup;

	ext4_set_inode_flag(inode, EXT4_INODE_VERITY);
	ext4_set_inode_flags(inode, false);
	err = ext4_mark_iloc_dirty(handle, inode, &iloc);
	if (err)
		goto stop_and_cleanup;

	ext4_journal_stop(handle);

	ext4_clear_inode_state(inode, EXT4_STATE_VERITY_IN_PROGRESS);
	return 0;

stop_and_cleanup:
	ext4_journal_stop(handle);
cleanup:


	truncate_inode_pages(inode->i_mapping, inode->i_size);
	ext4_truncate(inode);
	ext4_orphan_del(NULL, inode);
	ext4_clear_inode_state(inode, EXT4_STATE_VERITY_IN_PROGRESS);
	return err;
}


/**
 * ext4_get_verity_descriptor_location - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_get_verity_descriptor_location(struct inode *inode,
					       size_t *desc_size_ret,
					       u64 *desc_pos_ret)
{
	struct ext4_ext_path *path;
	struct ext4_extent *last_extent;
	u32 end_lblk;
	u64 desc_size_pos;
	__le32 desc_size_disk;
	u32 desc_size;
	u64 desc_pos;
	int err;


	if (!ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS)) {
		EXT4_ERROR_INODE(inode, "verity file doesn't use extents");
		return -EFSCORRUPTED;
	}

	path = ext4_find_extent(inode, EXT_MAX_BLOCKS - 1, NULL, 0);
	if (IS_ERR(path))
		return PTR_ERR(path);

	last_extent = path[path->p_depth].p_ext;
	if (!last_extent) {
		EXT4_ERROR_INODE(inode, "verity file has no extents");
		ext4_free_ext_path(path);
		return -EFSCORRUPTED;
	}

	end_lblk = le32_to_cpu(last_extent->ee_block) +
		   ext4_ext_get_actual_len(last_extent);
	desc_size_pos = (u64)end_lblk << inode->i_blkbits;
	ext4_free_ext_path(path);

	if (desc_size_pos < sizeof(desc_size_disk))
		goto bad;
	desc_size_pos -= sizeof(desc_size_disk);

	err = pagecache_read(inode, &desc_size_disk, sizeof(desc_size_disk),
			     desc_size_pos);
	if (err)
		return err;
	desc_size = le32_to_cpu(desc_size_disk);


	if (desc_size > INT_MAX || desc_size > desc_size_pos)
		goto bad;

	desc_pos = round_down(desc_size_pos - desc_size, i_blocksize(inode));
	if (desc_pos < ext4_verity_metadata_pos(inode))
		goto bad;

	*desc_size_ret = desc_size;
	*desc_pos_ret = desc_pos;
	return 0;

bad:
	EXT4_ERROR_INODE(inode, "verity file corrupted; can't find descriptor");
	return -EFSCORRUPTED;
}


/**
 * ext4_get_verity_descriptor - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_get_verity_descriptor(struct inode *inode, void *buf,
				      size_t buf_size)
{
	size_t desc_size = 0;
	u64 desc_pos = 0;
	int err;

	err = ext4_get_verity_descriptor_location(inode, &desc_size, &desc_pos);
	if (err)
		return err;

	if (buf_size) {
		if (desc_size > buf_size)
			return -ERANGE;
		err = pagecache_read(inode, buf, desc_size, desc_pos);
		if (err)
			return err;
	}
	return desc_size;
}


/**
 * ext4_read_merkle_tree_page - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct page *ext4_read_merkle_tree_page(struct inode *inode,
					       pgoff_t index,
					       unsigned long num_ra_pages)
{
	struct folio *folio;

	index += ext4_verity_metadata_pos(inode) >> PAGE_SHIFT;

	folio = __filemap_get_folio(inode->i_mapping, index, FGP_ACCESSED, 0);
	if (IS_ERR(folio) || !folio_test_uptodate(folio)) {
		DEFINE_READAHEAD(ractl, NULL, NULL, inode->i_mapping, index);

		if (!IS_ERR(folio))
			folio_put(folio);
		else if (num_ra_pages > 1)
			page_cache_ra_unbounded(&ractl, num_ra_pages, 0);
		folio = read_mapping_folio(inode->i_mapping, index, NULL);
		if (IS_ERR(folio))
			return ERR_CAST(folio);
	}
	return folio_file_page(folio, index);
}


/**
 * ext4_write_merkle_tree_block - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_write_merkle_tree_block(struct inode *inode, const void *buf,
					u64 pos, unsigned int size)
{
	pos += ext4_verity_metadata_pos(inode);

	return pagecache_write(inode, buf, size, pos);
}

const struct fsverity_operations ext4_verityops = {
	.begin_enable_verity	= ext4_begin_enable_verity,
	.end_enable_verity	= ext4_end_enable_verity,
	.get_verity_descriptor	= ext4_get_verity_descriptor,
	.read_merkle_tree_page	= ext4_read_merkle_tree_page,
	.write_merkle_tree_block = ext4_write_merkle_tree_block,
};
