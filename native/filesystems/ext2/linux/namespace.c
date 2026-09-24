/* Infiltrator Filesystem Support — EXT2 Linux namespace adapter.
 * Directory iteration and namespace mutation are kept together; portable
 * directory-record semantics remain in the canonical EXT2 core.
 */

/*
 * Infiltrator Filesystem Support — EXT2 Linux directory adapter.
 *
 * On-disk record geometry and validation are owned by the canonical EXT2 core.
 * This file binds those rules to Linux folios, VFS enumeration and mutation.
 */

#include "linux_adapter.h"

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


/* ===== namespace mutation ===== */
/*
 * Infiltrator Filesystem Support — EXT2 Linux namespace adapter.
 *
 * Directory-record semantics live in the EXT2 directory/core implementation.
 * This unit translates Linux VFS namespace operations into those primitives.
 */

#include <linux/pagemap.h>
#include <linux/quotaops.h>

#include "linux_adapter.h"

static int ext2_publish_new_nondir(struct dentry *dentry, struct inode *inode)
{
	int result = ext2_add_link(dentry, inode);

	if (result == 0) {
		d_instantiate_new(dentry, inode);
		return 0;
	}

	inode_dec_link_count(inode);
	discard_new_inode(inode);
	return result;
}

static struct dentry *ext2_lookup(
	struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	struct inode *inode = NULL;
	ino_t ino;
	int result;

	if (dentry->d_name.len > EXT2_NAME_LEN)
		return ERR_PTR(-ENAMETOOLONG);

	result = ext2_inode_by_name(dir, &dentry->d_name, &ino);
	if (result == -ENOENT)
		return d_splice_alias(NULL, dentry);
	if (result != 0)
		return ERR_PTR(result);

	inode = ext2_iget(dir->i_sb, ino);
	if (inode == ERR_PTR(-ESTALE)) {
		ext2_error(dir->i_sb, __func__,
			   "directory references deleted inode %lu",
			   (unsigned long)ino);
		return ERR_PTR(-EIO);
	}

	return d_splice_alias(inode, dentry);
}

struct dentry *ext2_get_parent(struct dentry *child)
{
	ino_t parent_ino;
	int result;

	result = ext2_inode_by_name(d_inode(child), &dotdot_name, &parent_ino);
	if (result != 0)
		return ERR_PTR(result);

	return d_obtain_alias(ext2_iget(child->d_sb, parent_ino));
}

static int ext2_create(
	struct mnt_idmap *idmap, struct inode *dir,
	struct dentry *dentry, umode_t mode, bool exclusive)
{
	struct inode *inode;
	int result;

	result = dquot_initialize(dir);
	if (result != 0)
		return result;

	inode = ext2_new_inode(dir, mode, &dentry->d_name);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	ext2_set_file_ops(inode);
	mark_inode_dirty(inode);
	return ext2_publish_new_nondir(dentry, inode);
}

static int ext2_tmpfile(
	struct mnt_idmap *idmap, struct inode *dir,
	struct file *file, umode_t mode)
{
	struct inode *inode;

	inode = ext2_new_inode(dir, mode, NULL);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	ext2_set_file_ops(inode);
	mark_inode_dirty(inode);
	d_tmpfile(file, inode);
	unlock_new_inode(inode);
	return finish_open_simple(file, 0);
}

static int ext2_mknod(
	struct mnt_idmap *idmap, struct inode *dir,
	struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct inode *inode;
	int result;

	result = dquot_initialize(dir);
	if (result != 0)
		return result;

	inode = ext2_new_inode(dir, mode, &dentry->d_name);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	init_special_inode(inode, inode->i_mode, rdev);
	inode->i_op = &ext2_special_inode_operations;
	mark_inode_dirty(inode);
	return ext2_publish_new_nondir(dentry, inode);
}

static int ext2_symlink(
	struct mnt_idmap *idmap, struct inode *dir,
	struct dentry *dentry, const char *target)
{
	struct super_block *sb = dir->i_sb;
	struct inode *inode;
	size_t bytes;
	int result;

	bytes = strlen(target) + 1U;
	if (bytes > sb->s_blocksize)
		return -ENAMETOOLONG;

	result = dquot_initialize(dir);
	if (result != 0)
		return result;

	inode = ext2_new_inode(
		dir, S_IFLNK | S_IRWXUGO, &dentry->d_name);
	if (IS_ERR(inode))
		return PTR_ERR(inode);

	if (bytes > sizeof(EXT2_I(inode)->i_data)) {
		inode->i_op = &ext2_symlink_inode_operations;
		inode_nohighmem(inode);
		inode->i_mapping->a_ops = &ext2_aops;
		result = page_symlink(inode, target, bytes);
		if (result != 0)
			goto fail_inode;
	} else {
		inode->i_op = &ext2_fast_symlink_inode_operations;
		inode->i_link = (char *)EXT2_I(inode)->i_data;
		memcpy(inode->i_link, target, bytes);
		inode->i_size = bytes - 1U;
	}

	mark_inode_dirty(inode);
	return ext2_publish_new_nondir(dentry, inode);

fail_inode:
	inode_dec_link_count(inode);
	discard_new_inode(inode);
	return result;
}

static int ext2_link(
	struct dentry *old_dentry, struct inode *dir,
	struct dentry *new_dentry)
{
	struct inode *inode = d_inode(old_dentry);
	int result;

	result = dquot_initialize(dir);
	if (result != 0)
		return result;

	inode_set_ctime_current(inode);
	inode_inc_link_count(inode);
	ihold(inode);

	result = ext2_add_link(new_dentry, inode);
	if (result == 0) {
		d_instantiate(new_dentry, inode);
		return 0;
	}

	inode_dec_link_count(inode);
	iput(inode);
	return result;
}

static int ext2_mkdir(
	struct mnt_idmap *idmap, struct inode *dir,
	struct dentry *dentry, umode_t mode)
{
	struct inode *inode;
	int result;

	result = dquot_initialize(dir);
	if (result != 0)
		return result;

	inode_inc_link_count(dir);
	inode = ext2_new_inode(
		dir, S_IFDIR | mode, &dentry->d_name);
	if (IS_ERR(inode)) {
		result = PTR_ERR(inode);
		goto fail_parent_link;
	}

	inode->i_op = &ext2_dir_inode_operations;
	inode->i_fop = &ext2_dir_operations;
	inode->i_mapping->a_ops = &ext2_aops;
	inode_inc_link_count(inode);

	result = ext2_make_empty(inode, dir);
	if (result != 0)
		goto fail_child;

	result = ext2_add_link(dentry, inode);
	if (result != 0)
		goto fail_child;

	d_instantiate_new(dentry, inode);
	return 0;

fail_child:
	inode_dec_link_count(inode);
	inode_dec_link_count(inode);
	discard_new_inode(inode);
fail_parent_link:
	inode_dec_link_count(dir);
	return result;
}

static int ext2_unlink(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	struct ext2_dir_entry_2 *entry;
	struct folio *folio;
	int result;

	result = dquot_initialize(dir);
	if (result != 0)
		return result;

	entry = ext2_find_entry(dir, &dentry->d_name, &folio);
	if (IS_ERR(entry))
		return PTR_ERR(entry);

	result = ext2_delete_entry(entry, folio);
	folio_release_kmap(folio, entry);
	if (result != 0)
		return result;

	inode_set_ctime_to_ts(inode, inode_get_ctime(dir));
	inode_dec_link_count(inode);
	return 0;
}

static int ext2_rmdir(struct inode *dir, struct dentry *dentry)
{
	struct inode *inode = d_inode(dentry);
	int result;

	if (!ext2_empty_dir(inode))
		return -ENOTEMPTY;

	result = ext2_unlink(dir, dentry);
	if (result != 0)
		return result;

	inode->i_size = 0;
	inode_dec_link_count(inode);
	inode_dec_link_count(dir);
	return 0;
}

static int ext2_rename(
	struct mnt_idmap *idmap,
	struct inode *old_dir, struct dentry *old_dentry,
	struct inode *new_dir, struct dentry *new_dentry,
	unsigned int flags)
{
	struct inode *old_inode = d_inode(old_dentry);
	struct inode *new_inode = d_inode(new_dentry);
	const bool directory = S_ISDIR(old_inode->i_mode);
	struct folio *old_folio = NULL;
	struct folio *dotdot_folio = NULL;
	struct ext2_dir_entry_2 *old_entry;
	struct ext2_dir_entry_2 *dotdot_entry = NULL;
	int result;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;

	result = dquot_initialize(old_dir);
	if (result != 0)
		return result;
	result = dquot_initialize(new_dir);
	if (result != 0)
		return result;

	old_entry = ext2_find_entry(
		old_dir, &old_dentry->d_name, &old_folio);
	if (IS_ERR(old_entry))
		return PTR_ERR(old_entry);

	if (directory && old_dir != new_dir) {
		dotdot_entry = ext2_dotdot(old_inode, &dotdot_folio);
		if (!dotdot_entry) {
			result = -EIO;
			goto out_old;
		}
	}

	if (new_inode) {
		struct folio *new_folio;
		struct ext2_dir_entry_2 *new_entry;

		if (directory && !ext2_empty_dir(new_inode)) {
			result = -ENOTEMPTY;
			goto out_dotdot;
		}

		new_entry = ext2_find_entry(
			new_dir, &new_dentry->d_name, &new_folio);
		if (IS_ERR(new_entry)) {
			result = PTR_ERR(new_entry);
			goto out_dotdot;
		}

		result = ext2_set_link(
			new_dir, new_entry, new_folio, old_inode, true);
		folio_release_kmap(new_folio, new_entry);
		if (result != 0)
			goto out_dotdot;

		inode_set_ctime_current(new_inode);
		if (directory)
			drop_nlink(new_inode);
		inode_dec_link_count(new_inode);
	} else {
		result = ext2_add_link(new_dentry, old_inode);
		if (result != 0)
			goto out_dotdot;
		if (directory)
			inode_inc_link_count(new_dir);
	}

	inode_set_ctime_current(old_inode);
	mark_inode_dirty(old_inode);

	result = ext2_delete_entry(old_entry, old_folio);
	if (result == 0 && directory) {
		if (old_dir != new_dir)
			result = ext2_set_link(
				old_inode, dotdot_entry,
				dotdot_folio, new_dir, false);
		inode_dec_link_count(old_dir);
	}

out_dotdot:
	if (dotdot_entry)
		folio_release_kmap(dotdot_folio, dotdot_entry);
out_old:
	folio_release_kmap(old_folio, old_entry);
	return result;
}

const struct inode_operations ext2_dir_inode_operations = {
	.create = ext2_create,
	.lookup = ext2_lookup,
	.link = ext2_link,
	.unlink = ext2_unlink,
	.symlink = ext2_symlink,
	.mkdir = ext2_mkdir,
	.rmdir = ext2_rmdir,
	.mknod = ext2_mknod,
	.rename = ext2_rename,
	.listxattr = ext2_listxattr,
	.getattr = ext2_getattr,
	.setattr = ext2_setattr,
	.get_inode_acl = ext2_get_acl,
	.set_acl = ext2_set_acl,
	.tmpfile = ext2_tmpfile,
	.fileattr_get = ext2_fileattr_get,
	.fileattr_set = ext2_fileattr_set,
};

const struct inode_operations ext2_special_inode_operations = {
	.listxattr = ext2_listxattr,
	.getattr = ext2_getattr,
	.setattr = ext2_setattr,
	.get_inode_acl = ext2_get_acl,
	.set_acl = ext2_set_acl,
};
