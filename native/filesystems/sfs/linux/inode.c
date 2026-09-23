#include <linux/buffer_head.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/time.h>
#include "asfs_fs.h"

#define SFS_UNIX_EPOCH_DELTA ((365LL * 8LL + 2LL) * 24LL * 60LL * 60LL)

#ifdef CONFIG_ASFS_RW
static int sfs_create(
    struct mnt_idmap *idmap,
    struct inode *dir,
    struct dentry *dentry,
    umode_t mode,
    bool exclusive);
static int sfs_mkdir(
    struct mnt_idmap *idmap,
    struct inode *dir,
    struct dentry *dentry,
    umode_t mode);
static int sfs_symlink(
    struct mnt_idmap *idmap,
    struct inode *dir,
    struct dentry *dentry,
    const char *target);
static int sfs_rmdir(struct inode *dir, struct dentry *dentry);
static int sfs_unlink(struct inode *dir, struct dentry *dentry);
static int sfs_rename(
    struct mnt_idmap *idmap,
    struct inode *old_dir,
    struct dentry *old_dentry,
    struct inode *new_dir,
    struct dentry *new_dentry,
    unsigned int flags);
static int sfs_setattr(
    struct mnt_idmap *idmap,
    struct dentry *dentry,
    struct iattr *attributes);
#endif

static const struct address_space_operations sfs_aops = {
    .dirty_folio = block_dirty_folio,
    .invalidate_folio = block_invalidate_folio,
    .read_folio = asfs_read_folio,
    .readahead = asfs_readahead,
    .bmap = asfs_bmap,
#ifdef CONFIG_ASFS_RW
    .write_begin = asfs_write_begin,
    .write_end = generic_write_end,
    .writepages = asfs_writepages,
#endif
};

static const struct file_operations sfs_file_operations = {
    .llseek = generic_file_llseek,
    .read_iter = generic_file_read_iter,
    .mmap = generic_file_mmap,
    .splice_read = filemap_splice_read,
#ifdef CONFIG_ASFS_RW
    .write_iter = generic_file_write_iter,
    .open = asfs_file_open,
    .release = asfs_file_release,
    .fsync = generic_file_fsync,
    .splice_write = iter_file_splice_write,
#endif
};

static const struct file_operations sfs_dir_operations = {
    .read = generic_read_dir,
    .iterate_shared = asfs_readdir,
    .llseek = generic_file_llseek,
};

static const struct inode_operations sfs_dir_inode_operations = {
    .lookup = asfs_lookup,
#ifdef CONFIG_ASFS_RW
    .create = sfs_create,
    .unlink = sfs_unlink,
    .symlink = sfs_symlink,
    .mkdir = sfs_mkdir,
    .rmdir = sfs_rmdir,
    .rename = sfs_rename,
    .setattr = sfs_setattr,
#endif
};

static const struct inode_operations sfs_file_inode_operations = {
#ifdef CONFIG_ASFS_RW
    .setattr = sfs_setattr,
#endif
};

static const struct inode_operations sfs_symlink_inode_operations = {
    .get_link = asfs_get_link,
#ifdef CONFIG_ASFS_RW
    .setattr = sfs_setattr,
#endif
};

static umode_t sfs_directory_mode(umode_t base)
{
    umode_t mode = base | S_IFDIR;

    if ((base & 0400) != 0)
        mode |= 0100;
    if ((base & 0040) != 0)
        mode |= 0010;
    if ((base & 0004) != 0)
        mode |= 0001;
    return mode;
}

void asfs_read_locked_inode(struct inode *inode, void *argument)
{
    struct fsObject *object = argument;
    struct super_block *sb = inode->i_sb;
    const time64_t timestamp =
        (time64_t)be32_to_cpu(object->datemodified) +
        SFS_UNIX_EPOCH_DELTA;

    inode->i_mode = ASFS_SB(sb)->mode;
    inode_set_atime(inode, timestamp, 0);
    inode_set_mtime(inode, timestamp, 0);
    inode_set_ctime(inode, timestamp, 0);
    i_uid_write(inode, ASFS_SB(sb)->uid);
    i_gid_write(inode, ASFS_SB(sb)->gid);
    atomic_set(&ASFS_I(inode)->i_opencnt, 0);
    ASFS_I(inode)->modified = 0;
    ASFS_I(inode)->hashtable = 0U;
    ASFS_I(inode)->ext_cache.startblock = 0U;
    ASFS_I(inode)->ext_cache.key = 0U;
    ASFS_I(inode)->ext_cache.next = 0U;
    ASFS_I(inode)->ext_cache.blocks = 0U;

    if ((object->bits & OTYPE_DIR) != 0U) {
        inode->i_size = 0;
        inode->i_mode = sfs_directory_mode(inode->i_mode);
        inode->i_op = &sfs_dir_inode_operations;
        inode->i_fop = &sfs_dir_operations;
        ASFS_I(inode)->firstblock =
            be32_to_cpu(object->object.dir.firstdirblock);
        ASFS_I(inode)->hashtable =
            be32_to_cpu(object->object.dir.hashtable);
        return;
    }

    if ((object->bits & OTYPE_LINK) != 0U &&
        (object->bits & OTYPE_HARDLINK) == 0U) {
        inode->i_size = 0;
        inode->i_mode = S_IFLNK | S_IRWXUGO;
        inode->i_op = &sfs_symlink_inode_operations;
        ASFS_I(inode)->firstblock =
            be32_to_cpu(object->object.file.data);
        return;
    }

    inode->i_size = be32_to_cpu(object->object.file.size);
    inode->i_blocks = DIV_ROUND_UP(
        (u64)inode->i_size, sb->s_blocksize);
    inode->i_mode |= S_IFREG;
    inode->i_op = &sfs_file_inode_operations;
    inode->i_fop = &sfs_file_operations;
    inode->i_mapping->a_ops = &sfs_aops;
    ASFS_I(inode)->firstblock =
        be32_to_cpu(object->object.file.data);
    ASFS_I(inode)->mmu_private = inode->i_size;
}

struct inode *asfs_get_root_inode(struct super_block *sb)
{
    struct buffer_head *bh;
    struct fsObjectContainer *container;
    struct fsObject *object;
    struct inode *inode = NULL;
    u32 node;

    bh = asfs_breadcheck(
        sb, ASFS_SB(sb)->rootobjectcontainer,
        ASFS_OBJECTCONTAINER_ID);
    if (!bh)
        return NULL;

    container = (struct fsObjectContainer *)bh->b_data;
    object = &container->object[0];
    if (!asfs_object_slot_fits(sb, container, object))
        goto out;

    node = be32_to_cpu(object->objectnode);
    if (node == 0U)
        goto out;

    inode = iget_locked(sb, node);
    if (inode && (inode->i_state & I_NEW) != 0) {
        asfs_read_locked_inode(inode, object);
        unlock_new_inode(inode);
    }

out:
    asfs_brelse(bh);
    return inode;
}

#ifdef CONFIG_ASFS_RW

static void sfs_set_current_times(struct inode *inode)
{
    const struct timespec64 now = current_time(inode);

    inode_set_atime(inode, now.tv_sec, now.tv_nsec);
    inode_set_mtime_to_ts(inode, now);
    inode_set_ctime_to_ts(inode, now);
}

static void sfs_sync_directory_object(
    struct inode *dir,
    struct fsObject *object)
{
    ASFS_I(dir)->firstblock =
        be32_to_cpu(object->object.dir.firstdirblock);
    ASFS_I(dir)->hashtable =
        be32_to_cpu(object->object.dir.hashtable);
    ASFS_I(dir)->modified = 1;
    sfs_set_current_times(dir);
    object->datemodified = cpu_to_be32(
        (u32)(inode_get_mtime_sec(dir) -
              SFS_UNIX_EPOCH_DELTA));
}

static void sfs_release_pair(
    struct buffer_head *first,
    struct buffer_head *second)
{
    if (second && second != first)
        asfs_brelse(second);
    asfs_brelse(first);
}

enum sfs_new_object_type {
    SFS_NEW_FILE = 0,
    SFS_NEW_DIRECTORY,
    SFS_NEW_SYMLINK
};

static int sfs_create_object(
    struct inode *dir,
    struct dentry *dentry,
    umode_t mode,
    enum sfs_new_object_type type,
    const char *symlink_target)
{
    struct super_block *sb = dir->i_sb;
    struct inode *inode;
    struct buffer_head *dir_bh = NULL;
    struct buffer_head *object_bh = NULL;
    struct fsObject *dir_object = NULL;
    struct fsObject *object = NULL;
    struct fsObject template;
    u8 disk_name[ASFS_MAXFN_BUF];
    int result;

    asfs_translate(
        disk_name, (u8 *)dentry->d_name.name,
        ASFS_SB(sb)->nls_disk, ASFS_SB(sb)->nls_io,
        sizeof(disk_name));
    result = asfs_check_name(
        disk_name,
        strnlen((const char *)disk_name, sizeof(disk_name)));
    if (result != 0)
        return result;

    inode = new_inode(sb);
    if (!inode)
        return -ENOMEM;

    sfs_set_current_times(inode);
    memset(&template, 0, sizeof(template));
    template.protection = cpu_to_be32(
        FIBF_READ | FIBF_WRITE | FIBF_EXECUTE | FIBF_DELETE);
    template.datemodified = cpu_to_be32(
        (u32)(inode_get_mtime_sec(inode) -
              SFS_UNIX_EPOCH_DELTA));

    if (type == SFS_NEW_DIRECTORY)
        template.bits = OTYPE_DIR;
    else if (type == SFS_NEW_SYMLINK)
        template.bits = OTYPE_LINK;

    mutex_lock(&ASFS_SB(sb)->lock);

    result = asfs_readobject(
        sb, (u32)dir->i_ino, &dir_bh, &dir_object);
    if (result != 0)
        goto fail_locked;

    object_bh = dir_bh;
    object = dir_object;
    result = asfs_createobject(
        sb, &object_bh, &object, &template, disk_name, FALSE);
    if (result != 0)
        goto fail_buffers;

    inode->i_ino = be32_to_cpu(object->objectnode);
    inode->i_size = 0;
    inode->i_blocks = 0;
    i_uid_write(inode, i_uid_read(dir));
    i_gid_write(inode, i_gid_read(dir));
    inode->i_mode = mode | ASFS_SB(sb)->mode;
    atomic_set(&ASFS_I(inode)->i_opencnt, 0);
    ASFS_I(inode)->modified = 0;
    ASFS_I(inode)->ext_cache.key = 0U;

    switch (type) {
    case SFS_NEW_DIRECTORY:
        inode->i_mode = sfs_directory_mode(inode->i_mode);
        inode->i_op = &sfs_dir_inode_operations;
        inode->i_fop = &sfs_dir_operations;
        ASFS_I(inode)->firstblock =
            be32_to_cpu(object->object.dir.firstdirblock);
        ASFS_I(inode)->hashtable =
            be32_to_cpu(object->object.dir.hashtable);
        break;
    case SFS_NEW_SYMLINK:
        inode->i_mode = S_IFLNK | S_IRWXUGO;
        inode->i_op = &sfs_symlink_inode_operations;
        ASFS_I(inode)->firstblock =
            be32_to_cpu(object->object.file.data);
        result = asfs_write_symlink(inode, symlink_target);
        break;
    case SFS_NEW_FILE:
        inode->i_mode |= S_IFREG;
        inode->i_op = &sfs_file_inode_operations;
        inode->i_fop = &sfs_file_operations;
        inode->i_mapping->a_ops = &sfs_aops;
        ASFS_I(inode)->firstblock =
            be32_to_cpu(object->object.file.data);
        ASFS_I(inode)->mmu_private = 0;
        break;
    }

    if (result != 0) {
        (void)asfs_deleteobject(sb, object_bh, object);
        goto fail_buffers;
    }

    asfs_bstore(sb, object_bh);
    insert_inode_hash(inode);
    mark_inode_dirty(inode);
    d_instantiate(dentry, inode);

    sfs_sync_directory_object(dir, dir_object);
    asfs_bstore(sb, dir_bh);
    mark_inode_dirty(dir);

    sfs_release_pair(object_bh, dir_bh);
    mutex_unlock(&ASFS_SB(sb)->lock);
    return 0;

fail_buffers:
    sfs_release_pair(object_bh, dir_bh);
fail_locked:
    mutex_unlock(&ASFS_SB(sb)->lock);
    iput(inode);
    return result;
}

static int sfs_create(
    struct mnt_idmap *idmap,
    struct inode *dir,
    struct dentry *dentry,
    umode_t mode,
    bool exclusive)
{
    (void)idmap;
    (void)exclusive;
    return sfs_create_object(
        dir, dentry, mode, SFS_NEW_FILE, NULL);
}

static int sfs_mkdir(
    struct mnt_idmap *idmap,
    struct inode *dir,
    struct dentry *dentry,
    umode_t mode)
{
    (void)idmap;
    return sfs_create_object(
        dir, dentry, mode, SFS_NEW_DIRECTORY, NULL);
}

static int sfs_symlink(
    struct mnt_idmap *idmap,
    struct inode *dir,
    struct dentry *dentry,
    const char *target)
{
    (void)idmap;
    return sfs_create_object(
        dir, dentry, 0, SFS_NEW_SYMLINK, target);
}

static int sfs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);
    struct super_block *sb = dir->i_sb;
    struct buffer_head *object_bh = NULL;
    struct buffer_head *dir_bh = NULL;
    struct fsObject *object = NULL;
    struct fsObject *dir_object = NULL;
    int result;

    mutex_lock(&ASFS_SB(sb)->lock);

    result = asfs_readobject(
        sb, (u32)inode->i_ino, &object_bh, &object);
    if (result != 0)
        goto out_unlock;

    result = asfs_deleteobject(sb, object_bh, object);
    asfs_brelse(object_bh);
    object_bh = NULL;
    if (result != 0)
        goto out_unlock;

    result = asfs_readobject(
        sb, (u32)dir->i_ino, &dir_bh, &dir_object);
    if (result != 0)
        goto out_unlock;

    sfs_sync_directory_object(dir, dir_object);
    asfs_bstore(sb, dir_bh);
    asfs_brelse(dir_bh);
    dir_bh = NULL;

    drop_nlink(inode);
    mark_inode_dirty(inode);
    mark_inode_dirty(dir);

out_unlock:
    asfs_brelse(object_bh);
    asfs_brelse(dir_bh);
    mutex_unlock(&ASFS_SB(sb)->lock);
    return result;
}

static int sfs_rmdir(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);

    if (ASFS_I(inode)->firstblock != 0U)
        return -ENOTEMPTY;

    return sfs_unlink(dir, dentry);
}

static int sfs_rename(
    struct mnt_idmap *idmap,
    struct inode *old_dir,
    struct dentry *old_dentry,
    struct inode *new_dir,
    struct dentry *new_dentry,
    unsigned int flags)
{
    struct super_block *sb = old_dir->i_sb;
    struct buffer_head *source_bh = NULL;
    struct buffer_head *new_dir_bh = NULL;
    struct buffer_head *old_dir_bh = NULL;
    struct fsObject *source = NULL;
    struct fsObject *new_dir_object = NULL;
    struct fsObject *old_dir_object = NULL;
    u8 disk_name[ASFS_MAXFN_BUF];
    int result;

    (void)idmap;

    if (flags != 0U)
        return -EINVAL;

    asfs_translate(
        disk_name, (u8 *)new_dentry->d_name.name,
        ASFS_SB(sb)->nls_disk, ASFS_SB(sb)->nls_io,
        sizeof(disk_name));
    result = asfs_check_name(
        disk_name,
        strnlen((const char *)disk_name, sizeof(disk_name)));
    if (result != 0)
        return result;

    if (d_really_is_positive(new_dentry)) {
        result = S_ISDIR(d_inode(new_dentry)->i_mode)
            ? sfs_rmdir(new_dir, new_dentry)
            : sfs_unlink(new_dir, new_dentry);
        if (result != 0)
            return result;
    }

    mutex_lock(&ASFS_SB(sb)->lock);

    result = asfs_readobject(
        sb, (u32)d_inode(old_dentry)->i_ino,
        &source_bh, &source);
    if (result != 0)
        goto out;

    result = asfs_readobject(
        sb, (u32)new_dir->i_ino,
        &new_dir_bh, &new_dir_object);
    if (result != 0)
        goto out;

    result = asfs_renameobject(
        sb, source_bh, source,
        new_dir_bh, new_dir_object, disk_name);
    if (result != 0)
        goto out;

    asfs_brelse(source_bh);
    source_bh = NULL;
    asfs_brelse(new_dir_bh);
    new_dir_bh = NULL;

    result = asfs_readobject(
        sb, (u32)old_dir->i_ino,
        &old_dir_bh, &old_dir_object);
    if (result != 0)
        goto out;

    if (old_dir == new_dir) {
        new_dir_bh = old_dir_bh;
        new_dir_object = old_dir_object;
    } else {
        result = asfs_readobject(
            sb, (u32)new_dir->i_ino,
            &new_dir_bh, &new_dir_object);
        if (result != 0)
            goto out;
    }

    sfs_sync_directory_object(old_dir, old_dir_object);
    if (old_dir != new_dir)
        sfs_sync_directory_object(new_dir, new_dir_object);

    asfs_bstore(sb, old_dir_bh);
    if (new_dir_bh != old_dir_bh)
        asfs_bstore(sb, new_dir_bh);

    mark_inode_dirty(old_dir);
    if (old_dir != new_dir)
        mark_inode_dirty(new_dir);

out:
    if (new_dir_bh && new_dir_bh != old_dir_bh)
        asfs_brelse(new_dir_bh);
    asfs_brelse(old_dir_bh);
    asfs_brelse(source_bh);
    mutex_unlock(&ASFS_SB(sb)->lock);
    return result;
}

static int sfs_setattr(
    struct mnt_idmap *idmap,
    struct dentry *dentry,
    struct iattr *attributes)
{
    struct inode *inode = d_inode(dentry);
    const loff_t old_size = i_size_read(inode);
    int result;

    result = setattr_prepare(idmap, dentry, attributes);
    if (result != 0)
        return result;

    if ((attributes->ia_valid & ATTR_SIZE) != 0 &&
        attributes->ia_size != old_size) {
        if (attributes->ia_size > ASFS_I(inode)->mmu_private)
            return -EOPNOTSUPP;

        truncate_setsize(inode, attributes->ia_size);
        result = asfs_truncate(inode);
        if (result != 0) {
            truncate_setsize(inode, old_size);
            return result;
        }
    }

    setattr_copy(idmap, inode, attributes);
    mark_inode_dirty(inode);
    return 0;
}

#endif
