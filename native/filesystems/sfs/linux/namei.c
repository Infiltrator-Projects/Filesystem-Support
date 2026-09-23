#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/nls.h>
#include <linux/string.h>
#include "asfs_fs.h"

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
