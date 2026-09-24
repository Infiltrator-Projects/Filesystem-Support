/*
 * Infiltrator Filesystem Support — EXT4 Linux directory adapter.
 *
 * Directory-record validation and format hashing live in the portable EXT4
 * core.  This file owns Linux VFS iteration, fscrypt/casefold translation and
 * the transient in-memory ordering used while walking HTree directories.
 */

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/iversion.h>
#include <linux/slab.h>
#include <linux/unicode.h>

#include "ext4.h"
#include "xattr.h"

struct ifs_ext4_dir_name {
	__u32 hash;
	__u32 minor_hash;
	struct rb_node node;
	struct ifs_ext4_dir_name *same_hash;
	__u32 inode;
	__u8 name_len;
	__u8 file_type;
	char name[];
};

static int ifs_ext4_dx_iterate(struct file *file, struct dir_context *ctx);

static bool ifs_ext4_uses_htree(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	if (!ext4_has_feature_dir_index(sb))
		return false;
	if (ext4_test_inode_flag(inode, EXT4_INODE_INDEX))
		return true;
	if ((inode->i_size >> sb->s_blocksize_bits) == 1)
		return true;
	return ext4_has_inline_data(inode);
}

static bool ifs_ext4_pseudo_dirent(const struct ext4_dir_entry_2 *de)
{
	if (de->file_type == EXT4_FT_DIR_CSUM)
		return true;
	if (!de->name_len || de->name_len > 2 || de->name[0] != '.')
		return false;
	return de->name_len == 1 || de->name[1] == '.';
}

int __ext4_check_dir_entry(const char *function, unsigned int line,
			   struct inode *dir, struct file *file,
			   struct ext4_dir_entry_2 *de,
			   struct buffer_head *bh, char *buffer, int size,
			   unsigned int offset)
{
	struct super_block *sb = dir->i_sb;
	bool pseudo = ifs_ext4_pseudo_dirent(de);
	bool hashes = ext4_hash_in_dirent(dir);
	bool checksummed = ext4_has_metadata_csum(sb);
	unsigned int record_length =
		ext4_rec_len_from_disk(de->rec_len, sb->s_blocksize);
	IfsExt4DirectoryRecordStatus status;
	const char *reason;
	unsigned long long physical = bh ?
		(unsigned long long)bh->b_blocknr : 0ULL;

	status = ifs_ext4_validate_directory_record(
		(ifs_ext4_u32)((char *)de - buffer),
		record_length,
		de->name_len,
		le32_to_cpu(de->inode),
		size,
		le32_to_cpu(EXT4_SB(sb)->s_es->s_inodes_count),
		!pseudo && hashes,
		!checksummed && hashes,
		de->name_len == 1 && de->name[0] == '.');
	if (likely(status == IFS_EXT4_DIRECTORY_RECORD_OK))
		return 0;

	reason = ifs_ext4_directory_record_status_string(status);
	if (file) {
		ext4_error_file(
			file, function, line, physical,
			"invalid directory entry: %s, offset=%u inode=%u rec_len=%u size=%d",
			reason, offset, le32_to_cpu(de->inode),
			record_length, size);
	} else {
		ext4_error_inode(
			dir, function, line, physical,
			"invalid directory entry: %s, offset=%u inode=%u rec_len=%u size=%d",
			reason, offset, le32_to_cpu(de->inode),
			record_length, size);
	}
	return 1;
}

static unsigned int ifs_ext4_resync_offset(struct inode *inode,
					    struct buffer_head *bh,
					    unsigned int wanted)
{
	unsigned int cursor = 0;
	unsigned int block_size = inode->i_sb->s_blocksize;

	while (cursor < wanted && cursor < block_size) {
		struct ext4_dir_entry_2 *de =
			(struct ext4_dir_entry_2 *)(bh->b_data + cursor);
		unsigned int length =
			ext4_rec_len_from_disk(de->rec_len, block_size);

		if (length < ext4_dir_rec_len(1, inode) ||
		    length > block_size - cursor)
			break;
		cursor += length;
	}
	return cursor;
}

static int ifs_ext4_emit_disk_name(struct inode *inode,
				    struct dir_context *ctx,
				    struct ext4_dir_entry_2 *de,
				    struct fscrypt_str *scratch)
{
	struct super_block *sb = inode->i_sb;
	u32 inode_number = le32_to_cpu(de->inode);
	unsigned int dtype = get_dtype(sb, de->file_type);

	if (!IS_ENCRYPTED(inode))
		return dir_emit(ctx, de->name, de->name_len,
				inode_number, dtype) ? 1 : 0;

	{
		struct fscrypt_str disk_name =
			FSTR_INIT(de->name, de->name_len);
		unsigned int capacity = scratch->len;
		u32 hash = 0;
		u32 minor = 0;
		int error;

		if (IS_CASEFOLDED(inode)) {
			hash = EXT4_DIRENT_HASH(de);
			minor = EXT4_DIRENT_MINOR_HASH(de);
		}

		error = fscrypt_fname_disk_to_usr(
			inode, hash, minor, &disk_name, scratch);
		if (error)
			return error;

		disk_name = *scratch;
		scratch->len = capacity;
		return dir_emit(ctx, disk_name.name, disk_name.len,
				inode_number, dtype) ? 1 : 0;
	}
}

static int ifs_ext4_linear_iterate(struct file *file,
				    struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct dir_private_info *state = file->private_data;
	struct buffer_head *bh = NULL;
	struct fscrypt_str scratch = FSTR_INIT(NULL, 0);
	int error = 0;

	if (IS_ENCRYPTED(inode)) {
		error = fscrypt_fname_alloc_buffer(EXT4_NAME_LEN, &scratch);
		if (error)
			return error;
	}

	while (ctx->pos < inode->i_size) {
		struct ext4_map_blocks map = { };
		unsigned int offset =
			(unsigned int)(ctx->pos & (sb->s_blocksize - 1));
		int mapped;

		if (fatal_signal_pending(current)) {
			error = -ERESTARTSYS;
			break;
		}
		cond_resched();

		map.m_lblk = ctx->pos >> EXT4_BLOCK_SIZE_BITS(sb);
		map.m_len = 1;
		mapped = ext4_map_blocks(NULL, inode, &map, 0);
		if (mapped < 0) {
			error = mapped;
			break;
		}

		if (!mapped) {
			unsigned int hole_blocks = map.m_len ? map.m_len : 1;
			ctx->pos += (loff_t)hole_blocks * sb->s_blocksize;
			continue;
		}

		{
			pgoff_t page_index =
				map.m_pblk >> (PAGE_SHIFT - inode->i_blkbits);

			if (!ra_has_index(&file->f_ra, page_index))
				page_cache_sync_readahead(
					sb->s_bdev->bd_mapping, &file->f_ra,
					file, page_index, 1);
			file->f_ra.prev_pos =
				(loff_t)page_index << PAGE_SHIFT;
		}

		bh = ext4_bread(NULL, inode, map.m_lblk, 0);
		if (IS_ERR(bh)) {
			error = PTR_ERR(bh);
			bh = NULL;
			break;
		}
		if (!bh) {
			if (ctx->pos > inode->i_blocks << 9)
				break;
			ctx->pos += sb->s_blocksize - offset;
			continue;
		}

		if (!buffer_verified(bh)) {
			if (!ext4_dirblock_csum_verify(inode, bh)) {
				EXT4_ERROR_FILE(
					file, 0,
					"directory checksum failed at offset %llu",
					(unsigned long long)ctx->pos);
				ctx->pos += sb->s_blocksize - offset;
				brelse(bh);
				bh = NULL;
				continue;
			}
			set_buffer_verified(bh);
		}

		if (!inode_eq_iversion(inode, state->cookie)) {
			offset = ifs_ext4_resync_offset(inode, bh, offset);
			ctx->pos =
				(ctx->pos & ~(loff_t)(sb->s_blocksize - 1)) |
				offset;
			state->cookie = inode_query_iversion(inode);
		}

		while (ctx->pos < inode->i_size &&
		       offset < sb->s_blocksize) {
			struct ext4_dir_entry_2 *de =
				(struct ext4_dir_entry_2 *)(bh->b_data + offset);
			unsigned int record_length;
			int emitted;

			if (ext4_check_dir_entry(
				    inode, file, de, bh,
				    bh->b_data, bh->b_size, offset)) {
				ctx->pos =
					(ctx->pos | (sb->s_blocksize - 1)) + 1;
				break;
			}

			record_length = ext4_rec_len_from_disk(
				de->rec_len, sb->s_blocksize);
			if (le32_to_cpu(de->inode)) {
				emitted = ifs_ext4_emit_disk_name(
					inode, ctx, de, &scratch);
				if (emitted < 0) {
					error = emitted;
					goto out;
				}
				if (!emitted)
					goto out;
			}

			offset += record_length;
			ctx->pos += record_length;
		}

		if (ctx->pos < inode->i_size &&
		    !dir_relax_shared(inode))
			break;

		brelse(bh);
		bh = NULL;
	}

out:
	fscrypt_fname_free_buffer(&scratch);
	brelse(bh);
	return error;
}

static int ext4_readdir(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	int error;

	error = fscrypt_prepare_readdir(inode);
	if (error)
		return error;

	if (ifs_ext4_uses_htree(inode)) {
		error = ifs_ext4_dx_iterate(file, ctx);
		if (error != ERR_BAD_DX_DIR)
			return error;

		/*
		 * A checksum-protected indexed directory must not silently change
		 * interpretation.  Older non-checksummed directories may fall back
		 * to their linear representation after a damaged index.
		 */
		if (!ext4_has_metadata_csum(inode->i_sb))
			ext4_clear_inode_flag(inode, EXT4_INODE_INDEX);
	}

	if (ext4_has_inline_data(inode)) {
		int remains_inline = 1;

		error = ext4_read_inline_dir(
			file, ctx, &remains_inline);
		if (remains_inline)
			return error;
	}

	return ifs_ext4_linear_iterate(file, ctx);
}

static bool ifs_ext4_hash_positions_are_32_bit(struct file *file)
{
#ifdef CONFIG_COMPAT
	if (in_compat_syscall())
		return true;
#endif
	if (file->f_mode & FMODE_32BITHASH)
		return true;
	if (file->f_mode & FMODE_64BITHASH)
		return false;
	return BITS_PER_LONG == 32;
}

static loff_t ifs_ext4_hash_position(struct file *file,
				     u32 major, u32 minor)
{
	if (ifs_ext4_hash_positions_are_32_bit(file))
		return major >> 1;
	return ((u64)(major >> 1) << 32) | minor;
}

static u32 ifs_ext4_position_major(struct file *file, loff_t position)
{
	if (ifs_ext4_hash_positions_are_32_bit(file))
		return (u32)position << 1;
	return (u32)(position >> 32) << 1;
}

static u32 ifs_ext4_position_minor(struct file *file, loff_t position)
{
	return ifs_ext4_hash_positions_are_32_bit(file) ?
		0 : (u32)position;
}

static loff_t ifs_ext4_hash_eof(struct file *file)
{
	return ifs_ext4_hash_positions_are_32_bit(file) ?
		EXT4_HTREE_EOF_32BIT : EXT4_HTREE_EOF_64BIT;
}

static loff_t ext4_dir_llseek(struct file *file,
			      loff_t offset, int whence)
{
	struct inode *inode = file_inode(file);
	struct dir_private_info *state = file->private_data;
	loff_t result;

	if (ifs_ext4_uses_htree(inode)) {
		loff_t end = ifs_ext4_hash_eof(file);

		result = generic_file_llseek_size(
			file, offset, whence, end, end);
	} else {
		result = ext4_llseek(file, offset, whence);
	}

	if (state)
		state->cookie = inode_peek_iversion(inode) - 1;
	return result;
}

static void ifs_ext4_free_name_tree(struct rb_root *root)
{
	struct ifs_ext4_dir_name *entry;
	struct ifs_ext4_dir_name *next;

	rbtree_postorder_for_each_entry_safe(entry, next, root, node) {
		struct ifs_ext4_dir_name *same = entry;

		while (same) {
			struct ifs_ext4_dir_name *old = same;
			same = same->same_hash;
			kfree(old);
		}
	}
	*root = RB_ROOT;
}

static struct ifs_ext4_dir_name *
ifs_ext4_name_from_node(struct rb_node *node)
{
	return rb_entry(node, struct ifs_ext4_dir_name, node);
}

static void ifs_ext4_prepare_hash_cursor(struct file *file, loff_t position)
{
	struct dir_private_info *state = file->private_data;

	if (state->initialized)
		return;
	if (!ifs_ext4_uses_htree(file_inode(file)))
		return;

	state->curr_hash = ifs_ext4_position_major(file, position);
	state->curr_minor_hash = ifs_ext4_position_minor(file, position);
	state->initialized = true;
}

void ext4_htree_free_dir_info(struct dir_private_info *state)
{
	if (!state)
		return;
	ifs_ext4_free_name_tree(&state->root);
	kfree(state);
}

int ext4_htree_store_dirent(struct file *file, __u32 hash,
			    __u32 minor_hash,
			    struct ext4_dir_entry_2 *de,
			    struct fscrypt_str *display_name)
{
	struct dir_private_info *state = file->private_data;
	struct rb_node **link = &state->root.rb_node;
	struct rb_node *parent = NULL;
	struct ifs_ext4_dir_name *entry;
	struct ifs_ext4_dir_name *created;
	size_t allocation;

	allocation = sizeof(*created) + display_name->len + 1;
	created = kzalloc(allocation, GFP_KERNEL);
	if (!created)
		return -ENOMEM;

	created->hash = hash;
	created->minor_hash = minor_hash;
	created->inode = le32_to_cpu(de->inode);
	created->name_len = display_name->len;
	created->file_type = de->file_type;
	memcpy(created->name, display_name->name, display_name->len);

	while (*link) {
		parent = *link;
		entry = ifs_ext4_name_from_node(parent);

		if (hash == entry->hash &&
		    minor_hash == entry->minor_hash) {
			created->same_hash = entry->same_hash;
			entry->same_hash = created;
			return 0;
		}

		if (hash < entry->hash ||
		    (hash == entry->hash &&
		     minor_hash < entry->minor_hash))
			link = &parent->rb_left;
		else
			link = &parent->rb_right;
	}

	rb_link_node(&created->node, parent, link);
	rb_insert_color(&created->node, &state->root);
	return 0;
}

static int ifs_ext4_emit_hash_chain(struct file *file,
				    struct dir_context *ctx,
				    struct ifs_ext4_dir_name *entry)
{
	struct dir_private_info *state = file->private_data;
	struct super_block *sb = file_inode(file)->i_sb;

	if (!entry)
		return 0;

	ctx->pos = ifs_ext4_hash_position(
		file, entry->hash, entry->minor_hash);
	for (; entry; entry = entry->same_hash) {
		if (!dir_emit(ctx, entry->name, entry->name_len,
			      entry->inode,
			      get_dtype(sb, entry->file_type))) {
			state->extra_fname =
				(struct fname *)entry;
			return 1;
		}
	}
	return 0;
}

static int ifs_ext4_dx_iterate(struct file *file,
				struct dir_context *ctx)
{
	struct dir_private_info *state = file->private_data;
	struct inode *inode = file_inode(file);
	int result = 0;

	ifs_ext4_prepare_hash_cursor(file, ctx->pos);
	if (ctx->pos == ifs_ext4_hash_eof(file))
		return 0;

	if (state->last_pos != ctx->pos) {
		ifs_ext4_free_name_tree(&state->root);
		state->curr_node = NULL;
		state->extra_fname = NULL;
		state->curr_hash =
			ifs_ext4_position_major(file, ctx->pos);
		state->curr_minor_hash =
			ifs_ext4_position_minor(file, ctx->pos);
	}

	if (state->extra_fname) {
		if (ifs_ext4_emit_hash_chain(
			    file, ctx,
			    (struct ifs_ext4_dir_name *)state->extra_fname))
			goto done;
		state->extra_fname = NULL;
		goto advance;
	}

	if (!state->curr_node)
		state->curr_node = rb_first(&state->root);

	for (;;) {
		struct ifs_ext4_dir_name *entry;

		if (!state->curr_node ||
		    !inode_eq_iversion(inode, state->cookie)) {
			ifs_ext4_free_name_tree(&state->root);
			state->curr_node = NULL;
			state->cookie = inode_query_iversion(inode);

			result = ext4_htree_fill_tree(
				file, state->curr_hash,
				state->curr_minor_hash,
				&state->next_hash);
			if (result < 0)
				break;
			if (!result) {
				ctx->pos = ifs_ext4_hash_eof(file);
				result = 0;
				break;
			}
			state->curr_node = rb_first(&state->root);
		}

		entry = ifs_ext4_name_from_node(state->curr_node);
		state->curr_hash = entry->hash;
		state->curr_minor_hash = entry->minor_hash;
		if (ifs_ext4_emit_hash_chain(file, ctx, entry))
			break;

advance:
		state->curr_node = rb_next(state->curr_node);
		if (state->curr_node) {
			entry = ifs_ext4_name_from_node(state->curr_node);
			state->curr_hash = entry->hash;
			state->curr_minor_hash = entry->minor_hash;
			continue;
		}

		if (state->next_hash == ~0U) {
			ctx->pos = ifs_ext4_hash_eof(file);
			break;
		}
		state->curr_hash = state->next_hash;
		state->curr_minor_hash = 0;
	}

done:
	state->last_pos = ctx->pos;
	return result < 0 ? result : 0;
}

static int ext4_release_dir(struct inode *inode, struct file *file)
{
	(void)inode;
	ext4_htree_free_dir_info(file->private_data);
	file->private_data = NULL;
	return 0;
}

int ext4_check_all_de(struct inode *dir, struct buffer_head *bh,
		      void *buffer, int size)
{
	unsigned int offset = 0;

	while (offset < size) {
		struct ext4_dir_entry_2 *de =
			(struct ext4_dir_entry_2 *)((u8 *)buffer + offset);
		unsigned int length;

		if (ext4_check_dir_entry(
			    dir, NULL, de, bh, buffer, size, offset))
			return -EUCLEAN;

		length = ext4_rec_len_from_disk(de->rec_len, size);
		if (!length || length > size - offset)
			return -EUCLEAN;
		offset += length;
	}

	return offset == size ? 0 : -EUCLEAN;
}

static int ext4_dir_open(struct inode *inode, struct file *file)
{
	struct dir_private_info *state;

	(void)inode;
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	if (!state)
		return -ENOMEM;

	state->root = RB_ROOT;
	file->private_data = state;
	return 0;
}

const struct file_operations ext4_dir_operations = {
	.open = ext4_dir_open,
	.llseek = ext4_dir_llseek,
	.read = generic_read_dir,
	.iterate_shared = ext4_readdir,
	.unlocked_ioctl = ext4_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl = ext4_compat_ioctl,
#endif
	.fsync = ext4_sync_file,
	.release = ext4_release_dir,
};

static int ifs_ext4_hash_plain_name(const char *name, int length,
				     struct dx_hash_info *info)
{
	ifs_ext4_u32 major;
	ifs_ext4_u32 minor;
	int result;

	if (length < 0)
		return -EINVAL;

	result = ifs_ext4_directory_hash(
		(const unsigned char *)name,
		(ifs_ext4_u32)length,
		(ifs_ext4_u32)info->hash_version,
		(const ifs_ext4_u32 *)info->seed,
		&major, &minor);
	if (result)
		return -EINVAL;

	info->hash = major;
	info->minor_hash = minor;
	return 0;
}

static int ifs_ext4_hash_name(const struct inode *dir,
			      const char *name, int length,
			      struct dx_hash_info *info)
{
	if (info->hash_version == DX_HASH_SIPHASH) {
		struct qstr qname = QSTR_INIT(name, length);
		u64 combined;

		if (!fscrypt_has_encryption_key(dir)) {
			ext4_warning_inode(
				dir, "SipHash directory requires encryption key");
			return -ENOKEY;
		}

		combined = fscrypt_fname_siphash(dir, &qname);
		info->hash = (u32)(combined >> 32) & ~1U;
		info->minor_hash = (u32)combined;
		return 0;
	}

	return ifs_ext4_hash_plain_name(name, length, info);
}

int ext4fs_dirhash(const struct inode *dir, const char *name, int length,
		   struct dx_hash_info *info)
{
#if IS_ENABLED(CONFIG_UNICODE)
	const struct unicode_map *map = dir->i_sb->s_encoding;

	if (length && IS_CASEFOLDED(dir) &&
	    (!IS_ENCRYPTED(dir) ||
	     fscrypt_has_encryption_key(dir))) {
		struct qstr source = {
			.name = name,
			.len = length,
		};
		unsigned char *folded;
		int folded_length;
		int result;

		folded = kzalloc(PATH_MAX, GFP_KERNEL);
		if (!folded)
			return -ENOMEM;

		folded_length = utf8_casefold(
			map, &source, folded, PATH_MAX);
		if (folded_length >= 0) {
			result = ifs_ext4_hash_name(
				dir, (const char *)folded,
				folded_length, info);
			kfree(folded);
			return result;
		}
		kfree(folded);
	}
#endif
	return ifs_ext4_hash_name(dir, name, length, info);
}
