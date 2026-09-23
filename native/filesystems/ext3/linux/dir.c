/*
 *  linux/fs/ext3/dir.c
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
 *  ext3 directory handling functions
 *
 *  Big-endian to little-endian byte-swapping/bitmaps by
 *        David S. Miller (davem@caip.rutgers.edu), 1995
 *
 * Hash Tree Directory indexing (c) 2001  Daniel Phillips
 *
 */

/*
 * EXT3 — Directory representation
 *
 * Purpose:
 *   Parses, validates and iterates directory records and implements directory lookup-side mechanics, including indexed-directory hashing where applicable.
 *
 * Filesystem model:
 *   This file belongs to a standalone EXT3 VFS implementation with its historical JBD engine embedded in ext3.ko.
 *
 * Correctness focus:
 *   Directory record lengths, alignment and bounds are untrusted on-disk input and must be validated before pointer arithmetic or publication to VFS.
 *
 * Project rules:
 *   - EXT3 requires its journal semantics; it is not an EXT4 compatibility registration.
 *   - Preserve the journal, recovery, ordered/writeback/journal data modes and EXT3 on-disk limits.
 *   - JBD and the metadata cache are private implementation code, not separately deployed modules.
 *
 * Commentary policy:
 *   Comments explain invariants, ownership, persistence ordering and
 *   non-obvious design intent. They deliberately avoid restating C syntax.
 */

#include <linux/compat.h>
#include <linux/iversion.h>
#include "ext3.h"

static unsigned char ext3_filetype_table[] = {
	DT_UNKNOWN, DT_REG, DT_DIR, DT_CHR, DT_BLK, DT_FIFO, DT_SOCK, DT_LNK
};

static int ext3_dx_readdir(struct file *, struct dir_context *);


/**
 * get_dtype - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static unsigned char get_dtype(struct super_block *sb, int filetype)
{
	if (!EXT3_HAS_INCOMPAT_FEATURE(sb, EXT3_FEATURE_INCOMPAT_FILETYPE) ||
	    (filetype >= EXT3_FT_MAX))
		return DT_UNKNOWN;

	return (ext3_filetype_table[filetype]);
}


/**
 * is_dx_dir - Implements the is dx dir operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int is_dx_dir(struct inode *inode)
{
	struct super_block *sb = inode->i_sb;

	if (EXT3_HAS_COMPAT_FEATURE(inode->i_sb,
		     EXT3_FEATURE_COMPAT_DIR_INDEX) &&
	    ((EXT3_I(inode)->i_flags & EXT3_INDEX_FL) ||
	     ((inode->i_size >> sb->s_blocksize_bits) == 1)))
		return 1;

	return 0;
}


/**
 * ext3_check_dir_entry - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext3_check_dir_entry (const char * function, struct inode * dir,
			  struct ext3_dir_entry_2 * de,
			  struct buffer_head * bh,
			  unsigned long offset)
{
	const int rlen = ext3_rec_len_from_disk(de->rec_len);
	const IfsExt3DirectoryRecordStatus record_status =
		ifs_ext3_validate_directory_record(
			(ifs_ext3_u32)((char *)de - bh->b_data),
			(ifs_ext3_u32)rlen,
			de->name_len,
			le32_to_cpu(de->inode),
			dir->i_sb->s_blocksize,
			le32_to_cpu(EXT3_SB(dir->i_sb)->s_es->s_inodes_count));
	const char *error_msg =
		record_status == IFS_EXT3_DIRECTORY_RECORD_OK ? NULL :
		ifs_ext3_directory_record_status_string(record_status);

	if (unlikely(error_msg != NULL))
		ext3_error (dir->i_sb, function,
			"bad entry in directory #%lu: %s - "
			"offset=%lu, inode=%lu, rec_len=%d, name_len=%d",
			dir->i_ino, error_msg, offset,
			(unsigned long) le32_to_cpu(de->inode),
			rlen, de->name_len);

	return error_msg == NULL ? 1 : 0;
}


/**
 * ext3_readdir - Implements the readdir operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext3_readdir(struct file *file, struct dir_context *ctx)
{
	unsigned long offset;
	int i;
	struct ext3_dir_entry_2 *de;
	int err;
	struct inode *inode = file_inode(file);
	struct dir_private_info *info = file->private_data;
	struct super_block *sb = inode->i_sb;
	int dir_has_error = 0;

	if (is_dx_dir(inode)) {
		err = ext3_dx_readdir(file, ctx);
		if (err != ERR_BAD_DX_DIR)
			return err;


		EXT3_I(inode)->i_flags &= ~EXT3_INDEX_FL;
	}
	offset = ctx->pos & (sb->s_blocksize - 1);

	while (ctx->pos < inode->i_size) {
		unsigned long blk = ctx->pos >> EXT3_BLOCK_SIZE_BITS(sb);
		struct buffer_head map_bh;
		struct buffer_head *bh = NULL;

		map_bh.b_state = 0;
		err = ext3_get_blocks_handle(NULL, inode, blk, 1, &map_bh, 0);
		if (err > 0) {
			pgoff_t index = map_bh.b_blocknr >>
					(PAGE_SHIFT - inode->i_blkbits);
			if (!ra_has_index(&file->f_ra, index))
				page_cache_sync_readahead(
					sb->s_bdev->bd_mapping,
					&file->f_ra, file,
					index, 1);
			file->f_ra.prev_pos = (loff_t)index << PAGE_SHIFT;
			bh = ext3_bread(NULL, inode, blk, 0, &err);
		}


		if (!bh) {
			if (!dir_has_error) {
				ext3_error(sb, __func__, "directory #%lu "
					"contains a hole at offset %lld",
					inode->i_ino, ctx->pos);
				dir_has_error = 1;
			}

			if (ctx->pos > inode->i_blocks << 9)
				break;
			ctx->pos += sb->s_blocksize - offset;
			continue;
		}


		if (offset && info && !inode_eq_iversion(inode, info->cookie)) {
			for (i = 0; i < sb->s_blocksize && i < offset; ) {
				de = (struct ext3_dir_entry_2 *)
					(bh->b_data + i);


				if (ext3_rec_len_from_disk(de->rec_len) <
						EXT3_DIR_REC_LEN(1))
					break;
				i += ext3_rec_len_from_disk(de->rec_len);
			}
			offset = i;
			ctx->pos = (ctx->pos & ~(sb->s_blocksize - 1))
				| offset;
			info->cookie = inode_query_iversion(inode);
		}

		while (ctx->pos < inode->i_size
		       && offset < sb->s_blocksize) {
			de = (struct ext3_dir_entry_2 *) (bh->b_data + offset);
			if (!ext3_check_dir_entry ("ext3_readdir", inode, de,
						   bh, offset)) {


				ctx->pos = (ctx->pos |
						(sb->s_blocksize - 1)) + 1;
				break;
			}
			offset += ext3_rec_len_from_disk(de->rec_len);
			if (le32_to_cpu(de->inode)) {
				if (!dir_emit(ctx, de->name, de->name_len,
					      le32_to_cpu(de->inode),
					      get_dtype(sb, de->file_type))) {
					brelse(bh);
					return 0;
				}
			}
			ctx->pos += ext3_rec_len_from_disk(de->rec_len);
		}
		offset = 0;
		brelse (bh);
		if (ctx->pos < inode->i_size)
			if (!dir_relax(inode))
				return 0;
	}
	return 0;
}


/**
 * is_32bit_api - Implements the is 32bit api operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
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
 * transaction preconditions established by the surrounding EXT3
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
 * transaction preconditions established by the surrounding EXT3
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
 * transaction preconditions established by the surrounding EXT3
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
 * ext3_get_htree_eof - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static inline loff_t ext3_get_htree_eof(struct file *filp)
{
	if ((filp->f_mode & FMODE_32BITHASH) ||
	    (!(filp->f_mode & FMODE_64BITHASH) && is_32bit_api()))
		return EXT3_HTREE_EOF_32BIT;
	else
		return EXT3_HTREE_EOF_64BIT;
}


/**
 * ext3_dir_llseek - Implements the dir llseek operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static loff_t ext3_dir_llseek(struct file *file, loff_t offset, int whence)
{
	struct inode *inode = file->f_mapping->host;
	int dx_dir = is_dx_dir(inode);
	loff_t htree_max = ext3_get_htree_eof(file);

	if (likely(dx_dir))
		return generic_file_llseek_size(file, offset, whence,
					        htree_max, htree_max);
	else
		return generic_file_llseek(file, offset, whence);
}


/**
 * struct fname - Private EXT3 state/data structure used by directory representation.
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
	char		name[0];
};


/**
 * free_rb_tree_fname - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void free_rb_tree_fname(struct rb_root *root)
{
	struct fname *fname, *next;

	rbtree_postorder_for_each_entry_safe(fname, next, root, rb_hash)
		do {
			struct fname *old = fname;
			fname = fname->next;
			kfree(old);
		} while (fname);

	*root = RB_ROOT;
}


/**
 * ext3_htree_create_dir_info - Performs a namespace mutation that must remain transactionally consistent across all affected directory and inode state.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static struct dir_private_info *ext3_htree_create_dir_info(struct file *filp,
							   loff_t pos)
{
	struct dir_private_info *p;

	p = kzalloc(sizeof(struct dir_private_info), GFP_KERNEL);
	if (!p)
		return NULL;
	p->curr_hash = pos2maj_hash(filp, pos);
	p->curr_minor_hash = pos2min_hash(filp, pos);
	p->cookie = inode_query_iversion(file_inode(filp));
	return p;
}


/**
 * ext3_htree_free_dir_info - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext3_htree_free_dir_info(struct dir_private_info *p)
{
	free_rb_tree_fname(&p->root);
	kfree(p);
}


/**
 * ext3_htree_store_dirent - Implements the htree store dirent operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext3_htree_store_dirent(struct file *dir_file, __u32 hash,
			     __u32 minor_hash,
			     struct ext3_dir_entry_2 *dirent)
{
	struct rb_node **p, *parent = NULL;
	struct fname * fname, *new_fn;
	struct dir_private_info *info;
	int len;

	info = (struct dir_private_info *) dir_file->private_data;
	p = &info->root.rb_node;


	len = sizeof(struct fname) + dirent->name_len + 1;
	new_fn = kzalloc(len, GFP_KERNEL);
	if (!new_fn)
		return -ENOMEM;
	new_fn->hash = hash;
	new_fn->minor_hash = minor_hash;
	new_fn->inode = le32_to_cpu(dirent->inode);
	new_fn->name_len = dirent->name_len;
	new_fn->file_type = dirent->file_type;
	memcpy(new_fn->name, dirent->name, dirent->name_len);
	new_fn->name[dirent->name_len] = 0;

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
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static bool call_filldir(struct file *file, struct dir_context *ctx,
			struct fname *fname)
{
	struct dir_private_info *info = file->private_data;
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;

	if (!fname) {
		printk("call_filldir: called with null fname?!?\n");
		return true;
	}
	ctx->pos = hash2pos(file, fname->hash, fname->minor_hash);
	while (fname) {
		if (!dir_emit(ctx, fname->name, fname->name_len,
				fname->inode,
				get_dtype(sb, fname->file_type))) {
			info->extra_fname = fname;
			return false;
		}
		fname = fname->next;
	}
	return true;
}


/**
 * ext3_dx_readdir - Implements the dx readdir operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext3_dx_readdir(struct file *file, struct dir_context *ctx)
{
	struct dir_private_info *info = file->private_data;
	struct inode *inode = file_inode(file);
	struct fname *fname;
	int	ret;

	if (!info) {
		info = ext3_htree_create_dir_info(file, ctx->pos);
		if (!info)
			return -ENOMEM;
		file->private_data = info;
	}

	if (ctx->pos == ext3_get_htree_eof(file))
		return 0;


	if (info->last_pos != ctx->pos) {
		free_rb_tree_fname(&info->root);
		info->curr_node = NULL;
		info->extra_fname = NULL;
		info->curr_hash = pos2maj_hash(file, ctx->pos);
		info->curr_minor_hash = pos2min_hash(file, ctx->pos);
	}


	if (info->extra_fname) {
		if (!call_filldir(file, ctx, info->extra_fname))
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
			ret = ext3_htree_fill_tree(file, info->curr_hash,
						   info->curr_minor_hash,
						   &info->next_hash);
			if (ret < 0)
				return ret;
			if (ret == 0) {
				ctx->pos = ext3_get_htree_eof(file);
				break;
			}
			info->curr_node = rb_first(&info->root);
		}

		fname = rb_entry(info->curr_node, struct fname, rb_hash);
		info->curr_hash = fname->hash;
		info->curr_minor_hash = fname->minor_hash;
		if (!call_filldir(file, ctx, fname))
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
				ctx->pos = ext3_get_htree_eof(file);
				break;
			}
			info->curr_hash = info->next_hash;
			info->curr_minor_hash = 0;
		}
	}
finished:
	info->last_pos = ctx->pos;
	return 0;
}


/**
 * ext3_dir_open - Allocates per-open directory iteration state.
 *
 * Linux no longer stores filesystem directory-version state in struct file.
 * Keep the cache cookie private to this wrapper and derive it from the inode
 * i_version so both linear and indexed iteration detect namespace changes.
 */
static int ext3_dir_open(struct inode *inode, struct file *file)
{
	struct dir_private_info *info;

	info = ext3_htree_create_dir_info(file, 0);
	if (!info)
		return -ENOMEM;
	info->cookie = inode_query_iversion(inode);
	file->private_data = info;
	return 0;
}


/**
 * ext3_release_dir - Releases filesystem state and reconciles the corresponding accounting or ownership metadata.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext3_release_dir (struct inode * inode, struct file * filp)
{
       if (filp->private_data)
		ext3_htree_free_dir_info(filp->private_data);

	return 0;
}

const struct file_operations ext3_dir_operations = {
	.open		= ext3_dir_open,
	.llseek		= ext3_dir_llseek,
	.read		= generic_read_dir,
	.iterate_shared	= ext3_readdir,
	.unlocked_ioctl = ext3_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= ext3_compat_ioctl,
#endif
	.fsync		= ext3_sync_file,
	.release	= ext3_release_dir,
};


#define EXT3_MD4_F(x, y, z) ((z) ^ ((x) & ((y) ^ (z))))
#define EXT3_MD4_G(x, y, z) (((x) & (y)) + (((x) ^ (y)) & (z)))
#define EXT3_MD4_H(x, y, z) ((x) ^ (y) ^ (z))
#define EXT3_MD4_ROUND(f, a, b, c, d, x, s) \
	(a += f(b, c, d) + x, a = rol32(a, s))

static __u32 ext3_half_md4_transform(__u32 buf[4], const __u32 in[8])
{
	__u32 a = buf[0], b = buf[1], c = buf[2], d = buf[3];

#define K1 0
#define K2 013240474631UL
#define K3 015666365641UL
	EXT3_MD4_ROUND(EXT3_MD4_F, a, b, c, d, in[0] + K1, 3);
	EXT3_MD4_ROUND(EXT3_MD4_F, d, a, b, c, in[1] + K1, 7);
	EXT3_MD4_ROUND(EXT3_MD4_F, c, d, a, b, in[2] + K1, 11);
	EXT3_MD4_ROUND(EXT3_MD4_F, b, c, d, a, in[3] + K1, 19);
	EXT3_MD4_ROUND(EXT3_MD4_F, a, b, c, d, in[4] + K1, 3);
	EXT3_MD4_ROUND(EXT3_MD4_F, d, a, b, c, in[5] + K1, 7);
	EXT3_MD4_ROUND(EXT3_MD4_F, c, d, a, b, in[6] + K1, 11);
	EXT3_MD4_ROUND(EXT3_MD4_F, b, c, d, a, in[7] + K1, 19);

	EXT3_MD4_ROUND(EXT3_MD4_G, a, b, c, d, in[1] + K2, 3);
	EXT3_MD4_ROUND(EXT3_MD4_G, d, a, b, c, in[3] + K2, 5);
	EXT3_MD4_ROUND(EXT3_MD4_G, c, d, a, b, in[5] + K2, 9);
	EXT3_MD4_ROUND(EXT3_MD4_G, b, c, d, a, in[7] + K2, 13);
	EXT3_MD4_ROUND(EXT3_MD4_G, a, b, c, d, in[0] + K2, 3);
	EXT3_MD4_ROUND(EXT3_MD4_G, d, a, b, c, in[2] + K2, 5);
	EXT3_MD4_ROUND(EXT3_MD4_G, c, d, a, b, in[4] + K2, 9);
	EXT3_MD4_ROUND(EXT3_MD4_G, b, c, d, a, in[6] + K2, 13);

	EXT3_MD4_ROUND(EXT3_MD4_H, a, b, c, d, in[3] + K3, 3);
	EXT3_MD4_ROUND(EXT3_MD4_H, d, a, b, c, in[7] + K3, 9);
	EXT3_MD4_ROUND(EXT3_MD4_H, c, d, a, b, in[2] + K3, 11);
	EXT3_MD4_ROUND(EXT3_MD4_H, b, c, d, a, in[6] + K3, 15);
	EXT3_MD4_ROUND(EXT3_MD4_H, a, b, c, d, in[1] + K3, 3);
	EXT3_MD4_ROUND(EXT3_MD4_H, d, a, b, c, in[5] + K3, 9);
	EXT3_MD4_ROUND(EXT3_MD4_H, c, d, a, b, in[0] + K3, 11);
	EXT3_MD4_ROUND(EXT3_MD4_H, b, c, d, a, in[4] + K3, 15);
#undef K1
#undef K2
#undef K3

	buf[0] += a;
	buf[1] += b;
	buf[2] += c;
	buf[3] += d;
	return buf[1];
}

#undef EXT3_MD4_ROUND
#undef EXT3_MD4_H
#undef EXT3_MD4_G
#undef EXT3_MD4_F

#define DELTA 0x9E3779B9


/**
 * TEA_transform - Implements the TEA transform operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
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
	} while(--n);

	buf[0] += b0;
	buf[1] += b1;
}


/**
 * dx_hack_hash_unsigned - Implements the dx hack hash unsigned operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
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
 * transaction preconditions established by the surrounding EXT3
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
 * transaction preconditions established by the surrounding EXT3
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
		if ((i % 4) == 0)
			val = pad;
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
 * transaction preconditions established by the surrounding EXT3
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
	for (i=0; i < len; i++) {
		if ((i % 4) == 0)
			val = pad;
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
 * ext3fs_dirhash - Implements the ext3fs dirhash operation within the directory representation subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT3
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext3fs_dirhash(const char *name, int len, struct dx_hash_info *hinfo)
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
		for (i=0; i < 4; i++) {
			if (hinfo->seed[i])
				break;
		}
		if (i < 4)
			memcpy(buf, hinfo->seed, sizeof(buf));
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
			ext3_half_md4_transform(buf, in);
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
	default:
		hinfo->hash = 0;
		return -1;
	}
	hash = hash & ~1;
	if (hash == (EXT3_HTREE_EOF_32BIT << 1))
		hash = (EXT3_HTREE_EOF_32BIT - 1) << 1;
	hinfo->hash = hash;
	hinfo->minor_hash = minor_hash;
	return 0;
}
