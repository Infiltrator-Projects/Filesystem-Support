/*
 * Shared project-authored Linux namespace adapter for AmigaDOS OFS/FFS.
 * Canonical name folding, validation, directory hashing and symlink encoding
 * live in amiga_dos_core.
 */

#include <linux/exportfs.h>

static int ifs_amiga_name_hash(
    const struct dentry *parent, struct qstr *name, const bool international)
{
    const unsigned char *cursor = name->name;
    unsigned long hash;
    u32 remaining;
    int result;

    result = affs_check_name(
        name->name, (int)name->len, affs_nofilenametruncate(parent));
    if (result != 0)
        return result;

    hash = init_name_hash(parent);
    remaining = min_t(u32, (u32)name->len, (u32)AFFSNAMEMAX);
    while (remaining-- != 0U)
        hash = partial_name_hash(
            ifs_amiga_fold_character(*cursor++, international ? 1 : 0),
            hash);

    name->hash = end_name_hash(hash);
    return 0;
}

static int ifs_amiga_hash_dentry(
    const struct dentry *parent, struct qstr *name)
{
    return ifs_amiga_name_hash(parent, name, false);
}

static int ifs_amiga_intl_hash_dentry(
    const struct dentry *parent, struct qstr *name)
{
    return ifs_amiga_name_hash(parent, name, true);
}

static int ifs_amiga_name_compare(
    const struct dentry *parent,
    unsigned int existing_length,
    const char *existing_name,
    const struct qstr *candidate,
    const bool international)
{
    u32 length;
    u32 index;

    if (affs_check_name(
            candidate->name, (int)candidate->len,
            affs_nofilenametruncate(parent)) != 0)
        return 1;

    if (existing_length >= AFFSNAMEMAX) {
        if (candidate->len < AFFSNAMEMAX)
            return 1;
        length = AFFSNAMEMAX;
    } else {
        if (existing_length != candidate->len)
            return 1;
        length = existing_length;
    }

    for (index = 0U; index < length; ++index) {
        if (ifs_amiga_fold_character(
                (ifs_amiga_u8)existing_name[index],
                international ? 1 : 0) !=
            ifs_amiga_fold_character(
                candidate->name[index], international ? 1 : 0))
            return 1;
    }

    return 0;
}

static int ifs_amiga_compare_dentry(
    const struct dentry *parent, unsigned int length,
    const char *existing, const struct qstr *candidate)
{
    return ifs_amiga_name_compare(
        parent, length, existing, candidate, false);
}

static int ifs_amiga_intl_compare_dentry(
    const struct dentry *parent, unsigned int length,
    const char *existing, const struct qstr *candidate)
{
    return ifs_amiga_name_compare(
        parent, length, existing, candidate, true);
}

static bool ifs_amiga_disk_name_matches(
    const struct dentry *dentry,
    const u8 *disk_name,
    const bool international)
{
    u32 length = (u32)dentry->d_name.len;
    u32 disk_length = disk_name[0];
    u32 index;

    if (length >= AFFSNAMEMAX) {
        if (disk_length < AFFSNAMEMAX)
            return false;
        length = AFFSNAMEMAX;
    } else if (length != disk_length) {
        return false;
    }

    for (index = 0U; index < length; ++index) {
        if (ifs_amiga_fold_character(
                dentry->d_name.name[index], international ? 1 : 0) !=
            ifs_amiga_fold_character(
                disk_name[index + 1U], international ? 1 : 0))
            return false;
    }

    return true;
}

int affs_hash_name(struct super_block *sb, const u8 *name, unsigned int length)
{
    return (int)ifs_amiga_directory_hash(
        name, (ifs_amiga_u32)length,
        (ifs_amiga_u32)AFFS_SB(sb)->s_hashsize,
        affs_test_opt(AFFS_SB(sb)->s_flags, SF_INTL) ? 1 : 0);
}

static struct buffer_head *ifs_amiga_find_entry(
    struct inode *dir, struct dentry *dentry)
{
    struct super_block *sb = dir->i_sb;
    struct buffer_head *bh;
    const bool international =
        affs_test_opt(AFFS_SB(sb)->s_flags, SF_INTL) != 0;
    const int slot = affs_hash_name(
        sb, dentry->d_name.name, dentry->d_name.len);
    u32 key;
    u32 budget;

    if (slot < 0 || slot >= AFFS_SB(sb)->s_hashsize)
        return ERR_PTR(-EUCLEAN);

    bh = affs_bread(sb, (u32)dir->i_ino);
    if (!bh)
        return ERR_PTR(-EIO);

    key = be32_to_cpu(AFFS_HEAD(bh)->table[slot]);
    affs_brelse(bh);
    budget = ifs_amiga_chain_budget(sb);

    while (key != 0U) {
        if (budget-- == 0U)
            return ERR_PTR(-EUCLEAN);

        bh = affs_bread(sb, key);
        if (!bh)
            return ERR_PTR(-EIO);

        if (ifs_amiga_disk_name_matches(
                dentry, AFFS_TAIL(sb, bh)->name, international))
            return bh;

        key = be32_to_cpu(AFFS_TAIL(sb, bh)->hash_chain);
        affs_brelse(bh);
    }

    return NULL;
}

struct dentry *affs_lookup(
    struct inode *dir, struct dentry *dentry, unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    struct buffer_head *bh;
    struct inode *inode = NULL;
    struct dentry *result;

    affs_lock_dir(dir);
    bh = ifs_amiga_find_entry(dir, dentry);
    if (IS_ERR(bh)) {
        affs_unlock_dir(dir);
        return ERR_CAST(bh);
    }

    if (bh) {
        u32 inode_block = (u32)bh->b_blocknr;
        const u32 type = be32_to_cpu(AFFS_TAIL(sb, bh)->stype);

        dentry->d_fsdata = (void *)(unsigned long)inode_block;
        if (type == ST_LINKFILE)
            inode_block = be32_to_cpu(AFFS_TAIL(sb, bh)->original);

        affs_brelse(bh);
        inode = affs_iget(sb, inode_block);
    }

    result = d_splice_alias(inode, dentry);
    if (!IS_ERR_OR_NULL(result))
        result->d_fsdata = dentry->d_fsdata;

    affs_unlock_dir(dir);
    return result;
}

int affs_unlink(struct inode *dir, struct dentry *dentry)
{
    return affs_remove_header(dentry);
}

int affs_create(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode, bool exclusive)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode;
    int result;

    inode = affs_new_inode(dir);
    if (!inode)
        return -ENOSPC;

    inode->i_mode = mode;
    affs_mode_to_prot(inode);
    mark_inode_dirty(inode);

    inode->i_op = &affs_file_inode_operations;
    inode->i_fop = &affs_file_operations;
    inode->i_mapping->a_ops =
        affs_test_opt(AFFS_SB(sb)->s_flags, SF_OFS) ?
        &affs_aops_ofs : &affs_aops;

    result = affs_add_entry(dir, inode, dentry, ST_FILE);
    if (result != 0) {
        clear_nlink(inode);
        iput(inode);
    }
    return result;
}

int affs_mkdir(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode)
{
    struct inode *inode;
    int result;

    inode = affs_new_inode(dir);
    if (!inode)
        return -ENOSPC;

    inode->i_mode = S_IFDIR | mode;
    affs_mode_to_prot(inode);
    inode->i_op = &affs_dir_inode_operations;
    inode->i_fop = &affs_dir_operations;

    result = affs_add_entry(dir, inode, dentry, ST_USERDIR);
    if (result != 0) {
        clear_nlink(inode);
        mark_inode_dirty(inode);
        iput(inode);
    }
    return result;
}

int affs_rmdir(struct inode *dir, struct dentry *dentry)
{
    return affs_remove_header(dentry);
}

int affs_symlink(
    struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, const char *target)
{
    struct super_block *sb = dir->i_sb;
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct buffer_head *bh = NULL;
    struct inode *inode;
    ifs_amiga_u32 encoded_length = 0U;
    IfsAmigaSymlinkStatus status;
    size_t target_length;
    size_t volume_length;
    u32 capacity;
    int result;

    target_length = strlen(target);
    if (target_length >= U32_MAX)
        return -ENAMETOOLONG;

    inode = affs_new_inode(dir);
    if (!inode)
        return -ENOSPC;

    inode->i_op = &affs_symlink_inode_operations;
    inode_nohighmem(inode);
    inode->i_data.a_ops = &affs_symlink_aops;
    inode->i_mode = S_IFLNK | 0777;
    affs_mode_to_prot(inode);

    bh = affs_bread(sb, (u32)inode->i_ino);
    if (!bh) {
        result = -EIO;
        goto fail;
    }

    capacity = (u32)sbi->s_hashsize * sizeof(u32);
    spin_lock(&sbi->symlink_lock);
    volume_length = strnlen(sbi->s_volume, sizeof(sbi->s_volume));
    status = ifs_amiga_encode_symlink(
        (const ifs_amiga_u8 *)target,
        (ifs_amiga_u32)target_length + 1U,
        (const ifs_amiga_u8 *)sbi->s_volume,
        (ifs_amiga_u32)volume_length,
        (ifs_amiga_u8 *)AFFS_HEAD(bh)->table,
        capacity, &encoded_length);
    spin_unlock(&sbi->symlink_lock);

    if (status != IFS_AMIGA_SYMLINK_OK) {
        result = status == IFS_AMIGA_SYMLINK_OUTPUT_TOO_SMALL ?
                 -ENAMETOOLONG : -EINVAL;
        goto fail;
    }

    inode->i_size = encoded_length + 1U;
    mark_buffer_dirty_inode(bh, inode);
    affs_brelse(bh);
    bh = NULL;
    mark_inode_dirty(inode);

    result = affs_add_entry(dir, inode, dentry, ST_SOFTLINK);
    if (result == 0)
        return 0;

fail:
    affs_brelse(bh);
    clear_nlink(inode);
    mark_inode_dirty(inode);
    iput(inode);
    return result;
}

int affs_link(
    struct dentry *old_dentry, struct inode *dir, struct dentry *dentry)
{
    return affs_add_entry(dir, d_inode(old_dentry), dentry, ST_LINKFILE);
}

static u32 ifs_amiga_dentry_block(const struct dentry *dentry)
{
    const unsigned long stored = (unsigned long)dentry->d_fsdata;

    return stored != 0UL ? (u32)stored : (u32)d_inode(dentry)->i_ino;
}

static int ifs_amiga_remove_from_directory(
    struct inode *dir, struct buffer_head *bh)
{
    int result;

    affs_lock_dir(dir);
    result = affs_remove_hash(dir, bh);
    affs_unlock_dir(dir);
    return result;
}

static int ifs_amiga_insert_into_directory(
    struct inode *dir, struct buffer_head *bh)
{
    int result;

    affs_lock_dir(dir);
    result = affs_insert_hash(dir, bh);
    affs_unlock_dir(dir);
    return result;
}

static int ifs_amiga_rename(
    struct inode *old_dir, struct dentry *old_dentry,
    struct inode *new_dir, struct dentry *new_dentry)
{
    struct super_block *sb = old_dir->i_sb;
    struct buffer_head *bh;
    unsigned char old_name[32];
    int result;
    int rollback;

    result = affs_check_name(
        new_dentry->d_name.name, (int)new_dentry->d_name.len,
        affs_nofilenametruncate(old_dentry));
    if (result != 0)
        return result;

    if (d_really_is_positive(new_dentry)) {
        result = affs_remove_header(new_dentry);
        if (result != 0)
            return result;
    }

    bh = affs_bread(sb, ifs_amiga_dentry_block(old_dentry));
    if (!bh)
        return -EIO;

    memcpy(old_name, AFFS_TAIL(sb, bh)->name, sizeof(old_name));

    result = ifs_amiga_remove_from_directory(old_dir, bh);
    if (result != 0)
        goto out;

    affs_copy_name(AFFS_TAIL(sb, bh)->name, new_dentry);
    affs_fix_checksum(sb, bh);
    result = ifs_amiga_insert_into_directory(new_dir, bh);
    if (result == 0)
        goto out;

    memcpy(AFFS_TAIL(sb, bh)->name, old_name, sizeof(old_name));
    affs_fix_checksum(sb, bh);
    rollback = ifs_amiga_insert_into_directory(old_dir, bh);
    if (rollback != 0)
        affs_error(sb, "ifs_amiga_rename",
                   "Could not restore source after failed rename");

out:
    mark_buffer_dirty_inode(bh, result == 0 ? new_dir : old_dir);
    affs_brelse(bh);
    return result;
}

static int ifs_amiga_exchange(
    struct inode *old_dir, struct dentry *old_dentry,
    struct inode *new_dir, struct dentry *new_dentry)
{
    struct super_block *sb = old_dir->i_sb;
    struct buffer_head *old_bh = NULL;
    struct buffer_head *new_bh = NULL;
    unsigned char old_name[32];
    unsigned char new_name[32];
    bool old_removed = false;
    bool new_removed = false;
    bool old_inserted_new = false;
    int result;

    if (!d_really_is_positive(new_dentry))
        return -ENOENT;

    old_bh = affs_bread(sb, ifs_amiga_dentry_block(old_dentry));
    new_bh = affs_bread(sb, ifs_amiga_dentry_block(new_dentry));
    if (!old_bh || !new_bh) {
        result = -EIO;
        goto out;
    }

    memcpy(old_name, AFFS_TAIL(sb, old_bh)->name, sizeof(old_name));
    memcpy(new_name, AFFS_TAIL(sb, new_bh)->name, sizeof(new_name));

    result = ifs_amiga_remove_from_directory(old_dir, old_bh);
    if (result != 0)
        goto out;
    old_removed = true;

    result = ifs_amiga_remove_from_directory(new_dir, new_bh);
    if (result != 0)
        goto rollback;
    new_removed = true;

    affs_copy_name(AFFS_TAIL(sb, old_bh)->name, new_dentry);
    affs_fix_checksum(sb, old_bh);
    result = ifs_amiga_insert_into_directory(new_dir, old_bh);
    if (result != 0)
        goto rollback;
    old_inserted_new = true;

    affs_copy_name(AFFS_TAIL(sb, new_bh)->name, old_dentry);
    affs_fix_checksum(sb, new_bh);
    result = ifs_amiga_insert_into_directory(old_dir, new_bh);
    if (result == 0)
        goto out;

rollback:
    if (old_inserted_new)
        (void)ifs_amiga_remove_from_directory(new_dir, old_bh);

    memcpy(AFFS_TAIL(sb, old_bh)->name, old_name, sizeof(old_name));
    memcpy(AFFS_TAIL(sb, new_bh)->name, new_name, sizeof(new_name));
    affs_fix_checksum(sb, old_bh);
    affs_fix_checksum(sb, new_bh);

    if (old_removed &&
        ifs_amiga_insert_into_directory(old_dir, old_bh) != 0)
        affs_error(sb, "ifs_amiga_exchange",
                   "Could not restore first entry after failed exchange");

    if (new_removed &&
        ifs_amiga_insert_into_directory(new_dir, new_bh) != 0)
        affs_error(sb, "ifs_amiga_exchange",
                   "Could not restore second entry after failed exchange");

out:
    if (old_bh)
        mark_buffer_dirty_inode(old_bh, old_dir);
    if (new_bh)
        mark_buffer_dirty_inode(new_bh, new_dir);
    affs_brelse(old_bh);
    affs_brelse(new_bh);
    return result;
}

int affs_rename2(
    struct mnt_idmap *idmap,
    struct inode *old_dir, struct dentry *old_dentry,
    struct inode *new_dir, struct dentry *new_dentry,
    unsigned int flags)
{
    if ((flags & ~(RENAME_NOREPLACE | RENAME_EXCHANGE)) != 0U)
        return -EINVAL;

    if ((flags & RENAME_NOREPLACE) != 0U &&
        d_really_is_positive(new_dentry))
        return -EEXIST;

    if ((flags & RENAME_EXCHANGE) != 0U)
        return ifs_amiga_exchange(
            old_dir, old_dentry, new_dir, new_dentry);

    return ifs_amiga_rename(
        old_dir, old_dentry, new_dir, new_dentry);
}

static struct dentry *ifs_amiga_get_parent(struct dentry *child)
{
    struct buffer_head *bh;
    struct inode *parent;
    const u32 block = (u32)d_inode(child)->i_ino;

    bh = affs_bread(child->d_sb, block);
    if (!bh)
        return ERR_PTR(-EIO);

    parent = affs_iget(
        child->d_sb, be32_to_cpu(AFFS_TAIL(child->d_sb, bh)->parent));
    affs_brelse(bh);
    return d_obtain_alias(parent);
}

static struct inode *ifs_amiga_export_inode(
    struct super_block *sb, u64 inode_number, u32 generation)
{
    if (inode_number > U32_MAX ||
        !affs_validblock(sb, (int)inode_number))
        return ERR_PTR(-ESTALE);

    return affs_iget(sb, (unsigned long)inode_number);
}

static struct dentry *ifs_amiga_fh_to_dentry(
    struct super_block *sb, struct fid *fid, int length, int type)
{
    return generic_fh_to_dentry(
        sb, fid, length, type, ifs_amiga_export_inode);
}

static struct dentry *ifs_amiga_fh_to_parent(
    struct super_block *sb, struct fid *fid, int length, int type)
{
    return generic_fh_to_parent(
        sb, fid, length, type, ifs_amiga_export_inode);
}

const struct export_operations affs_export_ops = {
    .encode_fh = generic_encode_ino32_fh,
    .fh_to_dentry = ifs_amiga_fh_to_dentry,
    .fh_to_parent = ifs_amiga_fh_to_parent,
    .get_parent = ifs_amiga_get_parent,
};

const struct dentry_operations affs_dentry_operations = {
    .d_hash = ifs_amiga_hash_dentry,
    .d_compare = ifs_amiga_compare_dentry,
};

const struct dentry_operations affs_intl_dentry_operations = {
    .d_hash = ifs_amiga_intl_hash_dentry,
    .d_compare = ifs_amiga_intl_compare_dentry,
};
