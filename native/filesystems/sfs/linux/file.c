#include <linux/buffer_head.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/mpage.h>
#include <linux/pagemap.h>
#include "asfs_fs.h"

static int sfs_load_extent(
    struct super_block *sb,
    u32 key,
    struct fsExtentBNode *extent)
{
    struct buffer_head *bh = NULL;
    struct fsExtentBNode *disk_extent = NULL;
    int result;

    if (!extent || key == 0U)
        return -EUCLEAN;

    result = asfs_getextent(sb, key, &bh, &disk_extent);
    if (result != 0)
        return result;

    extent->key = disk_extent->key;
    extent->next = disk_extent->next;
    extent->blocks = disk_extent->blocks;
    asfs_brelse(bh);

    if (ifs_sfs_validate_extent(
            be32_to_cpu(extent->key),
            be32_to_cpu(extent->next),
            be16_to_cpu(extent->blocks),
            ASFS_SB(sb)->totalblocks) != 0)
        return -EUCLEAN;

    return 0;
}

#ifdef CONFIG_ASFS_RW
static int sfs_extend_file_to_block(
    struct inode *inode,
    sector_t block)
{
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh = NULL;
    struct fsObject *object = NULL;
    u32 requested;
    u32 new_space = 0U;
    u32 added = 0U;
    int result;

    if (block > U32_MAX || block < inode->i_blocks)
        return block < inode->i_blocks ? 0 : -EFBIG;

    requested = (u32)block - (u32)inode->i_blocks + 1U;
    if (requested < ASFS_BLOCKCHUNKS)
        requested = ASFS_BLOCKCHUNKS;

    result = asfs_readobject(
        sb, (u32)inode->i_ino, &bh, &object);
    if (result != 0)
        return result;

    result = asfs_addblockstofile(
        sb, bh, object, requested, &new_space, &added);
    if (result == 0) {
        if ((u64)added * sb->s_blocksize >
            U64_MAX - (u64)ASFS_I(inode)->mmu_private) {
            result = -EOVERFLOW;
        } else {
            ASFS_I(inode)->mmu_private +=
                (u64)added * sb->s_blocksize;
            inode->i_blocks += added;
            ASFS_I(inode)->ext_cache.key = 0U;
            ASFS_I(inode)->firstblock =
                be32_to_cpu(object->object.file.data);
            ASFS_I(inode)->modified = TRUE;
        }
    }

    asfs_brelse(bh);
    return result;
}
#endif

static int sfs_map_block(
    struct inode *inode,
    sector_t logical_block,
    struct buffer_head *result_bh,
    int create)
{
    struct super_block *sb = inode->i_sb;
    struct fsExtentBNode extent;
    u32 logical_start = 0U;
    u32 traversed = 0U;
    u32 physical;
    int result = 0;

    if (!result_bh)
        return -EINVAL;
    if (logical_block > U32_MAX)
        return -EFBIG;

#ifndef CONFIG_ASFS_RW
    if (create)
        return -EROFS;
#endif

    if (!create && logical_block >= inode->i_blocks)
        return -EIO;

    mutex_lock(&ASFS_SB(sb)->lock);

#ifdef CONFIG_ASFS_RW
    if (create && logical_block >= inode->i_blocks) {
        result = sfs_extend_file_to_block(inode, logical_block);
        if (result != 0)
            goto out_unlock;
    }
#endif

    if (ASFS_I(inode)->firstblock == 0U) {
        result = -EUCLEAN;
        goto out_unlock;
    }

    if (ASFS_I(inode)->ext_cache.key != 0U &&
        ASFS_I(inode)->ext_cache.startblock <= logical_block) {
        extent.key = cpu_to_be32(ASFS_I(inode)->ext_cache.key);
        extent.next = cpu_to_be32(ASFS_I(inode)->ext_cache.next);
        extent.blocks = cpu_to_be16(ASFS_I(inode)->ext_cache.blocks);
        logical_start = ASFS_I(inode)->ext_cache.startblock;

        if (ifs_sfs_validate_extent(
                ASFS_I(inode)->ext_cache.key,
                ASFS_I(inode)->ext_cache.next,
                ASFS_I(inode)->ext_cache.blocks,
                ASFS_SB(sb)->totalblocks) != 0) {
            ASFS_I(inode)->ext_cache.key = 0U;
            logical_start = 0U;
            result = sfs_load_extent(
                sb, ASFS_I(inode)->firstblock, &extent);
            if (result != 0)
                goto out_unlock;
        }
    } else {
        result = sfs_load_extent(
            sb, ASFS_I(inode)->firstblock, &extent);
        if (result != 0)
            goto out_unlock;
    }

    while ((u64)logical_start + be16_to_cpu(extent.blocks) <=
           logical_block) {
        const u32 next = be32_to_cpu(extent.next);

        if (next == 0U) {
            result = -EUCLEAN;
            goto out_unlock;
        }
        if (++traversed > ASFS_SB(sb)->totalblocks) {
            result = -EUCLEAN;
            goto out_unlock;
        }

        logical_start += be16_to_cpu(extent.blocks);
        result = sfs_load_extent(sb, next, &extent);
        if (result != 0)
            goto out_unlock;
    }

    physical =
        be32_to_cpu(extent.key) +
        (u32)logical_block - logical_start;
    if (physical >= ASFS_SB(sb)->totalblocks) {
        result = -EUCLEAN;
        goto out_unlock;
    }

    ASFS_I(inode)->ext_cache.startblock = logical_start;
    ASFS_I(inode)->ext_cache.key = be32_to_cpu(extent.key);
    ASFS_I(inode)->ext_cache.next = be32_to_cpu(extent.next);
    ASFS_I(inode)->ext_cache.blocks = be16_to_cpu(extent.blocks);

    map_bh(result_bh, sb, physical);
    if (create)
        set_buffer_new(result_bh);

out_unlock:
    mutex_unlock(&ASFS_SB(sb)->lock);
    return result;
}

int asfs_read_folio(struct file *file, struct folio *folio)
{
    (void)file;
    return mpage_read_folio(folio, sfs_map_block);
}

void asfs_readahead(struct readahead_control *rac)
{
    mpage_readahead(rac, sfs_map_block);
}

sector_t asfs_bmap(struct address_space *mapping, sector_t block)
{
    return generic_block_bmap(mapping, block, sfs_map_block);
}

#ifdef CONFIG_ASFS_RW

int asfs_writepages(
    struct address_space *mapping,
    struct writeback_control *writeback)
{
    return mpage_writepages(mapping, writeback, sfs_map_block);
}

int asfs_write_begin(
    struct file *file,
    struct address_space *mapping,
    loff_t position,
    unsigned int length,
    struct folio **folio,
    void **fsdata)
{
    (void)file;
    (void)fsdata;
    return block_write_begin(
        mapping, position, length, folio, sfs_map_block);
}

int asfs_truncate(struct inode *inode)
{
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh = NULL;
    struct fsObject *object = NULL;
    int result;

    if (inode->i_size > ASFS_I(inode)->mmu_private)
        return -EOPNOTSUPP;

    mutex_lock(&ASFS_SB(sb)->lock);

    result = asfs_readobject(
        sb, (u32)inode->i_ino, &bh, &object);
    if (result != 0)
        goto out_unlock;

    result = asfs_truncateblocksinfile(
        sb, bh, object, (u32)inode->i_size);
    if (result == 0) {
        object->object.file.size =
            cpu_to_be32((u32)inode->i_size);
        ASFS_I(inode)->mmu_private = inode->i_size;
        ASFS_I(inode)->modified = TRUE;
        ASFS_I(inode)->ext_cache.key = 0U;
        inode->i_blocks = DIV_ROUND_UP(
            (u64)inode->i_size, sb->s_blocksize);
        asfs_bstore(sb, bh);
    }

    asfs_brelse(bh);

out_unlock:
    mutex_unlock(&ASFS_SB(sb)->lock);
    return result;
}

int asfs_file_open(struct inode *inode, struct file *file)
{
    (void)file;
    atomic_inc(&ASFS_I(inode)->i_opencnt);
    return 0;
}

int asfs_file_release(struct inode *inode, struct file *file)
{
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh = NULL;
    struct fsObject *object = NULL;
    int result = 0;

    (void)file;

    if (!atomic_dec_and_test(&ASFS_I(inode)->i_opencnt))
        return 0;
    if (ASFS_I(inode)->modified != TRUE)
        return 0;

    mutex_lock(&ASFS_SB(sb)->lock);

    result = asfs_readobject(
        sb, (u32)inode->i_ino, &bh, &object);
    if (result != 0)
        goto out_unlock;

    object->datemodified = cpu_to_be32(
        (u32)(inode_get_mtime_sec(inode) -
              (365LL * 8LL + 2LL) * 24LL * 60LL * 60LL));

    if (S_ISREG(inode->i_mode)) {
        result = asfs_truncateblocksinfile(
            sb, bh, object, (u32)inode->i_size);
        if (result == 0) {
            object->object.file.size =
                cpu_to_be32((u32)inode->i_size);
            ASFS_I(inode)->mmu_private = inode->i_size;
            inode->i_blocks = DIV_ROUND_UP(
                (u64)inode->i_size, sb->s_blocksize);
            ASFS_I(inode)->ext_cache.key = 0U;
        }
    }

    if (result == 0)
        asfs_bstore(sb, bh);

    asfs_brelse(bh);

out_unlock:
    mutex_unlock(&ASFS_SB(sb)->lock);
    if (result == 0)
        ASFS_I(inode)->modified = FALSE;
    return result;
}

#endif
