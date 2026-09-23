/*
 * Project-authored Linux inode adapter for Amiga FFS.
 */

#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/gfp.h>

static void ifs_amiga_reset_inode_private(struct inode *inode)
{
    struct affs_inode_info *info = AFFS_I(inode);

    atomic_set(&info->i_opencnt, 0);
    info->i_blkcnt = 0U;
    info->i_extcnt = 1U;
    info->i_ext_last = ~1U;
    info->i_protect = 0U;
    info->i_lc = NULL;
    info->i_lc_size = 0U;
    info->i_lc_shift = 0U;
    info->i_lc_mask = 0U;
    info->i_ac = NULL;
    info->i_ext_bh = NULL;
    info->mmu_private = 0;
    info->i_lastalloc = 0U;
    info->i_pa_cnt = 0;
}

static time64_t ifs_amiga_inode_time(const struct affs_date *date)
{
    return (time64_t)be32_to_cpu(date->days) * 86400LL +
           (time64_t)be32_to_cpu(date->mins) * 60LL +
           (time64_t)be32_to_cpu(date->ticks) / 50LL +
           AFFS_EPOCH_DELTA +
           (time64_t)sys_tz.tz_minuteswest * 60LL;
}

struct inode *affs_iget(struct super_block *sb, unsigned long inode_number)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct buffer_head *bh = NULL;
    struct affs_tail *tail;
    struct inode *inode;
    u32 protection;
    u32 type;
    u16 id;
    time64_t timestamp;

    if (inode_number > U32_MAX ||
        !affs_validblock(sb, (int)inode_number))
        return ERR_PTR(-ESTALE);

    inode = iget_locked(sb, inode_number);
    if (!inode)
        return ERR_PTR(-ENOMEM);
    if ((inode->i_state & I_NEW) == 0)
        return inode;

    bh = affs_bread(sb, (u32)inode_number);
    if (!bh) {
        affs_warning(sb, "affs_iget",
                     "Cannot read inode block %lu", inode_number);
        goto bad_inode;
    }

    if (affs_checksum_block(sb, bh) != 0U ||
        be32_to_cpu(AFFS_HEAD(bh)->ptype) != T_SHORT) {
        affs_warning(sb, "affs_iget",
                     "Invalid checksum or primary type in block %lu",
                     inode_number);
        goto bad_inode;
    }

    tail = AFFS_TAIL(sb, bh);
    protection = be32_to_cpu(tail->protect);
    type = be32_to_cpu(tail->stype);

    inode->i_size = 0;
    set_nlink(inode, 1);
    inode->i_mode = 0;
    ifs_amiga_reset_inode_private(inode);
    AFFS_I(inode)->i_protect = protection;

    if (affs_test_opt(sbi->s_flags, SF_SETMODE))
        inode->i_mode = sbi->s_mode;
    else
        inode->i_mode = affs_prot_to_mode(protection);

    id = be16_to_cpu(tail->uid);
    if (id == 0U || affs_test_opt(sbi->s_flags, SF_SETUID))
        inode->i_uid = sbi->s_uid;
    else if (id == 0xffffU && affs_test_opt(sbi->s_flags, SF_MUFS))
        i_uid_write(inode, 0U);
    else
        i_uid_write(inode, id);

    id = be16_to_cpu(tail->gid);
    if (id == 0U || affs_test_opt(sbi->s_flags, SF_SETGID))
        inode->i_gid = sbi->s_gid;
    else if (id == 0xffffU && affs_test_opt(sbi->s_flags, SF_MUFS))
        i_gid_write(inode, 0U);
    else
        i_gid_write(inode, id);

    switch (type) {
    case ST_ROOT:
        inode->i_uid = sbi->s_uid;
        inode->i_gid = sbi->s_gid;
        if (!affs_test_opt(sbi->s_flags, SF_SETMODE))
            inode->i_mode = S_IRUGO | S_IXUGO | S_IWUSR;
        fallthrough;

    case ST_USERDIR:
        if (type == ST_USERDIR ||
            affs_test_opt(sbi->s_flags, SF_SETMODE)) {
            if ((inode->i_mode & S_IRUSR) != 0)
                inode->i_mode |= S_IXUSR;
            if ((inode->i_mode & S_IRGRP) != 0)
                inode->i_mode |= S_IXGRP;
            if ((inode->i_mode & S_IROTH) != 0)
                inode->i_mode |= S_IXOTH;
        }
        inode->i_mode |= S_IFDIR;
        inode->i_op = &affs_dir_inode_operations;
        inode->i_fop = &affs_dir_operations;
        break;

    case ST_LINKDIR:
        inode->i_mode |= S_IFDIR;
        break;

    case ST_LINKFILE:
        affs_warning(sb, "affs_iget",
                     "Unexpected hard-link header %lu", inode_number);
        goto bad_inode;

    case ST_FILE: {
        const u32 size = be32_to_cpu(tail->size);

        if (sbi->s_data_blksize == 0U || sbi->s_hashsize <= 0)
            goto bad_inode;

        inode->i_mode |= S_IFREG;
        inode->i_size = size;
        AFFS_I(inode)->mmu_private = size;

        if (ifs_amiga_file_block_count(
                size, sbi->s_data_blksize,
                &AFFS_I(inode)->i_blkcnt) != 0 ||
            ifs_amiga_file_extension_count(
                AFFS_I(inode)->i_blkcnt, (u32)sbi->s_hashsize,
                &AFFS_I(inode)->i_extcnt) != 0)
            goto bad_inode;

        if (tail->link_chain != 0)
            set_nlink(inode, 2);

        inode->i_mapping->a_ops =
            affs_test_opt(sbi->s_flags, SF_OFS) ?
            &affs_aops_ofs : &affs_aops;
        inode->i_op = &affs_file_inode_operations;
        inode->i_fop = &affs_file_operations;
        break;
    }

    case ST_SOFTLINK: {
        const size_t capacity = (size_t)sbi->s_hashsize * sizeof(u32);
        const size_t length =
            strnlen((const char *)AFFS_HEAD(bh)->table, capacity);

        if (length == capacity) {
            affs_warning(sb, "affs_iget",
                         "Unterminated symbolic link in block %lu",
                         inode_number);
            goto bad_inode;
        }

        inode->i_size = (loff_t)length;
        inode->i_mode |= S_IFLNK;
        inode_nohighmem(inode);
        inode->i_op = &affs_symlink_inode_operations;
        inode->i_data.a_ops = &affs_symlink_aops;
        break;
    }

    default:
        affs_warning(sb, "affs_iget",
                     "Unsupported secondary type %u in block %lu",
                     type, inode_number);
        goto bad_inode;
    }

    timestamp = ifs_amiga_inode_time(&tail->change);
    inode_set_ctime(inode, timestamp, 0);
    inode_set_atime(inode, timestamp, 0);
    inode_set_mtime(inode, timestamp, 0);

    affs_brelse(bh);
    unlock_new_inode(inode);
    return inode;

bad_inode:
    affs_brelse(bh);
    iget_failed(inode);
    return ERR_PTR(-EIO);
}

int affs_write_inode(struct inode *inode, struct writeback_control *writeback)
{
    struct super_block *sb = inode->i_sb;
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct buffer_head *bh;
    struct affs_tail *tail;
    uid_t uid;
    gid_t gid;

    if (inode->i_nlink == 0)
        return 0;

    if (inode->i_size < 0 || inode->i_size > U32_MAX)
        return -EFBIG;

    bh = affs_bread(sb, (u32)inode->i_ino);
    if (!bh) {
        affs_error(sb, "affs_write_inode",
                   "Cannot read inode block %lu", inode->i_ino);
        return -EIO;
    }

    tail = AFFS_TAIL(sb, bh);
    if (be32_to_cpu(tail->stype) == ST_ROOT) {
        affs_secs_to_datestamp(
            inode_get_mtime_sec(inode),
            &AFFS_ROOT_TAIL(sb, bh)->root_change);
    } else {
        tail->protect = cpu_to_be32(AFFS_I(inode)->i_protect);
        tail->size = cpu_to_be32((u32)inode->i_size);
        affs_secs_to_datestamp(
            inode_get_mtime_sec(inode), &tail->change);

        if (inode->i_ino != sbi->s_root_block) {
            uid = i_uid_read(inode);
            gid = i_gid_read(inode);

            if (affs_test_opt(sbi->s_flags, SF_MUFS)) {
                if (uid == 0U || uid == 0xffffU)
                    uid ^= 0xffffU;
                if (gid == 0U || gid == 0xffffU)
                    gid ^= 0xffffU;
            }

            if (!affs_test_opt(sbi->s_flags, SF_SETUID))
                tail->uid = cpu_to_be16((u16)uid);
            if (!affs_test_opt(sbi->s_flags, SF_SETGID))
                tail->gid = cpu_to_be16((u16)gid);
        }
    }

    affs_fix_checksum(sb, bh);
    mark_buffer_dirty_inode(bh, inode);
    affs_brelse(bh);
    affs_free_prealloc(inode);
    return 0;
}

int affs_notify_change(
    struct mnt_idmap *idmap, struct dentry *dentry, struct iattr *attributes)
{
    struct inode *inode = d_inode(dentry);
    struct affs_sb_info *sbi = AFFS_SB(inode->i_sb);
    int result;

    result = setattr_prepare(&nop_mnt_idmap, dentry, attributes);
    if (result != 0)
        return result;

    if (((attributes->ia_valid & ATTR_UID) != 0 &&
         affs_test_opt(sbi->s_flags, SF_SETUID)) ||
        ((attributes->ia_valid & ATTR_GID) != 0 &&
         affs_test_opt(sbi->s_flags, SF_SETGID)) ||
        ((attributes->ia_valid & ATTR_MODE) != 0 &&
         (sbi->s_flags &
          (AFFS_MOUNT_SF_SETMODE | AFFS_MOUNT_SF_IMMUTABLE)) != 0UL)) {
        return affs_test_opt(sbi->s_flags, SF_QUIET) ? 0 : -EPERM;
    }

    if ((attributes->ia_valid & ATTR_SIZE) != 0 &&
        attributes->ia_size != i_size_read(inode)) {
        if (attributes->ia_size < 0 || attributes->ia_size > U32_MAX)
            return -EFBIG;

        result = inode_newsize_ok(inode, attributes->ia_size);
        if (result != 0)
            return result;

        truncate_setsize(inode, attributes->ia_size);
        affs_truncate(inode);
    }

    setattr_copy(&nop_mnt_idmap, inode, attributes);
    if ((attributes->ia_valid & ATTR_MODE) != 0)
        affs_mode_to_prot(inode);

    mark_inode_dirty(inode);
    return 0;
}

void affs_evict_inode(struct inode *inode)
{
    unsigned long cache_page;

    truncate_inode_pages_final(&inode->i_data);

    if (inode->i_nlink == 0) {
        inode->i_size = 0;
        affs_truncate(inode);
    }

    invalidate_inode_buffers(inode);
    clear_inode(inode);
    affs_free_prealloc(inode);

    cache_page = (unsigned long)AFFS_I(inode)->i_lc;
    if (cache_page != 0UL) {
        AFFS_I(inode)->i_lc = NULL;
        AFFS_I(inode)->i_ac = NULL;
        free_page(cache_page);
    }

    affs_brelse(AFFS_I(inode)->i_ext_bh);
    AFFS_I(inode)->i_ext_bh = NULL;
    AFFS_I(inode)->i_ext_last = ~1U;

    if (inode->i_nlink == 0)
        affs_free_block(inode->i_sb, (u32)inode->i_ino);
}

struct inode *affs_new_inode(struct inode *dir)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode;
    struct buffer_head *bh;
    u32 block;

    inode = new_inode(sb);
    if (!inode)
        return NULL;

    block = affs_alloc_block(dir, (u32)dir->i_ino);
    if (block == 0U) {
        iput(inode);
        return NULL;
    }

    inode->i_ino = block;
    bh = affs_getzeroblk(sb, block);
    if (!bh) {
        affs_free_block(sb, block);
        iput(inode);
        return NULL;
    }

    inode->i_uid = current_fsuid();
    inode->i_gid = current_fsgid();
    set_nlink(inode, 1);
    simple_inode_init_ts(inode);
    ifs_amiga_reset_inode_private(inode);
    insert_inode_hash(inode);

    mark_buffer_dirty_inode(bh, inode);
    affs_brelse(bh);
    return inode;
}

int affs_add_entry(
    struct inode *dir, struct inode *inode,
    struct dentry *dentry, s32 type)
{
    struct super_block *sb = dir->i_sb;
    struct buffer_head *inode_bh = NULL;
    struct buffer_head *entry_bh = NULL;
    __be32 old_link_chain = 0;
    u32 link_block = 0U;
    bool is_link = type == ST_LINKFILE || type == ST_LINKDIR;
    bool chain_published = false;
    int result = -EIO;

    inode_bh = affs_bread(sb, (u32)inode->i_ino);
    if (!inode_bh)
        return -EIO;

    affs_lock_link(inode);

    if (is_link) {
        link_block = affs_alloc_block(dir, (u32)dir->i_ino);
        if (link_block == 0U) {
            result = -ENOSPC;
            goto out_unlock;
        }

        entry_bh = affs_getzeroblk(sb, link_block);
        if (!entry_bh) {
            result = -EIO;
            goto out_unlock;
        }
    } else {
        entry_bh = inode_bh;
    }

    AFFS_HEAD(entry_bh)->ptype = cpu_to_be32(T_SHORT);
    AFFS_HEAD(entry_bh)->key = cpu_to_be32((u32)entry_bh->b_blocknr);
    affs_copy_name(AFFS_TAIL(sb, entry_bh)->name, dentry);
    AFFS_TAIL(sb, entry_bh)->stype = cpu_to_be32(type);
    AFFS_TAIL(sb, entry_bh)->parent = cpu_to_be32((u32)dir->i_ino);

    if (is_link) {
        old_link_chain = AFFS_TAIL(sb, inode_bh)->link_chain;
        AFFS_TAIL(sb, entry_bh)->original =
            cpu_to_be32((u32)inode->i_ino);
        AFFS_TAIL(sb, entry_bh)->link_chain = old_link_chain;

        AFFS_TAIL(sb, inode_bh)->link_chain =
            cpu_to_be32(link_block);
        affs_fix_checksum(sb, inode_bh);
        mark_buffer_dirty_inode(inode_bh, inode);
        chain_published = true;
    }

    affs_fix_checksum(sb, entry_bh);
    mark_buffer_dirty_inode(entry_bh, inode);

    affs_lock_dir(dir);
    result = affs_insert_hash(dir, entry_bh);
    affs_unlock_dir(dir);
    if (result != 0)
        goto rollback;

    dentry->d_fsdata =
        (void *)(unsigned long)entry_bh->b_blocknr;

    if (is_link) {
        set_nlink(inode, 2);
        ihold(inode);
    }

    affs_unlock_link(inode);
    d_instantiate(dentry, inode);

    if (entry_bh != inode_bh)
        affs_brelse(entry_bh);
    affs_brelse(inode_bh);
    return 0;

rollback:
    if (chain_published) {
        AFFS_TAIL(sb, inode_bh)->link_chain = old_link_chain;
        affs_fix_checksum(sb, inode_bh);
        mark_buffer_dirty_inode(inode_bh, inode);
    }

out_unlock:
    if (is_link && link_block != 0U)
        affs_free_block(sb, link_block);

    affs_unlock_link(inode);
    if (entry_bh && entry_bh != inode_bh)
        affs_brelse(entry_bh);
    affs_brelse(inode_bh);
    return result;
}
