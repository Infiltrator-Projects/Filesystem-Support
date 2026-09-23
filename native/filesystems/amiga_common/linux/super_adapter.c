/*
 * Shared project-authored Linux superblock/lifecycle adapter for AmigaDOS
 * OFS and FFS. The including filesystem supplies its format classifier,
 * filesystem name and OFS/FFS identity.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/statfs.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/sched.h>
#include <linux/cred.h>
#include <linux/slab.h>
#include <linux/writeback.h>
#include <linux/blkdev.h>
#include <linux/seq_file.h>
#include <linux/iversion.h>
#include <linux/math64.h>
#include <linux/limits.h>

struct ifs_amiga_mount_config {
    kuid_t uid;
    kgid_t gid;
    umode_t mode;
    int reserved;
    s32 root_block;
    int blocksize;
    char *prefix;
    char volume[32];
    unsigned long flags;
};

static int affs_statfs(struct dentry *dentry, struct kstatfs *buffer);
static int affs_show_options(struct seq_file *output, struct dentry *root);
static int affs_remount(struct super_block *sb, int *flags, char *data);

static void affs_commit_super(struct super_block *sb, bool wait)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct buffer_head *bh;
    struct affs_root_tail *tail;

    if (!sbi || !sbi->s_root_bh || sb_rdonly(sb))
        return;

    bh = sbi->s_root_bh;
    tail = AFFS_ROOT_TAIL(sb, bh);

    lock_buffer(bh);
    affs_secs_to_datestamp(ktime_get_real_seconds(), &tail->disk_change);
    affs_fix_checksum(sb, bh);
    unlock_buffer(bh);

    mark_buffer_dirty(bh);
    if (wait)
        sync_dirty_buffer(bh);
}

static void affs_put_super(struct super_block *sb)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);

    if (sbi)
        cancel_delayed_work_sync(&sbi->sb_work);
}

static int affs_sync_fs(struct super_block *sb, int wait)
{
    affs_commit_super(sb, wait != 0);
    return 0;
}

static void ifs_amiga_flush_super_work(struct work_struct *work)
{
    struct affs_sb_info *sbi =
        container_of(work, struct affs_sb_info, sb_work.work);

    spin_lock(&sbi->work_lock);
    sbi->work_queued = 0;
    spin_unlock(&sbi->work_lock);

    affs_commit_super(sbi->sb, true);
}

void affs_mark_sb_dirty(struct super_block *sb)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    unsigned long delay;

    if (!sbi || sb_rdonly(sb))
        return;

    spin_lock(&sbi->work_lock);
    if (!sbi->work_queued) {
        delay = msecs_to_jiffies(dirty_writeback_interval * 10U);
        sbi->work_queued = 1;
        queue_delayed_work(system_long_wq, &sbi->sb_work, delay);
    }
    spin_unlock(&sbi->work_lock);
}

static struct kmem_cache *affs_inode_cache;

static struct inode *ifs_amiga_alloc_inode(struct super_block *sb)
{
    struct affs_inode_info *info;

    info = alloc_inode_sb(sb, affs_inode_cache, GFP_KERNEL);
    if (!info)
        return NULL;

    inode_set_iversion(&info->vfs_inode, 1);
    info->i_lc = NULL;
    info->i_ext_bh = NULL;
    info->i_pa_cnt = 0;
    return &info->vfs_inode;
}

static void ifs_amiga_free_inode(struct inode *inode)
{
    kmem_cache_free(affs_inode_cache, AFFS_I(inode));
}

static void ifs_amiga_inode_init_once(void *object)
{
    struct affs_inode_info *info = object;

    mutex_init(&info->i_link_lock);
    mutex_init(&info->i_ext_lock);
    inode_init_once(&info->vfs_inode);
}

static int __init ifs_amiga_init_inode_cache(void)
{
    affs_inode_cache = kmem_cache_create(
        IFS_AMIGA_INODE_CACHE_NAME,
        sizeof(struct affs_inode_info), 0,
        SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
        ifs_amiga_inode_init_once);

    return affs_inode_cache ? 0 : -ENOMEM;
}

static void ifs_amiga_destroy_inode_cache(void)
{
    rcu_barrier();
    kmem_cache_destroy(affs_inode_cache);
    affs_inode_cache = NULL;
}

static const struct super_operations affs_sops = {
    .alloc_inode = ifs_amiga_alloc_inode,
    .free_inode = ifs_amiga_free_inode,
    .write_inode = affs_write_inode,
    .evict_inode = affs_evict_inode,
    .put_super = affs_put_super,
    .sync_fs = affs_sync_fs,
    .statfs = affs_statfs,
    .remount_fs = affs_remount,
    .show_options = affs_show_options,
};

enum ifs_amiga_option {
    IFS_OPT_BLOCKSIZE,
    IFS_OPT_MODE,
    IFS_OPT_MUFS,
    IFS_OPT_NO_TRUNCATE,
    IFS_OPT_PREFIX,
    IFS_OPT_PROTECT,
    IFS_OPT_RESERVED,
    IFS_OPT_ROOT,
    IFS_OPT_SETGID,
    IFS_OPT_SETUID,
    IFS_OPT_VERBOSE,
    IFS_OPT_VOLUME,
    IFS_OPT_IGNORE,
    IFS_OPT_ERROR,
};

static const match_table_t ifs_amiga_tokens = {
    { IFS_OPT_BLOCKSIZE, "bs=%u" },
    { IFS_OPT_MODE, "mode=%o" },
    { IFS_OPT_MUFS, "mufs" },
    { IFS_OPT_NO_TRUNCATE, "nofilenametruncate" },
    { IFS_OPT_PREFIX, "prefix=%s" },
    { IFS_OPT_PROTECT, "protect" },
    { IFS_OPT_RESERVED, "reserved=%u" },
    { IFS_OPT_ROOT, "root=%u" },
    { IFS_OPT_SETGID, "setgid=%u" },
    { IFS_OPT_SETUID, "setuid=%u" },
    { IFS_OPT_VERBOSE, "verbose" },
    { IFS_OPT_VOLUME, "volume=%s" },
    { IFS_OPT_IGNORE, "grpquota" },
    { IFS_OPT_IGNORE, "noquota" },
    { IFS_OPT_IGNORE, "quota" },
    { IFS_OPT_IGNORE, "usrquota" },
    { IFS_OPT_ERROR, NULL },
};

static void ifs_amiga_mount_config_defaults(
    struct ifs_amiga_mount_config *config)
{
    memset(config, 0, sizeof(*config));
    config->uid = current_uid();
    config->gid = current_gid();
    config->reserved = 2;
    config->root_block = -1;
    config->blocksize = -1;
    config->volume[0] = ':';
    config->volume[1] = '\0';
}

static int ifs_amiga_mount_config_from_super(
    struct super_block *sb, struct ifs_amiga_mount_config *config)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);

    memset(config, 0, sizeof(*config));
    config->uid = sbi->s_uid;
    config->gid = sbi->s_gid;
    config->mode = sbi->s_mode;
    config->reserved = sbi->s_reserved;
    config->root_block = (s32)sbi->s_root_block;
    config->blocksize = (int)sb->s_blocksize;
    config->flags = sbi->s_flags;
    memcpy(config->volume, sbi->s_volume, sizeof(config->volume));

    if (sbi->s_prefix) {
        config->prefix = kstrdup(sbi->s_prefix, GFP_KERNEL);
        if (!config->prefix)
            return -ENOMEM;
    }
    return 0;
}

static int ifs_amiga_parse_options(
    char *options, struct ifs_amiga_mount_config *config)
{
    char *item;

    if (!options)
        return 0;

    while ((item = strsep(&options, ",")) != NULL) {
        substring_t arguments[MAX_OPT_ARGS];
        int token;
        int value;

        if (*item == '\0')
            continue;

        token = match_token(item, ifs_amiga_tokens, arguments);
        switch (token) {
        case IFS_OPT_BLOCKSIZE:
            if (match_int(&arguments[0], &value) != 0 ||
                (value != 512 && value != 1024 &&
                 value != 2048 && value != 4096))
                return -EINVAL;
            config->blocksize = value;
            break;

        case IFS_OPT_MODE:
            if (match_octal(&arguments[0], &value) != 0)
                return -EINVAL;
            config->mode = value & 0777;
            affs_set_opt(config->flags, SF_SETMODE);
            break;

        case IFS_OPT_MUFS:
            affs_set_opt(config->flags, SF_MUFS);
            break;

        case IFS_OPT_NO_TRUNCATE:
            affs_set_opt(config->flags, SF_NO_TRUNCATE);
            break;

        case IFS_OPT_PREFIX: {
            char *new_prefix = match_strdup(&arguments[0]);

            if (!new_prefix)
                return -ENOMEM;
            kfree(config->prefix);
            config->prefix = new_prefix;
            affs_set_opt(config->flags, SF_PREFIX);
            break;
        }

        case IFS_OPT_PROTECT:
            affs_set_opt(config->flags, SF_IMMUTABLE);
            break;

        case IFS_OPT_RESERVED:
            if (match_int(&arguments[0], &value) != 0 || value < 2)
                return -EINVAL;
            config->reserved = value;
            break;

        case IFS_OPT_ROOT:
            if (match_int(&arguments[0], &value) != 0 || value < 0)
                return -EINVAL;
            config->root_block = value;
            break;

        case IFS_OPT_SETGID:
            if (match_int(&arguments[0], &value) != 0 || value < 0)
                return -EINVAL;
            config->gid = make_kgid(current_user_ns(), value);
            if (!gid_valid(config->gid))
                return -EINVAL;
            affs_set_opt(config->flags, SF_SETGID);
            break;

        case IFS_OPT_SETUID:
            if (match_int(&arguments[0], &value) != 0 || value < 0)
                return -EINVAL;
            config->uid = make_kuid(current_user_ns(), value);
            if (!uid_valid(config->uid))
                return -EINVAL;
            affs_set_opt(config->flags, SF_SETUID);
            break;

        case IFS_OPT_VERBOSE:
            affs_set_opt(config->flags, SF_VERBOSE);
            break;

        case IFS_OPT_VOLUME: {
            char *volume = match_strdup(&arguments[0]);

            if (!volume)
                return -ENOMEM;
            strscpy(config->volume, volume, sizeof(config->volume));
            kfree(volume);
            break;
        }

        case IFS_OPT_IGNORE:
            break;

        default:
            pr_warn("Unrecognized mount option \"%s\"\n", item);
            return -EINVAL;
        }
    }

    return 0;
}

static int affs_show_options(struct seq_file *output, struct dentry *root)
{
    struct super_block *sb = root->d_sb;
    struct affs_sb_info *sbi = AFFS_SB(sb);

    if (sb->s_blocksize != 0U)
        seq_printf(output, ",bs=%lu", sb->s_blocksize);
    if (affs_test_opt(sbi->s_flags, SF_SETMODE))
        seq_printf(output, ",mode=%o", sbi->s_mode);
    if (affs_test_opt(sbi->s_flags, SF_MUFS))
        seq_puts(output, ",mufs");
    if (affs_test_opt(sbi->s_flags, SF_NO_TRUNCATE))
        seq_puts(output, ",nofilenametruncate");
    if (affs_test_opt(sbi->s_flags, SF_PREFIX) && sbi->s_prefix)
        seq_printf(output, ",prefix=%s", sbi->s_prefix);
    if (affs_test_opt(sbi->s_flags, SF_IMMUTABLE))
        seq_puts(output, ",protect");
    if (sbi->s_reserved != 2)
        seq_printf(output, ",reserved=%u", sbi->s_reserved);
    if (sbi->s_root_block !=
        (u32)(sbi->s_reserved + sbi->s_partition_size - 1) / 2U)
        seq_printf(output, ",root=%u", sbi->s_root_block);
    if (affs_test_opt(sbi->s_flags, SF_SETGID))
        seq_printf(output, ",setgid=%u",
                   from_kgid_munged(&init_user_ns, sbi->s_gid));
    if (affs_test_opt(sbi->s_flags, SF_SETUID))
        seq_printf(output, ",setuid=%u",
                   from_kuid_munged(&init_user_ns, sbi->s_uid));
    if (affs_test_opt(sbi->s_flags, SF_VERBOSE))
        seq_puts(output, ",verbose");
    if (sbi->s_volume[0] != '\0')
        seq_printf(output, ",volume=%s", sbi->s_volume);
    return 0;
}

static int ifs_amiga_find_root(
    struct super_block *sb,
    const struct ifs_amiga_mount_config *config,
    struct buffer_head **root_out)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    const sector_t sectors = bdev_nr_sectors(sb->s_bdev);
    int first_blocksize = bdev_logical_block_size(sb->s_bdev);
    int last_blocksize = PAGE_SIZE;
    int blocksize;

    if (config->blocksize > 0)
        first_blocksize = last_blocksize = config->blocksize;

    if (first_blocksize < 512 || first_blocksize > PAGE_SIZE ||
        (first_blocksize & (first_blocksize - 1)) != 0)
        return -EINVAL;

    for (blocksize = first_blocksize;
         blocksize <= last_blocksize;
         blocksize <<= 1) {
        const u64 divisor = (u64)blocksize / 512ULL;
        const u64 block_count = div_u64((u64)sectors, divisor);
        u64 root;
        unsigned int attempt;

        if (block_count <= (u64)config->reserved ||
            block_count > INT_MAX)
            continue;

        if (!sb_set_blocksize(sb, blocksize))
            continue;

        sbi->s_partition_size = (int)block_count;
        root = config->root_block >= 0 ?
               (u64)config->root_block :
               ((u64)config->reserved + block_count - 1ULL) / 2ULL;

        for (attempt = 0U; attempt < 2U; ++attempt) {
            struct buffer_head *candidate;
            const u64 candidate_block = root + attempt;

            if (candidate_block > INT_MAX ||
                candidate_block >= block_count ||
                candidate_block < (u64)config->reserved)
                continue;

            sbi->s_root_block = (u32)candidate_block;
            candidate = affs_bread(sb, (int)candidate_block);
            if (!candidate)
                continue;

            if (affs_checksum_block(sb, candidate) == 0U &&
                be32_to_cpu(AFFS_ROOT_HEAD(candidate)->ptype) == T_SHORT &&
                be32_to_cpu(AFFS_ROOT_TAIL(sb, candidate)->stype) == ST_ROOT) {
                sbi->s_hashsize = blocksize / 4 - 56;
                if (sbi->s_hashsize <= 0) {
                    affs_brelse(candidate);
                    return -EUCLEAN;
                }

                *root_out = candidate;
                return 0;
            }

            affs_brelse(candidate);
        }
    }

    return -EINVAL;
}

static int ifs_amiga_apply_format_identity(
    struct super_block *sb, const u32 dostype)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    u32 variant_flags = 0U;
    int result;

    result = ifs_amiga_variant_classify(dostype, &variant_flags);
    if (result != 0) {
        pr_err("Not an %s filesystem on device %s: %08X\n",
               IFS_AMIGA_FORMAT_LABEL, sb->s_id, dostype);
        return -EINVAL;
    }

    if ((variant_flags & IFS_AMIGA_VARIANT_DIRCACHE) != 0U &&
        !sb_rdonly(sb)) {
        pr_notice("Dircache media - mounting %s read only\n", sb->s_id);
        sb->s_flags |= SB_RDONLY;
    }

    if ((variant_flags & IFS_AMIGA_VARIANT_MUFS) != 0U)
        affs_set_opt(sbi->s_flags, SF_MUFS);
    if ((variant_flags & IFS_AMIGA_VARIANT_INTL) != 0U)
        affs_set_opt(sbi->s_flags, SF_INTL);

#if IFS_AMIGA_IS_OFS
    affs_set_opt(sbi->s_flags, SF_OFS);
    sb->s_flags |= SB_NOEXEC;
#endif

    return 0;
}

static void ifs_amiga_free_super_info(struct super_block *sb)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);

    if (!sbi)
        return;

    cancel_delayed_work_sync(&sbi->sb_work);
    affs_free_bitmap(sb);
    affs_brelse(sbi->s_root_bh);
    sbi->s_root_bh = NULL;
    kfree(sbi->s_prefix);
    sbi->s_prefix = NULL;
    mutex_destroy(&sbi->s_bmlock);
    kfree(sbi);
    sb->s_fs_info = NULL;
}

static int affs_fill_super(struct super_block *sb, void *data, int silent)
{
    struct ifs_amiga_mount_config config;
    struct affs_sb_info *sbi;
    struct buffer_head *root_bh = NULL;
    struct buffer_head *boot_bh = NULL;
    struct inode *root_inode;
    u8 signature[4];
    u32 dostype;
    int bitmap_flags;
    int result;

    ifs_amiga_mount_config_defaults(&config);
    result = ifs_amiga_parse_options(data, &config);
    if (result != 0)
        goto out_config;

    sb->s_magic = AFFS_SUPER_MAGIC;
    sb->s_op = &affs_sops;
    sb->s_flags |= SB_NODIRATIME;
    sb->s_time_gran = NSEC_PER_SEC;
    sb->s_time_min =
        (time64_t)sys_tz.tz_minuteswest * 60LL + AFFS_EPOCH_DELTA;
    sb->s_time_max =
        86400LL * (time64_t)U32_MAX + 86400LL + sb->s_time_min;
    sb->s_maxbytes = U32_MAX;

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi) {
        result = -ENOMEM;
        goto out_config;
    }

    sb->s_fs_info = sbi;
    sbi->sb = sb;
    sbi->s_flags = config.flags;
    sbi->s_mode = config.mode;
    sbi->s_uid = config.uid;
    sbi->s_gid = config.gid;
    sbi->s_reserved = config.reserved;
    sbi->s_prefix = config.prefix;
    config.prefix = NULL;
    memcpy(sbi->s_volume, config.volume, sizeof(sbi->s_volume));

    mutex_init(&sbi->s_bmlock);
    spin_lock_init(&sbi->symlink_lock);
    spin_lock_init(&sbi->work_lock);
    INIT_DELAYED_WORK(&sbi->sb_work, ifs_amiga_flush_super_work);

    result = ifs_amiga_find_root(sb, &config, &root_bh);
    if (result != 0) {
        if (!silent)
            pr_err("No valid root block on device %s\n", sb->s_id);
        goto fail;
    }

    sbi->s_root_bh = root_bh;
    root_bh = NULL;

    boot_bh = sb_bread(sb, 0);
    if (!boot_bh) {
        result = -EIO;
        goto fail;
    }

    memcpy(signature, boot_bh->b_data, sizeof(signature));
    brelse(boot_bh);
    boot_bh = NULL;
    dostype = be32_to_cpu(*(__be32 *)signature);

    result = ifs_amiga_apply_format_identity(sb, dostype);
    if (result != 0)
        goto fail;

    if (affs_test_opt(sbi->s_flags, SF_VERBOSE)) {
        const u8 stored_length =
            AFFS_ROOT_TAIL(sb, sbi->s_root_bh)->disk_name[0];
        const int display_length =
            stored_length > 31U ? 31 : (int)stored_length;

        pr_notice(
            "Mounting volume \"%.*s\": Type=%.3s\\%c, Blocksize=%lu\n",
            display_length,
            AFFS_ROOT_TAIL(sb, sbi->s_root_bh)->disk_name + 1,
            signature, signature[3] + '0', sb->s_blocksize);
    }

    sb->s_flags |= SB_NODEV | SB_NOSUID;
    sbi->s_data_blksize = (u32)sb->s_blocksize;
#if IFS_AMIGA_IS_OFS
    if (sbi->s_data_blksize <= sizeof(struct affs_data_head)) {
        result = -EUCLEAN;
        goto fail;
    }
    sbi->s_data_blksize -= sizeof(struct affs_data_head);
#endif

    bitmap_flags = sb->s_flags;
    result = affs_init_bitmap(sb, &bitmap_flags);
    if (result != 0)
        goto fail;
    sb->s_flags = bitmap_flags;

    root_inode = affs_iget(sb, sbi->s_root_block);
    if (IS_ERR(root_inode)) {
        result = PTR_ERR(root_inode);
        goto fail;
    }

    sb->s_d_op = affs_test_opt(sbi->s_flags, SF_INTL) ?
                 &affs_intl_dentry_operations :
                 &affs_dentry_operations;

    sb->s_root = d_make_root(root_inode);
    if (!sb->s_root) {
        result = -ENOMEM;
        goto fail;
    }

    sb->s_export_op = &affs_export_ops;
    result = 0;
    goto out_config;

fail:
    brelse(boot_bh);
    affs_brelse(root_bh);
    ifs_amiga_free_super_info(sb);

out_config:
    kfree(config.prefix);
    return result;
}

static int affs_remount(struct super_block *sb, int *flags, char *data)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct ifs_amiga_mount_config config;
    char *old_prefix;
    int result;

    result = ifs_amiga_mount_config_from_super(sb, &config);
    if (result != 0)
        return result;

    result = ifs_amiga_parse_options(data, &config);
    if (result != 0)
        goto out;

    if (config.reserved != sbi->s_reserved ||
        config.root_block != (s32)sbi->s_root_block ||
        config.blocksize != (int)sb->s_blocksize) {
        result = -EINVAL;
        goto out;
    }

    sync_filesystem(sb);
    flush_delayed_work(&sbi->sb_work);
    *flags |= SB_NODIRATIME;

    /*
     * Media-derived format identity cannot be changed by remount options.
     */
    config.flags |= sbi->s_flags &
        (AFFS_MOUNT_SF_INTL | AFFS_MOUNT_SF_MUFS |
         AFFS_MOUNT_SF_OFS);

    sbi->s_flags = config.flags;
    sbi->s_mode = config.mode;
    sbi->s_uid = config.uid;
    sbi->s_gid = config.gid;

    spin_lock(&sbi->symlink_lock);
    old_prefix = sbi->s_prefix;
    sbi->s_prefix = config.prefix;
    config.prefix = NULL;
    memcpy(sbi->s_volume, config.volume, sizeof(sbi->s_volume));
    spin_unlock(&sbi->symlink_lock);
    kfree(old_prefix);

    if ((bool)(*flags & SB_RDONLY) != sb_rdonly(sb)) {
        if ((*flags & SB_RDONLY) != 0)
            affs_free_bitmap(sb);
        else {
            result = affs_init_bitmap(sb, flags);
            if (result != 0)
                goto out;
        }
    }

    result = 0;

out:
    kfree(config.prefix);
    return result;
}

static int affs_statfs(struct dentry *dentry, struct kstatfs *buffer)
{
    struct super_block *sb = dentry->d_sb;
    struct affs_sb_info *sbi = AFFS_SB(sb);
    const u64 id = huge_encode_dev(sb->s_bdev->bd_dev);
    const u32 free_blocks = affs_count_free_blocks(sb);

    buffer->f_type = AFFS_SUPER_MAGIC;
    buffer->f_bsize = sb->s_blocksize;
    buffer->f_blocks =
        (u64)sbi->s_partition_size - (u64)sbi->s_reserved;
    buffer->f_bfree = free_blocks;
    buffer->f_bavail = free_blocks;
    buffer->f_fsid = u64_to_fsid(id);
    buffer->f_namelen = AFFSNAMEMAX;
    return 0;
}

static struct dentry *affs_mount(
    struct file_system_type *type, int flags,
    const char *device, void *data)
{
    return mount_bdev(type, flags, device, data, affs_fill_super);
}

static void affs_kill_sb(struct super_block *sb)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);

    kill_block_super(sb);
    if (!sbi)
        return;

    affs_free_bitmap(sb);
    affs_brelse(sbi->s_root_bh);
    kfree(sbi->s_prefix);
    mutex_destroy(&sbi->s_bmlock);
    kfree_rcu(sbi, rcu);
}

static struct file_system_type affs_fs_type = {
    .owner = THIS_MODULE,
    .name = IFS_AMIGA_FS_NAME,
    .mount = affs_mount,
    .kill_sb = affs_kill_sb,
    .fs_flags = FS_REQUIRES_DEV,
};

MODULE_ALIAS_FS(IFS_AMIGA_FS_NAME);

static int __init ifs_amiga_init_fs(void)
{
    int result = ifs_amiga_init_inode_cache();

    if (result != 0)
        return result;

    result = register_filesystem(&affs_fs_type);
    if (result != 0)
        ifs_amiga_destroy_inode_cache();
    return result;
}

static void __exit ifs_amiga_exit_fs(void)
{
    unregister_filesystem(&affs_fs_type);
    ifs_amiga_destroy_inode_cache();
}

MODULE_DESCRIPTION(IFS_AMIGA_MODULE_DESCRIPTION);
MODULE_LICENSE("GPL");

module_init(ifs_amiga_init_fs)
module_exit(ifs_amiga_exit_fs)
