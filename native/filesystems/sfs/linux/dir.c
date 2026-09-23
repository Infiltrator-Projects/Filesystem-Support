#include <linux/buffer_head.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/nls.h>
#include <linux/string.h>
#include "asfs_fs.h"

extern const struct dentry_operations asfs_dentry_operations;

static unsigned int sfs_object_dtype(const struct fsObject *object)
{
    if ((object->bits & OTYPE_DIR) != 0U)
        return DT_DIR;
    if ((object->bits & OTYPE_LINK) != 0U &&
        (object->bits & OTYPE_HARDLINK) == 0U)
        return DT_LNK;
    return DT_REG;
}

static struct fsObject *sfs_find_translated_object(
    struct super_block *sb,
    struct fsObjectContainer *container,
    const u8 *name)
{
    struct fsObject *object = &container->object[0];
    u8 translated[ASFS_MAXFN_BUF];

    while (asfs_object_slot_fits(sb, container, object) &&
           be32_to_cpu(object->objectnode) != 0U) {
        struct fsObject *next = asfs_nextobject(sb, container, object);

        if (!next)
            return ERR_PTR(-EUCLEAN);

        asfs_translate(
            translated, object->name,
            ASFS_SB(sb)->nls_io, ASFS_SB(sb)->nls_disk,
            sizeof(translated));

        if (asfs_namecmp(
                translated, (u8 *)name,
                (ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE) != 0,
                ASFS_SB(sb)->nls_io) == 0)
            return object;

        object = next;
    }

    return NULL;
}

int asfs_readdir(struct file *file, struct dir_context *ctx)
{
    struct inode *dir = file_inode(file);
    struct super_block *sb = dir->i_sb;
    u32 block;
    u32 resume_node;
    u32 visited = 0U;
    bool emit_entries;

    if (ctx->pos >= ASFS_SB(sb)->totalblocks)
        return 0;
    if (!dir_emit_dots(file, ctx))
        return 0;

    if (ASFS_I(dir)->firstblock == 0U) {
        ctx->pos = ASFS_SB(sb)->totalblocks;
        ASFS_I(dir)->modified = 0;
        file->private_data = NULL;
        return 0;
    }

    if (ctx->pos == 2) {
        block = ASFS_I(dir)->firstblock;
        resume_node = 0U;
        emit_entries = true;
    } else {
        resume_node = (u32)(unsigned long)file->private_data;
        emit_entries = false;
        block = ASFS_I(dir)->modified == 0
            ? (u32)ctx->pos
            : ASFS_I(dir)->firstblock;
    }

    while (block != 0U) {
        struct buffer_head *bh;
        struct fsObjectContainer *container;
        struct fsObject *object;
        u32 next_block;

        if (++visited > ASFS_SB(sb)->totalblocks)
            return -EUCLEAN;

        bh = asfs_breadcheck(sb, block, ASFS_OBJECTCONTAINER_ID);
        if (!bh)
            return -EIO;

        container = (struct fsObjectContainer *)bh->b_data;
        object = &container->object[0];

        while (asfs_object_slot_fits(sb, container, object) &&
               be32_to_cpu(object->objectnode) != 0U) {
            struct fsObject *next =
                asfs_nextobject(sb, container, object);
            const u32 object_node = be32_to_cpu(object->objectnode);

            if (!next) {
                asfs_brelse(bh);
                return -EUCLEAN;
            }

            if (!emit_entries && object_node == resume_node)
                emit_entries = true;

            if (emit_entries && (object->bits & OTYPE_HIDDEN) == 0U) {
                u8 display_name[ASFS_MAXFN_BUF];

                asfs_translate(
                    display_name, object->name,
                    ASFS_SB(sb)->nls_io, ASFS_SB(sb)->nls_disk,
                    sizeof(display_name));
                ctx->pos = block;

                if (!dir_emit(
                        ctx, display_name,
                        strnlen((const char *)display_name,
                                sizeof(display_name)),
                        object_node, sfs_object_dtype(object))) {
                    file->private_data =
                        (void *)(unsigned long)object_node;
                    ASFS_I(dir)->modified = 0;
                    asfs_brelse(bh);
                    return 0;
                }
            }

            object = next;
        }

        next_block = be32_to_cpu(container->next);
        asfs_brelse(bh);
        block = next_block;
    }

    ctx->pos = ASFS_SB(sb)->totalblocks;
    ASFS_I(dir)->modified = 0;
    file->private_data = NULL;
    return 0;
}

static int sfs_instantiate_lookup(
    struct super_block *sb,
    struct dentry *dentry,
    struct buffer_head *object_bh,
    struct fsObject *object)
{
    struct inode *inode;
    const u32 object_node = be32_to_cpu(object->objectnode);

    inode = iget_locked(sb, object_node);
    if (!inode)
        return -ENOMEM;

    if ((inode->i_state & I_NEW) != 0) {
        asfs_read_locked_inode(inode, object);
        unlock_new_inode(inode);
    }

    asfs_brelse(object_bh);
    d_set_d_op(dentry, &asfs_dentry_operations);
    d_add(dentry, inode);
    return 0;
}

static int sfs_lookup_hashed(
    struct inode *dir,
    struct dentry *dentry,
    u8 *disk_name)
{
    struct super_block *sb = dir->i_sb;
    struct buffer_head *hash_bh;
    u16 hash;
    u32 node;
    u32 budget = ASFS_SB(sb)->totalblocks;

    hash_bh = asfs_breadcheck(
        sb, ASFS_I(dir)->hashtable, ASFS_HASHTABLE_ID);
    if (!hash_bh)
        return -EIO;

    hash = asfs_hash(
        disk_name,
        (ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE) != 0);
    node = be32_to_cpu(
        ((struct fsHashTable *)hash_bh->b_data)->
            hashentry[HASHCHAIN(hash)]);
    asfs_brelse(hash_bh);

    while (node != 0U) {
        struct buffer_head *node_bh = NULL;
        struct fsObjectNode *node_entry = NULL;
        struct buffer_head *object_bh;
        struct fsObject *object;
        u32 next_node;
        int result;

        if (budget-- == 0U)
            return -EUCLEAN;

        result = asfs_getnode(sb, node, &node_bh, &node_entry);
        if (result != 0)
            return result;

        next_node = be32_to_cpu(node_entry->next);
        if (be16_to_cpu(node_entry->hash16) == hash) {
            object_bh = asfs_breadcheck(
                sb, be32_to_cpu(node_entry->node.data),
                ASFS_OBJECTCONTAINER_ID);
            if (!object_bh) {
                asfs_brelse(node_bh);
                return -EIO;
            }

            object = asfs_find_obj_by_name(
                sb, (struct fsObjectContainer *)object_bh->b_data,
                disk_name);
            if (IS_ERR(object)) {
                result = PTR_ERR(object);
                asfs_brelse(object_bh);
                asfs_brelse(node_bh);
                return result;
            }

            if (object) {
                asfs_brelse(node_bh);
                return sfs_instantiate_lookup(
                    sb, dentry, object_bh, object);
            }

            asfs_brelse(object_bh);
        }

        asfs_brelse(node_bh);
        node = next_node;
    }

    return -ENOENT;
}

static int sfs_lookup_linear(
    struct inode *dir,
    struct dentry *dentry,
    const u8 *io_name)
{
    struct super_block *sb = dir->i_sb;
    u32 block = ASFS_I(dir)->firstblock;
    u32 budget = ASFS_SB(sb)->totalblocks;

    while (block != 0U) {
        struct buffer_head *bh;
        struct fsObjectContainer *container;
        struct fsObject *object;
        u32 next_block;

        if (budget-- == 0U)
            return -EUCLEAN;

        bh = asfs_breadcheck(sb, block, ASFS_OBJECTCONTAINER_ID);
        if (!bh)
            return -EIO;

        container = (struct fsObjectContainer *)bh->b_data;
        object = sfs_find_translated_object(sb, container, io_name);
        if (IS_ERR(object)) {
            int result = PTR_ERR(object);
            asfs_brelse(bh);
            return result;
        }
        if (object)
            return sfs_instantiate_lookup(sb, dentry, bh, object);

        next_block = be32_to_cpu(container->next);
        asfs_brelse(bh);
        block = next_block;
    }

    return -ENOENT;
}

struct dentry *asfs_lookup(
    struct inode *dir,
    struct dentry *dentry,
    unsigned int flags)
{
    struct super_block *sb = dir->i_sb;
    u8 disk_name[ASFS_MAXFN_BUF];
    int result;

    (void)flags;

    result = asfs_check_name(
        dentry->d_name.name, (int)dentry->d_name.len);
    if (result != 0)
        return ERR_PTR(result);

    asfs_translate(
        disk_name, (u8 *)dentry->d_name.name,
        ASFS_SB(sb)->nls_disk, ASFS_SB(sb)->nls_io,
        sizeof(disk_name));

    mutex_lock(&ASFS_SB(sb)->lock);
    if (ASFS_I(dir)->hashtable != 0U &&
        strchr((const char *)disk_name, '?') == NULL)
        result = sfs_lookup_hashed(dir, dentry, disk_name);
    else
        result = sfs_lookup_linear(
            dir, dentry, dentry->d_name.name);
    mutex_unlock(&ASFS_SB(sb)->lock);

    if (result == 0)
        return NULL;
    if (result != -ENOENT)
        return ERR_PTR(result);

    d_set_d_op(dentry, &asfs_dentry_operations);
    d_add(dentry, NULL);
    return NULL;
}
