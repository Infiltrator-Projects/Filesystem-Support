#include <linux/fs.h>
#include <linux/quotaops.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>

#include "ext4.h"
#include "ext4_extents.h"
#include "ext4_jbd2.h"

static struct ext4_ext_path *
ext4_move_find_path(struct inode *inode, ext4_lblk_t block,
		    struct ext4_ext_path *path)
{
	path = ext4_find_extent(inode, block, path, EXT4_EX_NOCACHE);
	if (IS_ERR(path))
		return path;

	if (!path[path->p_depth].p_ext) {
		ext4_free_ext_path(path);
		return ERR_PTR(-ENODATA);
	}
	return path;
}

void ext4_double_down_write_data_sem(struct inode *first, struct inode *second)
{
	if (first < second) {
		down_write(&EXT4_I(first)->i_data_sem);
		down_write_nested(
			&EXT4_I(second)->i_data_sem, I_DATA_SEM_OTHER);
	} else {
		down_write(&EXT4_I(second)->i_data_sem);
		down_write_nested(
			&EXT4_I(first)->i_data_sem, I_DATA_SEM_OTHER);
	}
}

void ext4_double_up_write_data_sem(struct inode *first, struct inode *second)
{
	up_write(&EXT4_I(first)->i_data_sem);
	up_write(&EXT4_I(second)->i_data_sem);
}

static int ext4_move_same_extent_state(struct inode *inode,
				       ext4_lblk_t start,
				       ext4_lblk_t count,
				       bool unwritten,
				       int *err)
{
	struct ext4_ext_path *path = NULL;
	const ext4_lblk_t end = start + count;
	int covered = 0;

	while (start < end) {
		struct ext4_extent *extent;
		ext4_lblk_t extent_start;
		ext4_lblk_t extent_end;

		path = ext4_move_find_path(inode, start, path);
		if (IS_ERR(path)) {
			*err = PTR_ERR(path);
			return covered;
		}

		extent = path[path->p_depth].p_ext;
		if (unwritten != ext4_ext_is_unwritten(extent))
			goto out;

		extent_start = le32_to_cpu(extent->ee_block);
		extent_end = extent_start + ext4_ext_get_actual_len(extent);
		if (start < extent_start) {
			*err = -ENODATA;
			goto out;
		}
		start = extent_end;
	}

	covered = 1;
out:
	ext4_free_ext_path(path);
	return covered;
}

static int ext4_move_lock_folios(struct inode *first_inode,
				  struct inode *second_inode,
				  pgoff_t first_index,
				  pgoff_t second_index,
				  struct folio *folios[2])
{
	struct address_space *mapping[2];
	unsigned int nofs;

	if (first_inode < second_inode) {
		mapping[0] = first_inode->i_mapping;
		mapping[1] = second_inode->i_mapping;
	} else {
		swap(first_index, second_index);
		mapping[0] = second_inode->i_mapping;
		mapping[1] = first_inode->i_mapping;
	}

	nofs = memalloc_nofs_save();
	folios[0] = __filemap_get_folio(
		mapping[0], first_index, FGP_WRITEBEGIN,
		mapping_gfp_mask(mapping[0]));
	if (IS_ERR(folios[0])) {
		memalloc_nofs_restore(nofs);
		return PTR_ERR(folios[0]);
	}

	folios[1] = __filemap_get_folio(
		mapping[1], second_index, FGP_WRITEBEGIN,
		mapping_gfp_mask(mapping[1]));
	memalloc_nofs_restore(nofs);
	if (IS_ERR(folios[1])) {
		folio_unlock(folios[0]);
		folio_put(folios[0]);
		return PTR_ERR(folios[1]);
	}

	folio_wait_writeback(folios[0]);
	folio_wait_writeback(folios[1]);
	if (first_inode > second_inode)
		swap(folios[0], folios[1]);
	return 0;
}

static int ext4_move_ensure_folio_uptodate(struct folio *folio,
					   size_t from, size_t to)
{
	struct inode *inode = folio->mapping->host;
	const unsigned int block_size = i_blocksize(inode);
	struct buffer_head *head;
	struct buffer_head *bh;
	sector_t block;
	unsigned int block_start = 0;
	bool partial = false;
	int reads = 0;

	BUG_ON(!folio_test_locked(folio));
	BUG_ON(folio_test_writeback(folio));
	if (folio_test_uptodate(folio))
		return 0;

	head = folio_buffers(folio);
	if (!head)
		head = create_empty_buffers(folio, block_size, 0);

	block = folio_pos(folio) >> inode->i_blkbits;
	bh = head;
	do {
		const unsigned int block_end = block_start + block_size;

		if (block_end <= from || block_start >= to) {
			if (!buffer_uptodate(bh))
				partial = true;
			goto advance;
		}
		if (buffer_uptodate(bh))
			goto advance;

		if (!buffer_mapped(bh)) {
			int err = ext4_get_block(inode, block, bh, 0);

			if (err)
				return err;
			if (!buffer_mapped(bh)) {
				folio_zero_range(folio, block_start, block_size);
				set_buffer_uptodate(bh);
				goto advance;
			}
		}

		lock_buffer(bh);
		if (buffer_uptodate(bh)) {
			unlock_buffer(bh);
			goto advance;
		}
		ext4_read_bh_nowait(bh, 0, NULL, false);
		reads++;

advance:
		block++;
		block_start += block_size;
		bh = bh->b_this_page;
	} while (bh != head);

	if (reads) {
		bh = head;
		do {
			if (bh_offset(bh) + block_size <= from)
				goto next_wait;
			if (bh_offset(bh) >= to)
				break;
			wait_on_buffer(bh);
			if (!buffer_uptodate(bh))
				return -EIO;
next_wait:
			bh = bh->b_this_page;
		} while (bh != head);
	}

	if (!partial)
		folio_mark_uptodate(folio);
	return 0;
}

static void ext4_move_unlock_folios(struct folio *folios[2])
{
	unsigned int i;

	for (i = 0; i < 2; ++i) {
		if (!folios[i])
			continue;
		folio_unlock(folios[i]);
		folio_put(folios[i]);
		folios[i] = NULL;
	}
}

static int ext4_move_one_folio(struct file *file,
			       struct inode *donor,
			       pgoff_t source_page,
			       pgoff_t donor_page,
			       int block_offset,
			       int block_count,
			       bool unwritten,
			       int *err)
{
	struct inode *source = file_inode(file);
	struct super_block *sb = source->i_sb;
	struct folio *folios[2] = { NULL, NULL };
	const unsigned int blocks_per_page =
		PAGE_SIZE >> source->i_blkbits;
	const unsigned int block_size = 1U << source->i_blkbits;
	const ext4_lblk_t source_block =
		source_page * blocks_per_page + block_offset;
	const ext4_lblk_t donor_block =
		donor_page * blocks_per_page + block_offset;
	const int from = block_offset << source->i_blkbits;
	unsigned int data_size;
	unsigned int replaced_size;
	handle_t *handle;
	struct buffer_head *bh;
	int journal_blocks;
	int retries = 0;
	int replaced = 0;
	int i;

retry:
	*err = 0;
	journal_blocks = ext4_writepage_trans_blocks(source) * 2;
	handle = ext4_journal_start(
		source, EXT4_HT_MOVE_EXTENTS, journal_blocks);
	if (IS_ERR(handle)) {
		*err = PTR_ERR(handle);
		return 0;
	}

	if (source_block + block_count - 1 ==
	    (source->i_size - 1) >> source->i_blkbits) {
		const unsigned int tail = source->i_size & (block_size - 1);
		data_size = (tail ? tail : block_size) +
			((block_count - 1) << source->i_blkbits);
	} else {
		data_size = block_count << source->i_blkbits;
	}
	replaced_size = data_size;

	*err = ext4_move_lock_folios(
		source, donor, source_page, donor_page, folios);
	if (*err)
		goto stop;

	VM_BUG_ON_FOLIO(folio_test_large(folios[0]), folios[0]);
	VM_BUG_ON_FOLIO(folio_test_large(folios[1]), folios[1]);

	if (unwritten) {
		bool both_unwritten;

		ext4_double_down_write_data_sem(source, donor);
		both_unwritten = ext4_move_same_extent_state(
			source, source_block, block_count, true, err);
		if (!*err)
			both_unwritten &=
				ext4_move_same_extent_state(
					donor, donor_block,
					block_count, true, err);
		if (*err) {
			ext4_double_up_write_data_sem(source, donor);
			goto unlock;
		}

		if (both_unwritten) {
			if (!filemap_release_folio(folios[0], 0) ||
			    !filemap_release_folio(folios[1], 0)) {
				*err = -EBUSY;
				ext4_double_up_write_data_sem(source, donor);
				goto unlock;
			}
			replaced = ext4_swap_extents(
				handle, source, donor,
				source_block, donor_block,
				block_count, 1, err);
			ext4_double_up_write_data_sem(source, donor);
			goto unlock;
		}
		ext4_double_up_write_data_sem(source, donor);
	}

	*err = ext4_move_ensure_folio_uptodate(
		folios[0], from, from + replaced_size);
	if (*err)
		goto unlock;

	if (!filemap_release_folio(folios[0], 0) ||
	    !filemap_release_folio(folios[1], 0)) {
		*err = -EBUSY;
		goto unlock;
	}

	ext4_double_down_write_data_sem(source, donor);
	replaced = ext4_swap_extents(
		handle, source, donor,
		source_block, donor_block,
		block_count, 1, err);
	ext4_double_up_write_data_sem(source, donor);

	if (*err && replaced == 0)
		goto unlock;
	if (*err) {
		block_count = replaced;
		replaced_size = block_count << source->i_blkbits;
	}

	bh = folio_buffers(folios[0]);
	if (!bh)
		bh = create_empty_buffers(
			folios[0], 1U << source->i_blkbits, 0);
	for (i = 0; i < block_offset; ++i)
		bh = bh->b_this_page;

	for (i = 0; i < block_count; ++i) {
		*err = ext4_get_block(source, source_block + i, bh, 0);
		if (*err)
			goto rollback;
		bh = bh->b_this_page;
	}

	block_commit_write(&folios[0]->page, from, from + replaced_size);
	*err = ext4_jbd2_inode_add_write(
		handle, source,
		(loff_t)source_page << PAGE_SHIFT,
		replaced_size);
	goto unlock;

rollback:
	{
		int rollback_err = 0;
		int restored;

		ext4_double_down_write_data_sem(source, donor);
		restored = ext4_swap_extents(
			handle, donor, source,
			source_block, donor_block,
			block_count, 0, &rollback_err);
		ext4_double_up_write_data_sem(source, donor);

		if (restored != block_count) {
			ext4_error_inode_block(
				source, source_block, EIO,
				"extent relocation rollback incomplete");
			*err = -EIO;
		}
		replaced = 0;
	}

unlock:
	ext4_move_unlock_folios(folios);
stop:
	ext4_journal_stop(handle);

	if (*err == -ENOSPC &&
	    ext4_should_retry_alloc(sb, &retries))
		goto retry;

	if (*err == -EBUSY && retries++ < 4 &&
	    EXT4_SB(sb)->s_journal &&
	    jbd2_journal_force_commit_nested(EXT4_SB(sb)->s_journal))
		goto retry;

	return replaced;
}

static int ext4_move_validate(struct inode *source, struct inode *donor,
			      __u64 source_start, __u64 donor_start,
			      __u64 *length)
{
	const unsigned int block_bits = source->i_blkbits;
	const unsigned int block_size = 1U << block_bits;
	const __u64 source_eof =
		(i_size_read(source) + block_size - 1) >> block_bits;
	const __u64 donor_eof =
		(i_size_read(donor) + block_size - 1) >> block_bits;
	const __u64 page_block_mask = ~(PAGE_MASK >> block_bits);

	if (donor->i_mode & (S_ISUID | S_ISGID))
		return -EINVAL;
	if (IS_IMMUTABLE(donor) || IS_APPEND(donor))
		return -EPERM;
	if (IS_SWAPFILE(source) || IS_SWAPFILE(donor))
		return -ETXTBSY;
	if (ext4_is_quota_file(source) || ext4_is_quota_file(donor))
		return -EOPNOTSUPP;
	if (!ext4_test_inode_flag(source, EXT4_INODE_EXTENTS) ||
	    !ext4_test_inode_flag(donor, EXT4_INODE_EXTENTS))
		return -EOPNOTSUPP;
	if (!source->i_size || !donor->i_size)
		return -EINVAL;
	if ((source_start & page_block_mask) !=
	    (donor_start & page_block_mask))
		return -EINVAL;

	if (source_start >= EXT_MAX_BLOCKS ||
	    donor_start >= EXT_MAX_BLOCKS ||
	    *length > EXT_MAX_BLOCKS ||
	    source_start > EXT_MAX_BLOCKS - *length ||
	    donor_start > EXT_MAX_BLOCKS - *length)
		return -EINVAL;

	if (source_eof <= source_start || donor_eof <= donor_start)
		return -EINVAL;
	if (*length > source_eof - source_start)
		*length = source_eof - source_start;
	if (*length > donor_eof - donor_start)
		*length = donor_eof - donor_start;

	return *length ? 0 : -EINVAL;
}

int ext4_move_extents(struct file *source_file, struct file *donor_file,
		      __u64 source_block, __u64 donor_block,
		      __u64 length, __u64 *moved)
{
	struct inode *source = file_inode(source_file);
	struct inode *donor = file_inode(donor_file);
	struct ext4_ext_path *path = NULL;
	const int blocks_per_page = PAGE_SIZE >> source->i_blkbits;
	ext4_lblk_t source_cursor = source_block;
	ext4_lblk_t donor_cursor = donor_block;
	ext4_lblk_t source_end;
	int err = 0;

	if (source->i_sb != donor->i_sb ||
	    source == donor ||
	    !S_ISREG(source->i_mode) ||
	    !S_ISREG(donor->i_mode))
		return -EINVAL;
	if (ext4_should_journal_data(source) ||
	    ext4_should_journal_data(donor))
		return -EOPNOTSUPP;
	if (IS_ENCRYPTED(source) || IS_ENCRYPTED(donor))
		return -EOPNOTSUPP;

	lock_two_nondirectories(source, donor);
	inode_dio_wait(source);
	inode_dio_wait(donor);
	ext4_double_down_write_data_sem(source, donor);

	err = ext4_move_validate(
		source, donor, source_block, donor_block, &length);
	if (err)
		goto out;

	source_end = source_cursor + length;
	*moved = 0;

	while (source_cursor < source_end) {
		struct ext4_extent *extent;
		ext4_lblk_t extent_start;
		ext4_lblk_t next;
		int extent_length;
		int block_offset;
		pgoff_t source_page;
		pgoff_t donor_page;
		bool unwritten;

		path = ext4_move_find_path(source, source_cursor, path);
		if (IS_ERR(path)) {
			err = PTR_ERR(path);
			path = NULL;
			goto out;
		}

		extent = path[path->p_depth].p_ext;
		extent_start = le32_to_cpu(extent->ee_block);
		extent_length = ext4_ext_get_actual_len(extent);

		if (extent_start + extent_length <= source_cursor) {
			next = ext4_ext_next_allocated_block(path);
			if (next == EXT_MAX_BLOCKS) {
				err = -ENODATA;
				goto out;
			}
			donor_cursor += next - source_cursor;
			source_cursor = next;
			continue;
		}

		if (extent_start > source_cursor) {
			donor_cursor += extent_start - source_cursor;
			source_cursor = extent_start;
			if (source_cursor >= source_end)
				break;
		} else {
			extent_length -= source_cursor - extent_start;
		}

		if (extent_length > source_end - source_cursor)
			extent_length = source_end - source_cursor;

		unwritten = ext4_ext_is_unwritten(extent);
		source_page = source_cursor >>
			(PAGE_SHIFT - source->i_blkbits);
		donor_page = donor_cursor >>
			(PAGE_SHIFT - donor->i_blkbits);
		block_offset = source_cursor % blocks_per_page;
		if (extent_length > blocks_per_page - block_offset)
			extent_length = blocks_per_page - block_offset;

		ext4_double_up_write_data_sem(source, donor);
		*moved += ext4_move_one_folio(
			source_file, donor,
			source_page, donor_page,
			block_offset, extent_length,
			unwritten, &err);
		ext4_double_down_write_data_sem(source, donor);

		if (err)
			break;

		source_cursor += extent_length;
		donor_cursor += extent_length;
	}

out:
	if (*moved) {
		ext4_discard_preallocations(source);
		ext4_discard_preallocations(donor);
	}

	ext4_free_ext_path(path);
	ext4_double_up_write_data_sem(source, donor);
	unlock_two_nondirectories(source, donor);
	return err;
}
