#ifndef __LINUX_IFS_SFS2_ADAPTER_H
#define __LINUX_IFS_SFS2_ADAPTER_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mutex.h>
#include <linux/version.h>
#include <asm/byteorder.h>
#include "disk_layout.h"
#include "../core/sfs2_core.h"

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#define IFS_SFS2_AOPS_WRITE_CONTEXT const struct kiocb *iocb
#define IFS_SFS2_AOPS_WRITE_CONTEXT_ARG iocb
#define IFS_SFS2_INODE_IS_NEW(inode) \
    ((inode_state_read_once(inode) & I_NEW) != 0)
#define IFS_SFS2_MKDIR_RETURN struct dentry *
#define IFS_SFS2_MKDIR_FAILURE(error) ERR_PTR(error)
#define IFS_SFS2_MKDIR_SUCCESS NULL
#else
#define IFS_SFS2_AOPS_WRITE_CONTEXT struct file *file
#define IFS_SFS2_AOPS_WRITE_CONTEXT_ARG file
#define IFS_SFS2_INODE_IS_NEW(inode) (((inode)->i_state & I_NEW) != 0)
#define IFS_SFS2_MKDIR_RETURN int
#define IFS_SFS2_MKDIR_FAILURE(error) (error)
#define IFS_SFS2_MKDIR_SUCCESS 0
#endif

#define ifs_sfs2_debug(fmt,arg...) /* no debug at all */
//#define ifs_sfs2_debug(fmt,arg...) printk(fmt,##arg)  /* general debug infos */

#if !defined (__BIG_ENDIAN) && !defined (__LITTLE_ENDIAN)
#error Endianes must be known for ASFS to work. Sorry.
#endif

#define IFS_SFS2_MAXFN_BUF (IFS_SFS2_MAXFN + 4)
#define IFS_SFS2_DEFAULT_UID 0
#define IFS_SFS2_DEFAULT_GID 0
#define IFS_SFS2_DEFAULT_MODE 0644	/* default permission bits for files, dirs have same permission, but with "x" set */

/* Extent structure located in RAM (e.g. inside inode structure), 
   currently used to store last used extent */

struct inramExtent {
	u32 startblock;	/* Block from begginig of the file */
	u32 key;
	u32 next;
	u32 blocks;
};

/* inode in-kernel data */

struct ifs_sfs2_inode_info {
	atomic_t i_opencnt;
	u32 firstblock;
	u32 hashtable;
	int modified;
	loff_t mmu_private;
	struct inramExtent ext_cache;
	struct inode vfs_inode;
};

/* short cut to get to the asfs specific inode data */
static inline struct ifs_sfs2_inode_info *IFS_SFS2_I(struct inode *inode)
{
   return list_entry(inode, struct ifs_sfs2_inode_info, vfs_inode);
}

/* Amiga SFS superblock in-core data */

struct ifs_sfs2_sb_info {
	/* Serialises filesystem-wide metadata mutations formerly guarded by lock_super(). */
	struct mutex lock;
	u32 totalblocks;
	u32 rootobjectcontainer;
	u32 extentbnoderoot;
	u32 objectnoderoot;

	u32 adminspacecontainer;
	u32 bitmapbase;
	u32 freeblocks;
	u32 blocks_inbitmap;
	u32 blocks_bitmap;
	u32 block_rovingblockptr;

	uid_t uid;
	gid_t gid;
	umode_t mode;
	u16 flags;
	char *prefix;
	char *root_volume;		/* Volume prefix for absolute symlinks. */
	char *iocharset;
	char *codepage;
	struct nls_table *nls_io;
	struct nls_table *nls_disk;
};

/* short cut to get to the asfs specific sb data */
static inline struct ifs_sfs2_sb_info *IFS_SFS2_SB(struct super_block *sb)
{
	return sb->s_fs_info;
}
 
/* io inline code */

u32 ifs_sfs2_calcchecksum(void *block, u32 blocksize);

static inline int
ifs_sfs2_check_block(struct fsBlockHeader *block, u32 blocksize, u32 n, u32 id)
{
	return ifs_sfs2_validate_block_header(
		(const unsigned char *)block, blocksize, n, id) ?
		TRUE : FALSE;
}

/* get fs structure from block and do some checks... */
static inline struct buffer_head *
ifs_sfs2_breadcheck(struct super_block *sb, u32 n, u32 type)
{
	struct buffer_head *bh;

	if (IFS_SFS2_SB(sb)->totalblocks != 0U &&
	    n >= IFS_SFS2_SB(sb)->totalblocks)
		return NULL;

	bh = sb_bread(sb, n);
	if (bh) {
		if (ifs_sfs2_check_block((void *)bh->b_data, sb->s_blocksize,
				     n, type))
			return bh;
		brelse(bh);
	}
	return NULL;
}

static inline struct buffer_head *
ifs_sfs2_getzeroblk(struct super_block *sb, int block)
{
	struct buffer_head *bh;

	if (block < 0 ||
	    (IFS_SFS2_SB(sb)->totalblocks != 0U &&
	     (u32)block >= IFS_SFS2_SB(sb)->totalblocks))
		return NULL;

	bh = sb_getblk(sb, block);
	if (!bh)
		return NULL;

	lock_buffer(bh);
	memset(bh->b_data, 0, sb->s_blocksize);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	return bh;
}

static inline void 
ifs_sfs2_bstore(struct super_block *sb, struct buffer_head *bh)
{
	((struct fsBlockHeader *) (bh->b_data))->checksum =
	    cpu_to_be32(ifs_sfs2_calcchecksum(bh->b_data, sb->s_blocksize));
	mark_buffer_dirty(bh);
}

static inline void ifs_sfs2_brelse(struct buffer_head *bh)
{
	brelse(bh);
}

static inline void dec_count(struct inode *inode)
{
	drop_nlink(inode);
	mark_inode_dirty(inode);
}

/* all prototypes */

/* adminspace.c */
int ifs_sfs2_allocadminspace(struct super_block *sb, u32 * block);
int ifs_sfs2_freeadminspace(struct super_block *sb, u32 block);
int ifs_sfs2_markspace(struct super_block *sb, u32 block, u32 blocks);
int ifs_sfs2_freespace(struct super_block *sb, u32 block, u32 blocks);
int ifs_sfs2_findspace(struct super_block *sb, u32 maxneeded, u32 start, u32 end,
	      u32 * returned_block, u32 * returned_blocks);

/* dir.c */
int ifs_sfs2_readdir(struct file *filp, struct dir_context *ctx);
struct dentry *ifs_sfs2_lookup(struct inode *dir, struct dentry *dentry,
                           unsigned int flags);

/* extents.c */
int ifs_sfs2_getextent(struct super_block *sb, u32 key, struct buffer_head **ret_bh,
	      struct fsExtentBNode **ret_ebn);
int ifs_sfs2_deletebnode(struct super_block *sb, struct buffer_head *cb, u32 key);
int ifs_sfs2_deleteextents(struct super_block *sb, u32 key);
int ifs_sfs2_addblocks(struct super_block *sb, u32 blocks, u32 newspace,
	      u32 objectnode, u32 * io_lastextentbnode);

/* file.c */
int ifs_sfs2_read_folio(struct file *file, struct folio *folio);
void ifs_sfs2_readahead(struct readahead_control *rac);
sector_t ifs_sfs2_bmap(struct address_space *mapping, sector_t block);
int ifs_sfs2_writepages(struct address_space *mapping,
                    struct writeback_control *wbc);
int ifs_sfs2_write_begin(IFS_SFS2_AOPS_WRITE_CONTEXT, struct address_space *mapping,
                     loff_t pos, unsigned len, struct folio **foliop,
                     void **fsdata);
int ifs_sfs2_truncate(struct inode *inode);
int ifs_sfs2_file_open(struct inode *inode, struct file *filp);
int ifs_sfs2_file_release(struct inode *inode, struct file *filp);

/* inode.c */
struct inode *ifs_sfs2_get_root_inode(struct super_block *sb);
void ifs_sfs2_read_locked_inode(struct inode *inode, void *arg);

/* namei */
u8 ifs_sfs2_lowerchar(u8 c);
int ifs_sfs2_check_name(const u8 *name, int len);
int ifs_sfs2_namecmp(u8 *s, u8 *ct, int casesensitive, struct nls_table *t);
u16 ifs_sfs2_hash(u8 *name, int casesensitive);
void ifs_sfs2_translate(u8 *to, u8 *from, struct nls_table *nls_to, struct nls_table *nls_from, int limit);

/* nodes */
int ifs_sfs2_getnode(struct super_block *sb, u32 nodeno,
	    struct buffer_head **ret_bh, struct fsObjectNode **ret_node);
int ifs_sfs2_createnode(struct super_block *sb, struct buffer_head **returned_cb,
	       struct fsNode **returned_node, u32 * returned_nodeno);
int ifs_sfs2_deletenode(struct super_block *sb, u32 objectnode);

static inline bool ifs_sfs2_object_slot_fits(
    struct super_block *sb,
    struct fsObjectContainer *container,
    struct fsObject *object)
{
    const u8 *const start = (const u8 *)container;
    const u8 *const end = start + sb->s_blocksize;
    const u8 *const object_bytes = (const u8 *)object;

    if (object_bytes < start || object_bytes > end)
        return false;
    return (size_t)(end - object_bytes) >= IFS_SFS2_OBJECT_FIXED_SIZE + 2U;
}

/* objects */
struct fsObject *ifs_sfs2_nextobject(struct super_block *sb,
		struct fsObjectContainer *container, struct fsObject *obj);
struct fsObject *ifs_sfs2_find_obj_by_name(struct super_block *sb,
		struct fsObjectContainer *objcont, u8 * name);
int ifs_sfs2_readobject(struct super_block *sb, u32 objectnode,
	       struct buffer_head **cb, struct fsObject **returned_object);
int ifs_sfs2_createobject(struct super_block *sb, struct buffer_head **io_cb,
		 struct fsObject **io_o, struct fsObject *src_o,
		 u8 * objname, int force);
int ifs_sfs2_deleteobject(struct super_block *sb, struct buffer_head *cb,
		 struct fsObject *o);
int ifs_sfs2_renameobject(struct super_block *sb, struct buffer_head *cb1,
		 struct fsObject *o1, struct buffer_head *cbparent,
		 struct fsObject *oparent, u8 * newname);

int ifs_sfs2_addblockstofile(struct super_block *sb, struct buffer_head *objcb,
		    struct fsObject *o, u32 blocks, u32 * newspace,
		    u32 * addedblocks);
int ifs_sfs2_truncateblocksinfile(struct super_block *sb, struct buffer_head *bh,
			 struct fsObject *o, u32 newsize);

/* symlink.c */
const char *ifs_sfs2_get_link(struct dentry *dentry, struct inode *inode,
                          struct delayed_call *done);
int ifs_sfs2_write_symlink(struct inode *symfile, const char *symname);


/* Bitmap adapter API consolidated from the migration helper header. */

#include <linux/types.h>
#include "../core/sfs2_core.h"

static inline int bfffo(u32 data, int bitoffset)
{
    if (bitoffset < 0)
        return -1;
    return ifs_sfs2_bitmap_word_find_set(data, (ifs_sfs2_u32)bitoffset);
}

static inline int bfffz(u32 data, int bitoffset)
{
    if (bitoffset < 0)
        return -1;
    return ifs_sfs2_bitmap_word_find_zero(data, (ifs_sfs2_u32)bitoffset);
}

static inline u32 bfset(u32 data, int bitoffset, int bits)
{
    if (bitoffset < 0 || bits < 0)
        return data;
    return ifs_sfs2_bitmap_word_set(
        data, (ifs_sfs2_u32)bitoffset, (ifs_sfs2_u32)bits);
}

static inline u32 bfclr(u32 data, int bitoffset, int bits)
{
    if (bitoffset < 0 || bits < 0)
        return data;
    return ifs_sfs2_bitmap_word_clear(
        data, (ifs_sfs2_u32)bitoffset, (ifs_sfs2_u32)bits);
}

int bmffo(u32 *bitmap, int longs, int bitoffset);
int bmffz(u32 *bitmap, int longs, int bitoffset);
int bmclr(u32 *bitmap, int longs, int bitoffset, int bits);
int bmset(u32 *bitmap, int longs, int bitoffset, int bits);


#endif
