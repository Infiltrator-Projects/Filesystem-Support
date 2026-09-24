/*
 * EXT2 Linux inode adapter.
 *
 * Filesystem layout decisions are delegated to the canonical EXT2 core.
 * This unit owns only Linux inode/page-cache integration and the mutation
 * ordering needed to publish direct and indirect block pointers safely.
 */

#include <linux/time.h>
#include <linux/highuid.h>
#include <linux/pagemap.h>
#include <linux/dax.h>
#include <linux/blkdev.h>
#include <linux/quotaops.h>
#include <linux/writeback.h>
#include <linux/buffer_head.h>
#include <linux/mpage.h>
#include <linux/fiemap.h>
#include <linux/iomap.h>
#include <linux/namei.h>
#include <linux/uio.h>

#include "ext2.h"

struct ifs_ext2_link {
	__le32 *slot;
	struct buffer_head *owner;
	ext2_fsblk_t block;
};

static unsigned long ifs_ext2_sectors_per_block(const struct inode *inode)
{
	return inode->i_sb->s_blocksize >> 9;
}

static void ifs_ext2_account_alloc(struct inode *inode)
{
	inode->i_blocks += ifs_ext2_sectors_per_block(inode);
}

static void ifs_ext2_account_free(struct inode *inode)
{
	const unsigned long sectors = ifs_ext2_sectors_per_block(inode);

	if (inode->i_blocks >= sectors)
		inode->i_blocks -= sectors;
	else
		inode->i_blocks = 0;
}

static bool ifs_ext2_fast_symlink(struct inode *inode)
{
	const unsigned long ea_sectors = EXT2_I(inode)->i_file_acl ?
		ifs_ext2_sectors_per_block(inode) : 0;

	return S_ISLNK(inode->i_mode) &&
	       inode->i_blocks == ea_sectors;
}

static int ifs_ext2_allocate_block(
	struct inode *inode, ext2_fsblk_t goal, ext2_fsblk_t *block)
{
	unsigned long count = 1;
	int error = 0;
	ext2_fsblk_t allocated;

	allocated = ext2_new_blocks(inode, goal, &count, &error, 0);
	if (error)
		return error;
	if (allocated == 0 || count != 1) {
		if (allocated != 0 && count != 0)
			ext2_free_blocks(inode, allocated, count);
		return -ENOSPC;
	}

	*block = allocated;
	ifs_ext2_account_alloc(inode);
	return 0;
}

static void ifs_ext2_release_block(struct inode *inode, ext2_fsblk_t block)
{
	if (block == 0)
		return;

	ext2_free_blocks(inode, block, 1);
	ifs_ext2_account_free(inode);
}

static int ifs_ext2_zero_metadata_block(
	struct inode *inode, ext2_fsblk_t block, struct buffer_head **result)
{
	struct buffer_head *bh;

	bh = sb_getblk(inode->i_sb, block);
	if (!bh)
		return -ENOMEM;

	lock_buffer(bh);
	memset(bh->b_data, 0, inode->i_sb->s_blocksize);
	set_buffer_uptodate(bh);
	unlock_buffer(bh);
	mark_buffer_dirty_inode(bh, inode);

	if (S_ISDIR(inode->i_mode) && IS_DIRSYNC(inode)) {
		sync_dirty_buffer(bh);
		if (!buffer_uptodate(bh)) {
			brelse(bh);
			return -EIO;
		}
	}

	*result = bh;
	return 0;
}

static void ifs_ext2_mark_pointer_owner(
	struct inode *inode, struct buffer_head *owner)
{
	if (owner)
		mark_buffer_dirty_inode(owner, inode);
	else
		mark_inode_dirty(inode);
}

static int ifs_ext2_map_one(
	struct inode *inode, sector_t logical, bool create,
	ext2_fsblk_t *physical, bool *created, bool *boundary)
{
	IfsExt2BlockPath path;
	struct ext2_inode_info *ei = EXT2_I(inode);
	struct buffer_head *levels[3] = { NULL, NULL, NULL };
	struct ifs_ext2_link new_links[4];
	unsigned int new_count = 0;
	__le32 *slot;
	struct buffer_head *slot_owner = NULL;
	ext2_fsblk_t mapped_block = 0;
	ext2_fsblk_t goal;
	unsigned int level;
	int error = 0;

	if (logical > U32_MAX)
		return -EFBIG;
	if (ifs_ext2_block_to_path(
		    inode->i_sb->s_blocksize, (ifs_ext2_u64)logical,
		    &path) != IFS_EXT2_OK)
		return -EFBIG;

	*physical = 0;
	*created = false;
	*boundary = path.boundary == 0U;

	mutex_lock(&ei->truncate_mutex);

	slot = &ei->i_data[path.offsets[0]];
	goal = ext2_group_first_block_no(inode->i_sb, ei->i_block_group);

	for (level = 0; level < path.depth; ++level) {
		const bool leaf = level + 1U == path.depth;

		mapped_block = le32_to_cpu(*slot);
		if (mapped_block == 0) {
			struct buffer_head *new_meta = NULL;

			if (!create)
				goto out;

			error = ifs_ext2_allocate_block(inode, goal, &mapped_block);
			if (error)
				goto rollback;
			goal = mapped_block + 1;

			if (!leaf) {
				error = ifs_ext2_zero_metadata_block(
					inode, mapped_block, &new_meta);
				if (error) {
					ifs_ext2_release_block(inode, mapped_block);
					goto rollback;
				}
			}

			*slot = cpu_to_le32(mapped_block);
			ifs_ext2_mark_pointer_owner(inode, slot_owner);

			new_links[new_count].slot = slot;
			new_links[new_count].owner = slot_owner;
			new_links[new_count].block = mapped_block;
			new_count++;

			if (leaf) {
				*created = true;
			} else {
				levels[level] = new_meta;
			}
		} else {
			if (!ext2_data_block_valid(EXT2_SB(inode->i_sb), mapped_block, 1)) {
				error = -EFSCORRUPTED;
				goto rollback;
			}
			goal = mapped_block + 1;
		}

		if (leaf) {
			*physical = mapped_block;
			goto out;
		}

		if (!levels[level]) {
			levels[level] = sb_bread(inode->i_sb, mapped_block);
			if (!levels[level]) {
				error = -EIO;
				goto rollback;
			}
		}

		slot_owner = levels[level];
		slot = (__le32 *)slot_owner->b_data + path.offsets[level + 1U];
	}

	error = -EIO;
	goto rollback;

rollback:
	while (new_count > 0) {
		struct ifs_ext2_link *link = &new_links[--new_count];

		if (le32_to_cpu(*link->slot) == link->block) {
			*link->slot = 0;
			ifs_ext2_mark_pointer_owner(inode, link->owner);
		}
		ifs_ext2_release_block(inode, link->block);
	}
out:
	for (level = 0; level < ARRAY_SIZE(levels); ++level)
		brelse(levels[level]);

	if (new_count != 0)
		mark_inode_dirty(inode);
	mutex_unlock(&ei->truncate_mutex);
	return error;
}

int ext2_get_block(
	struct inode *inode, sector_t block,
	struct buffer_head *bh, int create)
{
	ext2_fsblk_t physical;
	bool created;
	bool boundary;
	int error;

	error = ifs_ext2_map_one(
		inode, block, create != 0, &physical, &created, &boundary);
	if (error)
		return error;
	if (physical == 0)
		return 0;

	map_bh(bh, inode->i_sb, physical);
	bh->b_size = inode->i_sb->s_blocksize;
	if (created)
		set_buffer_new(bh);
	if (boundary)
		set_buffer_boundary(bh);
	return 0;
}

static bool ifs_ext2_block_all_zero(const __le32 *entries, u32 count);

static u64 ifs_ext2_subtree_span(unsigned int level, u32 ptrs)
{
	u64 span = 1;

	while (level-- != 0)
		span *= ptrs;
	return span;
}

static int ifs_ext2_free_subtree(
	struct inode *inode, ext2_fsblk_t block, unsigned int level)
{
	struct buffer_head *bh;
	__le32 *pointers;
	u32 ptrs;
	u32 index;
	int first_error = 0;

	if (block == 0)
		return 0;
	if (level == 0) {
		ifs_ext2_release_block(inode, block);
		return 0;
	}

	bh = sb_bread(inode->i_sb, block);
	if (!bh)
		return -EIO;

	pointers = (__le32 *)bh->b_data;
	ptrs = inode->i_sb->s_blocksize / sizeof(__le32);
	for (index = 0; index < ptrs; ++index) {
		const ext2_fsblk_t child = le32_to_cpu(pointers[index]);
		int error;

		if (!child)
			continue;

		error = ifs_ext2_free_subtree(inode, child, level - 1U);
		if (error) {
			if (!first_error)
				first_error = error;
			continue;
		}

		pointers[index] = 0;
	}

	if (ifs_ext2_block_all_zero(pointers, ptrs)) {
		bforget(bh);
		ifs_ext2_release_block(inode, block);
	} else {
		mark_buffer_dirty_inode(bh, inode);
		brelse(bh);
	}

	return first_error;
}

static bool ifs_ext2_block_all_zero(const __le32 *entries, u32 count)
{
	u32 index;

	for (index = 0; index < count; ++index)
		if (entries[index] != 0)
			return false;
	return true;
}

static int ifs_ext2_prune_slot(
	struct inode *inode, __le32 *slot, struct buffer_head *owner,
	unsigned int level, u64 logical_base, u64 keep_blocks)
{
	const u32 ptrs = inode->i_sb->s_blocksize / sizeof(__le32);
	const u64 span = ifs_ext2_subtree_span(level, ptrs);
	ext2_fsblk_t block = le32_to_cpu(*slot);
	struct buffer_head *bh;
	__le32 *entries;
	u64 child_span;
	u32 index;
	int error = 0;

	if (!block || keep_blocks >= logical_base + span)
		return 0;

	if (keep_blocks <= logical_base) {
		error = ifs_ext2_free_subtree(inode, block, level);
		if (!error) {
			*slot = 0;
			ifs_ext2_mark_pointer_owner(inode, owner);
		}
		return error;
	}

	if (level == 0)
		return 0;

	bh = sb_bread(inode->i_sb, block);
	if (!bh)
		return -EIO;

	entries = (__le32 *)bh->b_data;
	child_span = ifs_ext2_subtree_span(level - 1U, ptrs);

	for (index = 0; index < ptrs; ++index) {
		const u64 child_base = logical_base + (u64)index * child_span;

		if (keep_blocks >= child_base + child_span)
			continue;
		error = ifs_ext2_prune_slot(
			inode, &entries[index], bh, level - 1U,
			child_base, keep_blocks);
		if (error)
			break;
	}

	if (!error && ifs_ext2_block_all_zero(entries, ptrs)) {
		bforget(bh);
		ifs_ext2_release_block(inode, block);
		*slot = 0;
		ifs_ext2_mark_pointer_owner(inode, owner);
	} else {
		if (!error)
			mark_buffer_dirty_inode(bh, inode);
		brelse(bh);
	}
	return error;
}

static int ifs_ext2_truncate_tree(struct inode *inode, loff_t size)
{
	struct ext2_inode_info *ei = EXT2_I(inode);
	const u32 ptrs = inode->i_sb->s_blocksize / sizeof(__le32);
	const u64 keep_blocks =
		((u64)size + inode->i_sb->s_blocksize - 1U) >>
		inode->i_sb->s_blocksize_bits;
	u64 base = 0;
	u32 index;
	int error = 0;

	mutex_lock(&ei->truncate_mutex);

	for (index = 0; index < EXT2_NDIR_BLOCKS; ++index) {
		error = ifs_ext2_prune_slot(
			inode, &ei->i_data[index], NULL, 0, base, keep_blocks);
		if (error)
			goto out;
		base++;
	}

	error = ifs_ext2_prune_slot(
		inode, &ei->i_data[EXT2_IND_BLOCK], NULL,
		1, base, keep_blocks);
	if (error)
		goto out;
	base += ptrs;

	error = ifs_ext2_prune_slot(
		inode, &ei->i_data[EXT2_DIND_BLOCK], NULL,
		2, base, keep_blocks);
	if (error)
		goto out;
	base += (u64)ptrs * ptrs;

	error = ifs_ext2_prune_slot(
		inode, &ei->i_data[EXT2_TIND_BLOCK], NULL,
		3, base, keep_blocks);
out:
	ext2_discard_reservation(inode);
	mark_inode_dirty(inode);
	mutex_unlock(&ei->truncate_mutex);
	return error;
}

void ext2_write_failed(struct address_space *mapping, loff_t to)
{
	struct inode *inode = mapping->host;

	if (to <= i_size_read(inode))
		return;

	truncate_pagecache(inode, i_size_read(inode));
	(void)ifs_ext2_truncate_tree(inode, i_size_read(inode));
}

static int ifs_ext2_setsize(struct inode *inode, loff_t new_size)
{
	int error;

	if (!(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	      S_ISLNK(inode->i_mode)))
		return -EINVAL;
	if (ifs_ext2_fast_symlink(inode))
		return -EINVAL;
	if (IS_APPEND(inode) || IS_IMMUTABLE(inode))
		return -EPERM;

	inode_dio_wait(inode);

	if (IS_DAX(inode))
		error = dax_truncate_page(
			inode, new_size, NULL, &ext2_iomap_ops);
	else
		error = block_truncate_page(
			inode->i_mapping, new_size, ext2_get_block);
	if (error)
		return error;

	filemap_invalidate_lock(inode->i_mapping);
	truncate_setsize(inode, new_size);
	error = ifs_ext2_truncate_tree(inode, new_size);
	filemap_invalidate_unlock(inode->i_mapping);
	if (error)
		return error;

	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	mark_inode_dirty(inode);
	if (inode_needs_sync(inode)) {
		error = sync_mapping_buffers(inode->i_mapping);
		if (!error)
			error = sync_inode_metadata(inode, 1);
	}
	return error;
}

void ext2_evict_inode(struct inode *inode)
{
	const bool deleting = inode->i_nlink == 0 && !is_bad_inode(inode);
	struct ext2_block_alloc_info *allocation;

	if (deleting)
		dquot_initialize(inode);
	else
		dquot_drop(inode);

	truncate_inode_pages_final(&inode->i_data);

	if (deleting) {
		sb_start_intwrite(inode->i_sb);
		EXT2_I(inode)->i_dtime = ktime_get_real_seconds();
		mark_inode_dirty(inode);
		(void)ext2_write_inode(inode, &(struct writeback_control) {
			.sync_mode = inode_needs_sync(inode) ?
				WB_SYNC_ALL : WB_SYNC_NONE,
		});
		i_size_write(inode, 0);
		(void)ifs_ext2_truncate_tree(inode, 0);
		ext2_xattr_delete_inode(inode);
	}

	invalidate_inode_buffers(inode);
	clear_inode(inode);
	ext2_discard_reservation(inode);

	allocation = EXT2_I(inode)->i_block_alloc_info;
	EXT2_I(inode)->i_block_alloc_info = NULL;
	kfree(allocation);

	if (deleting) {
		ext2_free_inode(inode);
		sb_end_intwrite(inode->i_sb);
	}
}

static int ext2_iomap_begin(
	struct inode *inode, loff_t offset, loff_t length,
	unsigned flags, struct iomap *iomap, struct iomap *srcmap)
{
	const unsigned int bits = inode->i_blkbits;
	const sector_t logical = offset >> bits;
	const bool request_write = (flags & IOMAP_WRITE) != 0;
	bool create = request_write;
	struct buffer_head mapping = {
		.b_size = 1U << bits,
	};
	struct ext2_sb_info *sbi = EXT2_SB(inode->i_sb);
	int error;

	if ((flags & IOMAP_DIRECT) &&
	    ((loff_t)logical << bits) < i_size_read(inode))
		create = false;

	error = ext2_get_block(inode, logical, &mapping, create);
	if (error)
		return error;

	iomap->offset = (u64)logical << bits;
	iomap->length = 1ULL << bits;
	iomap->flags = 0;
	if (flags & IOMAP_DAX)
		iomap->dax_dev = sbi->s_daxdev;
	else
		iomap->bdev = inode->i_sb->s_bdev;

	if (!buffer_mapped(&mapping)) {
		if (!create && request_write && (flags & IOMAP_DIRECT))
			return -ENOTBLK;
		iomap->type = IOMAP_HOLE;
		iomap->addr = IOMAP_NULL_ADDR;
		return 0;
	}

	iomap->type = IOMAP_MAPPED;
	iomap->addr = (u64)mapping.b_blocknr << bits;
	if (flags & IOMAP_DAX)
		iomap->addr += sbi->s_dax_part_off;
	if (buffer_new(&mapping)) {
		iomap->flags |= IOMAP_F_NEW;
		if (flags & IOMAP_DAX) {
			error = sb_issue_zeroout(
				inode->i_sb, mapping.b_blocknr, 1, GFP_KERNEL);
			if (error)
				return error;
		}
	}
	return 0;
}

static int ext2_iomap_end(
	struct inode *inode, loff_t offset, loff_t length,
	ssize_t written, unsigned flags, struct iomap *iomap)
{
	if ((flags & IOMAP_DIRECT) && (flags & IOMAP_WRITE) && written == 0)
		return -ENOTBLK;
	if ((flags & IOMAP_WRITE) && iomap->type == IOMAP_MAPPED &&
	    written < length)
		ext2_write_failed(inode->i_mapping, offset + length);
	return 0;
}

const struct iomap_ops ext2_iomap_ops = {
	.iomap_begin = ext2_iomap_begin,
	.iomap_end = ext2_iomap_end,
};

int ext2_fiemap(
	struct inode *inode, struct fiemap_extent_info *fieinfo,
	u64 start, u64 len)
{
	loff_t size;
	int error;

	inode_lock(inode);
	size = i_size_read(inode);
	if (size == 0)
		size = 1;
	len = min_t(u64, len, size);
	error = iomap_fiemap(inode, fieinfo, start, len, &ext2_iomap_ops);
	inode_unlock(inode);
	return error;
}

static int ifs_ext2_read_folio(struct file *file, struct folio *folio)
{
	return mpage_read_folio(folio, ext2_get_block);
}

static void ifs_ext2_readahead(struct readahead_control *rac)
{
	mpage_readahead(rac, ext2_get_block);
}

static int ifs_ext2_write_begin(
	struct file *file, struct address_space *mapping,
	loff_t pos, unsigned len, struct folio **folio, void **fsdata)
{
	int error = block_write_begin(
		mapping, pos, len, folio, ext2_get_block);

	if (error)
		ext2_write_failed(mapping, pos + len);
	return error;
}

static int ifs_ext2_write_end(
	struct file *file, struct address_space *mapping,
	loff_t pos, unsigned len, unsigned copied,
	struct folio *folio, void *fsdata)
{
	const int written = generic_write_end(
		file, mapping, pos, len, copied, folio, fsdata);

	if (written < len)
		ext2_write_failed(mapping, pos + len);
	return written;
}

static sector_t ifs_ext2_bmap(
	struct address_space *mapping, sector_t block)
{
	return generic_block_bmap(mapping, block, ext2_get_block);
}

static int ifs_ext2_writepages(
	struct address_space *mapping, struct writeback_control *wbc)
{
	return mpage_writepages(mapping, wbc, ext2_get_block);
}

static int ifs_ext2_dax_writepages(
	struct address_space *mapping, struct writeback_control *wbc)
{
	struct ext2_sb_info *sbi = EXT2_SB(mapping->host->i_sb);

	return dax_writeback_mapping_range(mapping, sbi->s_daxdev, wbc);
}

const struct address_space_operations ext2_aops = {
	.dirty_folio = block_dirty_folio,
	.invalidate_folio = block_invalidate_folio,
	.read_folio = ifs_ext2_read_folio,
	.readahead = ifs_ext2_readahead,
	.write_begin = ifs_ext2_write_begin,
	.write_end = ifs_ext2_write_end,
	.bmap = ifs_ext2_bmap,
	.writepages = ifs_ext2_writepages,
	.migrate_folio = buffer_migrate_folio,
	.is_partially_uptodate = block_is_partially_uptodate,
	.error_remove_folio = generic_error_remove_folio,
};

static const struct address_space_operations ext2_dax_aops = {
	.writepages = ifs_ext2_dax_writepages,
	.dirty_folio = noop_dirty_folio,
};

static struct ext2_inode *ifs_ext2_raw_inode(
	struct super_block *sb, ino_t ino, struct buffer_head **buffer)
{
	struct ext2_group_desc *descriptor;
	struct buffer_head *bh;
	unsigned long group;
	unsigned long offset;
	unsigned long block;

	*buffer = NULL;
	if ((ino != EXT2_ROOT_INO && ino < EXT2_FIRST_INO(sb)) ||
	    ino > le32_to_cpu(EXT2_SB(sb)->s_es->s_inodes_count))
		return ERR_PTR(-EINVAL);

	group = (ino - 1) / EXT2_INODES_PER_GROUP(sb);
	descriptor = ext2_get_group_desc(sb, group, NULL);
	if (!descriptor)
		return ERR_PTR(-EIO);

	offset = ((ino - 1) % EXT2_INODES_PER_GROUP(sb)) *
		 EXT2_INODE_SIZE(sb);
	block = le32_to_cpu(descriptor->bg_inode_table) +
		(offset >> EXT2_BLOCK_SIZE_BITS(sb));

	bh = sb_bread(sb, block);
	if (!bh)
		return ERR_PTR(-EIO);

	offset &= EXT2_BLOCK_SIZE(sb) - 1;
	if (offset + EXT2_INODE_SIZE(sb) > sb->s_blocksize) {
		brelse(bh);
		return ERR_PTR(-EFSCORRUPTED);
	}

	*buffer = bh;
	return (struct ext2_inode *)(bh->b_data + offset);
}

void ext2_set_inode_flags(struct inode *inode)
{
	const unsigned int ext_flags = EXT2_I(inode)->i_flags;

	inode->i_flags &= ~(S_SYNC | S_APPEND | S_IMMUTABLE |
			    S_NOATIME | S_DIRSYNC | S_DAX);
	if (ext_flags & EXT2_SYNC_FL)
		inode->i_flags |= S_SYNC;
	if (ext_flags & EXT2_APPEND_FL)
		inode->i_flags |= S_APPEND;
	if (ext_flags & EXT2_IMMUTABLE_FL)
		inode->i_flags |= S_IMMUTABLE;
	if (ext_flags & EXT2_NOATIME_FL)
		inode->i_flags |= S_NOATIME;
	if (ext_flags & EXT2_DIRSYNC_FL)
		inode->i_flags |= S_DIRSYNC;
	if (test_opt(inode->i_sb, DAX) && S_ISREG(inode->i_mode))
		inode->i_flags |= S_DAX;
}

void ext2_set_file_ops(struct inode *inode)
{
	inode->i_op = &ext2_file_inode_operations;
	inode->i_fop = &ext2_file_operations;
	inode->i_mapping->a_ops = IS_DAX(inode) ?
		&ext2_dax_aops : &ext2_aops;
}

struct inode *ext2_iget(struct super_block *sb, unsigned long ino)
{
	struct inode *inode;
	struct ext2_inode_info *ei;
	struct ext2_inode *raw;
	struct buffer_head *bh = NULL;
	uid_t uid;
	gid_t gid;
	unsigned int index;
	long error = -EIO;

	inode = iget_locked(sb, ino);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode->i_state & I_NEW))
		return inode;

	ei = EXT2_I(inode);
	ei->i_block_alloc_info = NULL;
	raw = ifs_ext2_raw_inode(sb, ino, &bh);
	if (IS_ERR(raw)) {
		error = PTR_ERR(raw);
		goto fail;
	}

	inode->i_mode = le16_to_cpu(raw->i_mode);
	uid = le16_to_cpu(raw->i_uid_low);
	gid = le16_to_cpu(raw->i_gid_low);
	if (!test_opt(sb, NO_UID32)) {
		uid |= (uid_t)le16_to_cpu(raw->i_uid_high) << 16;
		gid |= (gid_t)le16_to_cpu(raw->i_gid_high) << 16;
	}
	i_uid_write(inode, uid);
	i_gid_write(inode, gid);
	set_nlink(inode, le16_to_cpu(raw->i_links_count));
	i_size_write(inode, le32_to_cpu(raw->i_size));
	inode_set_atime(inode, (signed)le32_to_cpu(raw->i_atime), 0);
	inode_set_ctime(inode, (signed)le32_to_cpu(raw->i_ctime), 0);
	inode_set_mtime(inode, (signed)le32_to_cpu(raw->i_mtime), 0);

	ei->i_dtime = le32_to_cpu(raw->i_dtime);
	if (inode->i_nlink == 0) {
		error = (inode->i_mode == 0 || ei->i_dtime) ?
			-ESTALE : -EFSCORRUPTED;
		goto fail;
	}

	inode->i_blocks = le32_to_cpu(raw->i_blocks);
	ei->i_flags = le32_to_cpu(raw->i_flags);
	ei->i_faddr = le32_to_cpu(raw->i_faddr);
	ei->i_frag_no = raw->i_frag;
	ei->i_frag_size = raw->i_fsize;
	ei->i_file_acl = le32_to_cpu(raw->i_file_acl);
	ei->i_dir_acl = S_ISREG(inode->i_mode) ?
		0 : le32_to_cpu(raw->i_dir_acl);

	if (S_ISREG(inode->i_mode))
		i_size_write(inode,
			i_size_read(inode) |
			((u64)le32_to_cpu(raw->i_size_high) << 32));
	if (i_size_read(inode) < 0) {
		error = -EFSCORRUPTED;
		goto fail;
	}

	if (ei->i_file_acl &&
	    !ext2_data_block_valid(EXT2_SB(sb), ei->i_file_acl, 1)) {
		error = -EFSCORRUPTED;
		goto fail;
	}

	ei->i_dtime = 0;
	inode->i_generation = le32_to_cpu(raw->i_generation);
	ei->i_state = 0;
	ei->i_block_group = (ino - 1) / EXT2_INODES_PER_GROUP(sb);
	ei->i_dir_start_lookup = 0;
	for (index = 0; index < EXT2_N_BLOCKS; ++index)
		ei->i_data[index] = raw->i_block[index];

	ext2_set_inode_flags(inode);

	if (S_ISREG(inode->i_mode)) {
		ext2_set_file_ops(inode);
	} else if (S_ISDIR(inode->i_mode)) {
		inode->i_op = &ext2_dir_inode_operations;
		inode->i_fop = &ext2_dir_operations;
		inode->i_mapping->a_ops = &ext2_aops;
	} else if (S_ISLNK(inode->i_mode)) {
		if (ifs_ext2_fast_symlink(inode)) {
			inode->i_link = (char *)ei->i_data;
			inode->i_op = &ext2_fast_symlink_inode_operations;
			nd_terminate_link(
				ei->i_data, inode->i_size,
				sizeof(ei->i_data) - 1);
		} else {
			inode->i_op = &ext2_symlink_inode_operations;
			inode_nohighmem(inode);
			inode->i_mapping->a_ops = &ext2_aops;
		}
	} else {
		inode->i_op = &ext2_special_inode_operations;
		if (raw->i_block[0])
			init_special_inode(
				inode, inode->i_mode,
				old_decode_dev(le32_to_cpu(raw->i_block[0])));
		else
			init_special_inode(
				inode, inode->i_mode,
				new_decode_dev(le32_to_cpu(raw->i_block[1])));
	}

	brelse(bh);
	unlock_new_inode(inode);
	return inode;

fail:
	brelse(bh);
	iget_failed(inode);
	return ERR_PTR(error);
}

static int ifs_ext2_write_inode(
	struct inode *inode, bool synchronous)
{
	struct ext2_inode_info *ei = EXT2_I(inode);
	struct buffer_head *bh;
	struct ext2_inode *raw;
	const uid_t uid = i_uid_read(inode);
	const gid_t gid = i_gid_read(inode);
	unsigned int index;
	int error = 0;

	raw = ifs_ext2_raw_inode(inode->i_sb, inode->i_ino, &bh);
	if (IS_ERR(raw))
		return PTR_ERR(raw);

	if (ei->i_state & EXT2_STATE_NEW)
		memset(raw, 0, EXT2_SB(inode->i_sb)->s_inode_size);

	raw->i_mode = cpu_to_le16(inode->i_mode);
	if (!test_opt(inode->i_sb, NO_UID32)) {
		raw->i_uid_low = cpu_to_le16(low_16_bits(uid));
		raw->i_gid_low = cpu_to_le16(low_16_bits(gid));
		raw->i_uid_high = ei->i_dtime ?
			0 : cpu_to_le16(high_16_bits(uid));
		raw->i_gid_high = ei->i_dtime ?
			0 : cpu_to_le16(high_16_bits(gid));
	} else {
		raw->i_uid_low = cpu_to_le16(fs_high2lowuid(uid));
		raw->i_gid_low = cpu_to_le16(fs_high2lowgid(gid));
		raw->i_uid_high = 0;
		raw->i_gid_high = 0;
	}

	raw->i_links_count = cpu_to_le16(inode->i_nlink);
	raw->i_size = cpu_to_le32(i_size_read(inode));
	raw->i_atime = cpu_to_le32(inode_get_atime_sec(inode));
	raw->i_ctime = cpu_to_le32(inode_get_ctime_sec(inode));
	raw->i_mtime = cpu_to_le32(inode_get_mtime_sec(inode));
	raw->i_blocks = cpu_to_le32(inode->i_blocks);
	raw->i_dtime = cpu_to_le32(ei->i_dtime);
	raw->i_flags = cpu_to_le32(ei->i_flags);
	raw->i_faddr = cpu_to_le32(ei->i_faddr);
	raw->i_frag = ei->i_frag_no;
	raw->i_fsize = ei->i_frag_size;
	raw->i_file_acl = cpu_to_le32(ei->i_file_acl);
	raw->i_generation = cpu_to_le32(inode->i_generation);

	if (S_ISREG(inode->i_mode)) {
		raw->i_size_high = cpu_to_le32((u64)i_size_read(inode) >> 32);
		if ((u64)i_size_read(inode) > 0x7fffffffULL &&
		    !EXT2_HAS_RO_COMPAT_FEATURE(
			    inode->i_sb, EXT2_FEATURE_RO_COMPAT_LARGE_FILE)) {
			spin_lock(&EXT2_SB(inode->i_sb)->s_lock);
			ext2_update_dynamic_rev(inode->i_sb);
			EXT2_SET_RO_COMPAT_FEATURE(
				inode->i_sb,
				EXT2_FEATURE_RO_COMPAT_LARGE_FILE);
			spin_unlock(&EXT2_SB(inode->i_sb)->s_lock);
			ext2_sync_super(
				inode->i_sb, EXT2_SB(inode->i_sb)->s_es, 1);
		}
	} else {
		raw->i_dir_acl = cpu_to_le32(ei->i_dir_acl);
	}

	if (S_ISCHR(inode->i_mode) || S_ISBLK(inode->i_mode)) {
		memset(raw->i_block, 0, sizeof(raw->i_block));
		if (old_valid_dev(inode->i_rdev))
			raw->i_block[0] =
				cpu_to_le32(old_encode_dev(inode->i_rdev));
		else
			raw->i_block[1] =
				cpu_to_le32(new_encode_dev(inode->i_rdev));
	} else {
		for (index = 0; index < EXT2_N_BLOCKS; ++index)
			raw->i_block[index] = ei->i_data[index];
	}

	mark_buffer_dirty(bh);
	if (synchronous) {
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh))
			error = -EIO;
	}
	ei->i_state &= ~EXT2_STATE_NEW;
	brelse(bh);
	return error;
}

int ext2_write_inode(
	struct inode *inode, struct writeback_control *wbc)
{
	return ifs_ext2_write_inode(
		inode, wbc->sync_mode == WB_SYNC_ALL);
}

int ext2_getattr(
	struct mnt_idmap *idmap, const struct path *path,
	struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	const unsigned int flags =
		EXT2_I(inode)->i_flags & EXT2_FL_USER_VISIBLE;

	if (flags & EXT2_APPEND_FL)
		stat->attributes |= STATX_ATTR_APPEND;
	if (flags & EXT2_COMPR_FL)
		stat->attributes |= STATX_ATTR_COMPRESSED;
	if (flags & EXT2_IMMUTABLE_FL)
		stat->attributes |= STATX_ATTR_IMMUTABLE;
	if (flags & EXT2_NODUMP_FL)
		stat->attributes |= STATX_ATTR_NODUMP;

	stat->attributes_mask |=
		STATX_ATTR_APPEND | STATX_ATTR_COMPRESSED |
		STATX_ATTR_ENCRYPTED | STATX_ATTR_IMMUTABLE |
		STATX_ATTR_NODUMP;

	generic_fillattr(&nop_mnt_idmap, request_mask, inode, stat);
	return 0;
}

int ext2_setattr(
	struct mnt_idmap *idmap, struct dentry *dentry,
	struct iattr *attributes)
{
	struct inode *inode = d_inode(dentry);
	int error;

	error = setattr_prepare(&nop_mnt_idmap, dentry, attributes);
	if (error)
		return error;

	if (is_quota_modification(&nop_mnt_idmap, inode, attributes)) {
		error = dquot_initialize(inode);
		if (error)
			return error;
	}

	if (i_uid_needs_update(&nop_mnt_idmap, attributes, inode) ||
	    i_gid_needs_update(&nop_mnt_idmap, attributes, inode)) {
		error = dquot_transfer(
			&nop_mnt_idmap, inode, attributes);
		if (error)
			return error;
	}

	if ((attributes->ia_valid & ATTR_SIZE) &&
	    attributes->ia_size != i_size_read(inode)) {
		error = ifs_ext2_setsize(inode, attributes->ia_size);
		if (error)
			return error;
	}

	setattr_copy(&nop_mnt_idmap, inode, attributes);
	if (attributes->ia_valid & ATTR_MODE) {
		error = posix_acl_chmod(
			&nop_mnt_idmap, dentry, inode->i_mode);
		if (error)
			return error;
	}

	mark_inode_dirty(inode);
	return 0;
}

const struct inode_operations ext2_symlink_inode_operations = {
	.get_link = page_get_link,
	.getattr = ext2_getattr,
	.setattr = ext2_setattr,
	.listxattr = ext2_listxattr,
};

const struct inode_operations ext2_fast_symlink_inode_operations = {
	.get_link = simple_get_link,
	.getattr = ext2_getattr,
	.setattr = ext2_setattr,
	.listxattr = ext2_listxattr,
};
