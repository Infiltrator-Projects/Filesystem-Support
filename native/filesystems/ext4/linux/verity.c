#include <linux/quotaops.h>

#include "ext4.h"
#include "ext4_extents.h"
#include "ext4_jbd2.h"

static inline loff_t ext4_verity_area_start(const struct inode *inode)
{
	return round_up(inode->i_size, 65536);
}

static int ext4_verity_read(struct inode *inode, void *buffer,
			    size_t count, loff_t position)
{
	while (count != 0) {
		struct folio *folio;
		size_t copied;

		folio = read_mapping_folio(
			inode->i_mapping, position >> PAGE_SHIFT, NULL);
		if (IS_ERR(folio))
			return PTR_ERR(folio);

		copied = memcpy_from_file_folio(
			buffer, folio, position, count);
		folio_put(folio);
		if (copied == 0)
			return -EIO;

		buffer += copied;
		position += copied;
		count -= copied;
	}
	return 0;
}

static int ext4_verity_write(struct inode *inode, const void *buffer,
			     size_t count, loff_t position)
{
	struct address_space *mapping = inode->i_mapping;
	const struct address_space_operations *aops = mapping->a_ops;

	if (count > inode->i_sb->s_maxbytes - position)
		return -EFBIG;

	while (count != 0) {
		const size_t chunk = min_t(
			size_t, count, PAGE_SIZE - offset_in_page(position));
		struct folio *folio;
		void *fsdata = NULL;
		int written;

		written = aops->write_begin(
			NULL, mapping, position, chunk, &folio, &fsdata);
		if (written)
			return written;

		memcpy_to_folio(
			folio, offset_in_folio(folio, position),
			buffer, chunk);

		written = aops->write_end(
			NULL, mapping, position, chunk, chunk,
			folio, fsdata);
		if (written < 0)
			return written;
		if ((size_t)written != chunk)
			return -EIO;

		buffer += chunk;
		position += chunk;
		count -= chunk;
	}
	return 0;
}

static int ext4_verity_begin(struct file *file)
{
	struct inode *inode = file_inode(file);
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
		ext4_warning_inode(
			inode, "verity requires an extent-based file");
		return -EOPNOTSUPP;
	}

	err = ext4_truncate(inode);
	if (err)
		return err;

	handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
	if (IS_ERR(handle))
		return PTR_ERR(handle);

	err = ext4_orphan_add(handle, inode);
	if (!err)
		ext4_set_inode_state(inode, EXT4_STATE_VERITY_IN_PROGRESS);

	ext4_journal_stop(handle);
	return err;
}

static int ext4_verity_store_descriptor(
	struct inode *inode, const void *descriptor,
	size_t descriptor_size, u64 tree_size)
{
	const u64 descriptor_pos =
		round_up(ext4_verity_area_start(inode) + tree_size,
			 i_blocksize(inode));
	const u64 descriptor_end = descriptor_pos + descriptor_size;
	const __le32 encoded_size = cpu_to_le32(descriptor_size);
	const u64 size_pos =
		round_up(descriptor_end + sizeof(encoded_size),
			 i_blocksize(inode)) - sizeof(encoded_size);
	int err;

	err = ext4_verity_write(
		inode, descriptor, descriptor_size, descriptor_pos);
	if (err)
		return err;

	return ext4_verity_write(
		inode, &encoded_size, sizeof(encoded_size), size_pos);
}

static void ext4_verity_abort_enable(struct inode *inode)
{
	truncate_inode_pages(inode->i_mapping, inode->i_size);
	ext4_truncate(inode);
	ext4_orphan_del(NULL, inode);
	ext4_clear_inode_state(inode, EXT4_STATE_VERITY_IN_PROGRESS);
}

static int ext4_verity_finish(struct file *file, const void *descriptor,
			      size_t descriptor_size, u64 tree_size)
{
	struct inode *inode = file_inode(file);
	struct ext4_iloc iloc;
	handle_t *handle;
	int err;

	if (!descriptor) {
		ext4_verity_abort_enable(inode);
		return 0;
	}

	err = ext4_verity_store_descriptor(
		inode, descriptor, descriptor_size, tree_size);
	if (err)
		goto fail;

	err = filemap_write_and_wait(inode->i_mapping);
	if (err)
		goto fail;

	handle = ext4_journal_start(inode, EXT4_HT_INODE, 2);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto fail;
	}

	err = ext4_orphan_del(handle, inode);
	if (!err)
		err = ext4_reserve_inode_write(handle, inode, &iloc);
	if (!err) {
		ext4_set_inode_flag(inode, EXT4_INODE_VERITY);
		ext4_set_inode_flags(inode, false);
		err = ext4_mark_iloc_dirty(handle, inode, &iloc);
	}

	ext4_journal_stop(handle);
	if (err)
		goto fail;

	ext4_clear_inode_state(inode, EXT4_STATE_VERITY_IN_PROGRESS);
	return 0;

fail:
	ext4_verity_abort_enable(inode);
	return err;
}

static int ext4_verity_descriptor_location(
	struct inode *inode, size_t *descriptor_size,
	u64 *descriptor_pos)
{
	struct ext4_ext_path *path;
	struct ext4_extent *last;
	u64 size_pos;
	u32 end_block;
	__le32 encoded_size;
	u32 size;
	u64 pos;
	int err;

	if (!ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		return -EFSCORRUPTED;

	path = ext4_find_extent(inode, EXT_MAX_BLOCKS - 1, NULL, 0);
	if (IS_ERR(path))
		return PTR_ERR(path);

	last = path[path->p_depth].p_ext;
	if (!last) {
		ext4_free_ext_path(path);
		return -EFSCORRUPTED;
	}

	end_block = le32_to_cpu(last->ee_block) +
		    ext4_ext_get_actual_len(last);
	size_pos = (u64)end_block << inode->i_blkbits;
	ext4_free_ext_path(path);

	if (size_pos < sizeof(encoded_size))
		return -EFSCORRUPTED;
	size_pos -= sizeof(encoded_size);

	err = ext4_verity_read(
		inode, &encoded_size, sizeof(encoded_size), size_pos);
	if (err)
		return err;

	size = le32_to_cpu(encoded_size);
	if (size > INT_MAX || size > size_pos)
		return -EFSCORRUPTED;

	pos = round_down(size_pos - size, i_blocksize(inode));
	if (pos < ext4_verity_area_start(inode))
		return -EFSCORRUPTED;

	*descriptor_size = size;
	*descriptor_pos = pos;
	return 0;
}

static int ext4_verity_get_descriptor(
	struct inode *inode, void *buffer, size_t buffer_size)
{
	size_t size;
	u64 position;
	int err;

	err = ext4_verity_descriptor_location(inode, &size, &position);
	if (err)
		return err;
	if (!buffer_size)
		return size;
	if (size > buffer_size)
		return -ERANGE;

	err = ext4_verity_read(inode, buffer, size, position);
	return err ? err : (int)size;
}

static struct page *ext4_verity_read_tree_page(
	struct inode *inode, pgoff_t index, unsigned long readahead_pages)
{
	struct folio *folio;

	index += ext4_verity_area_start(inode) >> PAGE_SHIFT;
	folio = __filemap_get_folio(
		inode->i_mapping, index, FGP_ACCESSED, 0);

	if (IS_ERR(folio) || !folio_test_uptodate(folio)) {
		DEFINE_READAHEAD(ractl, NULL, NULL, inode->i_mapping, index);

		if (!IS_ERR(folio))
			folio_put(folio);
		else if (readahead_pages > 1)
			page_cache_ra_unbounded(
				&ractl, readahead_pages, 0);

		folio = read_mapping_folio(
			inode->i_mapping, index, NULL);
		if (IS_ERR(folio))
			return ERR_CAST(folio);
	}

	return folio_file_page(folio, index);
}

static int ext4_verity_write_tree_block(
	struct inode *inode, const void *buffer,
	u64 position, unsigned int size)
{
	return ext4_verity_write(
		inode, buffer, size,
		position + ext4_verity_area_start(inode));
}

const struct fsverity_operations ext4_verityops = {
	.begin_enable_verity = ext4_verity_begin,
	.end_enable_verity = ext4_verity_finish,
	.get_verity_descriptor = ext4_verity_get_descriptor,
	.read_merkle_tree_page = ext4_verity_read_tree_page,
	.write_merkle_tree_block = ext4_verity_write_tree_block,
};
