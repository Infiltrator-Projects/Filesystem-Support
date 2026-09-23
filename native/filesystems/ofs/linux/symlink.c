/*
 * Project-authored Linux adapter for canonical AmigaDOS symlink semantics.
 * Filesystem path interpretation lives in amiga_dos_core.c.
 */

#include "affs.h"

#define IFS_AMIGA_SYMLINK_MAX 1024U

static int affs_symlink_read_folio(struct file *file, struct folio *folio)
{
    struct inode *inode = folio->mapping->host;
    struct affs_sb_info *sbi = AFFS_SB(inode->i_sb);
    struct buffer_head *bh;
    struct slink_front *front;
    const char *prefix;
    char *link = folio_address(folio);
    size_t source_capacity;
    size_t prefix_length;
    size_t output_capacity;
    ifs_amiga_u32 output_length = 0U;
    IfsAmigaSymlinkStatus status;

    pr_debug("get_link(ino=%lu)\n", inode->i_ino);

    bh = affs_bread(inode->i_sb, inode->i_ino);
    if (!bh)
        goto io_error;

    if (bh->b_size <= sizeof(*front))
        goto malformed;

    front = (struct slink_front *)bh->b_data;
    source_capacity = bh->b_size - sizeof(*front);
    output_capacity = min_t(size_t, folio_size(folio),
                            (size_t)IFS_AMIGA_SYMLINK_MAX);

    spin_lock(&sbi->symlink_lock);
    prefix = sbi->s_prefix ? sbi->s_prefix : "/";
    prefix_length = strnlen(prefix, IFS_AMIGA_SYMLINK_MAX);

    status = ifs_amiga_translate_symlink(
        front->symname,
        (ifs_amiga_u32)source_capacity,
        (const ifs_amiga_u8 *)prefix,
        (ifs_amiga_u32)prefix_length,
        (ifs_amiga_u8 *)link,
        (ifs_amiga_u32)output_capacity,
        &output_length);
    spin_unlock(&sbi->symlink_lock);

    affs_brelse(bh);

    if (status != IFS_AMIGA_SYMLINK_OK)
        goto io_error_no_buffer;

    folio_mark_uptodate(folio);
    folio_unlock(folio);
    return 0;

malformed:
    affs_brelse(bh);
io_error_no_buffer:
    folio_unlock(folio);
    return -EIO;

io_error:
    folio_unlock(folio);
    return -EIO;
}

const struct address_space_operations affs_symlink_aops = {
    .read_folio = affs_symlink_read_folio,
};

const struct inode_operations affs_symlink_inode_operations = {
    .get_link = page_get_link,
    .setattr = affs_notify_change,
};
