/* Infiltrator Filesystem Support — SFS Linux namespace adapter.
 * Directory enumeration, namespace mutation and symlink translation are one
 * Linux-facing namespace responsibility.
 */

#include <linux/buffer_head.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/nls.h>
#include <linux/string.h>
#include "linux_adapter.h"

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


/* ===== namespace mutation ===== */
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/nls.h>
#include <linux/string.h>
#include "linux_adapter.h"

static u8 sfs_linux_upper_character(u8 character, struct nls_table *table)
{
    if (table) {
        const u8 mapped = table->charset2upper[character];
        return mapped != 0U ? mapped : character;
    }

    return ifs_sfs_fold_character(character);
}

u8 asfs_lowerchar(u8 character)
{
    return ifs_sfs_lower_character(character);
}

int asfs_check_name(const u8 *name, int length)
{
    IfsSfsNameStatus status;

    if (length < 0)
        return -EINVAL;

    status = ifs_sfs_validate_name(name, (ifs_sfs_u32)length);
    switch (status) {
    case IFS_SFS_NAME_OK:
        return 0;
    case IFS_SFS_NAME_TOO_LONG:
        return -ENAMETOOLONG;
    case IFS_SFS_NAME_INVALID_CHARACTER:
    default:
        return -EINVAL;
    }
}

static int sfs_hash_dentry(const struct dentry *parent, struct qstr *name)
{
    struct super_block *sb = d_inode(parent)->i_sb;
    struct nls_table *nls = ASFS_SB(sb)->nls_io;
    const bool case_sensitive =
        (ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE) != 0;
    const u8 *cursor = name->name;
    unsigned long hash;
    unsigned int index;
    int result;

    result = asfs_check_name(name->name, (int)name->len);
    if (result != 0)
        return result;

    hash = init_name_hash(parent);
    for (index = 0U; index < name->len; ++index) {
        const u8 character = case_sensitive
            ? cursor[index]
            : sfs_linux_upper_character(cursor[index], nls);
        hash = partial_name_hash(character, hash);
    }

    name->hash = end_name_hash(hash);
    return 0;
}

static int sfs_compare_dentry(
    const struct dentry *parent,
    unsigned int existing_length,
    const char *existing_name,
    const struct qstr *candidate)
{
    struct super_block *sb = d_inode(parent)->i_sb;
    struct nls_table *nls = ASFS_SB(sb)->nls_io;
    const bool case_sensitive =
        (ASFS_SB(sb)->flags & ASFS_ROOTBITS_CASESENSITIVE) != 0;
    unsigned int index;

    if (asfs_check_name(candidate->name, (int)candidate->len) != 0 ||
        existing_length != candidate->len)
        return 1;

    if (case_sensitive)
        return memcmp(existing_name, candidate->name, existing_length) != 0;

    for (index = 0U; index < existing_length; ++index) {
        if (sfs_linux_upper_character((u8)existing_name[index], nls) !=
            sfs_linux_upper_character(candidate->name[index], nls))
            return 1;
    }

    return 0;
}

const struct dentry_operations asfs_dentry_operations = {
    .d_hash = sfs_hash_dentry,
    .d_compare = sfs_compare_dentry,
};

int asfs_namecmp(
    u8 *disk_name,
    u8 *component,
    int case_sensitive,
    struct nls_table *table)
{
    while (*disk_name != 0U &&
           *component != 0U &&
           *component != (u8)'/') {
        const u8 left = case_sensitive
            ? *disk_name
            : sfs_linux_upper_character(*disk_name, table);
        const u8 right = case_sensitive
            ? *component
            : sfs_linux_upper_character(*component, table);

        if (left != right)
            return (int)right - (int)left;

        disk_name++;
        component++;
    }

    if (*disk_name == 0U &&
        (*component == 0U || *component == (u8)'/'))
        return 0;

    return (int)*component - (int)*disk_name;
}

u16 asfs_hash(u8 *name, int case_sensitive)
{
    return ifs_sfs_component_hash(name, case_sensitive);
}

void asfs_translate(
    u8 *destination,
    u8 *source,
    struct nls_table *destination_nls,
    struct nls_table *source_nls,
    int limit)
{
    int source_offset = 0;
    int destination_remaining;

    if (!destination || !source || limit <= 0)
        return;

    destination_remaining = limit;

    if (!destination_nls || !source_nls) {
        strscpy((char *)destination, (const char *)source, limit);
        return;
    }

    while (source[source_offset] != 0U && destination_remaining > 1) {
        wchar_t unicode;
        int source_count;
        int destination_count;

        source_count = source_nls->char2uni(
            (const unsigned char *)&source[source_offset],
            strlen((const char *)&source[source_offset]),
            &unicode);
        if (source_count <= 0) {
            source_offset++;
            continue;
        }

        source_offset += source_count;
        destination_count = destination_nls->uni2char(
            unicode, destination, destination_remaining);
        if (destination_count < 0) {
            *destination++ = (u8)'?';
            destination_remaining--;
            continue;
        }
        if (destination_count == 0)
            continue;

        destination += destination_count;
        destination_remaining -= destination_count;
    }

    *destination = 0U;
}


/* ===== symlink translation ===== */
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/nls.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "linux_adapter.h"

struct sfs_link_writer {
    char *data;
    size_t capacity;
    size_t length;
};

static int sfs_link_append_byte(struct sfs_link_writer *writer, char value)
{
    if (!writer || writer->length + 1U >= writer->capacity)
        return -ENAMETOOLONG;

    writer->data[writer->length++] = value;
    writer->data[writer->length] = '\0';
    return 0;
}

static int sfs_link_append_bytes(
    struct sfs_link_writer *writer,
    const char *bytes,
    size_t count)
{
    size_t index;

    for (index = 0U; index < count; ++index) {
        int result = sfs_link_append_byte(writer, bytes[index]);
        if (result != 0)
            return result;
    }
    return 0;
}

static int sfs_link_convert_one(
    struct sfs_link_writer *writer,
    const char *source,
    size_t source_bytes,
    struct nls_table *source_nls,
    struct nls_table *destination_nls,
    bool lower,
    size_t *consumed)
{
    wchar_t unicode;
    int input_count;
    int output_count;
    char temporary[NLS_MAX_CHARSET_SIZE];
    unsigned char raw;

    if (!writer || !source || source_bytes == 0U || !consumed)
        return -EINVAL;

    if (!source_nls || !destination_nls) {
        raw = (unsigned char)source[0];
        if (lower)
            raw = asfs_lowerchar(raw);
        *consumed = 1U;
        return sfs_link_append_byte(writer, (char)raw);
    }

    input_count = source_nls->char2uni(
        (const unsigned char *)source, source_bytes, &unicode);
    if (input_count <= 0) {
        *consumed = 1U;
        return sfs_link_append_byte(writer, '?');
    }

    output_count = destination_nls->uni2char(
        unicode, (unsigned char *)temporary, sizeof(temporary));
    *consumed = (size_t)input_count;
    if (output_count <= 0)
        return sfs_link_append_byte(writer, '?');

    if (lower && output_count == 1)
        temporary[0] = (char)asfs_lowerchar((u8)temporary[0]);

    return sfs_link_append_bytes(
        writer, temporary, (size_t)output_count);
}

static void sfs_free_link(void *link)
{
    kfree(link);
}

const char *asfs_get_link(
    struct dentry *dentry,
    struct inode *inode,
    struct delayed_call *done)
{
    struct super_block *sb;
    struct buffer_head *bh;
    struct fsSoftLink *disk_link;
    struct sfs_link_writer writer;
    const char *source;
    const char *end;
    const char *colon;
    const char *prefix;
    size_t source_length;
    size_t source_index = 0U;
    char previous = 0;
    int result = 0;

    if (!dentry)
        return ERR_PTR(-ECHILD);

    sb = inode->i_sb;
    bh = asfs_breadcheck(
        sb, ASFS_I(inode)->firstblock, ASFS_SOFTLINK_ID);
    if (!bh)
        return ERR_PTR(-EIO);

    if (sb->s_blocksize <= sizeof(struct fsSoftLink)) {
        asfs_brelse(bh);
        return ERR_PTR(-EUCLEAN);
    }

    disk_link = (struct fsSoftLink *)bh->b_data;
    source = (const char *)disk_link->string;
    end = memchr(
        source, '\0',
        sb->s_blocksize - sizeof(struct fsSoftLink));
    if (!end) {
        asfs_brelse(bh);
        return ERR_PTR(-EUCLEAN);
    }

    source_length = (size_t)(end - source);
    writer.data = kzalloc(PATH_MAX, GFP_KERNEL);
    if (!writer.data) {
        asfs_brelse(bh);
        return ERR_PTR(-ENOMEM);
    }
    writer.capacity = PATH_MAX;
    writer.length = 0U;

    colon = memchr(source, ':', source_length);
    if (colon) {
        const size_t volume_length = (size_t)(colon - source);
        const char *root_volume = ASFS_SB(sb)->root_volume;
        const bool is_root_volume =
            root_volume != NULL &&
            strlen(root_volume) == volume_length &&
            memcmp(source, root_volume, volume_length) == 0;

        if (is_root_volume) {
            result = sfs_link_append_byte(&writer, '/');
        } else {
            size_t index = 0U;

            prefix = ASFS_SB(sb)->prefix;
            if (!prefix)
                prefix = "/";

            result = sfs_link_append_bytes(
                &writer, prefix, strlen(prefix));
            while (result == 0 && index < volume_length) {
                size_t consumed = 0U;
                result = sfs_link_convert_one(
                    &writer, source + index, volume_length - index,
                    ASFS_SB(sb)->nls_disk, ASFS_SB(sb)->nls_io,
                    (ASFS_SB(sb)->flags & ASFS_VOL_LOWERCASE) != 0,
                    &consumed);
                if (consumed == 0U)
                    result = -EUCLEAN;
                index += consumed;
            }
            if (result == 0)
                result = sfs_link_append_byte(&writer, '/');
        }

        if (result != 0)
            goto fail;

        source_index = volume_length + 1U;
        previous = '/';
    }

    while (source_index < source_length) {
        size_t consumed = 0U;
        const char character = source[source_index];

        if (character == '/' && previous == '/') {
            result = sfs_link_append_bytes(&writer, "..", 2U);
            if (result != 0)
                goto fail;
        }

        if (character == '/') {
            result = sfs_link_append_byte(&writer, '/');
            consumed = 1U;
        } else {
            result = sfs_link_convert_one(
                &writer, source + source_index,
                source_length - source_index,
                ASFS_SB(sb)->nls_disk, ASFS_SB(sb)->nls_io,
                false, &consumed);
        }
        if (result != 0 || consumed == 0U)
            goto fail;

        previous = character;
        source_index += consumed;
    }

    asfs_brelse(bh);
    set_delayed_call(done, sfs_free_link, writer.data);
    return writer.data;

fail:
    asfs_brelse(bh);
    kfree(writer.data);
    return ERR_PTR(result != 0 ? result : -EUCLEAN);
}

#ifdef CONFIG_ASFS_RW

int asfs_write_symlink(struct inode *inode, const char *target)
{
    struct super_block *sb = inode->i_sb;
    struct buffer_head *bh;
    struct fsSoftLink *disk_link;
    struct sfs_link_writer writer;
    const char *cursor = target;
    size_t target_length;
    int result = 0;

    if (!target)
        return -EINVAL;

    bh = asfs_breadcheck(
        sb, ASFS_I(inode)->firstblock, ASFS_SOFTLINK_ID);
    if (!bh)
        return -EIO;

    if (sb->s_blocksize <= sizeof(struct fsSoftLink) + 1U) {
        asfs_brelse(bh);
        return -EUCLEAN;
    }

    disk_link = (struct fsSoftLink *)bh->b_data;
    writer.data = (char *)disk_link->string;
    writer.capacity = sb->s_blocksize - sizeof(struct fsSoftLink);
    writer.length = 0U;
    writer.data[0] = '\0';

    target_length = strnlen(target, PATH_MAX);
    if (target_length == PATH_MAX) {
        result = -ENAMETOOLONG;
        goto out;
    }

    if (*cursor == '/') {
        const char *prefix = ASFS_SB(sb)->prefix;
        size_t prefix_length = prefix ? strlen(prefix) : 0U;

        while (*cursor == '/')
            cursor++;

        if (prefix && prefix_length != 0U &&
            strncmp(target, prefix, prefix_length) == 0) {
            const char *volume = target + prefix_length;
            const char *slash = strchr(volume, '/');
            size_t volume_length = slash
                ? (size_t)(slash - volume)
                : strlen(volume);
            size_t index = 0U;

            while (index < volume_length) {
                size_t consumed = 0U;
                result = sfs_link_convert_one(
                    &writer, volume + index, volume_length - index,
                    ASFS_SB(sb)->nls_io, ASFS_SB(sb)->nls_disk,
                    false, &consumed);
                if (result != 0 || consumed == 0U)
                    goto out;
                index += consumed;
            }
            result = sfs_link_append_byte(&writer, ':');
            if (result != 0)
                goto out;
            cursor = slash ? slash + 1 : volume + volume_length;
        } else if (ASFS_SB(sb)->root_volume) {
            result = sfs_link_append_bytes(
                &writer, ASFS_SB(sb)->root_volume,
                strlen(ASFS_SB(sb)->root_volume));
            if (result == 0)
                result = sfs_link_append_byte(&writer, ':');
            if (result != 0)
                goto out;
        } else {
            result = sfs_link_append_byte(&writer, '/');
            if (result != 0)
                goto out;
        }
    }

    while (*cursor != '\0') {
        size_t remaining = strlen(cursor);

        if (cursor[0] == '.' && cursor[1] == '.' && cursor[2] == '/') {
            result = sfs_link_append_byte(&writer, '/');
            if (result != 0)
                goto out;
            cursor += 3;
            continue;
        }
        if (cursor[0] == '.' && cursor[1] == '/') {
            cursor += 2;
            continue;
        }
        if (cursor[0] == '/') {
            result = sfs_link_append_byte(&writer, '/');
            if (result != 0)
                goto out;
            do {
                cursor++;
            } while (*cursor == '/');
            continue;
        }

        {
            size_t consumed = 0U;
            result = sfs_link_convert_one(
                &writer, cursor, remaining,
                ASFS_SB(sb)->nls_io, ASFS_SB(sb)->nls_disk,
                false, &consumed);
            if (result != 0 || consumed == 0U)
                goto out;
            cursor += consumed;
        }
    }

    asfs_bstore(sb, bh);

out:
    asfs_brelse(bh);
    return result;
}

#endif

