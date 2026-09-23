/*
 * Infiltrator Filesystem Support — EXT2 Linux directory adapter.
 *
 * On-disk record geometry and validation are owned by the canonical EXT2 core.
 * This file binds those rules to Linux folios, VFS enumeration and mutation.
 */

#include "ext2.h"

#include <linux/buffer_head.h>
#include <linux/iversion.h>
#include <linux/pagemap.h>
#include <linux/swap.h>

typedef struct ext2_dir_entry_2 ext2_dirent;

static unsigned ext2_dir_decode_length(__le16 disk_length)
{
	return ifs_ext2_directory_record_length_from_disk(
		le16_to_cpu(disk_length), PAGE_SIZE);
}

static __le16 ext2_dir_encode_length(unsigned length)
{
	ifs_ext2_u16 encoded = 0U;
	IfsExt2Status status;

	status = ifs_ext2_directory_record_length_to_disk(
		length, PAGE_SIZE, &encoded);
	BUG_ON(status != IFS_EXT2_OK);
	return cpu_to_le16(encoded);
}

static unsigned ext2_dir_chunk_size(const struct inode *inode)
{
	return inode->i_sb->s_blocksize;
}

static unsigned ext2_dir_page_bytes(
	const struct inode *inode, unsigned long page_index)
{
	loff_t start = (loff_t)page_index << PAGE_SHIFT;
	loff_t remaining;

	if (start >= inode->i_size)
		return 0;

	remaining = inode->i_size - start;
	return remaining < PAGE_SIZE ? (unsigned)remaining : PAGE_SIZE;
}

static ext2_dirent *ext2_dir_next(ext2_dirent *entry)
{
	return (ext2_dirent *)((char *)entry +
		ext2_dir_decode_length(entry->rec_len));
}

static bool ext2_dir_name_matches(
	const ext2_dirent *entry, const char *name, unsigned int length)
{
	return entry->inode != 0 &&
	       entry->name_len == length &&
	       memcmp(entry->name, name, length) == 0;
}

static void ext2_dir_set_type(ext2_dirent *entry, const struct inode *inode)
{
	if (EXT2_HAS_INCOMPAT_FEATURE(
		    inode->i_sb, EXT2_FEATURE_INCOMPAT_FILETYPE))
		entry->file_type = fs_umode_to_ftype(inode->i_mode);
	else
		entry->file_type = 0;
}

static void ext2_dir_commit(
	struct folio *folio, loff_t position, unsigned int length)
{
	struct inode *dir = folio->mapping->host;

	inode_inc_iversion(dir);
	block_write_end(
		NULL, folio->mapping,
		position, length, length, folio, NULL);

	if (position + length > dir->i_size) {
		i_size_write(dir, position + length);
		mark_inode_dirty(dir);
	}

	folio_unlock(folio);
}

static bool ext2_dir_validate_folio(
	struct folio *folio, bool quiet, char *base)
{
	struct inode *dir = folio->mapping->host;
	struct super_block *sb = dir->i_sb;
	const unsigned int chunk = ext2_dir_chunk_size(dir);
	const u32 max_inode =
		le32_to_cpu(EXT2_SB(sb)->s_es->s_inodes_count);
	unsigned int limit = folio_size(folio);
	unsigned int offset = 0;

	if (dir->i_size < folio_pos(folio) + limit) {
		limit = offset_in_folio(folio, dir->i_size);
		if ((limit & (chunk - 1U)) != 0U) {
			if (!quiet)
				ext2_error(sb, __func__,
					   "directory %lu size is not block aligned",
					   dir->i_ino);
			return false;
		}
	}

	while (offset < limit) {
		ext2_dirent *entry;
		unsigned int record_length;
		IfsExt2DirectoryRecordStatus status;

		if (limit - offset <
		    ifs_ext2_directory_record_required_length(1U)) {
			if (!quiet)
				ext2_error(sb, __func__,
					   "directory %lu ends inside a record",
					   dir->i_ino);
			return false;
		}

		entry = (ext2_dirent *)(base + offset);
		record_length =
			ext2_dir_decode_length(entry->rec_len);

		status = ifs_ext2_validate_directory_record(
			offset, record_length,
			entry->name_len, le32_to_cpu(entry->inode),
			chunk, max_inode);
		if (status != IFS_EXT2_DIRECTORY_RECORD_OK) {
			if (!quiet)
				ext2_error(
					sb, __func__,
					"directory %lu record at %llu is corrupt: %s",
					dir->i_ino,
					(unsigned long long)(
						folio_pos(folio) + offset),
					ifs_ext2_directory_record_status_string(
						status));
			return false;
		}

		offset += record_length;
	}

	if (offset != limit)
		return false;

	folio_set_checked(folio);
	return true;
}

static void *ext2_dir_map_folio(
	struct inode *dir, unsigned long page_index,
	bool quiet, struct folio **folio_out)
{
	struct folio *folio;
	void *base;

	folio = read_mapping_folio(
		dir->i_mapping, page_index, NULL);
	if (IS_ERR(folio))
		return ERR_CAST(folio);

	base = kmap_local_folio(folio, 0);
	if (!folio_test_checked(folio) &&
	    !ext2_dir_validate_folio(folio, quiet, base)) {
		folio_release_kmap(folio, base);
		return ERR_PTR(-EIO);
	}

	*folio_out = folio;
	return base;
}

static unsigned int ext2_dir_resume_offset(
	char *base, unsigned int requested, unsigned int chunk_mask)
{
	ext2_dirent *target =
		(ext2_dirent *)(base + requested);
	ext2_dirent *entry =
		(ext2_dirent *)(base + (requested & chunk_mask));

	while ((char *)entry < (char *)target) {
		unsigned int length =
			ext2_dir_decode_length(entry->rec_len);

		if (length == 0U)
			break;
		entry = (ext2_dirent *)((char *)entry + length);
	}

	return offset_in_page(entry);
}

static int ext2_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	const bool has_filetype =
		EXT2_HAS_INCOMPAT_FEATURE(
			sb, EXT2_FEATURE_INCOMPAT_FILETYPE);
	const unsigned int chunk_mask =
		~(ext2_dir_chunk_size(inode) - 1U);
	unsigned long page_index;
	unsigned long pages;
	unsigned int offset;
	bool revalidate;

	if (ctx->pos < 0)
		return -EINVAL;
	if (ctx->pos >
	    inode->i_size -
		(loff_t)ifs_ext2_directory_record_required_length(1U))
		return 0;

	page_index = ctx->pos >> PAGE_SHIFT;
	offset = ctx->pos & ~PAGE_MASK;
	pages = dir_pages(inode);
	revalidate =
		!inode_eq_iversion(
			inode, *(u64 *)file->private_data);

	for (; page_index < pages;
	     ++page_index, offset = 0U) {
		struct folio *folio;
		char *base = ext2_dir_map_folio(
			inode, page_index, false, &folio);
		char *end;
		ext2_dirent *entry;

		if (IS_ERR(base)) {
			ext2_error(sb, __func__,
				   "cannot read directory page %lu for inode %lu",
				   page_index, inode->i_ino);
			ctx->pos += PAGE_SIZE - offset;
			return PTR_ERR(base);
		}

		if (revalidate) {
			if (offset != 0U) {
				offset = ext2_dir_resume_offset(
					base, offset, chunk_mask);
				ctx->pos =
					((loff_t)page_index << PAGE_SHIFT) +
					offset;
			}
			*(u64 *)file->private_data =
				inode_query_iversion(inode);
			revalidate = false;
		}

		end = base + ext2_dir_page_bytes(
			inode, page_index);
		entry = (ext2_dirent *)(base + offset);

		while ((char *)entry +
		       ifs_ext2_directory_record_required_length(1U)
		       <= end) {
			unsigned int length =
				ext2_dir_decode_length(entry->rec_len);

			if (length == 0U ||
			    (char *)entry + length > end) {
				ext2_error(sb, __func__,
					   "corrupt directory record in inode %lu",
					   inode->i_ino);
				folio_release_kmap(folio, base);
				return -EIO;
			}

			if (entry->inode != 0) {
				unsigned char type = DT_UNKNOWN;

				if (has_filetype)
					type = fs_ftype_to_dtype(
						entry->file_type);
				if (!dir_emit(
					    ctx, entry->name, entry->name_len,
					    le32_to_cpu(entry->inode), type)) {
					folio_release_kmap(folio, base);
					return 0;
				}
			}

			ctx->pos += length;
			entry = (ext2_dirent *)((char *)entry + length);
		}

		folio_release_kmap(folio, base);
	}

	return 0;
}

struct ext2_dir_entry_2 *ext2_find_entry(
	struct inode *dir, const struct qstr *child,
	struct folio **folio_out)
{
	struct ext2_inode_info *info = EXT2_I(dir);
	const unsigned int needed =
		ifs_ext2_directory_record_required_length(
			(ifs_ext2_u32)child->len);
	const unsigned long pages = dir_pages(dir);
	unsigned long start;
	unsigned long page_index;

	if (needed == 0U || pages == 0U)
		return ERR_PTR(-ENOENT);

	start = info->i_dir_start_lookup;
	if (start >= pages)
		start = 0;
	page_index = start;

	do {
		struct folio *folio;
		char *base =
			ext2_dir_map_folio(
				dir, page_index, false, &folio);
		char *end;
		ext2_dirent *entry;

		if (IS_ERR(base))
			return ERR_CAST(base);

		end = base + ext2_dir_page_bytes(dir, page_index);
		entry = (ext2_dirent *)base;

		while ((char *)entry + needed <= end) {
			unsigned int length =
				ext2_dir_decode_length(entry->rec_len);

			if (length == 0U ||
			    (char *)entry + length > end) {
				ext2_error(dir->i_sb, __func__,
					   "corrupt directory record in inode %lu",
					   dir->i_ino);
				folio_release_kmap(folio, base);
				return ERR_PTR(-EIO);
			}

			if (ext2_dir_name_matches(
				    entry, child->name, child->len)) {
				info->i_dir_start_lookup = page_index;
				*folio_out = folio;
				return entry;
			}

			entry = (ext2_dirent *)((char *)entry + length);
		}

		folio_release_kmap(folio, base);
		if (++page_index >= pages)
			page_index = 0;

		if (page_index >
		    (dir->i_blocks >> (PAGE_SHIFT - 9))) {
			ext2_error(
				dir->i_sb, __func__,
				"directory %lu size %lld exceeds block count %llu",
				dir->i_ino, dir->i_size,
				(unsigned long long)dir->i_blocks);
			return ERR_PTR(-EUCLEAN);
		}
	} while (page_index != start);

	return ERR_PTR(-ENOENT);
}

struct ext2_dir_entry_2 *ext2_dotdot(
	struct inode *dir, struct folio **folio_out)
{
	struct folio *folio;
	char *base =
		ext2_dir_map_folio(dir, 0, false, &folio);
	ext2_dirent *first;
	ext2_dirent *second;

	if (IS_ERR(base))
		return NULL;

	first = (ext2_dirent *)base;
	second = ext2_dir_next(first);
	if ((char *)second >=
	    base + ext2_dir_page_bytes(dir, 0)) {
		folio_release_kmap(folio, base);
		return NULL;
	}

	*folio_out = folio;
	return second;
}

int ext2_inode_by_name(
	struct inode *dir, const struct qstr *child, ino_t *ino)
{
	struct folio *folio;
	ext2_dirent *entry =
		ext2_find_entry(dir, child, &folio);

	if (IS_ERR(entry))
		return PTR_ERR(entry);

	*ino = le32_to_cpu(entry->inode);
	folio_release_kmap(folio, entry);
	return 0;
}

static int ext2_dir_prepare(
	struct folio *folio, loff_t position, unsigned int length)
{
	return __block_write_begin(
		folio, position, length, ext2_get_block);
}

static int ext2_dir_sync(struct inode *dir)
{
	int result =
		filemap_write_and_wait(dir->i_mapping);

	if (result == 0)
		result = sync_inode_metadata(dir, 1);
	return result;
}

int ext2_set_link(
	struct inode *dir, struct ext2_dir_entry_2 *entry,
	struct folio *folio, struct inode *inode, bool update_times)
{
	const loff_t position =
		folio_pos(folio) + offset_in_folio(folio, entry);
	const unsigned int length =
		ext2_dir_decode_length(entry->rec_len);
	int result;

	folio_lock(folio);
	result = ext2_dir_prepare(folio, position, length);
	if (result != 0) {
		folio_unlock(folio);
		return result;
	}

	entry->inode = cpu_to_le32(inode->i_ino);
	ext2_dir_set_type(entry, inode);
	ext2_dir_commit(folio, position, length);

	if (update_times)
		inode_set_mtime_to_ts(
			dir, inode_set_ctime_current(dir));
	EXT2_I(dir)->i_flags &= ~EXT2_BTREE_FL;
	mark_inode_dirty(dir);
	return ext2_dir_sync(dir);
}

int ext2_add_link(struct dentry *dentry, struct inode *inode)
{
	struct inode *dir = d_inode(dentry->d_parent);
	const char *name = dentry->d_name.name;
	const unsigned int name_length = dentry->d_name.len;
	const unsigned int needed =
		ifs_ext2_directory_record_required_length(name_length);
	const unsigned int chunk = ext2_dir_chunk_size(dir);
	const unsigned long pages = dir_pages(dir);
	unsigned long page_index;

	if (needed == 0U)
		return -ENAMETOOLONG;

	for (page_index = 0; page_index <= pages; ++page_index) {
		struct folio *folio;
		char *base =
			ext2_dir_map_folio(
				dir, page_index, false, &folio);
		char *end;
		char *scan_end;
		ext2_dirent *entry;

		if (IS_ERR(base))
			return PTR_ERR(base);

		folio_lock(folio);
		end = base + ext2_dir_page_bytes(dir, page_index);
		scan_end = base + folio_size(folio) - needed;
		entry = (ext2_dirent *)base;

		while ((char *)entry <= scan_end) {
			unsigned int record_length;
			ifs_ext2_u32 occupied_length = 0U;

			if ((char *)entry == end) {
				record_length = chunk;
				entry->rec_len =
					ext2_dir_encode_length(chunk);
				entry->inode = 0;
				occupied_length = 0U;
				goto found;
			}

			record_length =
				ext2_dir_decode_length(entry->rec_len);
			if (record_length == 0U) {
				ext2_error(dir->i_sb, __func__,
					   "zero-length directory record");
				folio_unlock(folio);
				folio_release_kmap(folio, base);
				return -EIO;
			}

			if (ext2_dir_name_matches(
				    entry, name, name_length)) {
				folio_unlock(folio);
				folio_release_kmap(folio, base);
				return -EEXIST;
			}

			if (ifs_ext2_directory_record_can_insert(
				    record_length,
				    entry->name_len,
				    le32_to_cpu(entry->inode),
				    name_length,
				    &occupied_length)) {
found:
				{
					loff_t position =
						folio_pos(folio) +
						offset_in_folio(folio, entry);
					int result =
						ext2_dir_prepare(
							folio, position,
							record_length);

					if (result != 0) {
						folio_unlock(folio);
						folio_release_kmap(
							folio, base);
						return result;
					}

					if (entry->inode != 0) {
						ext2_dirent *new_entry =
							(ext2_dirent *)(
								(char *)entry +
								occupied_length);
						new_entry->rec_len =
							ext2_dir_encode_length(
								record_length -
								occupied_length);
						entry->rec_len =
							ext2_dir_encode_length(
								occupied_length);
						entry = new_entry;
					}

					entry->name_len = name_length;
					memcpy(entry->name, name, name_length);
					entry->inode =
						cpu_to_le32(inode->i_ino);
					ext2_dir_set_type(entry, inode);
					ext2_dir_commit(
						folio, position,
						record_length);
					inode_set_mtime_to_ts(
						dir,
						inode_set_ctime_current(dir));
					EXT2_I(dir)->i_flags &=
						~EXT2_BTREE_FL;
					mark_inode_dirty(dir);
					result = ext2_dir_sync(dir);
					folio_release_kmap(
						folio, base);
					return result;
				}
			}

			entry = (ext2_dirent *)(
				(char *)entry + record_length);
		}

		folio_unlock(folio);
		folio_release_kmap(folio, base);
	}

	return -ENOSPC;
}

int ext2_delete_entry(
	struct ext2_dir_entry_2 *target, struct folio *folio)
{
	struct inode *dir = folio->mapping->host;
	const unsigned int chunk = ext2_dir_chunk_size(dir);
	const unsigned int target_offset =
		offset_in_folio(folio, target);
	char *base = (char *)target - target_offset;
	unsigned int chunk_start =
		target_offset & ~(chunk - 1U);
	ext2_dirent *entry =
		(ext2_dirent *)(base + chunk_start);
	ext2_dirent *previous = NULL;
	ifs_ext2_u32 span_offset;
	ifs_ext2_u32 span_length;
	loff_t position;
	int result;

	while ((char *)entry < (char *)target) {
		unsigned int length =
			ext2_dir_decode_length(entry->rec_len);

		if (length == 0U)
			return -EUCLEAN;
		previous = entry;
		entry = (ext2_dirent *)((char *)entry + length);
	}

	result = ifs_ext2_directory_delete_span(
		target_offset,
		ext2_dir_decode_length(target->rec_len),
		previous != NULL,
		previous ? offset_in_folio(folio, previous) : 0U,
		chunk, &span_offset, &span_length);
	if (result != IFS_EXT2_OK)
		return -EFSCORRUPTED;

	position = folio_pos(folio) + span_offset;
	folio_lock(folio);
	result = ext2_dir_prepare(
		folio, position, span_length);
	if (result != 0) {
		folio_unlock(folio);
		return result;
	}

	if (previous)
		previous->rec_len =
			ext2_dir_encode_length(span_length);
	target->inode = 0;
	ext2_dir_commit(folio, position, span_length);

	inode_set_mtime_to_ts(
		dir, inode_set_ctime_current(dir));
	EXT2_I(dir)->i_flags &= ~EXT2_BTREE_FL;
	mark_inode_dirty(dir);
	return ext2_dir_sync(dir);
}

int ext2_make_empty(struct inode *inode, struct inode *parent)
{
	struct folio *folio =
		filemap_grab_folio(inode->i_mapping, 0);
	const unsigned int chunk = ext2_dir_chunk_size(inode);
	ifs_ext2_u32 dot_length;
	ifs_ext2_u32 dotdot_length;
	ext2_dirent *dot;
	ext2_dirent *dotdot;
	void *base;
	int result;

	if (IS_ERR(folio))
		return PTR_ERR(folio);

	result = ifs_ext2_directory_initial_layout(
		chunk, &dot_length, &dotdot_length);
	if (result != IFS_EXT2_OK) {
		folio_unlock(folio);
		folio_put(folio);
		return -EFSCORRUPTED;
	}

	result = ext2_dir_prepare(folio, 0, chunk);
	if (result != 0) {
		folio_unlock(folio);
		folio_put(folio);
		return result;
	}

	base = kmap_local_folio(folio, 0);
	memset(base, 0, chunk);

	dot = (ext2_dirent *)base;
	dot->inode = cpu_to_le32(inode->i_ino);
	dot->name_len = 1;
	dot->rec_len = ext2_dir_encode_length(dot_length);
	memcpy(dot->name, ".\0\0", 4);
	ext2_dir_set_type(dot, inode);

	dotdot = (ext2_dirent *)((char *)base + dot_length);
	dotdot->inode = cpu_to_le32(parent->i_ino);
	dotdot->name_len = 2;
	dotdot->rec_len = ext2_dir_encode_length(dotdot_length);
	memcpy(dotdot->name, "..\0", 4);
	ext2_dir_set_type(dotdot, inode);

	kunmap_local(base);
	ext2_dir_commit(folio, 0, chunk);
	result = ext2_dir_sync(inode);
	folio_put(folio);
	return result;
}

int ext2_empty_dir(struct inode *inode)
{
	unsigned long page_index;
	const unsigned long pages = dir_pages(inode);

	for (page_index = 0; page_index < pages; ++page_index) {
		struct folio *folio;
		char *base =
			ext2_dir_map_folio(
				inode, page_index, false, &folio);
		char *end;
		ext2_dirent *entry;

		if (IS_ERR(base))
			return 0;

		end = base + ext2_dir_page_bytes(inode, page_index);
		entry = (ext2_dirent *)base;

		while ((char *)entry +
		       ifs_ext2_directory_record_required_length(1U)
		       <= end) {
			unsigned int length =
				ext2_dir_decode_length(entry->rec_len);

			if (length == 0U ||
			    (char *)entry + length > end) {
				folio_release_kmap(folio, base);
				return 0;
			}

			if (entry->inode != 0) {
				if (entry->name_len == 1 &&
				    entry->name[0] == '.' &&
				    entry->inode ==
					cpu_to_le32(inode->i_ino)) {
					/* self */
				} else if (entry->name_len == 2 &&
					   entry->name[0] == '.' &&
					   entry->name[1] == '.') {
					/* parent */
				} else {
					folio_release_kmap(folio, base);
					return 0;
				}
			}

			entry = (ext2_dirent *)((char *)entry + length);
		}

		folio_release_kmap(folio, base);
	}

	return 1;
}

static int ext2_dir_open(struct inode *inode, struct file *file)
{
	file->private_data = kzalloc(sizeof(u64), GFP_KERNEL);
	return file->private_data ? 0 : -ENOMEM;
}

static int ext2_dir_release(struct inode *inode, struct file *file)
{
	kfree(file->private_data);
	file->private_data = NULL;
	return 0;
}

static loff_t ext2_dir_llseek(
	struct file *file, loff_t offset, int whence)
{
	return generic_llseek_cookie(
		file, offset, whence,
		(u64 *)file->private_data);
}

const struct file_operations ext2_dir_operations = {
	.open = ext2_dir_open,
	.release = ext2_dir_release,
	.llseek = ext2_dir_llseek,
	.read = generic_read_dir,
	.iterate_shared = ext2_readdir,
	.unlocked_ioctl = ext2_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ext2_compat_ioctl,
#endif
	.fsync = ext2_fsync,
};
