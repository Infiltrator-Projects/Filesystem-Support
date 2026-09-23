// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/fs/ext4/dir.c
 *
 * Copyright (C) 1992, 1993, 1994, 1995
 * Remy Card (card@masi.ibp.fr)
 * Laboratoire MASI - Institut Blaise Pascal
 * Universite Pierre et Marie Curie (Paris VI)
 *
 *  from
 *
 *  linux/fs/minix/dir.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 *
 *  ext4 directory handling functions
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *
 * Hash Tree Directory indexing (c) 2001  Daniel Phillips
 *
 */

/*
 * EXT4 — Directory representation
 *
 * Purpose:
 *   Parses, validates and iterates directory records and implements directory lookup-side mechanics, including indexed-directory hashing where applicable.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Directory record lengths, alignment and bounds are untrusted on-disk input and must be validated before pointer arithmetic or publication to VFS.
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
#include <linux/buffer_head.h>
#include <linux/slab.h>
#include <linux/iversion.h>
#include <linux/unicode.h>
#include "ext4.h"
#include "xattr.h"

static int ext4_dx_readdir(struct file *, struct dir_context *);


/**
 * is_dx_dir - Implements the is dx dir operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int is_dx_dir(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	if (ext4_has_feature_dir_index(inode->i_sb) &&
	    ((ext4_test_inode_flag(inode, EXT4_INODE_INDEX)) ||
	     ((inode->i_size >> sb->s_blocksize_bits) == 1) ||
	     ext4_has_inline_data(inode)))
		return 1;

	return 0;
}


/**
 * is_fake_dir_entry - Implements the is fake dir entry operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool is_fake_dir_entry(struct ext4_dir_entry_2 *de)
{

	if ((de->name_len > 0) && (de->name_len <= 2) && (de->name[0] == '.') &&
	    (de->name[1] == '.' || de->name[1] == '\0'))
		return true;

	if (de->file_type == EXT4_FT_DIR_CSUM)
		return true;
	return false;
}


/**
 * __ext4_check_dir_entry - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int __ext4_check_dir_entry(const char *function, unsigned int line,
			   struct inode *dir, struct file *filp,
			   struct ext4_dir_entry_2 *de,
			   struct buffer_head *bh, char *buf, int size,
			   unsigned int offset)
{
	const int rlen = ext4_rec_len_from_disk(de->rec_len,
						dir->i_sb->s_blocksize);
	const bool fake = is_fake_dir_entry(de);
	const bool has_csum = ext4_has_metadata_csum(dir->i_sb);
	const bool has_hash = ext4_hash_in_dirent(dir);
	const IfsExt4DirectoryRecordStatus record_status =
		ifs_ext4_validate_directory_record(
			(ifs_ext4_u32)((char *)de - buf),
			(ifs_ext4_u32)rlen,
			de->name_len,
			le32_to_cpu(de->inode),
			(ifs_ext4_u32)size,
			le32_to_cpu(EXT4_SB(dir->i_sb)->s_es->s_inodes_count),
			!fake && has_hash,
			!has_csum && has_hash,
			de->name_len == 1 && de->name[0] == '.');
	const char *error_msg;

	if (likely(record_status == IFS_EXT4_DIRECTORY_RECORD_OK))
		return 0;
	error_msg = ifs_ext4_directory_record_status_string(record_status);

	if (filp)
		ext4_error_file(filp, function, line, bh->b_blocknr,
				"bad entry in directory: %s - offset=%u, "
				"inode=%u, rec_len=%d, size=%d fake=%d",
				error_msg, offset, le32_to_cpu(de->inode),
				rlen, size, fake);
	else
		ext4_error_inode(dir, function, line, bh->b_blocknr,
				"bad entry in directory: %s - offset=%u, "
				"inode=%u, rec_len=%d, size=%d fake=%d",
				 error_msg, offset, le32_to_cpu(de->inode),
				 rlen, size, fake);

	return 1;
}


/**
 * ext4_readdir - Implements the readdir operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_readdir(struct file *file, struct dir_context *ctx)
{
	unsigned int offset;
	int i;
	struct ext4_dir_entry_2 *de;
	int err;
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct buffer_head *bh = NULL;
	struct fscrypt_str fstr = FSTR_INIT(NULL, 0);
	struct dir_private_info *info = file->private_data;

	err = fscrypt_prepare_readdir(inode);
	if (err)
		return err;

	if (is_dx_dir(inode)) {
		err = ext4_dx_readdir(file, ctx);
		if (err != ERR_BAD_DX_DIR)
			return err;


		if (!ext4_has_metadata_csum(sb)) {


			ext4_clear_inode_flag(inode, EXT4_INODE_INDEX);
		}
	}

	if (ext4_has_inline_data(inode)) {
		int has_inline_data = 1;
		err = ext4_read_inline_dir(file, ctx,
					   &has_inline_data);
		if (has_inline_data)
			return err;
	}

	if (IS_ENCRYPTED(inode)) {
		err = fscrypt_fname_alloc_buffer(EXT4_NAME_LEN, &fstr);
		if (err < 0)
			return err;
	}

	while (ctx->pos < inode->i_size) {
		struct ext4_map_blocks map;

		if (fatal_signal_pending(current)) {
			err = -ERESTARTSYS;
			goto errout;
		}
		cond_resched();
		offset = ctx->pos & (sb->s_blocksize - 1);
		map.m_lblk = ctx->pos >> EXT4_BLOCK_SIZE_BITS(sb);
		map.m_len = 1;
		err = ext4_map_blocks(NULL, inode, &map, 0);
		if (err == 0) {


			if (map.m_len == 0)
				map.m_len = 1;
			ctx->pos += map.m_len * sb->s_blocksize;
			continue;
		}
		if (err > 0) {
			pgoff_t index = map.m_pblk >>
					(PAGE_SHIFT - inode->i_blkbits);
			if (!ra_has_index(&file->f_ra, index))
				page_cache_sync_readahead(
					sb->s_bdev->bd_mapping,
					&file->f_ra, file,
					index, 1);
			file->f_ra.prev_pos = (loff_t)index << PAGE_SHIFT;
			bh = ext4_bread(NULL, inode, map.m_lblk, 0);
			if (IS_ERR(bh)) {
				err = PTR_ERR(bh);
				bh = NULL;
				goto errout;
			}
		}

		if (!bh) {

			if (ctx->pos > inode->i_blocks << 9)
				break;
			ctx->pos += sb->s_blocksize - offset;
			continue;
		}


		if (!buffer_verified(bh) &&
		    !ext4_dirblock_csum_verify(inode, bh)) {
			EXT4_ERROR_FILE(file, 0, "directory fails checksum "
					"at offset %llu",
					(unsigned long long)ctx->pos);
			ctx->pos += sb->s_blocksize - offset;
			brelse(bh);
			bh = NULL;
			continue;
		}
		set_buffer_verified(bh);


		if (!inode_eq_iversion(inode, info->cookie)) {
			for (i = 0; i < sb->s_blocksize && i < offset; ) {
				de = (struct ext4_dir_entry_2 *)
					(bh->b_data + i);


				if (ext4_rec_len_from_disk(de->rec_len,
					sb->s_blocksize) < ext4_dir_rec_len(1,
									inode))
					break;
				i += ext4_rec_len_from_disk(de->rec_len,
							    sb->s_blocksize);
			}
			offset = i;
			ctx->pos = (ctx->pos & ~(sb->s_blocksize - 1))
				| offset;
			info->cookie = inode_query_iversion(inode);
		}

		while (ctx->pos < inode->i_size
		       && offset < sb->s_blocksize) {
			de = (struct ext4_dir_entry_2 *) (bh->b_data + offset);
			if (ext4_check_dir_entry(inode, file, de, bh,
						 bh->b_data, bh->b_size,
						 offset)) {


				ctx->pos = (ctx->pos |
						(sb->s_blocksize - 1)) + 1;
				break;
			}
			offset += ext4_rec_len_from_disk(de->rec_len,
					sb->s_blocksize);
			if (le32_to_cpu(de->inode)) {
				if (!IS_ENCRYPTED(inode)) {
					if (!dir_emit(ctx, de->name,
					    de->name_len,
					    le32_to_cpu(de->inode),
					    get_dtype(sb, de->file_type)))
						goto done;
				} else {
					int save_len = fstr.len;
					struct fscrypt_str de_name =
							FSTR_INIT(de->name,
								de->name_len);
					u32 hash;
					u32 minor_hash;

					if (IS_CASEFOLDED(inode)) {
						hash = EXT4_DIRENT_HASH(de);
						minor_hash = EXT4_DIRENT_MINOR_HASH(de);
					} else {
						hash = 0;
						minor_hash = 0;
					}


					err = fscrypt_fname_disk_to_usr(inode,
						hash, minor_hash, &de_name, &fstr);
					de_name = fstr;
					fstr.len = save_len;
					if (err)
						goto errout;
					if (!dir_emit(ctx,
					    de_name.name, de_name.len,
					    le32_to_cpu(de->inode),
					    get_dtype(sb, de->file_type)))
						goto done;
				}
			}
			ctx->pos += ext4_rec_len_from_disk(de->rec_len,
						sb->s_blocksize);
		}
		if ((ctx->pos < inode->i_size) && !dir_relax_shared(inode))
			goto done;
		brelse(bh);
		bh = NULL;
	}
done:
	err = 0;
errout:
	fscrypt_fname_free_buffer(&fstr);
	brelse(bh);
	return err;
}


/**
 * is_32bit_api - Implements the is 32bit api operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline int is_32bit_api(void)
{
#ifdef CONFIG_COMPAT
	return in_compat_syscall();
#else
	return (BITS_PER_LONG == 32);
#endif
}


/**
 * hash2pos - Implements the hash2pos operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline loff_t hash2pos(struct file *filp, __u32 major, __u32 minor)
{
	if ((filp->f_mode & FMODE_32BITHASH) ||
	    (!(filp->f_mode & FMODE_64BITHASH) && is_32bit_api()))
		return major >> 1;
	else
		return ((__u64)(major >> 1) << 32) | (__u64)minor;
}


/**
 * pos2maj_hash - Implements the pos2maj hash operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline __u32 pos2maj_hash(struct file *filp, loff_t pos)
{
	if ((filp->f_mode & FMODE_32BITHASH) ||
	    (!(filp->f_mode & FMODE_64BITHASH) && is_32bit_api()))
		return (pos << 1) & 0xffffffff;
	else
		return ((pos >> 32) << 1) & 0xffffffff;
}


/**
 * pos2min_hash - Implements the pos2min hash operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline __u32 pos2min_hash(struct file *filp, loff_t pos)
{
	if ((filp->f_mode & FMODE_32BITHASH) ||
	    (!(filp->f_mode & FMODE_64BITHASH) && is_32bit_api()))
		return 0;
	else
		return pos & 0xffffffff;
}


/**
 * ext4_get_htree_eof - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline loff_t ext4_get_htree_eof(struct file *filp)
{
	if ((filp->f_mode & FMODE_32BITHASH) ||
	    (!(filp->f_mode & FMODE_64BITHASH) && is_32bit_api()))
		return EXT4_HTREE_EOF_32BIT;
	else
		return EXT4_HTREE_EOF_64BIT;
}


/**
 * ext4_dir_llseek - Implements the dir llseek operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static loff_t ext4_dir_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	struct dir_private_info *info = file->private_data;
	int dx_dir = is_dx_dir(inode);
	loff_t ret, htree_max = ext4_get_htree_eof(file);

	if (likely(dx_dir))
		ret = generic_file_llseek_size(file, offset, whence,
						    htree_max, htree_max);
	else
		ret = ext4_llseek(file, offset, whence);
	info->cookie = inode_peek_iversion(inode) - 1;
	return ret;
}


/**
 * struct fname - Private EXT4 state/data structure used by directory representation.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct fname {
	__u32		hash;
	__u32		minor_hash;
	struct rb_node	rb_hash;
	struct fname	*next;
	__u32		inode;
	__u8		name_len;
	__u8		file_type;
	char		name[];
};


/**
 * free_rb_tree_fname - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void free_rb_tree_fname(struct rb_root *root)
{
	struct fname *fname, *next;

	rbtree_postorder_for_each_entry_safe(fname, next, root, rb_hash)
		while (fname) {
			struct fname *old = fname;
			fname = fname->next;
			kfree(old);
		}

	*root = RB_ROOT;
}


/**
 * ext4_htree_init_dir_info - Initialises subsystem state and establishes the resources required by later operations.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_htree_init_dir_info(struct file *filp, loff_t pos)
{
	struct dir_private_info *p = filp->private_data;

	if (is_dx_dir(file_inode(filp)) && !p->initialized) {
		p->curr_hash = pos2maj_hash(filp, pos);
		p->curr_minor_hash = pos2min_hash(filp, pos);
		p->initialized = true;
	}
}


/**
 * ext4_htree_free_dir_info - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_htree_free_dir_info(struct dir_private_info *p)
{
	free_rb_tree_fname(&p->root);
	kfree(p);
}


/**
 * ext4_htree_store_dirent - Implements the htree store dirent operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_htree_store_dirent(struct file *dir_file, __u32 hash,
			     __u32 minor_hash,
			    struct ext4_dir_entry_2 *dirent,
			    struct fscrypt_str *ent_name)
{
	struct rb_node **p, *parent = NULL;
	struct fname *fname, *new_fn;
	struct dir_private_info *info;
	int len;

	info = dir_file->private_data;
	p = &info->root.rb_node;


	len = sizeof(struct fname) + ent_name->len + 1;
	new_fn = kzalloc(len, GFP_KERNEL);
	if (!new_fn)
		return -ENOMEM;
	new_fn->hash = hash;
	new_fn->minor_hash = minor_hash;
	new_fn->inode = le32_to_cpu(dirent->inode);
	new_fn->name_len = ent_name->len;
	new_fn->file_type = dirent->file_type;
	memcpy(new_fn->name, ent_name->name, ent_name->len);

	while (*p) {
		parent = *p;
		fname = rb_entry(parent, struct fname, rb_hash);


		if ((new_fn->hash == fname->hash) &&
		    (new_fn->minor_hash == fname->minor_hash)) {
			new_fn->next = fname->next;
			fname->next = new_fn;
			return 0;
		}

		if (new_fn->hash < fname->hash)
			p = &(*p)->rb_left;
		else if (new_fn->hash > fname->hash)
			p = &(*p)->rb_right;
		else if (new_fn->minor_hash < fname->minor_hash)
			p = &(*p)->rb_left;
		else
			p = &(*p)->rb_right;
	}

	rb_link_node(&new_fn->rb_hash, parent, p);
	rb_insert_color(&new_fn->rb_hash, &info->root);
	return 0;
}


/**
 * call_filldir - Implements the call filldir operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int call_filldir(struct file *file, struct dir_context *ctx,
			struct fname *fname)
{
	struct dir_private_info *info = file->private_data;
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;

	if (!fname) {
		ext4_msg(sb, KERN_ERR, "%s:%d: inode #%lu: comm %s: "
			 "called with null fname?!?", __func__, __LINE__,
			 inode->i_ino, current->comm);
		return 0;
	}
	ctx->pos = hash2pos(file, fname->hash, fname->minor_hash);
	while (fname) {
		if (!dir_emit(ctx, fname->name,
				fname->name_len,
				fname->inode,
				get_dtype(sb, fname->file_type))) {
			info->extra_fname = fname;
			return 1;
		}
		fname = fname->next;
	}
	return 0;
}


/**
 * ext4_dx_readdir - Implements the dx readdir operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_dx_readdir(struct file *file, struct dir_context *ctx)
{
	struct dir_private_info *info = file->private_data;
	struct inode *inode = file_inode(file);
	struct fname *fname;
	int ret = 0;

	ext4_htree_init_dir_info(file, ctx->pos);

	if (ctx->pos == ext4_get_htree_eof(file))
		return 0;


	if (info->last_pos != ctx->pos) {
		free_rb_tree_fname(&info->root);
		info->curr_node = NULL;
		info->extra_fname = NULL;
		info->curr_hash = pos2maj_hash(file, ctx->pos);
		info->curr_minor_hash = pos2min_hash(file, ctx->pos);
	}


	if (info->extra_fname) {
		if (call_filldir(file, ctx, info->extra_fname))
			goto finished;
		info->extra_fname = NULL;
		goto next_node;
	} else if (!info->curr_node)
		info->curr_node = rb_first(&info->root);

	while (1) {


		if ((!info->curr_node) ||
		    !inode_eq_iversion(inode, info->cookie)) {
			info->curr_node = NULL;
			free_rb_tree_fname(&info->root);
			info->cookie = inode_query_iversion(inode);
			ret = ext4_htree_fill_tree(file, info->curr_hash,
						   info->curr_minor_hash,
						   &info->next_hash);
			if (ret < 0)
				goto finished;
			if (ret == 0) {
				ctx->pos = ext4_get_htree_eof(file);
				break;
			}
			info->curr_node = rb_first(&info->root);
		}

		fname = rb_entry(info->curr_node, struct fname, rb_hash);
		info->curr_hash = fname->hash;
		info->curr_minor_hash = fname->minor_hash;
		if (call_filldir(file, ctx, fname))
			break;
	next_node:
		info->curr_node = rb_next(info->curr_node);
		if (info->curr_node) {
			fname = rb_entry(info->curr_node, struct fname,
					 rb_hash);
			info->curr_hash = fname->hash;
			info->curr_minor_hash = fname->minor_hash;
		} else {
			if (info->next_hash == ~0) {
				ctx->pos = ext4_get_htree_eof(file);
				break;
			}
			info->curr_hash = info->next_hash;
			info->curr_minor_hash = 0;
		}
	}
finished:
	info->last_pos = ctx->pos;
	return ret < 0 ? ret : 0;
}


/**
 * ext4_release_dir - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_release_dir(struct inode *inode, struct file *filp)
{
	if (filp->private_data)
		ext4_htree_free_dir_info(filp->private_data);

	return 0;
}


/**
 * ext4_check_all_de - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_check_all_de(struct inode *dir, struct buffer_head *bh, void *buf,
		      int buf_size)
{
	struct ext4_dir_entry_2 *de;
	int rlen;
	unsigned int offset = 0;
	char *top;

	de = buf;
	top = buf + buf_size;
	while ((char *) de < top) {
		if (ext4_check_dir_entry(dir, NULL, de, bh,
					 buf, buf_size, offset))
			return -EFSCORRUPTED;
		rlen = ext4_rec_len_from_disk(de->rec_len, buf_size);
		de = (struct ext4_dir_entry_2 *)((char *)de + rlen);
		offset += rlen;
	}
	if ((char *) de > top)
		return -EFSCORRUPTED;

	return 0;
}


/**
 * ext4_dir_open - Implements the dir open operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_dir_open(struct inode *inode, struct file *file)
{
	struct dir_private_info *info;

	info = kzalloc(sizeof(*info), GFP_KERNEL);
	if (!info)
		return -ENOMEM;
	file->private_data = info;
	return 0;
}

const struct file_operations ext4_dir_operations = {
	.open		= ext4_dir_open,
	.llseek		= ext4_dir_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= ext4_readdir,
	.unlocked_ioctl = ext4_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ext4_compat_ioctl,
#endif
	.fsync		= ext4_sync_file,
	.release	= ext4_release_dir,
};


#define DELTA 0x9E3779B9


/**
 * TEA_transform - Implements the TEA transform operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void TEA_transform(__u32 buf[4], __u32 const in[])
{
	__u32	sum = 0;
	__u32	b0 = buf[0], b1 = buf[1];
	__u32	a = in[0], b = in[1], c = in[2], d = in[3];
	int	n = 16;

	do {
		sum += DELTA;
		b0 += ((b1 << 4)+a) ^ (b1+sum) ^ ((b1 >> 5)+b);
		b1 += ((b0 << 4)+c) ^ (b0+sum) ^ ((b0 >> 5)+d);
	} while (--n);

	buf[0] += b0;
	buf[1] += b1;
}


#define F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define H(x, y, z) ((x) ^ (y) ^ (z))


#define ROUND(f, a, b, c, d, x, s)	\
	(a += f(b, c, d) + x, a = rol32(a, s))
#define K1 0
#define K2 013240474631UL
#define K3 015666365641UL


/**
 * half_md4_transform - Implements the half md4 transform operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static __u32 half_md4_transform(__u32 buf[4], __u32 const in[8])
{
	__u32 a = buf[0], b = buf[1], c = buf[2], d = buf[3];


	ROUND(F, a, b, c, d, in[0] + K1,  3);
	ROUND(F, d, a, b, c, in[1] + K1,  7);
	ROUND(F, c, d, a, b, in[2] + K1, 11);
	ROUND(F, b, c, d, a, in[3] + K1, 19);
	ROUND(F, a, b, c, d, in[4] + K1,  3);
	ROUND(F, d, a, b, c, in[5] + K1,  7);
	ROUND(F, c, d, a, b, in[6] + K1, 11);
	ROUND(F, b, c, d, a, in[7] + K1, 19);


	ROUND(G, a, b, c, d, in[1] + K2,  3);
	ROUND(G, d, a, b, c, in[3] + K2,  5);
	ROUND(G, c, d, a, b, in[5] + K2,  9);
	ROUND(G, b, c, d, a, in[7] + K2, 13);
	ROUND(G, a, b, c, d, in[0] + K2,  3);
	ROUND(G, d, a, b, c, in[2] + K2,  5);
	ROUND(G, c, d, a, b, in[4] + K2,  9);
	ROUND(G, b, c, d, a, in[6] + K2, 13);


	ROUND(H, a, b, c, d, in[3] + K3,  3);
	ROUND(H, d, a, b, c, in[7] + K3,  9);
	ROUND(H, c, d, a, b, in[2] + K3, 11);
	ROUND(H, b, c, d, a, in[6] + K3, 15);
	ROUND(H, a, b, c, d, in[1] + K3,  3);
	ROUND(H, d, a, b, c, in[5] + K3,  9);
	ROUND(H, c, d, a, b, in[0] + K3, 11);
	ROUND(H, b, c, d, a, in[4] + K3, 15);

	buf[0] += a;
	buf[1] += b;
	buf[2] += c;
	buf[3] += d;

	return buf[1];
}
#undef ROUND
#undef K1
#undef K2
#undef K3
#undef F
#undef G
#undef H


/**
 * dx_hack_hash_unsigned - Implements the dx hack hash unsigned operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static __u32 dx_hack_hash_unsigned(const char *name, int len)
{
	__u32 hash, hash0 = 0x12a3fe2d, hash1 = 0x37abe8f9;
	const unsigned char *ucp = (const unsigned char *) name;

	while (len--) {
		hash = hash1 + (hash0 ^ (((int) *ucp++) * 7152373));

		if (hash & 0x80000000)
			hash -= 0x7fffffff;
		hash1 = hash0;
		hash0 = hash;
	}
	return hash0 << 1;
}


/**
 * dx_hack_hash_signed - Implements the dx hack hash signed operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static __u32 dx_hack_hash_signed(const char *name, int len)
{
	__u32 hash, hash0 = 0x12a3fe2d, hash1 = 0x37abe8f9;
	const signed char *scp = (const signed char *) name;

	while (len--) {
		hash = hash1 + (hash0 ^ (((int) *scp++) * 7152373));

		if (hash & 0x80000000)
			hash -= 0x7fffffff;
		hash1 = hash0;
		hash0 = hash;
	}
	return hash0 << 1;
}


/**
 * str2hashbuf_signed - Implements the str2hashbuf signed operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void str2hashbuf_signed(const char *msg, int len, __u32 *buf, int num)
{
	__u32	pad, val;
	int	i;
	const signed char *scp = (const signed char *) msg;

	pad = (__u32)len | ((__u32)len << 8);
	pad |= pad << 16;

	val = pad;
	if (len > num*4)
		len = num * 4;
	for (i = 0; i < len; i++) {
		val = ((int) scp[i]) + (val << 8);
		if ((i % 4) == 3) {
			*buf++ = val;
			val = pad;
			num--;
		}
	}
	if (--num >= 0)
		*buf++ = val;
	while (--num >= 0)
		*buf++ = pad;
}


/**
 * str2hashbuf_unsigned - Implements the str2hashbuf unsigned operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void str2hashbuf_unsigned(const char *msg, int len, __u32 *buf, int num)
{
	__u32	pad, val;
	int	i;
	const unsigned char *ucp = (const unsigned char *) msg;

	pad = (__u32)len | ((__u32)len << 8);
	pad |= pad << 16;

	val = pad;
	if (len > num*4)
		len = num * 4;
	for (i = 0; i < len; i++) {
		val = ((int) ucp[i]) + (val << 8);
		if ((i % 4) == 3) {
			*buf++ = val;
			val = pad;
			num--;
		}
	}
	if (--num >= 0)
		*buf++ = val;
	while (--num >= 0)
		*buf++ = pad;
}


/**
 * __ext4fs_dirhash - Implements the ext4fs dirhash operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int __ext4fs_dirhash(const struct inode *dir, const char *name, int len,
			    struct dx_hash_info *hinfo)
{
	__u32	hash;
	__u32	minor_hash = 0;
	const char	*p;
	int		i;
	__u32		in[8], buf[4];
	void		(*str2hashbuf)(const char *, int, __u32 *, int) =
				str2hashbuf_signed;


	buf[0] = 0x67452301;
	buf[1] = 0xefcdab89;
	buf[2] = 0x98badcfe;
	buf[3] = 0x10325476;


	if (hinfo->seed) {
		for (i = 0; i < 4; i++) {
			if (hinfo->seed[i]) {
				memcpy(buf, hinfo->seed, sizeof(buf));
				break;
			}
		}
	}

	switch (hinfo->hash_version) {
	case DX_HASH_LEGACY_UNSIGNED:
		hash = dx_hack_hash_unsigned(name, len);
		break;
	case DX_HASH_LEGACY:
		hash = dx_hack_hash_signed(name, len);
		break;
	case DX_HASH_HALF_MD4_UNSIGNED:
		str2hashbuf = str2hashbuf_unsigned;
		fallthrough;
	case DX_HASH_HALF_MD4:
		p = name;
		while (len > 0) {
			(*str2hashbuf)(p, len, in, 8);
			half_md4_transform(buf, in);
			len -= 32;
			p += 32;
		}
		minor_hash = buf[2];
		hash = buf[1];
		break;
	case DX_HASH_TEA_UNSIGNED:
		str2hashbuf = str2hashbuf_unsigned;
		fallthrough;
	case DX_HASH_TEA:
		p = name;
		while (len > 0) {
			(*str2hashbuf)(p, len, in, 4);
			TEA_transform(buf, in);
			len -= 16;
			p += 16;
		}
		hash = buf[0];
		minor_hash = buf[1];
		break;
	case DX_HASH_SIPHASH:
	{
		struct qstr qname = QSTR_INIT(name, len);
		__u64	combined_hash;

		if (fscrypt_has_encryption_key(dir)) {
			combined_hash = fscrypt_fname_siphash(dir, &qname);
		} else {
			ext4_warning_inode(dir, "Siphash requires key");
			return -1;
		}

		hash = (__u32)(combined_hash >> 32);
		minor_hash = (__u32)combined_hash;
		break;
	}
	default:
		hinfo->hash = 0;
		hinfo->minor_hash = 0;
		ext4_warning(dir->i_sb,
			     "invalid/unsupported hash tree version %u",
			     hinfo->hash_version);
		return -EINVAL;
	}
	hash = hash & ~1;
	if (hash == (EXT4_HTREE_EOF_32BIT << 1))
		hash = (EXT4_HTREE_EOF_32BIT - 1) << 1;
	hinfo->hash = hash;
	hinfo->minor_hash = minor_hash;
	return 0;
}


/**
 * ext4fs_dirhash - Implements the ext4fs dirhash operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4fs_dirhash(const struct inode *dir, const char *name, int len,
		   struct dx_hash_info *hinfo)
{
#if IS_ENABLED(CONFIG_UNICODE)
	const struct unicode_map *um = dir->i_sb->s_encoding;
	int r, dlen;
	unsigned char *buff;
	struct qstr qstr = {.name = name, .len = len };

	if (len && IS_CASEFOLDED(dir) &&
	   (!IS_ENCRYPTED(dir) || fscrypt_has_encryption_key(dir))) {
		buff = kzalloc(sizeof(char) * PATH_MAX, GFP_KERNEL);
		if (!buff)
			return -ENOMEM;

		dlen = utf8_casefold(um, &qstr, buff, PATH_MAX);
		if (dlen < 0) {
			kfree(buff);
			goto opaque_seq;
		}

		r = __ext4fs_dirhash(dir, buff, dlen, hinfo);

		kfree(buff);
		return r;
	}
opaque_seq:
#endif
	return __ext4fs_dirhash(dir, name, len, hinfo);
}
