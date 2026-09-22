// SPDX-License-Identifier: LGPL-2.1
/*
 * Copyright (c) 2008,2009 NEC Software Tohoku, Ltd.
 * Written by Takashi Sato <t-sato@yk.jp.nec.com>
 *            Akira Fujita <a-fujita@rs.jp.nec.com>
 */

/*
 * EXT4 — Extent relocation
 *
 * Purpose:
 *   Implements controlled swapping/movement of mapped extents between files for online defragmentation-style operations.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Both files' mappings, data validity and journal state must change as one logical operation to prevent cross-file data exposure.
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

#include <linux/fs.h>
#include <linux/quotaops.h>
#include <linux/slab.h>
#include <linux/sched/mm.h>
#include "ext4_jbd2.h"
#include "ext4.h"
#include "ext4_extents.h"


/**
 * get_ext_path - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline struct ext4_ext_path *
get_ext_path(struct inode *inode, ext4_lblk_t lblock,
	     struct ext4_ext_path *path)
{
	path = ext4_find_extent(inode, lblock, path, EXT4_EX_NOCACHE);
	if (IS_ERR(path))
		return path;
	if (path[ext_depth(inode)].p_ext == NULL) {
		ext4_free_ext_path(path);
		return ERR_PTR(-ENODATA);
	}
	return path;
}


/**
 * ext4_double_down_write_data_sem - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void
ext4_double_down_write_data_sem(struct inode *first, struct inode *second)
{
	if (first < second) {
		down_write(&EXT4_I(first)->i_data_sem);
		down_write_nested(&EXT4_I(second)->i_data_sem, I_DATA_SEM_OTHER);
	} else {
		down_write(&EXT4_I(second)->i_data_sem);
		down_write_nested(&EXT4_I(first)->i_data_sem, I_DATA_SEM_OTHER);

	}
}


/**
 * ext4_double_up_write_data_sem - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void
ext4_double_up_write_data_sem(struct inode *orig_inode,
			      struct inode *donor_inode)
{
	up_write(&EXT4_I(orig_inode)->i_data_sem);
	up_write(&EXT4_I(donor_inode)->i_data_sem);
}


/**
 * mext_check_coverage - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
mext_check_coverage(struct inode *inode, ext4_lblk_t from, ext4_lblk_t count,
		    int unwritten, int *err)
{
	struct ext4_ext_path *path = NULL;
	struct ext4_extent *ext;
	int ret = 0;
	ext4_lblk_t last = from + count;
	while (from < last) {
		path = get_ext_path(inode, from, path);
		if (IS_ERR(path)) {
			*err = PTR_ERR(path);
			return ret;
		}
		ext = path[ext_depth(inode)].p_ext;
		if (unwritten != ext4_ext_is_unwritten(ext))
			goto out;
		from += ext4_ext_get_actual_len(ext);
	}
	ret = 1;
out:
	ext4_free_ext_path(path);
	return ret;
}


/**
 * mext_folio_double_lock - Implements the mext folio double lock operation within the extent relocation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
mext_folio_double_lock(struct inode *inode1, struct inode *inode2,
		      pgoff_t index1, pgoff_t index2, struct folio *folio[2])
{
	struct address_space *mapping[2];
	unsigned int flags;

	BUG_ON(!inode1 || !inode2);
	if (inode1 < inode2) {
		mapping[0] = inode1->i_mapping;
		mapping[1] = inode2->i_mapping;
	} else {
		swap(index1, index2);
		mapping[0] = inode2->i_mapping;
		mapping[1] = inode1->i_mapping;
	}

	flags = memalloc_nofs_save();
	folio[0] = __filemap_get_folio(mapping[0], index1, FGP_WRITEBEGIN,
			mapping_gfp_mask(mapping[0]));
	if (IS_ERR(folio[0])) {
		memalloc_nofs_restore(flags);
		return PTR_ERR(folio[0]);
	}

	folio[1] = __filemap_get_folio(mapping[1], index2, FGP_WRITEBEGIN,
			mapping_gfp_mask(mapping[1]));
	memalloc_nofs_restore(flags);
	if (IS_ERR(folio[1])) {
		folio_unlock(folio[0]);
		folio_put(folio[0]);
		return PTR_ERR(folio[1]);
	}


	folio_wait_writeback(folio[0]);
	folio_wait_writeback(folio[1]);
	if (inode1 > inode2)
		swap(folio[0], folio[1]);

	return 0;
}


/**
 * mext_page_mkuptodate - Implements the mext page mkuptodate operation within the extent relocation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int mext_page_mkuptodate(struct folio *folio, size_t from, size_t to)
{
	struct inode *inode = folio->mapping->host;
	sector_t block;
	struct buffer_head *bh, *head;
	unsigned int blocksize, block_start, block_end;
	int nr = 0;
	bool partial = false;

	BUG_ON(!folio_test_locked(folio));
	BUG_ON(folio_test_writeback(folio));

	if (folio_test_uptodate(folio))
		return 0;

	blocksize = i_blocksize(inode);
	head = folio_buffers(folio);
	if (!head)
		head = create_empty_buffers(folio, blocksize, 0);

	block = folio_pos(folio) >> inode->i_blkbits;
	block_end = 0;
	bh = head;
	do {
		block_start = block_end;
		block_end = block_start + blocksize;
		if (block_end <= from || block_start >= to) {
			if (!buffer_uptodate(bh))
				partial = true;
			continue;
		}
		if (buffer_uptodate(bh))
			continue;
		if (!buffer_mapped(bh)) {
			int err = ext4_get_block(inode, block, bh, 0);
			if (err)
				return err;
			if (!buffer_mapped(bh)) {
				folio_zero_range(folio, block_start, blocksize);
				set_buffer_uptodate(bh);
				continue;
			}
		}
		lock_buffer(bh);
		if (buffer_uptodate(bh)) {
			unlock_buffer(bh);
			continue;
		}
		ext4_read_bh_nowait(bh, 0, NULL, false);
		nr++;
	} while (block++, (bh = bh->b_this_page) != head);


	if (!nr)
		goto out;

	bh = head;
	do {
		if (bh_offset(bh) + blocksize <= from)
			continue;
		if (bh_offset(bh) >= to)
			break;
		wait_on_buffer(bh);
		if (buffer_uptodate(bh))
			continue;
		return -EIO;
	} while ((bh = bh->b_this_page) != head);
out:
	if (!partial)
		folio_mark_uptodate(folio);
	return 0;
}


/**
 * move_extent_per_page - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
move_extent_per_page(struct file *o_filp, struct inode *donor_inode,
		     pgoff_t orig_page_offset, pgoff_t donor_page_offset,
		     int data_offset_in_page,
		     int block_len_in_page, int unwritten, int *err)
{
	struct inode *orig_inode = file_inode(o_filp);
	struct folio *folio[2] = {NULL, NULL};
	handle_t *handle;
	ext4_lblk_t orig_blk_offset, donor_blk_offset;
	unsigned long blocksize = orig_inode->i_sb->s_blocksize;
	unsigned int tmp_data_size, data_size, replaced_size;
	int i, err2, jblocks, retries = 0;
	int replaced_count = 0;
	int from = data_offset_in_page << orig_inode->i_blkbits;
	int blocks_per_page = PAGE_SIZE >> orig_inode->i_blkbits;
	struct super_block *sb = orig_inode->i_sb;
	struct buffer_head *bh = NULL;


again:
	*err = 0;
	jblocks = ext4_writepage_trans_blocks(orig_inode) * 2;
	handle = ext4_journal_start(orig_inode, EXT4_HT_MOVE_EXTENTS, jblocks);
	if (IS_ERR(handle)) {
		*err = PTR_ERR(handle);
		return 0;
	}

	orig_blk_offset = orig_page_offset * blocks_per_page +
		data_offset_in_page;

	donor_blk_offset = donor_page_offset * blocks_per_page +
		data_offset_in_page;


	if ((orig_blk_offset + block_len_in_page - 1) ==
	    ((orig_inode->i_size - 1) >> orig_inode->i_blkbits)) {

		tmp_data_size = orig_inode->i_size & (blocksize - 1);


		if (tmp_data_size == 0)
			tmp_data_size = blocksize;

		data_size = tmp_data_size +
			((block_len_in_page - 1) << orig_inode->i_blkbits);
	} else
		data_size = block_len_in_page << orig_inode->i_blkbits;

	replaced_size = data_size;

	*err = mext_folio_double_lock(orig_inode, donor_inode, orig_page_offset,
				     donor_page_offset, folio);
	if (unlikely(*err < 0))
		goto stop_journal;


	VM_BUG_ON_FOLIO(folio_test_large(folio[0]), folio[0]);
	VM_BUG_ON_FOLIO(folio_test_large(folio[1]), folio[1]);
	VM_BUG_ON_FOLIO(folio_nr_pages(folio[0]) != folio_nr_pages(folio[1]), folio[1]);

	if (unwritten) {
		ext4_double_down_write_data_sem(orig_inode, donor_inode);


		unwritten = mext_check_coverage(orig_inode, orig_blk_offset,
						block_len_in_page, 1, err);
		if (*err)
			goto drop_data_sem;

		unwritten &= mext_check_coverage(donor_inode, donor_blk_offset,
						 block_len_in_page, 1, err);
		if (*err)
			goto drop_data_sem;

		if (!unwritten) {
			ext4_double_up_write_data_sem(orig_inode, donor_inode);
			goto data_copy;
		}
		if (!filemap_release_folio(folio[0], 0) ||
		    !filemap_release_folio(folio[1], 0)) {
			*err = -EBUSY;
			goto drop_data_sem;
		}
		replaced_count = ext4_swap_extents(handle, orig_inode,
						   donor_inode, orig_blk_offset,
						   donor_blk_offset,
						   block_len_in_page, 1, err);
	drop_data_sem:
		ext4_double_up_write_data_sem(orig_inode, donor_inode);
		goto unlock_folios;
	}
data_copy:
	*err = mext_page_mkuptodate(folio[0], from, from + replaced_size);
	if (*err)
		goto unlock_folios;


	if (!filemap_release_folio(folio[0], 0) ||
	    !filemap_release_folio(folio[1], 0)) {
		*err = -EBUSY;
		goto unlock_folios;
	}
	ext4_double_down_write_data_sem(orig_inode, donor_inode);
	replaced_count = ext4_swap_extents(handle, orig_inode, donor_inode,
					       orig_blk_offset, donor_blk_offset,
					   block_len_in_page, 1, err);
	ext4_double_up_write_data_sem(orig_inode, donor_inode);
	if (*err) {
		if (replaced_count) {
			block_len_in_page = replaced_count;
			replaced_size =
				block_len_in_page << orig_inode->i_blkbits;
		} else
			goto unlock_folios;
	}


	bh = folio_buffers(folio[0]);
	if (!bh)
		bh = create_empty_buffers(folio[0],
				1 << orig_inode->i_blkbits, 0);
	for (i = 0; i < data_offset_in_page; i++)
		bh = bh->b_this_page;
	for (i = 0; i < block_len_in_page; i++) {
		*err = ext4_get_block(orig_inode, orig_blk_offset + i, bh, 0);
		if (*err < 0)
			goto repair_branches;
		bh = bh->b_this_page;
	}

	block_commit_write(&folio[0]->page, from, from + replaced_size);


	*err = ext4_jbd2_inode_add_write(handle, orig_inode,
			(loff_t)orig_page_offset << PAGE_SHIFT, replaced_size);

unlock_folios:
	folio_unlock(folio[0]);
	folio_put(folio[0]);
	folio_unlock(folio[1]);
	folio_put(folio[1]);
stop_journal:
	ext4_journal_stop(handle);
	if (*err == -ENOSPC &&
	    ext4_should_retry_alloc(sb, &retries))
		goto again;


	if (*err == -EBUSY && retries++ < 4 && EXT4_SB(sb)->s_journal &&
	    jbd2_journal_force_commit_nested(EXT4_SB(sb)->s_journal))
		goto again;
	return replaced_count;

repair_branches:


	ext4_double_down_write_data_sem(orig_inode, donor_inode);
	replaced_count = ext4_swap_extents(handle, donor_inode, orig_inode,
					       orig_blk_offset, donor_blk_offset,
					   block_len_in_page, 0, &err2);
	ext4_double_up_write_data_sem(orig_inode, donor_inode);
	if (replaced_count != block_len_in_page) {
		ext4_error_inode_block(orig_inode, (sector_t)(orig_blk_offset),
				       EIO, "Unable to copy data block,"
				       " data will be lost.");
		*err = -EIO;
	}
	replaced_count = 0;
	goto unlock_folios;
}


/**
 * mext_check_arguments - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int
mext_check_arguments(struct inode *orig_inode,
		     struct inode *donor_inode, __u64 orig_start,
		     __u64 donor_start, __u64 *len)
{
	__u64 orig_eof, donor_eof;
	unsigned int blkbits = orig_inode->i_blkbits;
	unsigned int blocksize = 1 << blkbits;

	orig_eof = (i_size_read(orig_inode) + blocksize - 1) >> blkbits;
	donor_eof = (i_size_read(donor_inode) + blocksize - 1) >> blkbits;


	if (donor_inode->i_mode & (S_ISUID|S_ISGID)) {
		ext4_debug("ext4 move extent: suid or sgid is set"
			   " to donor file [ino:orig %lu, donor %lu]\n",
			   orig_inode->i_ino, donor_inode->i_ino);
		return -EINVAL;
	}

	if (IS_IMMUTABLE(donor_inode) || IS_APPEND(donor_inode))
		return -EPERM;


	if (IS_SWAPFILE(orig_inode) || IS_SWAPFILE(donor_inode)) {
		ext4_debug("ext4 move extent: The argument files should not be swap files [ino:orig %lu, donor %lu]\n",
			orig_inode->i_ino, donor_inode->i_ino);
		return -ETXTBSY;
	}

	if (ext4_is_quota_file(orig_inode) || ext4_is_quota_file(donor_inode)) {
		ext4_debug("ext4 move extent: The argument files should not be quota files [ino:orig %lu, donor %lu]\n",
			orig_inode->i_ino, donor_inode->i_ino);
		return -EOPNOTSUPP;
	}


	if (!(ext4_test_inode_flag(orig_inode, EXT4_INODE_EXTENTS))) {
		ext4_debug("ext4 move extent: orig file is not extents "
			"based file [ino:orig %lu]\n", orig_inode->i_ino);
		return -EOPNOTSUPP;
	} else if (!(ext4_test_inode_flag(donor_inode, EXT4_INODE_EXTENTS))) {
		ext4_debug("ext4 move extent: donor file is not extents "
			"based file [ino:donor %lu]\n", donor_inode->i_ino);
		return -EOPNOTSUPP;
	}

	if ((!orig_inode->i_size) || (!donor_inode->i_size)) {
		ext4_debug("ext4 move extent: File size is 0 byte\n");
		return -EINVAL;
	}


	if ((orig_start & ~(PAGE_MASK >> orig_inode->i_blkbits)) !=
	    (donor_start & ~(PAGE_MASK >> orig_inode->i_blkbits))) {
		ext4_debug("ext4 move extent: orig and donor's start "
			"offsets are not aligned [ino:orig %lu, donor %lu]\n",
			orig_inode->i_ino, donor_inode->i_ino);
		return -EINVAL;
	}

	if ((orig_start >= EXT_MAX_BLOCKS) ||
	    (donor_start >= EXT_MAX_BLOCKS) ||
	    (*len > EXT_MAX_BLOCKS) ||
	    (donor_start + *len >= EXT_MAX_BLOCKS) ||
	    (orig_start + *len >= EXT_MAX_BLOCKS))  {
		ext4_debug("ext4 move extent: Can't handle over [%u] blocks "
			"[ino:orig %lu, donor %lu]\n", EXT_MAX_BLOCKS,
			orig_inode->i_ino, donor_inode->i_ino);
		return -EINVAL;
	}
	if (orig_eof <= orig_start)
		*len = 0;
	else if (orig_eof < orig_start + *len - 1)
		*len = orig_eof - orig_start;
	if (donor_eof <= donor_start)
		*len = 0;
	else if (donor_eof < donor_start + *len - 1)
		*len = donor_eof - donor_start;
	if (!*len) {
		ext4_debug("ext4 move extent: len should not be 0 "
			"[ino:orig %lu, donor %lu]\n", orig_inode->i_ino,
			donor_inode->i_ino);
		return -EINVAL;
	}

	return 0;
}


/**
 * ext4_move_extents - Operates on logical-to-physical extent state while preserving extent-tree ordering and range invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int
ext4_move_extents(struct file *o_filp, struct file *d_filp, __u64 orig_blk,
		  __u64 donor_blk, __u64 len, __u64 *moved_len)
{
	struct inode *orig_inode = file_inode(o_filp);
	struct inode *donor_inode = file_inode(d_filp);
	struct ext4_ext_path *path = NULL;
	int blocks_per_page = PAGE_SIZE >> orig_inode->i_blkbits;
	ext4_lblk_t o_end, o_start = orig_blk;
	ext4_lblk_t d_start = donor_blk;
	int ret;

	if (orig_inode->i_sb != donor_inode->i_sb) {
		ext4_debug("ext4 move extent: The argument files "
			"should be in same FS [ino:orig %lu, donor %lu]\n",
			orig_inode->i_ino, donor_inode->i_ino);
		return -EINVAL;
	}


	if (orig_inode == donor_inode) {
		ext4_debug("ext4 move extent: The argument files should not "
			"be same inode [ino:orig %lu, donor %lu]\n",
			orig_inode->i_ino, donor_inode->i_ino);
		return -EINVAL;
	}


	if (!S_ISREG(orig_inode->i_mode) || !S_ISREG(donor_inode->i_mode)) {
		ext4_debug("ext4 move extent: The argument files should be "
			"regular file [ino:orig %lu, donor %lu]\n",
			orig_inode->i_ino, donor_inode->i_ino);
		return -EINVAL;
	}


	if (ext4_should_journal_data(orig_inode) ||
	    ext4_should_journal_data(donor_inode)) {
		ext4_msg(orig_inode->i_sb, KERN_ERR,
			 "Online defrag not supported with data journaling");
		return -EOPNOTSUPP;
	}

	if (IS_ENCRYPTED(orig_inode) || IS_ENCRYPTED(donor_inode)) {
		ext4_msg(orig_inode->i_sb, KERN_ERR,
			 "Online defrag not supported for encrypted files");
		return -EOPNOTSUPP;
	}


	lock_two_nondirectories(orig_inode, donor_inode);


	inode_dio_wait(orig_inode);
	inode_dio_wait(donor_inode);


	ext4_double_down_write_data_sem(orig_inode, donor_inode);

	ret = mext_check_arguments(orig_inode, donor_inode, orig_blk,
				    donor_blk, &len);
	if (ret)
		goto out;
	o_end = o_start + len;

	*moved_len = 0;
	while (o_start < o_end) {
		struct ext4_extent *ex;
		ext4_lblk_t cur_blk, next_blk;
		pgoff_t orig_page_index, donor_page_index;
		int offset_in_page;
		int unwritten, cur_len;

		path = get_ext_path(orig_inode, o_start, path);
		if (IS_ERR(path)) {
			ret = PTR_ERR(path);
			goto out;
		}
		ex = path[path->p_depth].p_ext;
		cur_blk = le32_to_cpu(ex->ee_block);
		cur_len = ext4_ext_get_actual_len(ex);

		if (cur_blk + cur_len - 1 < o_start) {
			next_blk = ext4_ext_next_allocated_block(path);
			if (next_blk == EXT_MAX_BLOCKS) {
				ret = -ENODATA;
				goto out;
			}
			d_start += next_blk - o_start;
			o_start = next_blk;
			continue;

		} else if (cur_blk > o_start) {

			d_start += cur_blk - o_start;
			o_start = cur_blk;

			if (cur_blk >= o_end)
				goto out;
		} else {
			cur_len += cur_blk - o_start;
		}
		unwritten = ext4_ext_is_unwritten(ex);
		if (o_end - o_start < cur_len)
			cur_len = o_end - o_start;

		orig_page_index = o_start >> (PAGE_SHIFT -
					       orig_inode->i_blkbits);
		donor_page_index = d_start >> (PAGE_SHIFT -
					       donor_inode->i_blkbits);
		offset_in_page = o_start % blocks_per_page;
		if (cur_len > blocks_per_page - offset_in_page)
			cur_len = blocks_per_page - offset_in_page;


		ext4_double_up_write_data_sem(orig_inode, donor_inode);

		*moved_len += move_extent_per_page(o_filp, donor_inode,
				     orig_page_index, donor_page_index,
				     offset_in_page, cur_len,
				     unwritten, &ret);
		ext4_double_down_write_data_sem(orig_inode, donor_inode);
		if (ret < 0)
			break;
		o_start += cur_len;
		d_start += cur_len;
	}

out:
	if (*moved_len) {
		ext4_discard_preallocations(orig_inode);
		ext4_discard_preallocations(donor_inode);
	}

	ext4_free_ext_path(path);
	ext4_double_up_write_data_sem(orig_inode, donor_inode);
	unlock_two_nondirectories(orig_inode, donor_inode);

	return ret;
}
