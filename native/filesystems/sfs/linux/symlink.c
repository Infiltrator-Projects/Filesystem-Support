#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/limits.h>
#include <linux/nls.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "asfs_fs.h"

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
