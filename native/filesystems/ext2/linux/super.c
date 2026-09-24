/*
 * Filesystem Support EXT2 Linux mount adapter.
 *
 * The canonical EXT2 core owns on-disk feature and geometry validation.
 * This unit owns only Linux VFS publication, mount policy, resource lifetime,
 * counters, export operations and durability plumbing.
 */

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/blkdev.h>
#include <linux/parser.h>
#include <linux/random.h>
#include <linux/buffer_head.h>
#include <linux/exportfs.h>
#include <linux/vfs.h>
#include <linux/seq_file.h>
#include <linux/mount.h>
#include <linux/log2.h>
#include <linux/quotaops.h>
#include <linux/dax.h>
#include <linux/iversion.h>

#include "ext2.h"

static struct kmem_cache *ext2_inode_cachep;

static void ext2_write_super(struct super_block *sb);
static int ext2_sync_fs(struct super_block *sb, int wait);
static int ext2_remount(struct super_block *sb, int *flags, char *data);
static int ext2_statfs(struct dentry *dentry, struct kstatfs *buf);
static int ext2_freeze(struct super_block *sb);
static int ext2_unfreeze(struct super_block *sb);

void ext2_msg(struct super_block *sb, const char *level,
	      const char *format, ...)
{
	struct va_format message;
	va_list arguments;

	va_start(arguments, format);
	message.fmt = format;
	message.va = &arguments;
	printk("%sEXT2-fs (%s): %pV\n", level, sb->s_id, &message);
	va_end(arguments);
}

void ext2_error(struct super_block *sb, const char *function,
		const char *format, ...)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct va_format message;
	va_list arguments;

	if (sbi && sbi->s_es && !sb_rdonly(sb)) {
		spin_lock(&sbi->s_lock);
		sbi->s_mount_state |= EXT2_ERROR_FS;
		sbi->s_es->s_state |= cpu_to_le16(EXT2_ERROR_FS);
		spin_unlock(&sbi->s_lock);
		ext2_sync_super(sb, sbi->s_es, 1);
	}

	va_start(arguments, format);
	message.fmt = format;
	message.va = &arguments;
	printk(KERN_CRIT "EXT2-fs (%s): %s: %pV\n",
	       sb->s_id, function, &message);
	va_end(arguments);

	if (sbi && test_opt(sb, ERRORS_PANIC))
		panic("EXT2-fs: panic requested after filesystem error\n");

	if (sbi && !sb_rdonly(sb) && test_opt(sb, ERRORS_RO)) {
		ext2_msg(sb, KERN_CRIT, "remounting filesystem read-only");
		sb->s_flags |= SB_RDONLY;
	}
}

void ext2_update_dynamic_rev(struct super_block *sb)
{
	struct ext2_super_block *es = EXT2_SB(sb)->s_es;

	if (le32_to_cpu(es->s_rev_level) != EXT2_GOOD_OLD_REV)
		return;

	es->s_first_ino = cpu_to_le32(EXT2_GOOD_OLD_FIRST_INO);
	es->s_inode_size = cpu_to_le16(EXT2_GOOD_OLD_INODE_SIZE);
	es->s_rev_level = cpu_to_le32(EXT2_DYNAMIC_REV);
	mark_buffer_dirty(EXT2_SB(sb)->s_sbh);
}

static void ext2_clear_super_io_error(struct super_block *sb)
{
	struct buffer_head *sbh = EXT2_SB(sb)->s_sbh;

	if (!buffer_write_io_error(sbh))
		return;

	ext2_msg(sb, KERN_ERR, "previous superblock write error detected");
	clear_buffer_write_io_error(sbh);
	set_buffer_uptodate(sbh);
}

void ext2_sync_super(struct super_block *sb, struct ext2_super_block *es,
		     int wait)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	ext2_clear_super_io_error(sb);

	spin_lock(&sbi->s_lock);
	es->s_free_blocks_count = cpu_to_le32(ext2_count_free_blocks(sb));
	es->s_free_inodes_count = cpu_to_le32(ext2_count_free_inodes(sb));
	es->s_wtime = cpu_to_le32((u32)ktime_get_real_seconds());
	spin_unlock(&sbi->s_lock);

	mark_buffer_dirty(sbi->s_sbh);
	if (wait)
		sync_dirty_buffer(sbi->s_sbh);
}

static struct inode *ext2_alloc_inode(struct super_block *sb)
{
	struct ext2_inode_info *info;

	info = alloc_inode_sb(sb, ext2_inode_cachep, GFP_KERNEL);
	if (!info)
		return NULL;

	info->i_block_alloc_info = NULL;
	inode_set_iversion(&info->vfs_inode, 1);
#ifdef CONFIG_QUOTA
	memset(info->i_dquot, 0, sizeof(info->i_dquot));
#endif
	return &info->vfs_inode;
}

static void ext2_free_core_inode(struct inode *inode)
{
	kmem_cache_free(ext2_inode_cachep, EXT2_I(inode));
}

static void ext2_inode_ctor(void *object)
{
	struct ext2_inode_info *info = object;

	rwlock_init(&info->i_meta_lock);
#ifdef CONFIG_EXT2_FS_XATTR
	init_rwsem(&info->xattr_sem);
#endif
	mutex_init(&info->truncate_mutex);
	INIT_LIST_HEAD(&info->i_orphan);
	inode_init_once(&info->vfs_inode);
}

static int ext2_create_inode_cache(void)
{
	ext2_inode_cachep = kmem_cache_create_usercopy(
		"ext2_inode_cache",
		sizeof(struct ext2_inode_info),
		0,
		SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
		offsetof(struct ext2_inode_info, i_data),
		sizeof_field(struct ext2_inode_info, i_data),
		ext2_inode_ctor);

	return ext2_inode_cachep ? 0 : -ENOMEM;
}

static void ext2_destroy_inode_cache(void)
{
	rcu_barrier();
	kmem_cache_destroy(ext2_inode_cachep);
	ext2_inode_cachep = NULL;
}

enum ext2_option {
	EXT2_OPT_BSD_DF,
	EXT2_OPT_MINIX_DF,
	EXT2_OPT_GRPID,
	EXT2_OPT_NOGRPID,
	EXT2_OPT_RESGID,
	EXT2_OPT_RESUID,
	EXT2_OPT_SB,
	EXT2_OPT_ERRORS_CONTINUE,
	EXT2_OPT_ERRORS_PANIC,
	EXT2_OPT_ERRORS_RO,
	EXT2_OPT_NOUID32,
	EXT2_OPT_DEBUG,
	EXT2_OPT_OLDALLOC,
	EXT2_OPT_ORLOV,
	EXT2_OPT_NOBH,
	EXT2_OPT_USER_XATTR,
	EXT2_OPT_NOUSER_XATTR,
	EXT2_OPT_ACL,
	EXT2_OPT_NOACL,
	EXT2_OPT_XIP,
	EXT2_OPT_DAX,
	EXT2_OPT_NOQUOTA,
	EXT2_OPT_QUOTA,
	EXT2_OPT_USRQUOTA,
	EXT2_OPT_GRPQUOTA,
	EXT2_OPT_RESERVATION,
	EXT2_OPT_NORESERVATION,
	EXT2_OPT_INVALID,
};

static const match_table_t ext2_option_tokens = {
	{ EXT2_OPT_BSD_DF, "bsddf" },
	{ EXT2_OPT_MINIX_DF, "minixdf" },
	{ EXT2_OPT_GRPID, "grpid" },
	{ EXT2_OPT_GRPID, "bsdgroups" },
	{ EXT2_OPT_NOGRPID, "nogrpid" },
	{ EXT2_OPT_NOGRPID, "sysvgroups" },
	{ EXT2_OPT_RESGID, "resgid=%u" },
	{ EXT2_OPT_RESUID, "resuid=%u" },
	{ EXT2_OPT_SB, "sb=%u" },
	{ EXT2_OPT_ERRORS_CONTINUE, "errors=continue" },
	{ EXT2_OPT_ERRORS_PANIC, "errors=panic" },
	{ EXT2_OPT_ERRORS_RO, "errors=remount-ro" },
	{ EXT2_OPT_NOUID32, "nouid32" },
	{ EXT2_OPT_DEBUG, "debug" },
	{ EXT2_OPT_OLDALLOC, "oldalloc" },
	{ EXT2_OPT_ORLOV, "orlov" },
	{ EXT2_OPT_NOBH, "nobh" },
	{ EXT2_OPT_USER_XATTR, "user_xattr" },
	{ EXT2_OPT_NOUSER_XATTR, "nouser_xattr" },
	{ EXT2_OPT_ACL, "acl" },
	{ EXT2_OPT_NOACL, "noacl" },
	{ EXT2_OPT_XIP, "xip" },
	{ EXT2_OPT_DAX, "dax" },
	{ EXT2_OPT_NOQUOTA, "noquota" },
	{ EXT2_OPT_QUOTA, "quota" },
	{ EXT2_OPT_USRQUOTA, "usrquota" },
	{ EXT2_OPT_GRPQUOTA, "grpquota" },
	{ EXT2_OPT_RESERVATION, "reservation" },
	{ EXT2_OPT_NORESERVATION, "noreservation" },
	{ EXT2_OPT_INVALID, NULL },
};

static void ext2_set_error_policy(struct ext2_mount_options *options,
				  unsigned long policy)
{
	clear_opt(options->s_mount_opt, ERRORS_CONT);
	clear_opt(options->s_mount_opt, ERRORS_RO);
	clear_opt(options->s_mount_opt, ERRORS_PANIC);
	options->s_mount_opt |= policy;
}

static int ext2_parse_options(char *options, struct super_block *sb,
			      struct ext2_mount_options *result)
{
	char *item;
	substring_t args[MAX_OPT_ARGS];

	while (options && (item = strsep(&options, ",")) != NULL) {
		int token;
		int value;
		kuid_t uid;
		kgid_t gid;

		if (!*item)
			continue;

		token = match_token(item, ext2_option_tokens, args);
		switch (token) {
		case EXT2_OPT_BSD_DF:
			clear_opt(result->s_mount_opt, MINIX_DF);
			break;
		case EXT2_OPT_MINIX_DF:
			set_opt(result->s_mount_opt, MINIX_DF);
			break;
		case EXT2_OPT_GRPID:
			set_opt(result->s_mount_opt, GRPID);
			break;
		case EXT2_OPT_NOGRPID:
			clear_opt(result->s_mount_opt, GRPID);
			break;
		case EXT2_OPT_RESGID:
			if (match_int(&args[0], &value))
				return -EINVAL;
			gid = make_kgid(current_user_ns(), value);
			if (!gid_valid(gid))
				return -EINVAL;
			result->s_resgid = gid;
			break;
		case EXT2_OPT_RESUID:
			if (match_int(&args[0], &value))
				return -EINVAL;
			uid = make_kuid(current_user_ns(), value);
			if (!uid_valid(uid))
				return -EINVAL;
			result->s_resuid = uid;
			break;
		case EXT2_OPT_SB:
			/* Consumed before normal option parsing on initial mount. */
			break;
		case EXT2_OPT_ERRORS_CONTINUE:
			ext2_set_error_policy(result, EXT2_MOUNT_ERRORS_CONT);
			break;
		case EXT2_OPT_ERRORS_PANIC:
			ext2_set_error_policy(result, EXT2_MOUNT_ERRORS_PANIC);
			break;
		case EXT2_OPT_ERRORS_RO:
			ext2_set_error_policy(result, EXT2_MOUNT_ERRORS_RO);
			break;
		case EXT2_OPT_NOUID32:
			set_opt(result->s_mount_opt, NO_UID32);
			break;
		case EXT2_OPT_DEBUG:
			set_opt(result->s_mount_opt, DEBUG);
			break;
		case EXT2_OPT_OLDALLOC:
			set_opt(result->s_mount_opt, OLDALLOC);
			break;
		case EXT2_OPT_ORLOV:
			clear_opt(result->s_mount_opt, OLDALLOC);
			break;
		case EXT2_OPT_NOBH:
			ext2_msg(sb, KERN_INFO, "nobh is not supported");
			break;
		case EXT2_OPT_USER_XATTR:
#ifdef CONFIG_EXT2_FS_XATTR
			set_opt(result->s_mount_opt, XATTR_USER);
#else
			return -EOPNOTSUPP;
#endif
			break;
		case EXT2_OPT_NOUSER_XATTR:
#ifdef CONFIG_EXT2_FS_XATTR
			clear_opt(result->s_mount_opt, XATTR_USER);
#else
			return -EOPNOTSUPP;
#endif
			break;
		case EXT2_OPT_ACL:
#ifdef CONFIG_EXT2_FS_POSIX_ACL
			set_opt(result->s_mount_opt, POSIX_ACL);
#else
			return -EOPNOTSUPP;
#endif
			break;
		case EXT2_OPT_NOACL:
#ifdef CONFIG_EXT2_FS_POSIX_ACL
			clear_opt(result->s_mount_opt, POSIX_ACL);
#else
			return -EOPNOTSUPP;
#endif
			break;
		case EXT2_OPT_XIP:
			set_opt(result->s_mount_opt, XIP);
			fallthrough;
		case EXT2_OPT_DAX:
#ifdef CONFIG_FS_DAX
			set_opt(result->s_mount_opt, DAX);
#else
			return -EOPNOTSUPP;
#endif
			break;
		case EXT2_OPT_QUOTA:
		case EXT2_OPT_USRQUOTA:
#ifdef CONFIG_QUOTA
			set_opt(result->s_mount_opt, USRQUOTA);
#else
			return -EOPNOTSUPP;
#endif
			break;
		case EXT2_OPT_GRPQUOTA:
#ifdef CONFIG_QUOTA
			set_opt(result->s_mount_opt, GRPQUOTA);
#else
			return -EOPNOTSUPP;
#endif
			break;
		case EXT2_OPT_NOQUOTA:
			break;
		case EXT2_OPT_RESERVATION:
			set_opt(result->s_mount_opt, RESERVATION);
			break;
		case EXT2_OPT_NORESERVATION:
			clear_opt(result->s_mount_opt, RESERVATION);
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static unsigned long ext2_extract_superblock_option(void **data)
{
	char *cursor = *data;
	char *end;
	unsigned long block;

	if (!cursor || strncmp(cursor, "sb=", 3) != 0)
		return 1;

	block = simple_strtoul(cursor + 3, &end, 0);
	if (*end != '\0' && *end != ',')
		return 1;

	if (*end == ',')
		end++;
	*data = end;
	return block;
}

static void ext2_default_mount_options(const struct ext2_super_block *es,
				       struct ext2_mount_options *options)
{
	unsigned long defaults = le32_to_cpu(es->s_default_mount_opts);

	memset(options, 0, sizeof(*options));
	options->s_resuid =
		make_kuid(&init_user_ns, le16_to_cpu(es->s_def_resuid));
	options->s_resgid =
		make_kgid(&init_user_ns, le16_to_cpu(es->s_def_resgid));

	if (defaults & EXT2_DEFM_DEBUG)
		set_opt(options->s_mount_opt, DEBUG);
	if (defaults & EXT2_DEFM_BSDGROUPS)
		set_opt(options->s_mount_opt, GRPID);
	if (defaults & EXT2_DEFM_UID16)
		set_opt(options->s_mount_opt, NO_UID32);
#ifdef CONFIG_EXT2_FS_XATTR
	if (defaults & EXT2_DEFM_XATTR_USER)
		set_opt(options->s_mount_opt, XATTR_USER);
#endif
#ifdef CONFIG_EXT2_FS_POSIX_ACL
	if (defaults & EXT2_DEFM_ACL)
		set_opt(options->s_mount_opt, POSIX_ACL);
#endif

	switch (le16_to_cpu(es->s_errors)) {
	case EXT2_ERRORS_PANIC:
		set_opt(options->s_mount_opt, ERRORS_PANIC);
		break;
	case EXT2_ERRORS_CONTINUE:
		set_opt(options->s_mount_opt, ERRORS_CONT);
		break;
	default:
		set_opt(options->s_mount_opt, ERRORS_RO);
		break;
	}

	set_opt(options->s_mount_opt, RESERVATION);
}

static loff_t ext2_linux_maxbytes(unsigned int block_bits)
{
	u64 pointers = 1ULL << (block_bits - 2);
	u64 data_blocks;
	u64 metadata_blocks;
	u64 sector_limit;
	u64 usable_blocks;
	u64 bytes;

	data_blocks = EXT2_NDIR_BLOCKS +
		      pointers +
		      pointers * pointers +
		      pointers * pointers * pointers;

	metadata_blocks = 1 +
			  (1 + pointers) +
			  (1 + pointers + pointers * pointers);

	sector_limit = U32_MAX;
	if (block_bits > 9)
		sector_limit >>= block_bits - 9;

	usable_blocks = data_blocks;
	if (usable_blocks + metadata_blocks > sector_limit) {
		usable_blocks = sector_limit;
		if (usable_blocks > metadata_blocks)
			usable_blocks -= metadata_blocks;
		else
			usable_blocks = 0;
	}

	bytes = usable_blocks << block_bits;
	if (bytes > MAX_LFS_FILESIZE)
		return MAX_LFS_FILESIZE;
	return (loff_t)bytes;
}

static unsigned long ext2_descriptor_block(struct super_block *sb,
					   unsigned long super_block,
					   unsigned int descriptor_index)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	unsigned long first_meta = le32_to_cpu(sbi->s_es->s_first_meta_bg);
	unsigned long group;

	if (!EXT2_HAS_INCOMPAT_FEATURE(sb, EXT2_FEATURE_INCOMPAT_META_BG) ||
	    descriptor_index < first_meta)
		return super_block + descriptor_index + 1;

	group = sbi->s_desc_per_block * descriptor_index;
	return ext2_group_first_block_no(sb, group) +
	       ext2_bg_has_super(sb, group);
}

static int ext2_validate_group_descriptors(struct super_block *sb)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	IfsExt2Superblock super_view;
	IfsExt2GroupDescriptor group_view;
	IfsExt2Status status;
	unsigned int group;

	status = ifs_ext2_decode_superblock(
		sbi->s_es, sizeof(*sbi->s_es), &super_view);
	if (status != IFS_EXT2_OK)
		return -EFSCORRUPTED;

	status = ifs_ext2_validate_superblock(
		&super_view, sb_bdev_nr_blocks(sb), !sb_rdonly(sb));
	if (status != IFS_EXT2_OK)
		return -EFSCORRUPTED;

	for (group = 0; group < sbi->s_groups_count; group++) {
		struct ext2_group_desc *descriptor =
			ext2_get_group_desc(sb, group, NULL);

		if (!descriptor)
			return -EIO;

		status = ifs_ext2_decode_group_descriptor(
			descriptor, sizeof(*descriptor), &group_view);
		if (status == IFS_EXT2_OK)
			status = ifs_ext2_validate_group_descriptor(
				&super_view, group, &group_view);
		if (status != IFS_EXT2_OK)
			return -EFSCORRUPTED;
	}

	return 0;
}

static int ext2_initialise_counters(struct super_block *sb)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	int error;

	error = percpu_counter_init(
		&sbi->s_freeblocks_counter, ext2_count_free_blocks(sb), GFP_KERNEL);
	if (error)
		return error;

	error = percpu_counter_init(
		&sbi->s_freeinodes_counter, ext2_count_free_inodes(sb), GFP_KERNEL);
	if (error) {
		percpu_counter_destroy(&sbi->s_freeblocks_counter);
		return error;
	}

	error = percpu_counter_init(
		&sbi->s_dirs_counter, ext2_count_dirs(sb), GFP_KERNEL);
	if (error) {
		percpu_counter_destroy(&sbi->s_freeinodes_counter);
		percpu_counter_destroy(&sbi->s_freeblocks_counter);
		return error;
	}

	return 0;
}

static void ext2_destroy_counters(struct ext2_sb_info *sbi)
{
	percpu_counter_destroy(&sbi->s_dirs_counter);
	percpu_counter_destroy(&sbi->s_freeinodes_counter);
	percpu_counter_destroy(&sbi->s_freeblocks_counter);
}

static int ext2_load_group_descriptors(struct super_block *sb,
				       unsigned long logical_super_block)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	unsigned int count;
	unsigned int index;

	count = DIV_ROUND_UP(sbi->s_groups_count, sbi->s_desc_per_block);
	sbi->s_group_desc = kvmalloc_array(
		count, sizeof(*sbi->s_group_desc), GFP_KERNEL);
	if (!sbi->s_group_desc)
		return -ENOMEM;

	for (index = 0; index < count; index++) {
		unsigned long block = ext2_descriptor_block(
			sb, logical_super_block, index);

		sbi->s_group_desc[index] = sb_bread(sb, block);
		if (!sbi->s_group_desc[index]) {
			while (index > 0)
				brelse(sbi->s_group_desc[--index]);
			kvfree(sbi->s_group_desc);
			sbi->s_group_desc = NULL;
			return -EIO;
		}
	}

	sbi->s_gdb_count = count;
	return 0;
}

static void ext2_release_group_descriptors(struct ext2_sb_info *sbi)
{
	unsigned int index;

	if (!sbi->s_group_desc)
		return;

	for (index = 0; index < sbi->s_gdb_count; index++)
		brelse(sbi->s_group_desc[index]);
	kvfree(sbi->s_group_desc);
	sbi->s_group_desc = NULL;
	sbi->s_gdb_count = 0;
}

static int ext2_prepare_writable_mount(struct super_block *sb)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = sbi->s_es;

	if (le32_to_cpu(es->s_rev_level) > EXT2_MAX_SUPP_REV)
		return -EROFS;

	if (!(sbi->s_mount_state & EXT2_VALID_FS))
		ext2_msg(sb, KERN_WARNING, "mounting filesystem not marked clean");
	else if (sbi->s_mount_state & EXT2_ERROR_FS)
		ext2_msg(sb, KERN_WARNING, "mounting filesystem marked with errors");

	if (le16_to_cpu(es->s_max_mnt_count) == 0)
		es->s_max_mnt_count = cpu_to_le16(EXT2_DFL_MAX_MNT_COUNT);

	le16_add_cpu(&es->s_mnt_count, 1);
	mark_buffer_dirty(sbi->s_sbh);
	return 0;
}

static int ext2_sync_fs(struct super_block *sb, int wait)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	dquot_writeback_dquots(sb, -1);

	spin_lock(&sbi->s_lock);
	if (!sb_rdonly(sb))
		sbi->s_es->s_state &= cpu_to_le16(~EXT2_VALID_FS);
	spin_unlock(&sbi->s_lock);

	ext2_sync_super(sb, sbi->s_es, wait);
	return 0;
}

static void ext2_write_super(struct super_block *sb)
{
	if (!sb_rdonly(sb))
		ext2_sync_fs(sb, 1);
}

static int ext2_freeze(struct super_block *sb)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	if (atomic_long_read(&sb->s_remove_count))
		return ext2_sync_fs(sb, 1);

	spin_lock(&sbi->s_lock);
	sbi->s_es->s_state = cpu_to_le16(sbi->s_mount_state);
	spin_unlock(&sbi->s_lock);

	ext2_sync_super(sb, sbi->s_es, 1);
	return 0;
}

static int ext2_unfreeze(struct super_block *sb)
{
	ext2_write_super(sb);
	return 0;
}

static int ext2_show_options(struct seq_file *seq, struct dentry *root)
{
	struct super_block *sb = root->d_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	if (sbi->s_sb_block != 1)
		seq_printf(seq, ",sb=%lu", sbi->s_sb_block);
	if (test_opt(sb, MINIX_DF))
		seq_puts(seq, ",minixdf");
	if (test_opt(sb, GRPID))
		seq_puts(seq, ",grpid");
	if (test_opt(sb, ERRORS_CONT))
		seq_puts(seq, ",errors=continue");
	if (test_opt(sb, ERRORS_RO))
		seq_puts(seq, ",errors=remount-ro");
	if (test_opt(sb, ERRORS_PANIC))
		seq_puts(seq, ",errors=panic");
	if (test_opt(sb, NO_UID32))
		seq_puts(seq, ",nouid32");
	if (test_opt(sb, DEBUG))
		seq_puts(seq, ",debug");
	if (test_opt(sb, OLDALLOC))
		seq_puts(seq, ",oldalloc");
#ifdef CONFIG_EXT2_FS_XATTR
	if (test_opt(sb, XATTR_USER))
		seq_puts(seq, ",user_xattr");
#endif
#ifdef CONFIG_EXT2_FS_POSIX_ACL
	if (test_opt(sb, POSIX_ACL))
		seq_puts(seq, ",acl");
#endif
#ifdef CONFIG_QUOTA
	if (test_opt(sb, USRQUOTA))
		seq_puts(seq, ",usrquota");
	if (test_opt(sb, GRPQUOTA))
		seq_puts(seq, ",grpquota");
#endif
	if (test_opt(sb, DAX))
		seq_puts(seq, ",dax");
	if (!test_opt(sb, RESERVATION))
		seq_puts(seq, ",noreservation");

	return 0;
}

static int ext2_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_super_block *es = sbi->s_es;
	unsigned long overhead = 0;
	unsigned long group;
	u64 free_blocks;
	u64 reserved_blocks;

	if (!test_opt(sb, MINIX_DF)) {
		overhead = le32_to_cpu(es->s_first_data_block);
		for (group = 0; group < sbi->s_groups_count; group++)
			overhead += ext2_bg_has_super(sb, group) +
				    ext2_bg_num_gdb(sb, group);
		overhead += sbi->s_groups_count * (2 + sbi->s_itb_per_group);
	}

	free_blocks = ext2_count_free_blocks(sb);
	reserved_blocks = le32_to_cpu(es->s_r_blocks_count);

	buf->f_type = EXT2_SUPER_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = le32_to_cpu(es->s_blocks_count) - overhead;
	buf->f_bfree = free_blocks;
	buf->f_bavail = free_blocks > reserved_blocks ?
			free_blocks - reserved_blocks : 0;
	buf->f_files = le32_to_cpu(es->s_inodes_count);
	buf->f_ffree = ext2_count_free_inodes(sb);
	buf->f_namelen = EXT2_NAME_LEN;
	buf->f_fsid = uuid_to_fsid(es->s_uuid);

	return 0;
}

#ifdef CONFIG_QUOTA
static int ext2_quota_off(struct super_block *sb, int type);

static void ext2_disable_quotas(struct super_block *sb)
{
	int type;

	for (type = 0; type < MAXQUOTAS; type++)
		ext2_quota_off(sb, type);
}

static struct dquot __rcu **ext2_get_dquots(struct inode *inode)
{
	return EXT2_I(inode)->i_dquot;
}

static ssize_t ext2_quota_read(struct super_block *sb, int type, char *data,
			       size_t length, loff_t offset)
{
	struct inode *inode = sb_dqopt(sb)->files[type];
	size_t remaining;
	sector_t logical;
	unsigned int in_block;

	if (offset >= i_size_read(inode))
		return 0;
	if (offset + length > i_size_read(inode))
		length = i_size_read(inode) - offset;

	remaining = length;
	logical = offset >> sb->s_blocksize_bits;
	in_block = offset & (sb->s_blocksize - 1);

	while (remaining) {
		struct buffer_head mapping = { };
		struct buffer_head *bh;
		size_t chunk = min_t(size_t, remaining,
				     sb->s_blocksize - in_block);
		int error;

		mapping.b_size = sb->s_blocksize;
		error = ext2_get_block(inode, logical, &mapping, 0);
		if (error)
			return error;

		if (!buffer_mapped(&mapping)) {
			memset(data, 0, chunk);
		} else {
			bh = sb_bread(sb, mapping.b_blocknr);
			if (!bh)
				return -EIO;
			memcpy(data, bh->b_data + in_block, chunk);
			brelse(bh);
		}

		data += chunk;
		remaining -= chunk;
		logical++;
		in_block = 0;
	}

	return length;
}

static ssize_t ext2_quota_write(struct super_block *sb, int type,
				const char *data, size_t length, loff_t offset)
{
	struct inode *inode = sb_dqopt(sb)->files[type];
	size_t remaining = length;
	sector_t logical = offset >> sb->s_blocksize_bits;
	unsigned int in_block = offset & (sb->s_blocksize - 1);
	loff_t initial_offset = offset;

	while (remaining) {
		struct buffer_head mapping = { };
		struct buffer_head *bh;
		size_t chunk = min_t(size_t, remaining,
				     sb->s_blocksize - in_block);
		int error;

		mapping.b_size = sb->s_blocksize;
		error = ext2_get_block(inode, logical, &mapping, 1);
		if (error)
			return length == remaining ? error : length - remaining;

		bh = (in_block || chunk != sb->s_blocksize) ?
		     sb_bread(sb, mapping.b_blocknr) :
		     sb_getblk(sb, mapping.b_blocknr);
		if (!bh)
			return length == remaining ? -EIO : length - remaining;

		lock_buffer(bh);
		memcpy(bh->b_data + in_block, data, chunk);
		flush_dcache_page(bh->b_page);
		set_buffer_uptodate(bh);
		mark_buffer_dirty(bh);
		unlock_buffer(bh);
		brelse(bh);

		data += chunk;
		remaining -= chunk;
		logical++;
		in_block = 0;
	}

	if (i_size_read(inode) < initial_offset + length)
		i_size_write(inode, initial_offset + length);
	inode_inc_iversion(inode);
	inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	mark_inode_dirty(inode);
	return length;
}

static int ext2_quota_on(struct super_block *sb, int type, int format_id,
			 const struct path *path)
{
	struct inode *inode;
	int error;

	error = dquot_quota_on(sb, type, format_id, path);
	if (error)
		return error;

	inode = d_inode(path->dentry);
	inode_lock(inode);
	EXT2_I(inode)->i_flags |= EXT2_NOATIME_FL | EXT2_IMMUTABLE_FL;
	inode_set_flags(inode, S_NOATIME | S_IMMUTABLE,
			S_NOATIME | S_IMMUTABLE);
	inode_unlock(inode);
	mark_inode_dirty(inode);
	return 0;
}

static int ext2_quota_off(struct super_block *sb, int type)
{
	struct inode *inode = sb_dqopt(sb)->files[type];
	int error;

	if (!inode || !igrab(inode))
		return dquot_quota_off(sb, type);

	error = dquot_quota_off(sb, type);
	if (!error) {
		inode_lock(inode);
		EXT2_I(inode)->i_flags &=
			~(EXT2_NOATIME_FL | EXT2_IMMUTABLE_FL);
		inode_set_flags(inode, 0, S_NOATIME | S_IMMUTABLE);
		inode_unlock(inode);
		mark_inode_dirty(inode);
	}
	iput(inode);
	return error;
}

static const struct quotactl_ops ext2_quotactl_ops = {
	.quota_on = ext2_quota_on,
	.quota_off = ext2_quota_off,
	.quota_sync = dquot_quota_sync,
	.get_state = dquot_get_state,
	.set_info = dquot_set_dqinfo,
	.get_dqblk = dquot_get_dqblk,
	.set_dqblk = dquot_set_dqblk,
	.get_nextdqblk = dquot_get_next_dqblk,
};
#else
static inline void ext2_disable_quotas(struct super_block *sb) { }
#endif

static void ext2_put_super(struct super_block *sb)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);

	ext2_disable_quotas(sb);
	ext2_xattr_destroy_cache(sbi->s_ea_block_cache);
	sbi->s_ea_block_cache = NULL;

	if (!sb_rdonly(sb)) {
		spin_lock(&sbi->s_lock);
		sbi->s_es->s_state = cpu_to_le16(sbi->s_mount_state);
		spin_unlock(&sbi->s_lock);
		ext2_sync_super(sb, sbi->s_es, 1);
	}

	ext2_release_group_descriptors(sbi);
	kfree(sbi->s_debts);
	ext2_destroy_counters(sbi);
	brelse(sbi->s_sbh);
	fs_put_dax(sbi->s_daxdev, NULL);
	kfree(sbi->s_blockgroup_lock);
	kfree(sbi);
	sb->s_fs_info = NULL;
}

static struct inode *ext2_export_inode(struct super_block *sb,
				      u64 inode_number, u32 generation)
{
	struct inode *inode;

	if ((inode_number < EXT2_FIRST_INO(sb) &&
	     inode_number != EXT2_ROOT_INO) ||
	    inode_number > le32_to_cpu(EXT2_SB(sb)->s_es->s_inodes_count))
		return ERR_PTR(-ESTALE);

	inode = ext2_iget(sb, inode_number);
	if (IS_ERR(inode))
		return inode;
	if (generation && inode->i_generation != generation) {
		iput(inode);
		return ERR_PTR(-ESTALE);
	}
	return inode;
}

static struct dentry *ext2_fh_to_dentry(struct super_block *sb,
					struct fid *fid, int fh_len,
					int fh_type)
{
	return generic_fh_to_dentry(
		sb, fid, fh_len, fh_type, ext2_export_inode);
}

static struct dentry *ext2_fh_to_parent(struct super_block *sb,
					struct fid *fid, int fh_len,
					int fh_type)
{
	return generic_fh_to_parent(
		sb, fid, fh_len, fh_type, ext2_export_inode);
}

static const struct export_operations ext2_export_ops = {
	.encode_fh = generic_encode_ino32_fh,
	.fh_to_dentry = ext2_fh_to_dentry,
	.fh_to_parent = ext2_fh_to_parent,
	.get_parent = ext2_get_parent,
};

static const struct super_operations ext2_sops = {
	.alloc_inode = ext2_alloc_inode,
	.free_inode = ext2_free_core_inode,
	.write_inode = ext2_write_inode,
	.evict_inode = ext2_evict_inode,
	.put_super = ext2_put_super,
	.sync_fs = ext2_sync_fs,
	.freeze_fs = ext2_freeze,
	.unfreeze_fs = ext2_unfreeze,
	.statfs = ext2_statfs,
	.remount_fs = ext2_remount,
	.show_options = ext2_show_options,
#ifdef CONFIG_QUOTA
	.quota_read = ext2_quota_read,
	.quota_write = ext2_quota_write,
	.get_dquots = ext2_get_dquots,
#endif
};

static int ext2_remount(struct super_block *sb, int *flags, char *data)
{
	struct ext2_sb_info *sbi = EXT2_SB(sb);
	struct ext2_mount_options options;
	bool was_readonly = sb_rdonly(sb);
	bool becomes_readonly = (*flags & SB_RDONLY) != 0;
	int error;

	sync_filesystem(sb);

	spin_lock(&sbi->s_lock);
	options.s_mount_opt = sbi->s_mount_opt;
	options.s_resuid = sbi->s_resuid;
	options.s_resgid = sbi->s_resgid;
	spin_unlock(&sbi->s_lock);

	error = ext2_parse_options(data, sb, &options);
	if (error)
		return error;

	if ((options.s_mount_opt ^ sbi->s_mount_opt) & EXT2_MOUNT_DAX)
		return -EBUSY;

	if (!was_readonly && becomes_readonly) {
#ifdef CONFIG_QUOTA
		error = dquot_suspend(sb, -1);
		if (error)
			return error;
#endif
		spin_lock(&sbi->s_lock);
		sbi->s_es->s_state = cpu_to_le16(sbi->s_mount_state);
		spin_unlock(&sbi->s_lock);
		ext2_sync_super(sb, sbi->s_es, 1);
	} else if (was_readonly && !becomes_readonly) {
		if (EXT2_HAS_RO_COMPAT_FEATURE(
			    sb, ~EXT2_FEATURE_RO_COMPAT_SUPP))
			return -EROFS;
		error = ext2_prepare_writable_mount(sb);
		if (error)
			return error;
#ifdef CONFIG_QUOTA
		dquot_resume(sb, -1);
#endif
	}

	spin_lock(&sbi->s_lock);
	sbi->s_mount_opt = options.s_mount_opt;
	sbi->s_resuid = options.s_resuid;
	sbi->s_resgid = options.s_resgid;
	spin_unlock(&sbi->s_lock);

	sb->s_flags = (sb->s_flags & ~SB_POSIXACL) |
		      (test_opt(sb, POSIX_ACL) ? SB_POSIXACL : 0);
	return 0;
}

static int ext2_fill_super(struct super_block *sb, void *data, int silent)
{
	struct ext2_sb_info *sbi = NULL;
	struct buffer_head *sbh = NULL;
	struct ext2_super_block *es;
	struct ext2_mount_options options;
	IfsExt2Superblock super_view;
	IfsExt2Status status;
	struct inode *root;
	unsigned long sb_block;
	unsigned long logical_sb;
	unsigned long offset;
	unsigned long byte_address;
	int blocksize;
	int error;
	bool counters_ready = false;

	sb_block = ext2_extract_superblock_option(&data);

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sbi->s_blockgroup_lock =
		kzalloc(sizeof(*sbi->s_blockgroup_lock), GFP_KERNEL);
	if (!sbi->s_blockgroup_lock) {
		kfree(sbi);
		return -ENOMEM;
	}

	sb->s_fs_info = sbi;
	sbi->s_sb_block = sb_block;
	spin_lock_init(&sbi->s_lock);
	spin_lock_init(&sbi->s_next_gen_lock);
	spin_lock_init(&sbi->s_rsv_window_lock);
	sbi->s_rsv_window_root = RB_ROOT;
	sbi->s_daxdev = fs_dax_get_by_bdev(
		sb->s_bdev, &sbi->s_dax_part_off, NULL, NULL);

	blocksize = sb_min_blocksize(sb, BLOCK_SIZE);
	if (!blocksize) {
		error = -EINVAL;
		goto fail;
	}

	byte_address = sb_block * BLOCK_SIZE;
	logical_sb = byte_address / blocksize;
	offset = byte_address % blocksize;

	sbh = sb_bread(sb, logical_sb);
	if (!sbh) {
		error = -EIO;
		goto fail;
	}

	es = (struct ext2_super_block *)(sbh->b_data + offset);
	if (le16_to_cpu(es->s_magic) != EXT2_SUPER_MAGIC) {
		error = -EINVAL;
		if (!silent)
			ext2_msg(sb, KERN_ERR, "no EXT2 superblock found");
		goto fail;
	}

	status = ifs_ext2_decode_superblock(es, sizeof(*es), &super_view);
	if (status == IFS_EXT2_OK)
		status = ifs_ext2_validate_superblock(
			&super_view, 0, !sb_rdonly(sb));
	if (status != IFS_EXT2_OK) {
		error = -EINVAL;
		goto fail;
	}

	if (sb->s_blocksize != super_view.block_size) {
		brelse(sbh);
		sbh = NULL;

		if (!sb_set_blocksize(sb, super_view.block_size)) {
			error = -EINVAL;
			goto fail;
		}

		blocksize = sb->s_blocksize;
		logical_sb = byte_address / blocksize;
		offset = byte_address % blocksize;

		sbh = sb_bread(sb, logical_sb);
		if (!sbh) {
			error = -EIO;
			goto fail;
		}

		es = (struct ext2_super_block *)(sbh->b_data + offset);
		if (le16_to_cpu(es->s_magic) != EXT2_SUPER_MAGIC) {
			error = -EINVAL;
			goto fail;
		}
	}

	status = ifs_ext2_decode_superblock(es, sizeof(*es), &super_view);
	if (status == IFS_EXT2_OK)
		status = ifs_ext2_validate_superblock(
			&super_view, sb_bdev_nr_blocks(sb), !sb_rdonly(sb));
	if (status != IFS_EXT2_OK) {
		ext2_msg(sb, KERN_ERR, "invalid EXT2 superblock: %s",
			 ifs_ext2_status_string(status));
		error = -EFSCORRUPTED;
		goto fail;
	}

	sbi->s_sbh = sbh;
	sbi->s_es = es;
	sbh = NULL;

	ext2_default_mount_options(es, &options);
	error = ext2_parse_options(data, sb, &options);
	if (error)
		goto fail;

	sbi->s_mount_opt = options.s_mount_opt;
	sbi->s_resuid = options.s_resuid;
	sbi->s_resgid = options.s_resgid;
	sbi->s_mount_state = le16_to_cpu(es->s_state);

	sbi->s_inode_size = super_view.inode_size;
	sbi->s_first_ino = super_view.first_inode;
	sbi->s_blocks_per_group = super_view.blocks_per_group;
	sbi->s_inodes_per_group = super_view.inodes_per_group;
	sbi->s_inodes_per_block = super_view.inodes_per_block;
	sbi->s_itb_per_group = super_view.inode_table_blocks_per_group;
	sbi->s_groups_count = super_view.group_count;
	sbi->s_desc_per_block =
		sb->s_blocksize / sizeof(struct ext2_group_desc);
	sbi->s_addr_per_block_bits = ilog2(EXT2_ADDR_PER_BLOCK(sb));
	sbi->s_desc_per_block_bits = ilog2(sbi->s_desc_per_block);

	if (!sbi->s_desc_per_block) {
		error = -EFSCORRUPTED;
		goto fail;
	}

	sb->s_magic = EXT2_SUPER_MAGIC;
	sb->s_maxbytes = ext2_linux_maxbytes(sb->s_blocksize_bits);
	sb->s_max_links = EXT2_LINK_MAX;
	sb->s_time_min = S32_MIN;
	sb->s_time_max = S32_MAX;
	sb->s_iflags |= SB_I_CGROUPWB;
	sb->s_flags = (sb->s_flags & ~SB_POSIXACL) |
		      (test_opt(sb, POSIX_ACL) ? SB_POSIXACL : 0);

	if (test_opt(sb, DAX)) {
		if (!sbi->s_daxdev || sb->s_blocksize != PAGE_SIZE)
			clear_opt(sbi->s_mount_opt, DAX);
	}

	bgl_lock_init(sbi->s_blockgroup_lock);

	sbi->s_debts = kcalloc(
		sbi->s_groups_count, sizeof(*sbi->s_debts), GFP_KERNEL);
	if (!sbi->s_debts) {
		error = -ENOMEM;
		goto fail;
	}

	error = ext2_load_group_descriptors(sb, logical_sb);
	if (error)
		goto fail;

	error = ext2_validate_group_descriptors(sb);
	if (error)
		goto fail;

	get_random_bytes(&sbi->s_next_generation, sizeof(sbi->s_next_generation));

	sbi->s_rsv_window_head.rsv_start =
		EXT2_RESERVE_WINDOW_NOT_ALLOCATED;
	sbi->s_rsv_window_head.rsv_end =
		EXT2_RESERVE_WINDOW_NOT_ALLOCATED;
	sbi->s_rsv_window_head.rsv_alloc_hit = 0;
	sbi->s_rsv_window_head.rsv_goal_size = 0;
	ext2_rsv_window_add(sb, &sbi->s_rsv_window_head);

	error = ext2_initialise_counters(sb);
	if (error)
		goto fail;
	counters_ready = true;

#ifdef CONFIG_EXT2_FS_XATTR
	sbi->s_ea_block_cache = ext2_xattr_create_cache();
	if (!sbi->s_ea_block_cache) {
		error = -ENOMEM;
		goto fail;
	}
#endif

	sb->s_op = &ext2_sops;
	sb->s_export_op = &ext2_export_ops;
	sb->s_xattr = ext2_xattr_handlers;
#ifdef CONFIG_QUOTA
	sb->dq_op = &dquot_operations;
	sb->s_qcop = &ext2_quotactl_ops;
	sb->s_quota_types = QTYPE_MASK_USR | QTYPE_MASK_GRP;
#endif

	root = ext2_iget(sb, EXT2_ROOT_INO);
	if (IS_ERR(root)) {
		error = PTR_ERR(root);
		goto fail;
	}
	if (!S_ISDIR(root->i_mode) || !i_size_read(root)) {
		iput(root);
		error = -EFSCORRUPTED;
		goto fail;
	}

	sb->s_root = d_make_root(root);
	if (!sb->s_root) {
		error = -ENOMEM;
		goto fail;
	}

	if (!sb_rdonly(sb)) {
		error = ext2_prepare_writable_mount(sb);
		if (error) {
			sb->s_flags |= SB_RDONLY;
			error = 0;
		}
	}
	ext2_write_super(sb);
	return 0;

fail:
	if (sbi) {
		ext2_xattr_destroy_cache(sbi->s_ea_block_cache);
		sbi->s_ea_block_cache = NULL;
		if (counters_ready)
			ext2_destroy_counters(sbi);
		ext2_release_group_descriptors(sbi);
		kfree(sbi->s_debts);
		if (sbi->s_sbh)
			brelse(sbi->s_sbh);
		fs_put_dax(sbi->s_daxdev, NULL);
		kfree(sbi->s_blockgroup_lock);
		kfree(sbi);
		sb->s_fs_info = NULL;
	}
	brelse(sbh);
	return error;
}

static struct dentry *ext2_mount(struct file_system_type *type,
				 int flags, const char *device, void *data)
{
	return mount_bdev(type, flags, device, data, ext2_fill_super);
}

static struct file_system_type ext2_fs_type = {
	.owner = THIS_MODULE,
	.name = "ext2",
	.mount = ext2_mount,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init ext2_init(void)
{
	int error;

#ifdef CONFIG_EXT2_FS_XATTR
	error = infiltratr_mbcache_init();
	if (error)
		return error;
#endif

	error = ext2_create_inode_cache();
	if (error)
		goto fail_cache;

	error = register_filesystem(&ext2_fs_type);
	if (error)
		goto fail_register;

	return 0;

fail_register:
	ext2_destroy_inode_cache();
fail_cache:
#ifdef CONFIG_EXT2_FS_XATTR
	infiltratr_mbcache_exit();
#endif
	return error;
}

static void __exit ext2_exit(void)
{
	unregister_filesystem(&ext2_fs_type);
	ext2_destroy_inode_cache();
#ifdef CONFIG_EXT2_FS_XATTR
	infiltratr_mbcache_exit();
#endif
}

MODULE_AUTHOR("Shannon Smith");
MODULE_DESCRIPTION("Infiltrator Filesystem Support EXT2 driver");
MODULE_ALIAS_FS("ext2");

module_init(ext2_init)
module_exit(ext2_exit)
