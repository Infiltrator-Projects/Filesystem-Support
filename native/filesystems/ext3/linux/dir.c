/*
 * Filesystem Support EXT3 Linux directory adapter.
 *
 * The canonical EXT3 core owns directory-record geometry and HTree hashing.
 * This file owns Linux VFS enumeration, position encoding and the temporary
 * HTree result cache used while a directory is open.
 */

#include <linux/compat.h>
#include <linux/iversion.h>
#include <linux/rbtree.h>
#include <linux/slab.h>

#include "ext3.h"

static const unsigned char ifs_ext3_file_types[] = {
	DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR,
	DT_BLK, DT_FIFO, DT_SOCK, DT_LNK
};

struct fname {
	u32 hash;
	u32 minor_hash;
	struct rb_node node;
	struct fname *collision;
	u32 inode;
	u8 name_len;
	u8 file_type;
	char name[];
};

static unsigned char ifs_ext3_dtype(
	const struct super_block *sb, const unsigned int file_type)
{
	if (!EXT3_HAS_INCOMPAT_FEATURE(
		    sb, EXT3_FEATURE_INCOMPAT_FILETYPE) ||
	    file_type >= ARRAY_SIZE(ifs_ext3_file_types))
		return DT_UNKNOWN;

	return ifs_ext3_file_types[file_type];
}

static bool ifs_ext3_indexed_directory(const struct inode *inode)
{
	const struct super_block *sb = inode->i_sb;

	if (!EXT3_HAS_COMPAT_FEATURE(
		    sb, EXT3_FEATURE_COMPAT_DIR_INDEX))
		return false;

	return (EXT3_I(inode)->i_flags & EXT3_INDEX_FL) != 0U ||
	       (inode->i_size >> sb->s_blocksize_bits) == 1;
}

int ext3_check_dir_entry(
	const char *function,
	struct inode *dir,
	struct ext3_dir_entry_2 *entry,
	struct buffer_head *bh,
	unsigned long offset)
{
	const unsigned int record_length =
		ext3_rec_len_from_disk(entry->rec_len);
	const IfsExt3DirectoryRecordStatus status =
		ifs_ext3_validate_directory_record(
			(ifs_ext3_u32)((char *)entry - bh->b_data),
			(ifs_ext3_u32)record_length,
			(ifs_ext3_u32)entry->name_len,
			le32_to_cpu(entry->inode),
			(ifs_ext3_u32)dir->i_sb->s_blocksize,
			le32_to_cpu(EXT3_SB(dir->i_sb)->s_es->s_inodes_count));

	if (likely(status == IFS_EXT3_DIRECTORY_RECORD_OK))
		return 1;

	ext3_error(
		dir->i_sb, function,
		"bad entry in directory #%lu: %s - "
		"offset=%lu, inode=%lu, rec_len=%u, name_len=%u",
		dir->i_ino,
		ifs_ext3_directory_record_status_string(status),
		offset,
		(unsigned long)le32_to_cpu(entry->inode),
		record_length,
		entry->name_len);
	return 0;
}

static unsigned int ifs_ext3_resynchronise_offset(
	const struct buffer_head *bh,
	unsigned int requested,
	unsigned int block_size)
{
	unsigned int offset = 0U;

	while (offset < requested &&
	       offset + EXT3_DIR_REC_LEN(1) <= block_size) {
		const struct ext3_dir_entry_2 *entry =
			(const struct ext3_dir_entry_2 *)(bh->b_data + offset);
		const unsigned int length =
			ext3_rec_len_from_disk(entry->rec_len);

		if (length < EXT3_DIR_REC_LEN(1) ||
		    (length & 3U) != 0U ||
		    length > block_size - offset)
			break;
		offset += length;
	}

	return offset;
}

static int ifs_ext3_linear_readdir(
	struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct dir_private_info *info = file->private_data;
	unsigned int offset = (unsigned int)(ctx->pos & (sb->s_blocksize - 1U));
	bool reported_hole = false;

	while (ctx->pos < inode->i_size) {
		const unsigned long logical =
			(unsigned long)(ctx->pos >> sb->s_blocksize_bits);
		struct buffer_head map = { 0 };
		struct buffer_head *bh = NULL;
		int error;

		error = ext3_get_blocks_handle(
			NULL, inode, logical, 1, &map, 0);
		if (error > 0) {
			const pgoff_t index =
				map.b_blocknr >>
				(PAGE_SHIFT - inode->i_blkbits);

			if (!ra_has_index(&file->f_ra, index))
				page_cache_sync_readahead(
					sb->s_bdev->bd_mapping,
					&file->f_ra, file, index, 1);
			file->f_ra.prev_pos = (loff_t)index << PAGE_SHIFT;
			bh = ext3_bread(
				NULL, inode, logical, 0, &error);
		}

		if (!bh) {
			if (!reported_hole) {
				ext3_error(
					sb, __func__,
					"directory #%lu contains a hole at offset %lld",
					inode->i_ino, ctx->pos);
				reported_hole = true;
			}
			if (ctx->pos > (loff_t)inode->i_blocks << 9)
				break;
			ctx->pos += sb->s_blocksize - offset;
			offset = 0U;
			continue;
		}

		if (offset != 0U && info &&
		    !inode_eq_iversion(inode, info->cookie)) {
			offset = ifs_ext3_resynchronise_offset(
				bh, offset, sb->s_blocksize);
			ctx->pos =
				(ctx->pos & ~(loff_t)(sb->s_blocksize - 1U)) |
				offset;
			info->cookie = inode_query_iversion(inode);
		}

		while (ctx->pos < inode->i_size &&
		       offset < sb->s_blocksize) {
			struct ext3_dir_entry_2 *entry =
				(struct ext3_dir_entry_2 *)(bh->b_data + offset);
			unsigned int length;

			if (!ext3_check_dir_entry(
				    __func__, inode, entry, bh, offset)) {
				ctx->pos =
					(ctx->pos | (sb->s_blocksize - 1U)) + 1U;
				break;
			}

			length = ext3_rec_len_from_disk(entry->rec_len);
			if (entry->inode != 0U &&
			    !dir_emit(
				    ctx, entry->name, entry->name_len,
				    le32_to_cpu(entry->inode),
				    ifs_ext3_dtype(sb, entry->file_type))) {
				brelse(bh);
				return 0;
			}

			offset += length;
			ctx->pos += length;
		}

		brelse(bh);
		offset = 0U;
		if (ctx->pos < inode->i_size && !dir_relax(inode))
			return 0;
	}

	return 0;
}

static bool ifs_ext3_hash_api_is_32bit(const struct file *file)
{
#ifdef CONFIG_COMPAT
	if (file->f_mode & FMODE_32BITHASH)
		return true;
	if (file->f_mode & FMODE_64BITHASH)
		return false;
	return in_compat_syscall();
#else
	if (file->f_mode & FMODE_32BITHASH)
		return true;
	if (file->f_mode & FMODE_64BITHASH)
		return false;
	return BITS_PER_LONG == 32;
#endif
}

static loff_t ifs_ext3_hash_position(
	const struct file *file, u32 major, u32 minor)
{
	if (ifs_ext3_hash_api_is_32bit(file))
		return (loff_t)(major >> 1);

	return (loff_t)(((u64)(major >> 1) << 32) | minor);
}

static u32 ifs_ext3_position_major(
	const struct file *file, loff_t position)
{
	if (ifs_ext3_hash_api_is_32bit(file))
		return (u32)position << 1;

	return (u32)((u64)position >> 32) << 1;
}

static u32 ifs_ext3_position_minor(
	const struct file *file, loff_t position)
{
	return ifs_ext3_hash_api_is_32bit(file) ?
		0U : (u32)position;
}

static loff_t ifs_ext3_hash_eof(const struct file *file)
{
	return ifs_ext3_hash_api_is_32bit(file) ?
		EXT3_HTREE_EOF_32BIT : EXT3_HTREE_EOF_64BIT;
}

static loff_t ifs_ext3_dir_llseek(
	struct file *file, loff_t offset, int whence)
{
	if (ifs_ext3_indexed_directory(file_inode(file))) {
		const loff_t limit = ifs_ext3_hash_eof(file);

		return generic_file_llseek_size(
			file, offset, whence, limit, limit);
	}

	return generic_file_llseek(file, offset, whence);
}

static void ifs_ext3_free_fname_tree(struct rb_root *root)
{
	struct fname *entry;
	struct fname *next;

	rbtree_postorder_for_each_entry_safe(
		entry, next, root, node) {
		while (entry) {
			struct fname *old = entry;
			entry = entry->collision;
			kfree(old);
		}
	}

	*root = RB_ROOT;
}

static struct dir_private_info *ifs_ext3_dir_state_create(
	struct file *file, loff_t position)
{
	struct dir_private_info *state =
		kzalloc(sizeof(*state), GFP_KERNEL);

	if (!state)
		return NULL;

	state->root = RB_ROOT;
	state->curr_hash =
		ifs_ext3_position_major(file, position);
	state->curr_minor_hash =
		ifs_ext3_position_minor(file, position);
	state->cookie = inode_query_iversion(file_inode(file));
	state->last_pos = position;
	return state;
}

void ext3_htree_free_dir_info(struct dir_private_info *state)
{
	if (!state)
		return;

	ifs_ext3_free_fname_tree(&state->root);
	kfree(state);
}

int ext3_htree_store_dirent(
	struct file *file,
	u32 hash,
	u32 minor_hash,
	struct ext3_dir_entry_2 *dirent)
{
	struct dir_private_info *state = file->private_data;
	struct rb_node **link;
	struct rb_node *parent = NULL;
	struct fname *entry;
	size_t allocation;

	if (!state || !dirent)
		return -EINVAL;

	allocation = sizeof(*entry) + dirent->name_len + 1U;
	entry = kzalloc(allocation, GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	entry->hash = hash;
	entry->minor_hash = minor_hash;
	entry->inode = le32_to_cpu(dirent->inode);
	entry->name_len = dirent->name_len;
	entry->file_type = dirent->file_type;
	memcpy(entry->name, dirent->name, dirent->name_len);
	entry->name[dirent->name_len] = '\0';

	link = &state->root.rb_node;
	while (*link) {
		struct fname *current;

		parent = *link;
		current = rb_entry(parent, struct fname, node);

		if (hash == current->hash &&
		    minor_hash == current->minor_hash) {
			entry->collision = current->collision;
			current->collision = entry;
			return 0;
		}

		if (hash < current->hash ||
		    (hash == current->hash &&
		     minor_hash < current->minor_hash))
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}

	rb_link_node(&entry->node, parent, link);
	rb_insert_color(&entry->node, &state->root);
	return 0;
}

static bool ifs_ext3_emit_fname_chain(
	struct file *file,
	struct dir_context *ctx,
	struct fname *entry)
{
	struct dir_private_info *state = file->private_data;
	const struct super_block *sb = file_inode(file)->i_sb;

	ctx->pos =
		ifs_ext3_hash_position(file, entry->hash, entry->minor_hash);

	while (entry) {
		if (!dir_emit(
			    ctx, entry->name, entry->name_len,
			    entry->inode,
			    ifs_ext3_dtype(sb, entry->file_type))) {
			state->extra_fname = entry;
			return false;
		}
		entry = entry->collision;
	}

	state->extra_fname = NULL;
	return true;
}

static void ifs_ext3_reset_htree_state(
	struct file *file,
	struct dir_private_info *state,
	loff_t position)
{
	ifs_ext3_free_fname_tree(&state->root);
	state->curr_node = NULL;
	state->extra_fname = NULL;
	state->curr_hash =
		ifs_ext3_position_major(file, position);
	state->curr_minor_hash =
		ifs_ext3_position_minor(file, position);
	state->last_pos = position;
}

static int ifs_ext3_dx_readdir(
	struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct dir_private_info *state = file->private_data;

	if (!state) {
		state = ifs_ext3_dir_state_create(file, ctx->pos);
		if (!state)
			return -ENOMEM;
		file->private_data = state;
	}

	if (ctx->pos == ifs_ext3_hash_eof(file))
		return 0;

	if (state->last_pos != ctx->pos)
		ifs_ext3_reset_htree_state(file, state, ctx->pos);

	if (state->extra_fname) {
		if (!ifs_ext3_emit_fname_chain(
			    file, ctx, state->extra_fname))
			goto out;
		state->extra_fname = NULL;
		if (state->curr_node)
			state->curr_node = rb_next(state->curr_node);
	}

	for (;;) {
		struct fname *entry;
		int result;

		if (!state->curr_node ||
		    !inode_eq_iversion(inode, state->cookie)) {
			ifs_ext3_free_fname_tree(&state->root);
			state->curr_node = NULL;
			state->cookie = inode_query_iversion(inode);

			result = ext3_htree_fill_tree(
				file,
				state->curr_hash,
				state->curr_minor_hash,
				&state->next_hash);
			if (result < 0)
				return result;
			if (result == 0) {
				ctx->pos = ifs_ext3_hash_eof(file);
				break;
			}

			state->curr_node = rb_first(&state->root);
			if (!state->curr_node) {
				ctx->pos = ifs_ext3_hash_eof(file);
				break;
			}
		}

		entry = rb_entry(state->curr_node, struct fname, node);
		state->curr_hash = entry->hash;
		state->curr_minor_hash = entry->minor_hash;

		if (!ifs_ext3_emit_fname_chain(file, ctx, entry))
			break;

		state->curr_node = rb_next(state->curr_node);
		if (state->curr_node) {
			entry = rb_entry(
				state->curr_node, struct fname, node);
			state->curr_hash = entry->hash;
			state->curr_minor_hash = entry->minor_hash;
			continue;
		}

		if (state->next_hash == ~0U) {
			ctx->pos = ifs_ext3_hash_eof(file);
			break;
		}

		state->curr_hash = state->next_hash;
		state->curr_minor_hash = 0U;
	}

out:
	state->last_pos = ctx->pos;
	return 0;
}

static int ifs_ext3_readdir(
	struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	int result;

	if (ifs_ext3_indexed_directory(inode)) {
		result = ifs_ext3_dx_readdir(file, ctx);
		if (result != ERR_BAD_DX_DIR)
			return result;

		EXT3_I(inode)->i_flags &= ~EXT3_INDEX_FL;
	}

	return ifs_ext3_linear_readdir(file, ctx);
}

static int ifs_ext3_dir_open(
	struct inode *inode, struct file *file)
{
	struct dir_private_info *state =
		ifs_ext3_dir_state_create(file, 0);

	if (!state)
		return -ENOMEM;

	state->cookie = inode_query_iversion(inode);
	file->private_data = state;
	return 0;
}

static int ifs_ext3_dir_release(
	struct inode *inode, struct file *file)
{
	(void)inode;
	ext3_htree_free_dir_info(file->private_data);
	file->private_data = NULL;
	return 0;
}

const struct file_operations ext3_dir_operations = {
	.open = ifs_ext3_dir_open,
	.llseek = ifs_ext3_dir_llseek,
	.read = generic_read_dir,
	.iterate_shared = ifs_ext3_readdir,
	.unlocked_ioctl = ext3_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ext3_compat_ioctl,
#endif
	.fsync = ext3_sync_file,
	.release = ifs_ext3_dir_release,
};

int ext3fs_dirhash(
	const char *name, int length, struct dx_hash_info *info)
{
	ifs_ext3_u32 major = 0U;
	ifs_ext3_u32 minor = 0U;

	if (!info || length < 0)
		return -1;

	if (ifs_ext3_directory_hash(
		    (const ifs_ext3_u8 *)name,
		    (ifs_ext3_u32)length,
		    info->hash_version,
		    info->seed,
		    &major,
		    &minor) != 0) {
		info->hash = 0U;
		info->minor_hash = 0U;
		return -1;
	}

	info->hash = major;
	info->minor_hash = minor;
	return 0;
}
