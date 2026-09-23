/*
 * Project-authored Linux regular-file adapter for Amiga FFS.
 *
 * Filesystem geometry is provided by the canonical AmigaDOS core. This unit
 * owns Linux page-cache, buffer-head and VFS integration only.
 */

#include "affs.h"

#include <linux/uio.h>
#include <linux/blkdev.h>
#include <linux/mpage.h>
#include <linux/pagemap.h>

static int ifs_ffs_map_block(
    struct inode *inode, sector_t logical,
    struct buffer_head *result, int create);

static int ifs_ffs_file_open(struct inode *inode, struct file *file)
{
    atomic_inc(&AFFS_I(inode)->i_opencnt);
    return 0;
}

static int ifs_ffs_file_release(struct inode *inode, struct file *file)
{
    if (atomic_dec_and_test(&AFFS_I(inode)->i_opencnt)) {
        inode_lock(inode);
        if (inode->i_size != AFFS_I(inode)->mmu_private)
            affs_truncate(inode);
        affs_free_prealloc(inode);
        inode_unlock(inode);
    }
    return 0;
}

static bool ifs_ffs_extension_valid(
    struct inode *inode, struct buffer_head *bh, u32 index)
{
    struct super_block *sb = inode->i_sb;
    const u32 expected_type = index == 0U ? T_SHORT : T_LIST;
    const struct affs_head *head;
    const struct affs_tail *tail;

    if (!bh || bh->b_blocknr > U32_MAX ||
        !affs_validblock(sb, (int)bh->b_blocknr))
        return false;

    head = AFFS_HEAD(bh);
    tail = AFFS_TAIL(sb, bh);

    if (affs_checksum_block(sb, bh) != 0U ||
        be32_to_cpu(head->ptype) != expected_type ||
        be32_to_cpu(head->key) != (u32)bh->b_blocknr ||
        be32_to_cpu(tail->stype) != ST_FILE ||
        be32_to_cpu(head->block_count) >
            (u32)AFFS_SB(sb)->s_hashsize)
        return false;

    if (be32_to_cpu(head->block_count) == 0U) {
        if (head->first_data != 0)
            return false;
    } else if (head->first_data != AFFS_BLOCK(sb, bh, 0U)) {
        return false;
    }

    if (index != 0U &&
        be32_to_cpu(tail->parent) != (u32)inode->i_ino)
        return false;

    return true;
}

static void ifs_ffs_cache_extension(
    struct inode *inode, struct buffer_head *bh, u32 index)
{
    struct affs_inode_info *info = AFFS_I(inode);

    if (info->i_ext_bh == bh && info->i_ext_last == index)
        return;

    affs_brelse(info->i_ext_bh);
    info->i_ext_bh = bh;
    info->i_ext_last = index;
    get_bh(bh);
}

static void ifs_ffs_clear_extension_cache(struct inode *inode)
{
    struct affs_inode_info *info = AFFS_I(inode);

    affs_brelse(info->i_ext_bh);
    info->i_ext_bh = NULL;
    info->i_ext_last = ~1U;
    info->i_lc_size = 0U;

    if (info->i_lc)
        memset(info->i_lc, 0, AFFS_LC_SIZE * sizeof(*info->i_lc));
    if (info->i_ac)
        memset(info->i_ac, 0, AFFS_AC_SIZE * sizeof(*info->i_ac));
}

static struct buffer_head *ifs_ffs_read_extension(
    struct inode *inode, u32 wanted)
{
    struct affs_inode_info *info = AFFS_I(inode);
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh;
    u32 index;

    if (wanted >= info->i_extcnt)
        return ERR_PTR(-EUCLEAN);

    if (info->i_ext_bh && info->i_ext_last == wanted) {
        get_bh(info->i_ext_bh);
        return info->i_ext_bh;
    }

    if (info->i_ext_bh && info->i_ext_last < wanted) {
        bh = info->i_ext_bh;
        index = info->i_ext_last;
        get_bh(bh);
    } else {
        bh = affs_bread(sb, (u32)inode->i_ino);
        index = 0U;
    }

    if (!bh)
        return ERR_PTR(-EIO);

    if (!ifs_ffs_extension_valid(inode, bh, index)) {
        affs_brelse(bh);
        return ERR_PTR(-EUCLEAN);
    }

    while (index < wanted) {
        const u32 next =
            be32_to_cpu(AFFS_TAIL(sb, bh)->extension);
        struct buffer_head *next_bh;

        if (next == 0U || !affs_validblock(sb, (int)next)) {
            affs_brelse(bh);
            return ERR_PTR(-EUCLEAN);
        }

        next_bh = affs_bread(sb, next);
        if (!next_bh) {
            affs_brelse(bh);
            return ERR_PTR(-EIO);
        }

        affs_brelse(bh);
        bh = next_bh;
        index++;

        if (!ifs_ffs_extension_valid(inode, bh, index)) {
            affs_brelse(bh);
            return ERR_PTR(-EUCLEAN);
        }
    }

    ifs_ffs_cache_extension(inode, bh, wanted);
    return bh;
}

static int ifs_ffs_zero_new_physical_block(
    struct super_block *sb, u32 block)
{
    struct buffer_head *bh = affs_getzeroblk(sb, block);

    if (!bh)
        return -EIO;

    mark_buffer_dirty(bh);
    affs_brelse(bh);
    return 0;
}

static int ifs_ffs_append_extension_and_data(
    struct inode *inode,
    u32 extension_index,
    u32 data_block,
    struct buffer_head **extension_out)
{
    struct super_block *sb = inode->i_sb;
    struct affs_inode_info *info = AFFS_I(inode);
    struct buffer_head *previous;
    struct buffer_head *new_extension;
    u32 extension_block;

    if (extension_index == 0U ||
        extension_index != info->i_extcnt)
        return -EUCLEAN;

    previous = ifs_ffs_read_extension(inode, extension_index - 1U);
    if (IS_ERR(previous))
        return PTR_ERR(previous);

    if (AFFS_TAIL(sb, previous)->extension != 0) {
        affs_brelse(previous);
        return -EUCLEAN;
    }

    extension_block = affs_alloc_block(inode, (u32)previous->b_blocknr);
    if (extension_block == 0U) {
        affs_brelse(previous);
        return -ENOSPC;
    }

    new_extension = affs_getzeroblk(sb, extension_block);
    if (!new_extension) {
        affs_free_block(sb, extension_block);
        affs_brelse(previous);
        return -EIO;
    }

    AFFS_HEAD(new_extension)->ptype = cpu_to_be32(T_LIST);
    AFFS_HEAD(new_extension)->key = cpu_to_be32(extension_block);
    AFFS_HEAD(new_extension)->block_count = cpu_to_be32(1U);
    AFFS_HEAD(new_extension)->first_data = cpu_to_be32(data_block);
    AFFS_BLOCK(sb, new_extension, 0U) = cpu_to_be32(data_block);
    AFFS_TAIL(sb, new_extension)->parent =
        cpu_to_be32((u32)inode->i_ino);
    AFFS_TAIL(sb, new_extension)->stype = cpu_to_be32(ST_FILE);
    affs_fix_checksum(sb, new_extension);
    mark_buffer_dirty_inode(new_extension, inode);

    AFFS_TAIL(sb, previous)->extension = cpu_to_be32(extension_block);
    affs_fix_checksum(sb, previous);
    mark_buffer_dirty_inode(previous, inode);
    affs_brelse(previous);

    info->i_extcnt++;
    ifs_ffs_cache_extension(inode, new_extension, extension_index);
    *extension_out = new_extension;
    return 0;
}

static int ifs_ffs_map_existing_block(
    struct inode *inode, u32 logical,
    struct buffer_head *result)
{
    struct super_block *sb = inode->i_sb;
    struct buffer_head *extension;
    u32 extension_index;
    u32 entry_index;
    u32 physical;
    int status;

    status = ifs_ffs_file_block_location(
        logical, (u32)AFFS_SB(sb)->s_hashsize,
        &extension_index, &entry_index);
    if (status != 0)
        return -EUCLEAN;

    extension = ifs_ffs_read_extension(inode, extension_index);
    if (IS_ERR(extension))
        return PTR_ERR(extension);

    physical = be32_to_cpu(
        AFFS_BLOCK(sb, extension, entry_index));
    affs_brelse(extension);

    if (physical == 0U ||
        !affs_validblock(sb, (int)physical))
        return -EUCLEAN;

    map_bh(result, sb, physical);
    return 0;
}

static int ifs_ffs_append_block(
    struct inode *inode, u32 logical,
    struct buffer_head *result)
{
    struct super_block *sb = inode->i_sb;
    struct affs_inode_info *info = AFFS_I(inode);
    struct buffer_head *extension = NULL;
    u32 extension_index;
    u32 entry_index;
    u32 physical;
    int status;

    status = ifs_ffs_file_block_location(
        logical, (u32)AFFS_SB(sb)->s_hashsize,
        &extension_index, &entry_index);
    if (status != 0)
        return -EUCLEAN;

    if (logical != info->i_blkcnt ||
        extension_index > info->i_extcnt)
        return -EUCLEAN;

    if (extension_index < info->i_extcnt) {
        extension = ifs_ffs_read_extension(inode, extension_index);
        if (IS_ERR(extension))
            return PTR_ERR(extension);

        if (AFFS_BLOCK(sb, extension, entry_index) != 0) {
            affs_brelse(extension);
            return -EUCLEAN;
        }

        physical = affs_alloc_block(inode, (u32)extension->b_blocknr);
        if (physical == 0U) {
            affs_brelse(extension);
            return -ENOSPC;
        }

        status = ifs_ffs_zero_new_physical_block(sb, physical);
        if (status != 0) {
            affs_free_block(sb, physical);
            affs_brelse(extension);
            return status;
        }

        AFFS_BLOCK(sb, extension, entry_index) = cpu_to_be32(physical);
        AFFS_HEAD(extension)->block_count =
            cpu_to_be32(entry_index + 1U);
        if (entry_index == 0U)
            AFFS_HEAD(extension)->first_data =
                cpu_to_be32(physical);
        affs_fix_checksum(sb, extension);
        mark_buffer_dirty_inode(extension, inode);
    } else {
        struct buffer_head *previous;

        if (entry_index != 0U || extension_index == 0U)
            return -EUCLEAN;

        previous = ifs_ffs_read_extension(
            inode, extension_index - 1U);
        if (IS_ERR(previous))
            return PTR_ERR(previous);

        physical = affs_alloc_block(
            inode, (u32)previous->b_blocknr);
        affs_brelse(previous);
        if (physical == 0U)
            return -ENOSPC;

        status = ifs_ffs_zero_new_physical_block(sb, physical);
        if (status != 0) {
            affs_free_block(sb, physical);
            return status;
        }

        status = ifs_ffs_append_extension_and_data(
            inode, extension_index, physical, &extension);
        if (status != 0) {
            affs_free_block(sb, physical);
            return status;
        }
    }

    info->i_blkcnt++;
    if (info->mmu_private <= U32_MAX - AFFS_SB(sb)->s_data_blksize)
        info->mmu_private += AFFS_SB(sb)->s_data_blksize;

    map_bh(result, sb, physical);
    set_buffer_new(result);
    affs_brelse(extension);
    mark_inode_dirty(inode);
    return 0;
}

static int ifs_ffs_map_block(
    struct inode *inode, sector_t logical,
    struct buffer_head *result, int create)
{
    struct affs_inode_info *info = AFFS_I(inode);
    int status;

    if (logical > U32_MAX)
        return -EFBIG;

    if (AFFS_SB(inode->i_sb)->s_hashsize <= 0)
        return -EUCLEAN;

    affs_lock_ext(inode);

    if ((u32)logical < info->i_blkcnt) {
        status = ifs_ffs_map_existing_block(
            inode, (u32)logical, result);
    } else if ((u32)logical == info->i_blkcnt && create) {
        status = ifs_ffs_append_block(
            inode, (u32)logical, result);
    } else if ((u32)logical == info->i_blkcnt && !create) {
        status = 0;
    } else {
        status = -EFBIG;
    }

    affs_unlock_ext(inode);
    return status;
}

static int ifs_ffs_writepages(
    struct address_space *mapping, struct writeback_control *control)
{
    return mpage_writepages(mapping, control, ifs_ffs_map_block);
}

static int ifs_ffs_read_folio(
    struct file *file, struct folio *folio)
{
    return block_read_full_folio(folio, ifs_ffs_map_block);
}

static void ifs_ffs_write_failed(
    struct address_space *mapping, loff_t attempted_end)
{
    struct inode *inode = mapping->host;

    if (attempted_end > inode->i_size) {
        truncate_pagecache(inode, inode->i_size);
        affs_truncate(inode);
    }
}

static ssize_t ifs_ffs_direct_io(
    struct kiocb *iocb, struct iov_iter *iter)
{
    struct file *file = iocb->ki_filp;
    struct address_space *mapping = file->f_mapping;
    struct inode *inode = mapping->host;
    const size_t count = iov_iter_count(iter);
    const loff_t offset = iocb->ki_pos;
    ssize_t result;

    if (iov_iter_rw(iter) == WRITE) {
        if (offset < 0 || offset > U32_MAX ||
            count > (size_t)(U32_MAX - (u64)offset))
            return -EFBIG;

        if (AFFS_I(inode)->mmu_private < offset + count)
            return 0;
    }

    result = blockdev_direct_IO(
        iocb, inode, iter, ifs_ffs_map_block);
    if (result < 0 && iov_iter_rw(iter) == WRITE)
        ifs_ffs_write_failed(mapping, offset + count);
    return result;
}

static int ifs_ffs_write_begin(
    struct file *file, struct address_space *mapping,
    loff_t position, unsigned int length,
    struct folio **folio, void **fsdata)
{
    int result;

    if (position < 0 || position > U32_MAX ||
        length > U32_MAX - (u64)position)
        return -EFBIG;

    result = cont_write_begin(
        file, mapping, position, length, folio, fsdata,
        ifs_ffs_map_block,
        &AFFS_I(mapping->host)->mmu_private);
    if (result != 0)
        ifs_ffs_write_failed(mapping, position + length);
    return result;
}

static int ifs_ffs_write_end(
    struct file *file, struct address_space *mapping,
    loff_t position, unsigned int length, unsigned int copied,
    struct folio *folio, void *fsdata)
{
    struct inode *inode = mapping->host;
    const int result = generic_write_end(
        file, mapping, position, length, copied, folio, fsdata);

    if (result > 0) {
        AFFS_I(inode)->mmu_private = inode->i_size;
        if ((AFFS_I(inode)->i_protect & FIBF_ARCHIVED) != 0U) {
            AFFS_I(inode)->i_protect &= ~FIBF_ARCHIVED;
            mark_inode_dirty(inode);
        }
    }
    return result;
}

static sector_t ifs_ffs_bmap(
    struct address_space *mapping, sector_t block)
{
    return generic_block_bmap(
        mapping, block, ifs_ffs_map_block);
}

const struct address_space_operations affs_aops = {
    .dirty_folio = block_dirty_folio,
    .invalidate_folio = block_invalidate_folio,
    .read_folio = ifs_ffs_read_folio,
    .writepages = ifs_ffs_writepages,
    .write_begin = ifs_ffs_write_begin,
    .write_end = ifs_ffs_write_end,
    .direct_IO = ifs_ffs_direct_io,
    .migrate_folio = buffer_migrate_folio,
    .bmap = ifs_ffs_bmap,
};

static int ifs_ffs_map_to_buffer(
    struct inode *inode, u32 logical, bool create,
    bool zero, struct buffer_head **buffer, bool *new_block)
{
    struct buffer_head mapping = { 0 };
    struct buffer_head *bh;
    int result;

    result = ifs_ffs_map_block(
        inode, logical, &mapping, create ? 1 : 0);
    if (result != 0)
        return result;
    if (!buffer_mapped(&mapping))
        return -EUCLEAN;

    if (new_block)
        *new_block = buffer_new(&mapping);

    bh = zero && buffer_new(&mapping) ?
         affs_getzeroblk(inode->i_sb, (u32)mapping.b_blocknr) :
         affs_bread(inode->i_sb, (u32)mapping.b_blocknr);
    if (!bh)
        return -EIO;

    *buffer = bh;
    return 0;
}

static int ifs_ffs_ofs_link_data_block(
    struct inode *inode, u32 logical, u32 physical)
{
    struct buffer_head *previous;
    u32 old_next;
    int result;

    if (logical == 0U)
        return 0;

    result = ifs_ffs_map_to_buffer(
        inode, logical - 1U, false, false,
        &previous, NULL);
    if (result != 0)
        return result;

    if (affs_checksum_block(inode->i_sb, previous) != 0U ||
        be32_to_cpu(AFFS_DATA_HEAD(previous)->ptype) != T_DATA ||
        be32_to_cpu(AFFS_DATA_HEAD(previous)->key) !=
            (u32)inode->i_ino ||
        be32_to_cpu(AFFS_DATA_HEAD(previous)->sequence) != logical) {
        affs_brelse(previous);
        return -EUCLEAN;
    }

    old_next = be32_to_cpu(AFFS_DATA_HEAD(previous)->next);
    if (old_next != 0U && old_next != physical) {
        affs_brelse(previous);
        return -EUCLEAN;
    }

    if (old_next == 0U) {
        AFFS_DATA_HEAD(previous)->next = cpu_to_be32(physical);
        affs_fix_checksum(inode->i_sb, previous);
        mark_buffer_dirty_inode(previous, inode);
    }

    affs_brelse(previous);
    return 0;
}

static int ifs_ffs_ofs_prepare_block(
    struct inode *inode, u32 logical,
    struct buffer_head **buffer)
{
    struct buffer_head *bh;
    bool new_block = false;
    int result;

    result = ifs_ffs_map_to_buffer(
        inode, logical, true, true, &bh, &new_block);
    if (result != 0)
        return result;

    if (new_block) {
        AFFS_DATA_HEAD(bh)->ptype = cpu_to_be32(T_DATA);
        AFFS_DATA_HEAD(bh)->key = cpu_to_be32((u32)inode->i_ino);
        AFFS_DATA_HEAD(bh)->sequence = cpu_to_be32(logical + 1U);
        AFFS_DATA_HEAD(bh)->size = 0;
        AFFS_DATA_HEAD(bh)->next = 0;
        affs_fix_checksum(inode->i_sb, bh);
        mark_buffer_dirty_inode(bh, inode);

        result = ifs_ffs_ofs_link_data_block(
            inode, logical, (u32)bh->b_blocknr);
        if (result != 0) {
            affs_brelse(bh);
            return result;
        }
    } else if (affs_checksum_block(inode->i_sb, bh) != 0U ||
               be32_to_cpu(AFFS_DATA_HEAD(bh)->ptype) != T_DATA ||
               be32_to_cpu(AFFS_DATA_HEAD(bh)->key) !=
                   (u32)inode->i_ino ||
               be32_to_cpu(AFFS_DATA_HEAD(bh)->sequence) !=
                   logical + 1U) {
        affs_brelse(bh);
        return -EUCLEAN;
    }

    *buffer = bh;
    return 0;
}

static int ifs_ffs_ofs_read_range(
    struct inode *inode, struct folio *folio,
    size_t destination_offset, u64 file_offset, size_t length)
{
    const u32 payload = AFFS_SB(inode->i_sb)->s_data_blksize;
    size_t copied = 0U;

    if (payload == 0U)
        return -EUCLEAN;

    while (copied < length) {
        const u64 current = file_offset + copied;
        const u32 logical = (u32)(current / payload);
        const u32 within = (u32)(current % payload);
        const size_t chunk =
            min_t(size_t, payload - within, length - copied);
        struct buffer_head *bh;
        u32 stored_size;
        int result;

        result = ifs_ffs_map_to_buffer(
            inode, logical, false, false, &bh, NULL);
        if (result != 0)
            return result;

        if (affs_checksum_block(inode->i_sb, bh) != 0U ||
            be32_to_cpu(AFFS_DATA_HEAD(bh)->ptype) != T_DATA ||
            be32_to_cpu(AFFS_DATA_HEAD(bh)->key) !=
                (u32)inode->i_ino ||
            be32_to_cpu(AFFS_DATA_HEAD(bh)->sequence) != logical + 1U) {
            affs_brelse(bh);
            return -EUCLEAN;
        }

        stored_size = be32_to_cpu(AFFS_DATA_HEAD(bh)->size);
        if (stored_size > payload ||
            within > stored_size ||
            chunk > stored_size - within) {
            affs_brelse(bh);
            return -EUCLEAN;
        }

        memcpy_to_folio(
            folio, destination_offset + copied,
            AFFS_DATA(bh) + within, chunk);
        affs_brelse(bh);
        copied += chunk;
    }

    return 0;
}

static int ifs_ffs_zero_extend_ffs(
    struct inode *inode, u32 target)
{
    const u32 block_size = AFFS_SB(inode->i_sb)->s_data_blksize;
    u64 position = AFFS_I(inode)->mmu_private;

    if (block_size == 0U)
        return -EUCLEAN;

    while (position < target) {
        const u32 logical = (u32)(position / block_size);
        const u32 within = (u32)(position % block_size);
        const u32 chunk = min_t(
            u32, block_size - within, target - (u32)position);
        struct buffer_head *bh;
        int result;

        result = ifs_ffs_map_to_buffer(
            inode, logical, true, false, &bh, NULL);
        if (result != 0)
            return result;

        memset(bh->b_data + within, 0, chunk);
        mark_buffer_dirty_inode(bh, inode);
        affs_brelse(bh);
        position += chunk;
    }

    AFFS_I(inode)->mmu_private = target;
    return 0;
}

static int ifs_ffs_zero_extend_ofs(
    struct inode *inode, u32 target)
{
    const u32 payload = AFFS_SB(inode->i_sb)->s_data_blksize;
    u64 position = AFFS_I(inode)->mmu_private;

    if (payload == 0U)
        return -EUCLEAN;

    while (position < target) {
        const u32 logical = (u32)(position / payload);
        const u32 within = (u32)(position % payload);
        const u32 chunk = min_t(
            u32, payload - within, target - (u32)position);
        struct buffer_head *bh;
        u32 stored_size;
        int result;

        result = ifs_ffs_ofs_prepare_block(inode, logical, &bh);
        if (result != 0)
            return result;

        stored_size = be32_to_cpu(AFFS_DATA_HEAD(bh)->size);
        if (stored_size > payload || stored_size < within) {
            affs_brelse(bh);
            return -EUCLEAN;
        }

        memset(AFFS_DATA(bh) + within, 0, chunk);
        if (stored_size < within + chunk)
            AFFS_DATA_HEAD(bh)->size =
                cpu_to_be32(within + chunk);
        affs_fix_checksum(inode->i_sb, bh);
        mark_buffer_dirty_inode(bh, inode);
        affs_brelse(bh);
        position += chunk;
    }

    AFFS_I(inode)->mmu_private = target;
    return 0;
}

static int ifs_ffs_ofs_read_folio(
    struct file *file, struct folio *folio)
{
    struct inode *inode = folio->mapping->host;
    const u64 start = folio_pos(folio);
    size_t length = 0U;
    int result = 0;

    if (start < (u64)inode->i_size)
        length = min_t(
            u64, folio_size(folio), (u64)inode->i_size - start);

    if (length != 0U)
        result = ifs_ffs_ofs_read_range(
            inode, folio, 0U, start, length);

    if (result == 0) {
        if (length < folio_size(folio))
            folio_zero_segment(folio, length, folio_size(folio));
        folio_mark_uptodate(folio);
    }

    folio_unlock(folio);
    return result;
}

static int ifs_ffs_ofs_write_begin(
    struct file *file, struct address_space *mapping,
    loff_t position, unsigned int length,
    struct folio **folio_out, void **fsdata)
{
    struct inode *inode = mapping->host;
    struct folio *folio;
    const u64 end = (u64)position + length;
    const pgoff_t index = position >> PAGE_SHIFT;
    const u64 folio_start = (u64)index << PAGE_SHIFT;
    size_t readable = 0U;
    int result;

    if (position < 0 || end > U32_MAX)
        return -EFBIG;

    if ((u64)position > (u64)AFFS_I(inode)->mmu_private) {
        result = ifs_ffs_zero_extend_ofs(
            inode, (u32)position);
        if (result != 0)
            return result;
    }

    folio = __filemap_get_folio(
        mapping, index, FGP_WRITEBEGIN,
        mapping_gfp_mask(mapping));
    if (IS_ERR(folio))
        return PTR_ERR(folio);

    *folio_out = folio;

    if (folio_test_uptodate(folio))
        return 0;

    if (folio_start < (u64)inode->i_size)
        readable = min_t(
            u64, folio_size(folio),
            (u64)inode->i_size - folio_start);

    if (readable != 0U) {
        result = ifs_ffs_ofs_read_range(
            inode, folio, 0U, folio_start, readable);
        if (result != 0) {
            folio_unlock(folio);
            folio_put(folio);
            return result;
        }
    }

    if (readable < folio_size(folio))
        folio_zero_segment(folio, readable, folio_size(folio));
    folio_mark_uptodate(folio);
    return 0;
}

static int ifs_ffs_ofs_write_end(
    struct file *file, struct address_space *mapping,
    loff_t position, unsigned int length, unsigned int copied,
    struct folio *folio, void *fsdata)
{
    struct inode *inode = mapping->host;
    struct super_block *sb = inode->i_sb;
    const u32 payload = AFFS_SB(sb)->s_data_blksize;
    const unsigned int from =
        (unsigned int)(position & (PAGE_SIZE - 1));
    const char *data = folio_address(folio);
    u32 written = 0U;
    int error = 0;

    if (copied > length)
        copied = length;

    while (written < copied) {
        const u64 absolute = (u64)position + written;
        const u32 logical = (u32)(absolute / payload);
        const u32 within = (u32)(absolute % payload);
        const u32 chunk = min_t(
            u32, payload - within, copied - written);
        struct buffer_head *bh;
        u32 stored_size;

        error = ifs_ffs_ofs_prepare_block(
            inode, logical, &bh);
        if (error != 0)
            break;

        stored_size = be32_to_cpu(AFFS_DATA_HEAD(bh)->size);
        if (stored_size > payload) {
            affs_brelse(bh);
            error = -EUCLEAN;
            break;
        }

        memcpy(
            AFFS_DATA(bh) + within,
            data + from + written, chunk);
        if (stored_size < within + chunk)
            AFFS_DATA_HEAD(bh)->size =
                cpu_to_be32(within + chunk);
        affs_fix_checksum(sb, bh);
        mark_buffer_dirty_inode(bh, inode);
        affs_brelse(bh);
        written += chunk;
    }

    if (written != 0U) {
        const u64 end = (u64)position + written;

        if (end > (u64)inode->i_size)
            inode->i_size = (loff_t)end;
        AFFS_I(inode)->mmu_private = inode->i_size;

        if ((AFFS_I(inode)->i_protect & FIBF_ARCHIVED) != 0U) {
            AFFS_I(inode)->i_protect &= ~FIBF_ARCHIVED;
            mark_inode_dirty(inode);
        }
        folio_mark_uptodate(folio);
    }

    folio_unlock(folio);
    folio_put(folio);

    if (written != 0U)
        return (int)written;
    return error;
}

const struct address_space_operations affs_aops_ofs = {
    .dirty_folio = block_dirty_folio,
    .invalidate_folio = block_invalidate_folio,
    .read_folio = ifs_ffs_ofs_read_folio,
    .write_begin = ifs_ffs_ofs_write_begin,
    .write_end = ifs_ffs_ofs_write_end,
    .migrate_folio = filemap_migrate_folio,
};

void affs_free_prealloc(struct inode *inode)
{
    struct affs_inode_info *info = AFFS_I(inode);

    while (info->i_pa_cnt > 0) {
        const u32 block = info->i_lastalloc + 1U;

        info->i_pa_cnt--;
        info->i_lastalloc = block;
        affs_free_block(inode->i_sb, block);
    }
}

static int ifs_ffs_zero_last_block_tail(
    struct inode *inode, u32 target, u32 kept_blocks)
{
    struct buffer_head *bh;
    const u32 payload = AFFS_SB(inode->i_sb)->s_data_blksize;
    const u32 used = target % payload;
    int result;

    if (target == 0U)
        return 0;

    if (affs_test_opt(AFFS_SB(inode->i_sb)->s_flags, SF_OFS)) {
        const u32 logical_size = used == 0U ? payload : used;

        result = ifs_ffs_ofs_prepare_block(
            inode, kept_blocks - 1U, &bh);
        if (result != 0)
            return result;

        if (logical_size < payload)
            memset(
                AFFS_DATA(bh) + logical_size, 0,
                payload - logical_size);
        AFFS_DATA_HEAD(bh)->size = cpu_to_be32(logical_size);
        AFFS_DATA_HEAD(bh)->next = 0;
        affs_fix_checksum(inode->i_sb, bh);
        mark_buffer_dirty_inode(bh, inode);
        affs_brelse(bh);
        return 0;
    }

    if (used == 0U)
        return 0;

    result = ifs_ffs_map_to_buffer(
        inode, kept_blocks - 1U, false, false,
        &bh, NULL);
    if (result != 0)
        return result;

    memset(bh->b_data + used, 0, payload - used);
    mark_buffer_dirty_inode(bh, inode);
    affs_brelse(bh);
    return 0;
}

static int ifs_ffs_shrink_file(
    struct inode *inode, u32 target)
{
    struct super_block *sb = inode->i_sb;
    struct affs_inode_info *info = AFFS_I(inode);
    const u32 entries = (u32)AFFS_SB(sb)->s_hashsize;
    const u32 payload = AFFS_SB(sb)->s_data_blksize;
    struct buffer_head *kept_extension;
    u32 kept_blocks;
    u32 kept_extensions;
    u32 last_extension_index;
    u32 first_free_entry;
    u32 next_extension;
    u32 entry;
    int result;

    if (ifs_ffs_file_block_count(
            target, payload, &kept_blocks) != 0 ||
        ifs_ffs_file_extension_count(
            kept_blocks, entries, &kept_extensions) != 0)
        return -EUCLEAN;

    if (kept_blocks > info->i_blkcnt ||
        kept_extensions > info->i_extcnt)
        return -EUCLEAN;

    affs_free_prealloc(inode);
    affs_lock_ext(inode);
    ifs_ffs_clear_extension_cache(inode);

    last_extension_index = kept_extensions - 1U;
    kept_extension = affs_bread(sb, (u32)inode->i_ino);
    if (!kept_extension) {
        result = -EIO;
        goto out_unlock;
    }

    for (entry = 0U; entry < last_extension_index; ++entry) {
        const u32 next =
            be32_to_cpu(AFFS_TAIL(sb, kept_extension)->extension);
        struct buffer_head *next_bh;

        if (next == 0U || !affs_validblock(sb, (int)next)) {
            affs_brelse(kept_extension);
            result = -EUCLEAN;
            goto out_unlock;
        }
        next_bh = affs_bread(sb, next);
        if (!next_bh) {
            affs_brelse(kept_extension);
            result = -EIO;
            goto out_unlock;
        }
        affs_brelse(kept_extension);
        kept_extension = next_bh;
    }

    if (!ifs_ffs_extension_valid(
            inode, kept_extension, last_extension_index)) {
        affs_brelse(kept_extension);
        result = -EUCLEAN;
        goto out_unlock;
    }

    first_free_entry =
        kept_blocks == 0U ? 0U : kept_blocks % entries;
    if (kept_blocks != 0U && first_free_entry == 0U)
        first_free_entry = entries;

    {
        const u32 old_count =
            be32_to_cpu(AFFS_HEAD(kept_extension)->block_count);

        if (old_count > entries || first_free_entry > old_count) {
            affs_brelse(kept_extension);
            result = -EUCLEAN;
            goto out_unlock;
        }

        for (entry = old_count; entry < entries; ++entry) {
            if (AFFS_BLOCK(sb, kept_extension, entry) != 0) {
                affs_brelse(kept_extension);
                result = -EUCLEAN;
                goto out_unlock;
            }
        }

        for (entry = first_free_entry; entry < old_count; ++entry) {
            const u32 physical = be32_to_cpu(
                AFFS_BLOCK(sb, kept_extension, entry));

            if (physical == 0U ||
                !affs_validblock(sb, (int)physical)) {
                affs_brelse(kept_extension);
                result = -EUCLEAN;
                goto out_unlock;
            }
            affs_free_block(sb, physical);
            AFFS_BLOCK(sb, kept_extension, entry) = 0;
        }
    }

    next_extension =
        be32_to_cpu(AFFS_TAIL(sb, kept_extension)->extension);
    AFFS_TAIL(sb, kept_extension)->extension = 0;
    AFFS_HEAD(kept_extension)->block_count =
        cpu_to_be32(
            first_free_entry == entries ? entries :
            first_free_entry);
    if (kept_blocks == 0U)
        AFFS_HEAD(kept_extension)->first_data = 0;
    affs_fix_checksum(sb, kept_extension);
    mark_buffer_dirty_inode(kept_extension, inode);
    affs_brelse(kept_extension);

    while (next_extension != 0U) {
        struct buffer_head *bh;
        u32 following;

        if (!affs_validblock(sb, (int)next_extension)) {
            result = -EUCLEAN;
            goto out_unlock;
        }

        bh = affs_bread(sb, next_extension);
        if (!bh) {
            result = -EIO;
            goto out_unlock;
        }

        if (!ifs_ffs_extension_valid(
                inode, bh, kept_extensions)) {
            affs_brelse(bh);
            result = -EUCLEAN;
            goto out_unlock;
        }

        following = be32_to_cpu(AFFS_TAIL(sb, bh)->extension);
        {
            const u32 count =
                be32_to_cpu(AFFS_HEAD(bh)->block_count);

            if (count > entries) {
                affs_brelse(bh);
                result = -EUCLEAN;
                goto out_unlock;
            }

            for (entry = count; entry < entries; ++entry) {
                if (AFFS_BLOCK(sb, bh, entry) != 0) {
                    affs_brelse(bh);
                    result = -EUCLEAN;
                    goto out_unlock;
                }
            }

            for (entry = 0U; entry < count; ++entry) {
                const u32 physical =
                    be32_to_cpu(AFFS_BLOCK(sb, bh, entry));

                if (physical == 0U ||
                    !affs_validblock(sb, (int)physical)) {
                    affs_brelse(bh);
                    result = -EUCLEAN;
                    goto out_unlock;
                }
                affs_free_block(sb, physical);
            }
        }

        affs_brelse(bh);
        affs_free_block(sb, next_extension);
        next_extension = following;
        kept_extensions++;
    }

    info->i_blkcnt = kept_blocks;
    if (ifs_ffs_file_extension_count(
            kept_blocks, entries, &info->i_extcnt) != 0) {
        result = -EUCLEAN;
        goto out_unlock;
    }
    info->mmu_private = target;
    result = 0;

out_unlock:
    affs_unlock_ext(inode);
    if (result == 0)
        result = ifs_ffs_zero_last_block_tail(
            inode, target, kept_blocks);
    return result;
}

void affs_truncate(struct inode *inode)
{
    const loff_t requested = inode->i_size;
    const loff_t previous = AFFS_I(inode)->mmu_private;
    int result;

    if (requested < 0 || requested > U32_MAX) {
        inode->i_size = previous;
        return;
    }

    if (requested == previous)
        return;

    if (requested > previous) {
        if (affs_test_opt(AFFS_SB(inode->i_sb)->s_flags, SF_OFS))
            result = ifs_ffs_zero_extend_ofs(
                inode, (u32)requested);
        else
            result = ifs_ffs_zero_extend_ffs(
                inode, (u32)requested);
    } else {
        result = ifs_ffs_shrink_file(
            inode, (u32)requested);
    }

    if (result != 0) {
        if (requested > previous) {
            inode->i_size = previous;
            (void)ifs_ffs_shrink_file(
                inode, (u32)previous);
        }
        inode->i_size = previous;
        AFFS_I(inode)->mmu_private = previous;
        truncate_pagecache(inode, previous);
        affs_warning(
            inode->i_sb, "affs_truncate",
            "Could not resize inode %lu: %d",
            inode->i_ino, result);
    }

    mark_inode_dirty(inode);
}

int affs_file_fsync(
    struct file *file, loff_t start, loff_t end, int datasync)
{
    struct inode *inode = file->f_mapping->host;
    int result;
    int block_result;

    result = file_write_and_wait_range(file, start, end);
    if (result != 0)
        return result;

    inode_lock(inode);
    result = write_inode_now(inode, 0);
    block_result = sync_blockdev(inode->i_sb->s_bdev);
    if (result == 0)
        result = block_result;
    inode_unlock(inode);
    return result;
}

const struct file_operations affs_file_operations = {
    .llseek = generic_file_llseek,
    .read_iter = generic_file_read_iter,
    .write_iter = generic_file_write_iter,
    .mmap = generic_file_mmap,
    .open = ifs_ffs_file_open,
    .release = ifs_ffs_file_release,
    .fsync = affs_file_fsync,
    .splice_read = filemap_splice_read,
};

const struct inode_operations affs_file_inode_operations = {
    .setattr = affs_notify_change,
};
