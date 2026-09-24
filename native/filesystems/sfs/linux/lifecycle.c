/*
 * Infiltrator Filesystem Support — SFS Linux superblock adapter.
 *
 * Filesystem-format validation lives in the canonical SFS core.  This unit
 * owns only Linux mount lifecycle, option handling and VFS publication.
 */

#define ASFS_VERSION "Infiltrator"

#include <linux/module.h>
#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/iversion.h>
#include <linux/buffer_head.h>
#include <linux/vfs.h>
#include <linux/parser.h>
#include <linux/nls.h>
#include <linux/string.h>

#include "linux_adapter.h"
#include "../core/sfs_core.h"

static struct kmem_cache *sfs_inode_cache;

static char sfs_default_codepage[] = CONFIG_ASFS_DEFAULT_CODEPAGE;
static char sfs_default_iocharset[] = CONFIG_NLS_DEFAULT;

u32 asfs_calcchecksum(void *block, u32 blocksize)
{
	return ifs_sfs_calculate_block_checksum(block, blocksize);
}

enum sfs_mount_token {
	SFS_OPT_MODE,
	SFS_OPT_GID,
	SFS_OPT_UID,
	SFS_OPT_PREFIX,
	SFS_OPT_VOLUME,
	SFS_OPT_LOWERCASE_VOLUME,
	SFS_OPT_IOCHARSET,
	SFS_OPT_CODEPAGE,
	SFS_OPT_IGNORE,
	SFS_OPT_ERROR,
};

static match_table_t sfs_mount_tokens = {
	{ SFS_OPT_MODE, "mode=%o" },
	{ SFS_OPT_GID, "setgid=%u" },
	{ SFS_OPT_UID, "setuid=%u" },
	{ SFS_OPT_PREFIX, "prefix=%s" },
	{ SFS_OPT_VOLUME, "volume=%s" },
	{ SFS_OPT_LOWERCASE_VOLUME, "lowercasevol" },
	{ SFS_OPT_IOCHARSET, "iocharset=%s" },
	{ SFS_OPT_CODEPAGE, "codepage=%s" },
	{ SFS_OPT_IGNORE, "grpquota" },
	{ SFS_OPT_IGNORE, "noquota" },
	{ SFS_OPT_IGNORE, "quota" },
	{ SFS_OPT_IGNORE, "usrquota" },
	{ SFS_OPT_ERROR, NULL },
};

static void sfs_replace_option_string(char **slot, char *replacement,
				      char *static_default)
{
	if (*slot && *slot != static_default)
		kfree(*slot);
	*slot = replacement;
}

static int sfs_parse_mount_options(char *options, struct super_block *sb)
{
	struct asfs_sb_info *sbi = ASFS_SB(sb);
	char *entry;

	if (!options)
		return 0;

	while ((entry = strsep(&options, ",")) != NULL) {
		substring_t args[MAX_OPT_ARGS];
		char *value;
		int parsed;
		int token;

		if (!*entry)
			continue;

		token = match_token(entry, sfs_mount_tokens, args);
		switch (token) {
		case SFS_OPT_MODE:
			if (match_octal(&args[0], &parsed))
				return -EINVAL;
			sbi->mode = parsed & 0777;
			break;
		case SFS_OPT_GID:
			if (match_int(&args[0], &parsed))
				return -EINVAL;
			sbi->gid = parsed;
			break;
		case SFS_OPT_UID:
			if (match_int(&args[0], &parsed))
				return -EINVAL;
			sbi->uid = parsed;
			break;
		case SFS_OPT_PREFIX:
			value = match_strdup(&args[0]);
			if (!value)
				return -ENOMEM;
			sfs_replace_option_string(&sbi->prefix, value, NULL);
			break;
		case SFS_OPT_VOLUME:
			value = match_strdup(&args[0]);
			if (!value)
				return -ENOMEM;
			sfs_replace_option_string(&sbi->root_volume, value, NULL);
			break;
		case SFS_OPT_LOWERCASE_VOLUME:
			sbi->flags |= ASFS_VOL_LOWERCASE;
			break;
		case SFS_OPT_IOCHARSET:
			value = match_strdup(&args[0]);
			if (!value)
				return -ENOMEM;
			sfs_replace_option_string(
				&sbi->iocharset, value, sfs_default_iocharset);
			break;
		case SFS_OPT_CODEPAGE:
			value = match_strdup(&args[0]);
			if (!value)
				return -ENOMEM;
			sfs_replace_option_string(
				&sbi->codepage, value, sfs_default_codepage);
			break;
		case SFS_OPT_IGNORE:
			break;
		default:
			return -EINVAL;
		}
	}

	return 0;
}

static IfsSfsRootStatus sfs_root_status(const struct fsRootBlock *root)
{
	return ifs_sfs_validate_root_layout(
		be32_to_cpu(root->bheader.id),
		be16_to_cpu(root->version),
		be32_to_cpu(root->blocksize),
		be32_to_cpu(root->totalblocks),
		be32_to_cpu(root->bitmapbase),
		be32_to_cpu(root->adminspacecontainer),
		be32_to_cpu(root->rootobjectcontainer),
		be32_to_cpu(root->extentbnoderoot),
		be32_to_cpu(root->objectnoderoot));
}

static int sfs_apply_root(struct super_block *sb,
			  const struct fsRootBlock *root)
{
	struct asfs_sb_info *sbi = ASFS_SB(sb);
	u32 bitmap_blocks;
	u32 blocks_per_bitmap;
	IfsSfsRootStatus status;

	status = sfs_root_status(root);
	if (status != IFS_SFS_ROOT_OK)
		return -EUCLEAN;

	if (ifs_sfs_compute_bitmap_layout(
		    be32_to_cpu(root->blocksize),
		    be32_to_cpu(root->totalblocks),
		    &blocks_per_bitmap, &bitmap_blocks) != 0)
		return -EUCLEAN;

	sbi->totalblocks = be32_to_cpu(root->totalblocks);
	sbi->rootobjectcontainer = be32_to_cpu(root->rootobjectcontainer);
	sbi->extentbnoderoot = be32_to_cpu(root->extentbnoderoot);
	sbi->objectnoderoot = be32_to_cpu(root->objectnoderoot);
	sbi->adminspacecontainer = be32_to_cpu(root->adminspacecontainer);
	sbi->bitmapbase = be32_to_cpu(root->bitmapbase);
	sbi->blocks_inbitmap = blocks_per_bitmap;
	sbi->blocks_bitmap = bitmap_blocks;
	sbi->block_rovingblockptr = 0;
	sbi->flags |= (u16)(root->bits & 0xffU);
	return 0;
}

static int sfs_load_root_pair(struct super_block *sb, int silent)
{
	struct asfs_sb_info *sbi = ASFS_SB(sb);
	struct buffer_head *probe_bh = NULL;
	struct buffer_head *primary_bh = NULL;
	struct buffer_head *backup_bh = NULL;
	struct fsRootBlock *probe;
	struct fsRootBlock *primary;
	struct fsRootBlock *backup;
	IfsSfsRootStatus probe_status;
	u32 block_size;
	u32 total_blocks;
	int primary_valid;
	int backup_valid;
	int selected;
	int result = -EINVAL;

	if (!sb_set_blocksize(sb, 512))
		return -EINVAL;

	probe_bh = sb_bread(sb, 0);
	if (!probe_bh)
		return -EIO;

	probe = (struct fsRootBlock *)probe_bh->b_data;
	block_size = be32_to_cpu(probe->blocksize);
	total_blocks = be32_to_cpu(probe->totalblocks);
	probe_status = ifs_sfs_validate_root_probe(
		be32_to_cpu(probe->bheader.id),
		be16_to_cpu(probe->version),
		block_size, total_blocks);
	brelse(probe_bh);
	probe_bh = NULL;

	if (probe_status != IFS_SFS_ROOT_OK)
		return -EINVAL;
	if (!sb_set_blocksize(sb, block_size))
		return -EINVAL;

	primary_bh = sb_bread(sb, 0);
	backup_bh = sb_bread(sb, total_blocks - 1U);
	primary = primary_bh ? (struct fsRootBlock *)primary_bh->b_data : NULL;
	backup = backup_bh ? (struct fsRootBlock *)backup_bh->b_data : NULL;

	primary_valid = primary &&
		asfs_check_block((struct fsBlockHeader *)primary, block_size,
				 0, ASFS_ROOTID) &&
		sfs_root_status(primary) == IFS_SFS_ROOT_OK;
	backup_valid = backup &&
		asfs_check_block((struct fsBlockHeader *)backup, block_size,
				 total_blocks - 1U, ASFS_ROOTID) &&
		sfs_root_status(backup) == IFS_SFS_ROOT_OK;

	selected = ifs_sfs_select_root_copy(
		primary_valid,
		primary_valid ? be16_to_cpu(primary->sequencenumber) : 0U,
		backup_valid,
		backup_valid ? be16_to_cpu(backup->sequencenumber) : 0U);
	if (selected < 0)
		goto out;

	result = sfs_apply_root(sb, selected == 0 ? primary : backup);
	if (result)
		goto out;

	if (!primary_valid || !backup_valid)
		sbi->flags |= ASFS_READONLY;

	result = 0;
out:
	if (result && !silent)
		pr_err("sfs: invalid root metadata on %s\n", sb->s_id);
	brelse(primary_bh);
	brelse(backup_bh);
	return result;
}

static int sfs_load_runtime_state(struct super_block *sb)
{
	struct asfs_sb_info *sbi = ASFS_SB(sb);
	struct buffer_head *bh;

	bh = asfs_breadcheck(
		sb, sbi->rootobjectcontainer, ASFS_OBJECTCONTAINER_ID);
	if (!bh) {
		sbi->freeblocks = 0;
		sbi->flags |= ASFS_READONLY;
	} else {
		struct fsRootInfo *root_info =
			(struct fsRootInfo *)((u8 *)bh->b_data +
				sb->s_blocksize - sizeof(struct fsRootInfo));

		sbi->freeblocks = be32_to_cpu(root_info->freeblocks);
		asfs_brelse(bh);
	}

#ifdef CONFIG_ASFS_RW
	bh = asfs_breadcheck(
		sb, sbi->rootobjectcontainer + 2U,
		ASFS_TRANSACTIONFAILURE_ID);
	if (bh) {
		sbi->flags |= ASFS_READONLY;
		asfs_brelse(bh);
	}
#else
	sbi->flags |= ASFS_READONLY;
#endif
	return 0;
}

static int sfs_load_nls(struct asfs_sb_info *sbi)
{
	if (!sbi->codepage[0] || strcmp(sbi->codepage, "none") == 0)
		return 0;

	sbi->nls_disk = load_nls(sbi->codepage);
	if (!sbi->nls_disk)
		return -EINVAL;

	sbi->nls_io = load_nls(sbi->iocharset);
	if (!sbi->nls_io) {
		unload_nls(sbi->nls_disk);
		sbi->nls_disk = NULL;
		return -EINVAL;
	}
	return 0;
}

static void sfs_release_options(struct asfs_sb_info *sbi)
{
	if (!sbi)
		return;

	unload_nls(sbi->nls_io);
	unload_nls(sbi->nls_disk);
	sbi->nls_io = NULL;
	sbi->nls_disk = NULL;

	kfree(sbi->prefix);
	kfree(sbi->root_volume);
	if (sbi->iocharset != sfs_default_iocharset)
		kfree(sbi->iocharset);
	if (sbi->codepage != sfs_default_codepage)
		kfree(sbi->codepage);
}

static void sfs_put_super(struct super_block *sb)
{
	struct asfs_sb_info *sbi = ASFS_SB(sb);

	sfs_release_options(sbi);
	kfree(sbi);
	sb->s_fs_info = NULL;
}

static int sfs_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	struct asfs_sb_info *sbi = ASFS_SB(sb);

	buf->f_type = ASFS_MAGIC;
	buf->f_bsize = sb->s_blocksize;
	buf->f_blocks = sbi->totalblocks;
	buf->f_bfree = sbi->freeblocks;
	buf->f_bavail = sbi->freeblocks;
	buf->f_namelen = ASFS_MAXFN;
	return 0;
}

#ifdef CONFIG_ASFS_RW
static int sfs_remount(struct super_block *sb, int *flags, char *data)
{
	struct asfs_sb_info *sbi = ASFS_SB(sb);
	int result = sfs_parse_mount_options(data, sb);

	if (result)
		return result;

	if ((*flags & SB_RDONLY) != 0) {
		sb->s_flags |= SB_RDONLY;
		return 0;
	}

	if (sbi->flags & ASFS_READONLY)
		return -EROFS;

	sb->s_flags &= ~SB_RDONLY;
	return 0;
}
#endif

static struct inode *sfs_alloc_inode(struct super_block *sb)
{
	struct asfs_inode_info *info =
		alloc_inode_sb(sb, sfs_inode_cache, GFP_KERNEL);

	if (!info)
		return NULL;

	inode_set_iversion(&info->vfs_inode, 1);
	return &info->vfs_inode;
}

static void sfs_free_inode(struct inode *inode)
{
	kmem_cache_free(sfs_inode_cache, ASFS_I(inode));
}

static void sfs_inode_init_once(void *object)
{
	struct asfs_inode_info *info = object;

	inode_init_once(&info->vfs_inode);
}

static int sfs_init_inode_cache(void)
{
	sfs_inode_cache = kmem_cache_create(
		"sfs_inode_cache", sizeof(struct asfs_inode_info),
		0, SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT,
		sfs_inode_init_once);
	return sfs_inode_cache ? 0 : -ENOMEM;
}

static void sfs_destroy_inode_cache(void)
{
	rcu_barrier();
	kmem_cache_destroy(sfs_inode_cache);
	sfs_inode_cache = NULL;
}

static const struct super_operations sfs_super_operations = {
	.alloc_inode = sfs_alloc_inode,
	.free_inode = sfs_free_inode,
	.put_super = sfs_put_super,
	.statfs = sfs_statfs,
#ifdef CONFIG_ASFS_RW
	.remount_fs = sfs_remount,
#endif
};

extern const struct dentry_operations asfs_dentry_operations;

static int sfs_fill_super(struct super_block *sb, void *data, int silent)
{
	struct asfs_sb_info *sbi;
	struct inode *root_inode;
	int result;

	sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
	if (!sbi)
		return -ENOMEM;

	sb->s_fs_info = sbi;
	mutex_init(&sbi->lock);
	sbi->uid = ASFS_DEFAULT_UID;
	sbi->gid = ASFS_DEFAULT_GID;
	sbi->mode = ASFS_DEFAULT_MODE;
	sbi->iocharset = sfs_default_iocharset;
	sbi->codepage = sfs_default_codepage;

	result = sfs_parse_mount_options(data, sb);
	if (result)
		goto fail;

	sb->s_maxbytes = ASFS_MAXFILESIZE;
	result = sfs_load_root_pair(sb, silent);
	if (result)
		goto fail;

	result = sfs_load_runtime_state(sb);
	if (result)
		goto fail;

	result = sfs_load_nls(sbi);
	if (result)
		goto fail;

	sb->s_magic = ASFS_MAGIC;
	sb->s_flags |= SB_NODEV | SB_NOSUID;
	if (sbi->flags & ASFS_READONLY)
		sb->s_flags |= SB_RDONLY;
	sb->s_op = &sfs_super_operations;

	root_inode = asfs_get_root_inode(sb);
	if (!root_inode) {
		result = -EIO;
		goto fail_after_ops;
	}

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		result = -ENOMEM;
		goto fail_after_ops;
	}

	d_set_d_op(sb->s_root, &asfs_dentry_operations);
	return 0;

fail_after_ops:
	sfs_put_super(sb);
	return result;
fail:
	sfs_release_options(sbi);
	kfree(sbi);
	sb->s_fs_info = NULL;
	return result;
}

static struct dentry *sfs_mount(
	struct file_system_type *type, int flags,
	const char *device, void *data)
{
	return mount_bdev(type, flags, device, data, sfs_fill_super);
}

static struct file_system_type sfs_type = {
	.owner = THIS_MODULE,
	.name = "sfs",
	.mount = sfs_mount,
	.kill_sb = kill_block_super,
	.fs_flags = FS_REQUIRES_DEV,
};

static int __init sfs_init(void)
{
	int result = sfs_init_inode_cache();

	if (result)
		return result;

	result = register_filesystem(&sfs_type);
	if (result)
		sfs_destroy_inode_cache();
	return result;
}

static void __exit sfs_exit(void)
{
	unregister_filesystem(&sfs_type);
	sfs_destroy_inode_cache();
}

MODULE_DESCRIPTION("Infiltrator SFS filesystem support");
MODULE_ALIAS_FS("sfs");
MODULE_LICENSE("GPL");

module_init(sfs_init);
module_exit(sfs_exit);
