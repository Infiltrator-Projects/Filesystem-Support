/*
 * Project-authored Linux directory adapter for AmigaDOS OFS/FFS media.
 *
 * The on-disk hash table and hash-chain layout are filesystem semantics.
 * This unit only binds that layout to Linux VFS directory iteration and keeps
 * the VFS cursor stable across partial reads.
 */

#include <linux/iversion.h>
#include "affs.h"

struct ifs_amiga_dir_state {
    u32 resume_block;
    u64 inode_version;
};

static int ifs_amiga_iterate_directory(struct file *file,
                                       struct dir_context *ctx);

static loff_t ifs_amiga_dir_llseek(struct file *file, loff_t offset, int whence)
{
    struct ifs_amiga_dir_state *state = file->private_data;

    return generic_llseek_cookie(file, offset, whence, &state->inode_version);
}

static int ifs_amiga_dir_open(struct inode *inode, struct file *file)
{
    struct ifs_amiga_dir_state *state;

    state = kzalloc(sizeof(*state), GFP_KERNEL);
    if (!state)
        return -ENOMEM;

    file->private_data = state;
    return 0;
}

static int ifs_amiga_dir_release(struct inode *inode, struct file *file)
{
    kfree(file->private_data);
    file->private_data = NULL;
    return 0;
}

const struct file_operations affs_dir_operations = {
    .open = ifs_amiga_dir_open,
    .read = generic_read_dir,
    .llseek = ifs_amiga_dir_llseek,
    .iterate_shared = ifs_amiga_iterate_directory,
    .fsync = affs_file_fsync,
    .release = ifs_amiga_dir_release,
};

const struct inode_operations affs_dir_inode_operations = {
    .create = affs_create,
    .lookup = affs_lookup,
    .link = affs_link,
    .unlink = affs_unlink,
    .symlink = affs_symlink,
    .mkdir = affs_mkdir,
    .rmdir = affs_rmdir,
    .rename = affs_rename2,
    .setattr = affs_notify_change,
};

static int ifs_amiga_iterate_directory(struct file *file,
                                       struct dir_context *ctx)
{
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct ifs_amiga_dir_state *state = file->private_data;
    struct buffer_head *directory = NULL;
    struct buffer_head *entry = NULL;
    u64 cursor;
    u64 bucket64;
    u32 block = 0U;
    u32 chain_index;
    u32 bucket;
    u32 walked;
    int result = 0;

    if (ctx->pos < 2) {
        state->resume_block = 0U;
        if (!dir_emit_dots(file, ctx))
            return 0;
    }

    cursor = (u64)(ctx->pos - 2);
    bucket64 = cursor >> 16;
    chain_index = (u32)(cursor & 0xffffU);

    if (chain_index == 0xffffU) {
        bucket64++;
        chain_index = 0U;
        ctx->pos = (loff_t)(bucket64 << 16) + 2;
    }

    if (bucket64 >= (u64)sbi->s_hashsize)
        return 0;

    bucket = (u32)bucket64;

    affs_lock_dir(inode);

    directory = affs_bread(sb, inode->i_ino);
    if (!directory) {
        result = -EIO;
        goto out;
    }

    if (state->resume_block != 0U &&
        inode_eq_iversion(inode, state->inode_version)) {
        block = state->resume_block;
        goto emit_chain;
    }

    block = be32_to_cpu(AFFS_HEAD(directory)->table[bucket]);
    for (walked = 0U; block != 0U && walked < chain_index; ++walked) {
        entry = affs_bread(sb, block);
        if (!entry) {
            result = -EIO;
            goto out;
        }

        block = be32_to_cpu(AFFS_TAIL(sb, entry)->hash_chain);
        affs_brelse(entry);
        entry = NULL;
    }

    if (block != 0U)
        goto emit_chain;

    bucket++;

    for (; bucket < (u32)sbi->s_hashsize; ++bucket) {
        block = be32_to_cpu(AFFS_HEAD(directory)->table[bucket]);
        if (block == 0U)
            continue;

        ctx->pos = ((loff_t)bucket << 16) + 2;

emit_chain:
        while (block != 0U) {
            const struct affs_tail *tail;
            const unsigned char *name;
            unsigned int name_length;

            entry = affs_bread(sb, block);
            if (!entry) {
                result = -EIO;
                goto out;
            }

            tail = AFFS_TAIL(sb, entry);
            name_length = min_t(unsigned int, tail->name[0], AFFSNAMEMAX);
            name = tail->name + 1;

            if (!dir_emit(ctx, name, name_length, block, DT_UNKNOWN))
                goto save_position;

            ctx->pos++;
            block = be32_to_cpu(tail->hash_chain);
            affs_brelse(entry);
            entry = NULL;
        }
    }

save_position:
    state->inode_version = inode_query_iversion(inode);
    state->resume_block = block;

out:
    affs_brelse(entry);
    affs_brelse(directory);
    affs_unlock_dir(inode);
    return result;
}
