/*
 * Project-authored Linux object-metadata adapter for Amiga OFS. Format-level checksum and name rules are delegated to the
 * canonical amiga_dos_core.
 */

#include <linux/math64.h>
#include <linux/iversion.h>

static int ifs_amiga_hash_slot(
    struct super_block *sb, const unsigned char *name, unsigned int length)
{
    const int slot = affs_hash_name(sb, name, length);

    if (slot < 0 || slot >= AFFS_SB(sb)->s_hashsize)
        return -EUCLEAN;
    return slot;
}

int affs_insert_hash(struct inode *dir, struct buffer_head *entry_bh)
{
    struct super_block *sb = dir->i_sb;
    struct buffer_head *cursor = NULL;
    struct affs_tail *entry_tail = AFFS_TAIL(sb, entry_bh);
    const u32 entry_block = (u32)entry_bh->b_blocknr;
    u32 next;
    u32 budget = ifs_amiga_chain_budget(sb);
    int slot;

    slot = ifs_amiga_hash_slot(
        sb, entry_tail->name + 1, entry_tail->name[0]);
    if (slot < 0)
        return slot;

    cursor = affs_bread(sb, (u32)dir->i_ino);
    if (!cursor)
        return -EIO;

    for (;;) {
        if (cursor->b_blocknr == dir->i_ino)
            next = be32_to_cpu(AFFS_HEAD(cursor)->table[slot]);
        else
            next = be32_to_cpu(AFFS_TAIL(sb, cursor)->hash_chain);

        if (next == 0U)
            break;
        if (budget-- == 0U) {
            affs_brelse(cursor);
            return -EUCLEAN;
        }

        affs_brelse(cursor);
        cursor = affs_bread(sb, next);
        if (!cursor)
            return -EIO;
    }

    entry_tail->parent = cpu_to_be32((u32)dir->i_ino);
    entry_tail->hash_chain = 0;
    affs_fix_checksum(sb, entry_bh);

    if (cursor->b_blocknr == dir->i_ino)
        AFFS_HEAD(cursor)->table[slot] = cpu_to_be32(entry_block);
    else
        AFFS_TAIL(sb, cursor)->hash_chain = cpu_to_be32(entry_block);

    affs_adjust_checksum(cursor, entry_block);
    mark_buffer_dirty_inode(cursor, dir);
    affs_brelse(cursor);

    inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
    inode_inc_iversion(dir);
    mark_inode_dirty(dir);
    return 0;
}

int affs_remove_hash(struct inode *dir, struct buffer_head *remove_bh)
{
    struct super_block *sb = dir->i_sb;
    struct affs_tail *remove_tail = AFFS_TAIL(sb, remove_bh);
    struct buffer_head *cursor = NULL;
    const u32 remove_block = (u32)remove_bh->b_blocknr;
    const __be32 replacement = remove_tail->hash_chain;
    u32 next;
    u32 budget = ifs_amiga_chain_budget(sb);
    int slot;
    int result = -ENOENT;

    slot = ifs_amiga_hash_slot(
        sb, remove_tail->name + 1, remove_tail->name[0]);
    if (slot < 0)
        return slot;

    cursor = affs_bread(sb, (u32)dir->i_ino);
    if (!cursor)
        return -EIO;

    for (;;) {
        __be32 *link_field;

        if (cursor->b_blocknr == dir->i_ino)
            link_field = &AFFS_HEAD(cursor)->table[slot];
        else
            link_field = &AFFS_TAIL(sb, cursor)->hash_chain;

        next = be32_to_cpu(*link_field);
        if (next == 0U)
            break;

        if (next == remove_block) {
            *link_field = replacement;
            affs_adjust_checksum(
                cursor, be32_to_cpu(replacement) - remove_block);
            mark_buffer_dirty_inode(cursor, dir);
            remove_tail->parent = 0;
            result = 0;
            break;
        }

        if (budget-- == 0U) {
            result = -EUCLEAN;
            break;
        }

        affs_brelse(cursor);
        cursor = affs_bread(sb, next);
        if (!cursor)
            return -EIO;
    }

    affs_brelse(cursor);

    if (result == 0) {
        inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
        inode_inc_iversion(dir);
        mark_inode_dirty(dir);
    }
    return result;
}

static void ifs_amiga_retarget_cached_dentry(
    struct inode *inode, const u32 old_block)
{
    struct dentry *alias;

    spin_lock(&inode->i_lock);
    hlist_for_each_entry(alias, &inode->i_dentry, d_u.d_alias) {
        if (old_block == (u32)(unsigned long)alias->d_fsdata) {
            alias->d_fsdata = (void *)(unsigned long)inode->i_ino;
            break;
        }
    }
    spin_unlock(&inode->i_lock);
}

static int ifs_amiga_directory_empty(struct inode *inode)
{
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh;
    int index;
    int result = 0;

    bh = affs_bread(sb, (u32)inode->i_ino);
    if (!bh)
        return -EIO;

    for (index = 0; index < AFFS_SB(sb)->s_hashsize; ++index) {
        if (AFFS_HEAD(bh)->table[index] != 0) {
            result = -ENOTEMPTY;
            break;
        }
    }

    affs_brelse(bh);
    return result;
}

static int ifs_amiga_promote_link_head(
    struct inode *inode,
    struct buffer_head *head_bh,
    const u32 promoted_block,
    struct buffer_head **promoted_bh_out)
{
    struct super_block *sb = inode->i_sb;
    struct buffer_head *promoted_bh;
    struct inode *dir;
    unsigned char original_name[32];
    int result;

    promoted_bh = affs_bread(sb, promoted_block);
    if (!promoted_bh)
        return -EIO;

    dir = affs_iget(
        sb, be32_to_cpu(AFFS_TAIL(sb, promoted_bh)->parent));
    if (IS_ERR(dir)) {
        result = PTR_ERR(dir);
        affs_brelse(promoted_bh);
        return result;
    }

    memcpy(original_name, AFFS_TAIL(sb, head_bh)->name,
           sizeof(original_name));

    affs_lock_dir(dir);
    ifs_amiga_retarget_cached_dentry(inode, promoted_block);

    result = affs_remove_hash(dir, promoted_bh);
    if (result != 0)
        goto out_unlock;

    memcpy(AFFS_TAIL(sb, head_bh)->name,
           AFFS_TAIL(sb, promoted_bh)->name, 32U);
    affs_fix_checksum(sb, head_bh);

    result = affs_insert_hash(dir, head_bh);
    if (result != 0) {
        memcpy(AFFS_TAIL(sb, head_bh)->name,
               original_name, sizeof(original_name));
        affs_fix_checksum(sb, head_bh);
        if (affs_insert_hash(dir, promoted_bh) != 0)
            affs_error(sb, "ifs_amiga_promote_link_head",
                       "Could not restore link after promotion failure");
        goto out_unlock;
    }

    mark_buffer_dirty_inode(head_bh, inode);
    mark_buffer_dirty_inode(promoted_bh, inode);

out_unlock:
    affs_unlock_dir(dir);
    iput(dir);

    if (result != 0) {
        affs_brelse(promoted_bh);
        return result;
    }

    *promoted_bh_out = promoted_bh;
    return 0;
}

static int ifs_amiga_remove_link(struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);
    struct super_block *sb = inode->i_sb;
    struct buffer_head *cursor = NULL;
    struct buffer_head *link_bh = NULL;
    u32 link_block = (u32)(unsigned long)dentry->d_fsdata;
    u32 next;
    u32 budget = ifs_amiga_chain_budget(sb);
    int result = -ENOENT;

    cursor = affs_bread(sb, (u32)inode->i_ino);
    if (!cursor)
        return -EIO;

    if (link_block == (u32)inode->i_ino) {
        link_block = be32_to_cpu(AFFS_TAIL(sb, cursor)->link_chain);
        if (link_block == 0U) {
            result = -EUCLEAN;
            goto out;
        }

        result = ifs_amiga_promote_link_head(
            inode, cursor, link_block, &link_bh);
        if (result != 0)
            goto out;
    } else {
        link_bh = affs_bread(sb, link_block);
        if (!link_bh) {
            result = -EIO;
            goto out;
        }
    }

    for (;;) {
        next = be32_to_cpu(AFFS_TAIL(sb, cursor)->link_chain);
        if (next == 0U)
            break;

        if (next == link_block) {
            const __be32 replacement =
                AFFS_TAIL(sb, link_bh)->link_chain;

            AFFS_TAIL(sb, cursor)->link_chain = replacement;
            affs_adjust_checksum(
                cursor, be32_to_cpu(replacement) - link_block);
            mark_buffer_dirty_inode(cursor, inode);

            switch (be32_to_cpu(AFFS_TAIL(sb, cursor)->stype)) {
            case ST_LINKDIR:
            case ST_LINKFILE:
                break;
            default:
                if (replacement == 0)
                    set_nlink(inode, 1);
                break;
            }

            affs_free_block(sb, link_block);
            result = 0;
            break;
        }

        if (budget-- == 0U) {
            result = -EUCLEAN;
            break;
        }

        affs_brelse(cursor);
        cursor = affs_bread(sb, next);
        if (!cursor) {
            result = -EIO;
            break;
        }
    }

out:
    affs_brelse(link_bh);
    affs_brelse(cursor);
    return result;
}

int affs_remove_header(struct dentry *dentry)
{
    struct inode *dir = d_inode(dentry->d_parent);
    struct inode *inode = d_inode(dentry);
    struct super_block *sb;
    struct buffer_head *bh = NULL;
    int result;

    if (!dir || !inode)
        return -ENOENT;

    sb = dir->i_sb;
    bh = affs_bread(sb, (u32)(unsigned long)dentry->d_fsdata);
    if (!bh)
        return -EIO;

    affs_lock_link(inode);
    affs_lock_dir(dir);

    if (be32_to_cpu(AFFS_TAIL(sb, bh)->stype) == ST_USERDIR) {
        affs_lock_dir(inode);
        result = ifs_amiga_directory_empty(inode);
        affs_unlock_dir(inode);
        if (result != 0)
            goto out_locked;
    }

    result = affs_remove_hash(dir, bh);
    if (result != 0)
        goto out_locked;

    mark_buffer_dirty_inode(bh, inode);
    affs_unlock_dir(dir);

    if (inode->i_nlink > 1)
        result = ifs_amiga_remove_link(dentry);
    else {
        clear_nlink(inode);
        result = 0;
    }

    affs_unlock_link(inode);
    inode_set_ctime_current(inode);
    mark_inode_dirty(inode);
    affs_brelse(bh);
    return result;

out_locked:
    affs_unlock_dir(dir);
    affs_unlock_link(inode);
    affs_brelse(bh);
    return result;
}

u32 affs_checksum_block(struct super_block *sb, struct buffer_head *bh)
{
    return ifs_amiga_block_checksum(
        (const ifs_amiga_u8 *)bh->b_data, (ifs_amiga_u32)sb->s_blocksize);
}

void affs_fix_checksum(struct super_block *sb, struct buffer_head *bh)
{
    __be32 *words = (__be32 *)bh->b_data;

    words[5] = cpu_to_be32(
        ifs_amiga_checksum_word_value(
            (const ifs_amiga_u8 *)bh->b_data,
            (ifs_amiga_u32)sb->s_blocksize, 5U));
}

void affs_secs_to_datestamp(time64_t seconds, struct affs_date *stamp)
{
    u32 days;
    u32 minutes;
    s32 remainder;

    seconds -= (time64_t)sys_tz.tz_minuteswest * 60 + AFFS_EPOCH_DELTA;
    if (seconds < 0)
        seconds = 0;

    days = (u32)div_s64_rem(seconds, 86400, &remainder);
    minutes = (u32)(remainder / 60);
    remainder -= (s32)minutes * 60;

    stamp->days = cpu_to_be32(days);
    stamp->mins = cpu_to_be32(minutes);
    stamp->ticks = cpu_to_be32((u32)remainder * 50U);
}

umode_t affs_prot_to_mode(u32 protection)
{
    umode_t mode = 0;

    if ((protection & FIBF_NOREAD) == 0U)
        mode |= 0400;
    if ((protection & FIBF_NOWRITE) == 0U)
        mode |= 0200;
    if ((protection & FIBF_NOEXECUTE) == 0U)
        mode |= 0100;

    if ((protection & FIBF_GRP_READ) != 0U)
        mode |= 0040;
    if ((protection & FIBF_GRP_WRITE) != 0U)
        mode |= 0020;
    if ((protection & FIBF_GRP_EXECUTE) != 0U)
        mode |= 0010;

    if ((protection & FIBF_OTR_READ) != 0U)
        mode |= 0004;
    if ((protection & FIBF_OTR_WRITE) != 0U)
        mode |= 0002;
    if ((protection & FIBF_OTR_EXECUTE) != 0U)
        mode |= 0001;

    return mode;
}

void affs_mode_to_prot(struct inode *inode)
{
    const umode_t mode = inode->i_mode;
    u32 protection = AFFS_I(inode)->i_protect;

    protection &= ~(FIBF_NOEXECUTE | FIBF_NOREAD | FIBF_NOWRITE |
                    FIBF_NODELETE | FIBF_GRP_EXECUTE | FIBF_GRP_READ |
                    FIBF_GRP_WRITE | FIBF_GRP_DELETE |
                    FIBF_OTR_EXECUTE | FIBF_OTR_READ |
                    FIBF_OTR_WRITE | FIBF_OTR_DELETE);

    if ((mode & 0100) == 0)
        protection |= FIBF_NOEXECUTE;
    if ((mode & 0400) == 0)
        protection |= FIBF_NOREAD;
    if ((mode & 0200) == 0)
        protection |= FIBF_NOWRITE;

    if ((mode & 0010) != 0)
        protection |= FIBF_GRP_EXECUTE;
    if ((mode & 0040) != 0)
        protection |= FIBF_GRP_READ;
    if ((mode & 0020) != 0)
        protection |= FIBF_GRP_WRITE;
    if ((mode & 0070) != 0)
        protection |= FIBF_GRP_DELETE;

    if ((mode & 0001) != 0)
        protection |= FIBF_OTR_EXECUTE;
    if ((mode & 0004) != 0)
        protection |= FIBF_OTR_READ;
    if ((mode & 0002) != 0)
        protection |= FIBF_OTR_WRITE;
    if ((mode & 0007) != 0)
        protection |= FIBF_OTR_DELETE;

    AFFS_I(inode)->i_protect = protection;
}

void affs_error(struct super_block *sb, const char *function,
                const char *format, ...)
{
    struct va_format message;
    va_list arguments;

    va_start(arguments, format);
    message.fmt = format;
    message.va = &arguments;
    pr_crit("error (device %s): %s(): %pV\n",
            sb->s_id, function, &message);
    if (!sb_rdonly(sb))
        pr_warn("Remounting filesystem read-only\n");
    sb->s_flags |= SB_RDONLY;
    va_end(arguments);
}

void affs_warning(struct super_block *sb, const char *function,
                  const char *format, ...)
{
    struct va_format message;
    va_list arguments;

    va_start(arguments, format);
    message.fmt = format;
    message.va = &arguments;
    pr_warn("(device %s): %s(): %pV\n",
            sb->s_id, function, &message);
    va_end(arguments);
}

bool affs_nofilenametruncate(const struct dentry *dentry)
{
    return affs_test_opt(
        AFFS_SB(dentry->d_sb)->s_flags, SF_NO_TRUNCATE) != 0;
}

int affs_check_name(const unsigned char *name, int length, bool no_truncate)
{
    const IfsAmigaNameStatus status = ifs_amiga_validate_name(
        name, length < 0 ? 0U : (ifs_amiga_u32)length,
        no_truncate ? 1 : 0);

    if (length < 0)
        return -EINVAL;

    switch (status) {
    case IFS_AMIGA_NAME_OK:
        return 0;
    case IFS_AMIGA_NAME_TOO_LONG:
        return -ENAMETOOLONG;
    case IFS_AMIGA_NAME_INVALID_CHARACTER:
    default:
        return -EINVAL;
    }
}

int affs_copy_name(unsigned char *destination, struct dentry *dentry)
{
    const u32 length =
        min_t(u32, (u32)dentry->d_name.len, (u32)AFFSNAMEMAX);

    destination[0] = (unsigned char)length;
    memcpy(destination + 1, dentry->d_name.name, length);
    return (int)length;
}
