/*
 * Infiltrator Filesystem Support — native Linux PFS3 read adapter.
 *
 * The adapter intentionally starts read-only.  It implements the PFS\1
 * hard-disk/split-anode format directly from the canonical core and the
 * on-disk contracts independently exercised by the project's Defragmenter.
 * Unsupported large-file/super-index variants fail closed.
 */
#include <linux/module.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/pagemap.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/version.h>
#include <linux/time.h>
#include <linux/ctype.h>
#include <linux/overflow.h>
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
#include <linux/fs_context.h>
#endif

#include "../core/pfs3_core.h"

#define IFS_PFS3_FS_NAME "pfs3"
#define IFS_PFS3_MAGIC 0x50465331U
#define IFS_PFS3_SECTOR_SIZE 512U
#define IFS_PFS3_ROOT_SECTOR 2U
#define IFS_PFS3_INDEX_ID 0x4942U
#define IFS_PFS3_SUPER_ID 0x5342U
#define IFS_PFS3_MAX_SUPER_INDEX 16U
#define IFS_PFS3_ROOT_ANODE 5U
#define IFS_PFS3_FIRST_USER_ANODE 6U
#define IFS_PFS3_ST_FILE (-3)
#define IFS_PFS3_ST_LINKFILE (-4)
#define IFS_PFS3_ST_ROLLOVERFILE (-16)
#define IFS_PFS3_ST_USERDIR 2
#define IFS_PFS3_ST_SOFTLINK 3
#define IFS_PFS3_ST_LINKDIR 4
#define IFS_PFS3_INDEX_HEADER 12U
#define IFS_PFS3_MAX_DIRECT_INDEX 99U
#define IFS_PFS3_DEFAULT_MODE 0755
#define IFS_PFS3_MAX_CHAIN 1048576U

struct ifs_pfs3_sb_info {
    struct mutex lock;
    IfsPfs3RootRecord root;
    u32 index_blocks[IFS_PFS3_MAX_DIRECT_INDEX];
    u32 superindex_blocks[IFS_PFS3_MAX_SUPER_INDEX];
    u16 filename_size;
    u32 sectors_per_reserved;
};

struct ifs_pfs3_inode_info {
    u32 first_anode;
    s8 disk_type;
    struct inode vfs_inode;
};

struct ifs_pfs3_dir_entry {
    s8 type;
    u32 anode;
    u64 size;
    u16 day;
    u16 minute;
    u16 tick;
    u8 protection;
    u8 name_length;
    u8 name[IFS_PFS3_MAX_FILENAME_SIZE + 1U];
};

static struct kmem_cache *ifs_pfs3_inode_cache;

static inline struct ifs_pfs3_sb_info *IFS_PFS3_SB(struct super_block *sb)
{
    return sb->s_fs_info;
}

static inline struct ifs_pfs3_inode_info *IFS_PFS3_I(struct inode *inode)
{
    return container_of(inode, struct ifs_pfs3_inode_info, vfs_inode);
}

static u16 pfs3_be16(const u8 *p)
{
    return ((u16)p[0] << 8) | (u16)p[1];
}

static u32 pfs3_be32(const u8 *p)
{
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) |
           ((u32)p[2] << 8) | (u32)p[3];
}

static int pfs3_read_sector(struct super_block *sb, u32 sector, u8 *out)
{
    struct buffer_head *bh;

    if (IFS_PFS3_SB(sb)->root.disk_size != 0U) {
        if ((u64)sector >= (u64)IFS_PFS3_SB(sb)->root.disk_size)
            return -EUCLEAN;
    } else if ((u64)sector >= (u64)bdev_nr_sectors(sb->s_bdev)) {
        return -EUCLEAN;
    }
    bh = sb_bread(sb, sector);
    if (!bh)
        return -EIO;
    memcpy(out, bh->b_data, IFS_PFS3_SECTOR_SIZE);
    brelse(bh);
    return 0;
}

static bool pfs3_reserved_pointer_valid(struct super_block *sb, u32 sector)
{
    struct ifs_pfs3_sb_info *sbi = IFS_PFS3_SB(sb);
    u32 rel;

    if (sector < sbi->root.first_reserved ||
        sector > sbi->root.last_reserved)
        return false;
    rel = sector - sbi->root.first_reserved;
    if (rel % sbi->sectors_per_reserved != 0U)
        return false;
    return sector <= sbi->root.last_reserved -
           (sbi->sectors_per_reserved - 1U);
}

static int pfs3_read_reserved(struct super_block *sb, u32 sector, u8 *out)
{
    struct ifs_pfs3_sb_info *sbi = IFS_PFS3_SB(sb);
    u32 i;

    if (!pfs3_reserved_pointer_valid(sb, sector))
        return -EUCLEAN;

    for (i = 0U; i < sbi->sectors_per_reserved; ++i) {
        int rc = pfs3_read_sector(
            sb, sector + i, out + (size_t)i * IFS_PFS3_SECTOR_SIZE);
        if (rc)
            return rc;
    }
    return 0;
}

static int pfs3_index_block_sector(struct super_block *sb,
                                   u32 index_number,
                                   u32 index_per_block,
                                   u32 *sector_out)
{
    struct ifs_pfs3_sb_info *sbi = IFS_PFS3_SB(sb);

    if (!sector_out || index_per_block == 0U)
        return -EINVAL;

    if ((sbi->root.options & IFS_PFS3_MODE_SUPERINDEX) == 0U) {
        if (index_number >= IFS_PFS3_MAX_DIRECT_INDEX ||
            sbi->index_blocks[index_number] == 0U)
            return -ENOENT;
        *sector_out = sbi->index_blocks[index_number];
        return 0;
    }

    {
        const u32 super_number = index_number / index_per_block;
        const u32 super_offset = index_number % index_per_block;
        u8 *super_block;
        u32 sector;
        int rc;

        if (super_number >= IFS_PFS3_MAX_SUPER_INDEX ||
            sbi->superindex_blocks[super_number] == 0U)
            return -ENOENT;

        super_block = kmalloc(sbi->root.reserved_block_size, GFP_KERNEL);
        if (!super_block)
            return -ENOMEM;

        rc = pfs3_read_reserved(
            sb, sbi->superindex_blocks[super_number], super_block);
        if (rc == 0 &&
            (pfs3_be16(super_block) != IFS_PFS3_SUPER_ID ||
             pfs3_be32(super_block + 8U) != super_number))
            rc = -EUCLEAN;

        if (rc == 0) {
            sector = pfs3_be32(
                super_block + IFS_PFS3_INDEX_HEADER + super_offset * 4U);
            if (sector == 0U || !pfs3_reserved_pointer_valid(sb, sector))
                rc = sector == 0U ? -ENOENT : -EUCLEAN;
            else
                *sector_out = sector;
        }

        kfree(super_block);
        return rc;
    }
}

static int pfs3_read_anode(struct super_block *sb, u32 number,
                           IfsPfs3AnodeRecord *record)
{
    struct ifs_pfs3_sb_info *sbi = IFS_PFS3_SB(sb);
    const u32 nodes_per_block =
        (sbi->root.reserved_block_size - IFS_PFS3_ANODEBLOCK_HEADER_BYTES) /
        IFS_PFS3_ANODE_BYTES;
    const u32 index_per_block =
        (sbi->root.reserved_block_size - IFS_PFS3_INDEX_HEADER) / 4U;
    u32 sequence;
    u32 offset;
    u32 index_sequence;
    u32 index_offset;
    u8 *index_block;
    u8 *anode_block;
    u32 index_block_sector;
    u32 anode_block_sector;
    size_t anode_offset;
    int rc;

    if (number == 0U || nodes_per_block == 0U || index_per_block == 0U)
        return -EUCLEAN;

    if ((sbi->root.options & IFS_PFS3_MODE_SPLITTED_ANODES) != 0U) {
        sequence = number >> 16;
        offset = number & 0xffffU;
    } else {
        sequence = number / nodes_per_block;
        offset = number % nodes_per_block;
    }

    if (offset >= nodes_per_block)
        return -EUCLEAN;

    index_sequence = sequence / index_per_block;
    index_offset = sequence % index_per_block;

    index_block = kmalloc(sbi->root.reserved_block_size, GFP_KERNEL);
    anode_block = kmalloc(sbi->root.reserved_block_size, GFP_KERNEL);
    if (!index_block || !anode_block) {
        rc = -ENOMEM;
        goto out;
    }

    rc = pfs3_index_block_sector(
        sb, index_sequence, index_per_block, &index_block_sector);
    if (rc)
        goto out;

    rc = pfs3_read_reserved(sb, index_block_sector, index_block);
    if (rc)
        goto out;
    if (pfs3_be16(index_block) != IFS_PFS3_INDEX_ID ||
        pfs3_be32(index_block + 8U) != index_sequence) {
        rc = -EUCLEAN;
        goto out;
    }

    anode_block_sector =
        pfs3_be32(index_block + IFS_PFS3_INDEX_HEADER + index_offset * 4U);
    if (anode_block_sector == 0U) {
        rc = -ENOENT;
        goto out;
    }

    rc = pfs3_read_reserved(sb, anode_block_sector, anode_block);
    if (rc)
        goto out;
    if (pfs3_be16(anode_block) != IFS_PFS3_ANODEBLOCK_ID ||
        pfs3_be32(anode_block + 8U) != sequence) {
        rc = -EUCLEAN;
        goto out;
    }

    anode_offset = IFS_PFS3_ANODEBLOCK_HEADER_BYTES +
                   (size_t)offset * IFS_PFS3_ANODE_BYTES;
    rc = ifs_pfs3_decode_anode(
        anode_block + anode_offset,
        sbi->root.reserved_block_size - (u32)anode_offset,
        record);
    if (rc != 0)
        rc = -EUCLEAN;
out:
    kfree(anode_block);
    kfree(index_block);
    return rc;
}

static int pfs3_map_file_sector(struct inode *inode, sector_t logical,
                                sector_t *physical)
{
    struct super_block *sb = inode->i_sb;
    u32 current = IFS_PFS3_I(inode)->first_anode;
    u64 remaining = logical;
    u32 guard = 0U;

    while (current != 0U) {
        IfsPfs3AnodeRecord anode;
        int rc;

        if (++guard > IFS_PFS3_MAX_CHAIN)
            return -ELOOP;
        rc = pfs3_read_anode(sb, current, &anode);
        if (rc)
            return rc;

        if (anode.cluster_size == 0U)
            return -EUCLEAN;
        if (anode.block_number <= IFS_PFS3_SB(sb)->root.last_reserved ||
            ifs_pfs3_validate_anode_extent(
                &anode, IFS_PFS3_SB(sb)->root.disk_size) != 0)
            return -EUCLEAN;

        if (remaining < anode.cluster_size) {
            *physical = (sector_t)anode.block_number + remaining;
            return 0;
        }
        remaining -= anode.cluster_size;
        current = anode.next_anode;
    }
    return -EIO;
}

static int pfs3_get_block(struct inode *inode, sector_t iblock,
                          struct buffer_head *bh, int create)
{
    sector_t physical;
    int rc;

    if (create)
        return -EROFS;
    if ((u64)iblock >= DIV_ROUND_UP_ULL((u64)i_size_read(inode),
                                        IFS_PFS3_SECTOR_SIZE))
        return -EIO;

    rc = pfs3_map_file_sector(inode, iblock, &physical);
    if (rc)
        return rc;
    map_bh(bh, inode->i_sb, physical);
    return 0;
}

static int pfs3_read_folio(struct file *file, struct folio *folio)
{
    return block_read_full_folio(folio, pfs3_get_block);
}

static void pfs3_readahead(struct readahead_control *rac)
{
    mpage_readahead(rac, pfs3_get_block);
}

static sector_t pfs3_bmap(struct address_space *mapping, sector_t block)
{
    return generic_block_bmap(mapping, block, pfs3_get_block);
}

static const struct address_space_operations pfs3_aops = {
    .read_folio = pfs3_read_folio,
    .readahead = pfs3_readahead,
    .bmap = pfs3_bmap,
};

static const struct file_operations pfs3_file_ops = {
    .llseek = generic_file_llseek,
    .read_iter = generic_file_read_iter,
    .mmap = generic_file_mmap,
    .splice_read = filemap_splice_read,
};

static time64_t pfs3_datestamp_to_unix(u16 day, u16 minute, u16 tick)
{
    /* Amiga epoch 1978-01-01; PFS stores 50 Hz ticks. */
    return (time64_t)day * 86400LL + (time64_t)minute * 60LL +
           (time64_t)(tick / 50U) + 252460800LL;
}

static int pfs3_dir_type_to_dtype(s8 type)
{
    switch (type) {
    case IFS_PFS3_ST_USERDIR:
    case IFS_PFS3_ST_LINKDIR:
        return DT_DIR;
    case IFS_PFS3_ST_SOFTLINK:
        return DT_LNK;
    default:
        return DT_REG;
    }
}

static umode_t pfs3_mode_from_type(s8 type)
{
    switch (type) {
    case IFS_PFS3_ST_USERDIR:
        return S_IFDIR | 0755;
    case IFS_PFS3_ST_SOFTLINK:
        return S_IFLNK | 0777;
    default:
        return S_IFREG | 0644;
    }
}

static bool pfs3_name_equal(const u8 *disk_name, u8 disk_len,
                            const struct qstr *name)
{
    unsigned int i;

    if (name->len != disk_len)
        return false;
    for (i = 0U; i < disk_len; ++i) {
        u8 a = disk_name[i];
        u8 b = (u8)name->name[i];

        if (a >= 'a' && a <= 'z')
            a -= 0x20U;
        if (b >= 'a' && b <= 'z')
            b -= 0x20U;
        if (a != b)
            return false;
    }
    return true;
}

static int pfs3_extra_u16(const u8 *record,
                          const IfsPfs3DirEntryView *decoded,
                          unsigned int field_index,
                          u16 *value)
{
    u32 cursor;
    unsigned int index;

    if (!record || !decoded || !value ||
        field_index >= IFS_PFS3_EXTRA_FIELD_WORDS)
        return -EINVAL;

    *value = 0U;
    if ((decoded->extra_flags & (1U << field_index)) == 0U)
        return 0;

    cursor = decoded->record_bytes - 2U;
    for (index = 0U; index <= field_index; ++index) {
        if ((decoded->extra_flags & (1U << index)) == 0U)
            continue;
        if (cursor < 2U)
            return -EUCLEAN;
        cursor -= 2U;
        if (index == field_index) {
            *value = pfs3_be16(record + cursor);
            return 0;
        }
    }

    return -EUCLEAN;
}

typedef int (*pfs3_entry_visitor)(struct super_block *,
                                 const struct ifs_pfs3_dir_entry *,
                                 void *);

static int pfs3_walk_directory(struct super_block *sb, u32 first_anode,
                               pfs3_entry_visitor visitor, void *opaque)
{
    struct ifs_pfs3_sb_info *sbi = IFS_PFS3_SB(sb);
    u32 current = first_anode;
    u32 guard = 0U;
    u8 *block;
    int rc = 0;

    block = kmalloc(sbi->root.reserved_block_size, GFP_KERNEL);
    if (!block)
        return -ENOMEM;

    while (current != 0U) {
        IfsPfs3AnodeRecord anode;
        u32 extent_sector;
        u32 extent_index;

        if (++guard > IFS_PFS3_MAX_CHAIN) {
            rc = -ELOOP;
            break;
        }
        rc = pfs3_read_anode(sb, current, &anode);
        if (rc)
            break;
        if (anode.cluster_size == 0U ||
            anode.block_number < sbi->root.first_reserved ||
            anode.block_number > sbi->root.last_reserved) {
            rc = -EUCLEAN;
            break;
        }

        extent_sector = anode.block_number;
        for (extent_index = 0U; extent_index < anode.cluster_size;
             ++extent_index) {
            IfsPfs3DirBlockView view;
            u32 entry_count = 0U;
            u32 offset = IFS_PFS3_DIRBLOCK_HEADER_BYTES;
            u64 physical =
                (u64)extent_sector +
                (u64)extent_index * sbi->sectors_per_reserved;

            if (physical > U32_MAX ||
                !pfs3_reserved_pointer_valid(sb, (u32)physical)) {
                rc = -EUCLEAN;
                goto out;
            }
            rc = pfs3_read_reserved(
                sb, (u32)physical, block);
            if (rc)
                goto out;
            if (ifs_pfs3_decode_directory_block(
                    block, sbi->root.reserved_block_size,
                    (sbi->root.options & IFS_PFS3_MODE_DIR_EXTENSION) != 0U,
                    &view, &entry_count) != 0 ||
                view.directory_anode != first_anode) {
                rc = -EUCLEAN;
                goto out;
            }

            while (offset < sbi->root.reserved_block_size) {
                IfsPfs3DirEntryView decoded;
                struct ifs_pfs3_dir_entry entry;
                IfsPfs3DirEntryStatus status;

                status = ifs_pfs3_decode_directory_entry(
                    block + offset,
                    sbi->root.reserved_block_size - offset,
                    (sbi->root.options & IFS_PFS3_MODE_DIR_EXTENSION) != 0U,
                    &decoded);
                if (status == IFS_PFS3_DIRENTRY_END)
                    break;
                if (status != IFS_PFS3_DIRENTRY_OK ||
                    decoded.record_bytes == 0U ||
                    decoded.name_length > sbi->filename_size) {
                    rc = -EUCLEAN;
                    goto out;
                }

                memset(&entry, 0, sizeof(entry));
                entry.type = decoded.type;
                entry.anode = decoded.anode;
                entry.size = decoded.file_size_low;
                if ((sbi->root.options & IFS_PFS3_MODE_LARGEFILE) != 0U) {
                    u16 size_high;

                    rc = pfs3_extra_u16(
                        block + offset, &decoded, 10U, &size_high);
                    if (rc != 0)
                        goto out;
                    entry.size |= (u64)size_high << 32;
                }
                entry.day = decoded.creation_day;
                entry.minute = decoded.creation_minute;
                entry.tick = decoded.creation_tick;
                entry.protection = decoded.protection;
                entry.name_length = decoded.name_length;
                memcpy(entry.name,
                       block + offset + IFS_PFS3_DIRENTRY_NAME_OFFSET,
                       entry.name_length);
                entry.name[entry.name_length] = '\0';

                if (entry.anode < IFS_PFS3_FIRST_USER_ANODE) {
                    rc = -EUCLEAN;
                    goto out;
                }
                rc = visitor(sb, &entry, opaque);
                if (rc)
                    goto out;
                offset += decoded.record_bytes;
            }
        }
        current = anode.next_anode;
    }
out:
    kfree(block);
    return rc;
}

static struct inode *pfs3_iget(struct super_block *sb, u32 anode, s8 type,
                               u32 size, u16 day, u16 minute, u16 tick);

struct pfs3_lookup_ctx {
    const struct qstr *name;
    struct inode *inode;
    int error;
};

static int pfs3_lookup_visit(struct super_block *sb,
                             const struct ifs_pfs3_dir_entry *entry,
                             void *opaque)
{
    struct pfs3_lookup_ctx *ctx = opaque;

    if (!pfs3_name_equal(entry->name, entry->name_length, ctx->name))
        return 0;

    if (entry->type == IFS_PFS3_ST_LINKFILE ||
        entry->type == IFS_PFS3_ST_LINKDIR ||
        entry->type == IFS_PFS3_ST_ROLLOVERFILE) {
        ctx->error = -EOPNOTSUPP;
        return 1;
    }
    ctx->inode = pfs3_iget(
        sb, entry->anode, entry->type, entry->size,
        entry->day, entry->minute, entry->tick);
    if (IS_ERR(ctx->inode)) {
        ctx->error = PTR_ERR(ctx->inode);
        ctx->inode = NULL;
    }
    return 1;
}

static struct dentry *pfs3_lookup(struct inode *dir, struct dentry *dentry,
                                  unsigned int flags)
{
    struct pfs3_lookup_ctx ctx = {
        .name = &dentry->d_name,
        .inode = NULL,
        .error = 0,
    };
    int rc;

    (void)flags;
    if (dentry->d_name.len > IFS_PFS3_SB(dir->i_sb)->filename_size)
        return ERR_PTR(-ENAMETOOLONG);

    rc = pfs3_walk_directory(
        dir->i_sb, IFS_PFS3_I(dir)->first_anode,
        pfs3_lookup_visit, &ctx);
    if (ctx.error)
        return ERR_PTR(ctx.error);
    if (rc < 0)
        return ERR_PTR(rc);

    return d_splice_alias(ctx.inode, dentry);
}

struct pfs3_readdir_ctx {
    struct dir_context *ctx;
    loff_t ordinal;
};

static int pfs3_readdir_visit(struct super_block *sb,
                              const struct ifs_pfs3_dir_entry *entry,
                              void *opaque)
{
    struct pfs3_readdir_ctx *walk = opaque;

    (void)sb;
    if (walk->ordinal++ < walk->ctx->pos - 2)
        return 0;
    if (!dir_emit(walk->ctx, entry->name, entry->name_length,
                  entry->anode, pfs3_dir_type_to_dtype(entry->type)))
        return 1;
    walk->ctx->pos++;
    return 0;
}

static int pfs3_iterate(struct file *file, struct dir_context *ctx)
{
    struct inode *inode = file_inode(file);
    struct pfs3_readdir_ctx walk = { .ctx = ctx, .ordinal = 0 };

    if (!dir_emit_dots(file, ctx))
        return 0;

    /*
     * Directory records do not carry a stable ordinal cookie.  Restarting a
     * full scan is safe; skip already-emitted records according to ctx->pos.
     */
    {
        const int rc = pfs3_walk_directory(
            inode->i_sb, IFS_PFS3_I(inode)->first_anode,
            pfs3_readdir_visit, &walk);
        return rc < 0 ? rc : 0;
    }
}

static const struct file_operations pfs3_dir_ops = {
    .owner = THIS_MODULE,
    .iterate_shared = pfs3_iterate,
    .llseek = generic_file_llseek,
};

static const struct inode_operations pfs3_dir_iops = {
    .lookup = pfs3_lookup,
};

static void pfs3_free_link(void *link)
{
    kfree(link);
}

static const char *pfs3_get_link(struct dentry *dentry, struct inode *inode,
                                 struct delayed_call *done)
{
    IfsPfs3AnodeRecord anode;
    char *target;
    size_t length;
    int rc;

    (void)dentry;
    if (!inode)
        return ERR_PTR(-ECHILD);
    if (IFS_PFS3_I(inode)->disk_type != IFS_PFS3_ST_SOFTLINK)
        return ERR_PTR(-EINVAL);

    rc = pfs3_read_anode(
        inode->i_sb, IFS_PFS3_I(inode)->first_anode, &anode);
    if (rc)
        return ERR_PTR(rc);
    if (anode.cluster_size == 0U ||
        anode.block_number <= IFS_PFS3_SB(inode->i_sb)->root.last_reserved ||
        ifs_pfs3_validate_anode_extent(
            &anode, IFS_PFS3_SB(inode->i_sb)->root.disk_size) != 0)
        return ERR_PTR(-EUCLEAN);

    target = kmalloc(IFS_PFS3_SECTOR_SIZE + 1U, GFP_KERNEL);
    if (!target)
        return ERR_PTR(-ENOMEM);

    rc = pfs3_read_sector(inode->i_sb, anode.block_number, (u8 *)target);
    if (rc) {
        kfree(target);
        return ERR_PTR(rc);
    }
    target[IFS_PFS3_SECTOR_SIZE] = '\0';
    length = strnlen(target, IFS_PFS3_SECTOR_SIZE);
    if (length == IFS_PFS3_SECTOR_SIZE) {
        kfree(target);
        return ERR_PTR(-EUCLEAN);
    }

    set_delayed_call(done, pfs3_free_link, target);
    return target;
}

static const struct inode_operations pfs3_file_iops = {
};

static const struct inode_operations pfs3_symlink_iops = {
    .get_link = pfs3_get_link,
};

static void pfs3_init_inode_common(struct inode *inode, s8 type,
                                   u16 day, u16 minute, u16 tick)
{
    const time64_t when = pfs3_datestamp_to_unix(day, minute, tick);

    inode->i_mode = pfs3_mode_from_type(type);
    i_uid_write(inode, 0);
    i_gid_write(inode, 0);
    inode_set_atime(inode, when, 0);
    inode_set_mtime(inode, when, 0);
    inode_set_ctime(inode, when, 0);
}

static struct inode *pfs3_iget(struct super_block *sb, u32 anode, s8 type,
                               u32 size, u16 day, u16 minute, u16 tick)
{
    struct inode *inode = iget_locked(sb, anode);

    if (!inode)
        return ERR_PTR(-ENOMEM);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
    if ((inode_state_read_once(inode) & I_NEW) == 0)
        return inode;
#else
    if ((inode->i_state & I_NEW) == 0)
        return inode;
#endif

    IFS_PFS3_I(inode)->first_anode = anode;
    IFS_PFS3_I(inode)->disk_type = type;
    pfs3_init_inode_common(inode, type, day, minute, tick);

    switch (type) {
    case IFS_PFS3_ST_USERDIR:
        inode->i_size = 0;
        inode->i_op = &pfs3_dir_iops;
        inode->i_fop = &pfs3_dir_ops;
        set_nlink(inode, 2);
        break;
    case IFS_PFS3_ST_FILE:
    case IFS_PFS3_ST_ROLLOVERFILE:
        inode->i_size = size;
        inode->i_blocks = DIV_ROUND_UP_ULL(size, IFS_PFS3_SECTOR_SIZE);
        inode->i_op = &pfs3_file_iops;
        inode->i_fop = &pfs3_file_ops;
        inode->i_mapping->a_ops = &pfs3_aops;
        set_nlink(inode, 1);
        break;
    case IFS_PFS3_ST_SOFTLINK:
        inode->i_size = 0;
        inode->i_blocks = 1;
        inode->i_op = &pfs3_symlink_iops;
        set_nlink(inode, 1);
        break;
    default:
        iget_failed(inode);
        return ERR_PTR(-EOPNOTSUPP);
    }

    unlock_new_inode(inode);
    return inode;
}

static struct inode *pfs3_get_root_inode(struct super_block *sb)
{
    struct ifs_pfs3_sb_info *sbi = IFS_PFS3_SB(sb);

    return pfs3_iget(
        sb, IFS_PFS3_ROOT_ANODE, IFS_PFS3_ST_USERDIR, 0U,
        sbi->root.creation_day, sbi->root.creation_minute,
        sbi->root.creation_tick);
}

static int pfs3_statfs(struct dentry *dentry, struct kstatfs *buf)
{
    struct ifs_pfs3_sb_info *sbi = IFS_PFS3_SB(dentry->d_sb);

    buf->f_type = IFS_PFS3_MAGIC;
    buf->f_bsize = IFS_PFS3_SECTOR_SIZE;
    buf->f_blocks = sbi->root.disk_size;
    buf->f_bfree = sbi->root.blocks_free;
    buf->f_bavail = sbi->root.blocks_free > sbi->root.always_free ?
        sbi->root.blocks_free - sbi->root.always_free : 0U;
    buf->f_namelen = sbi->filename_size;
    return 0;
}

static struct inode *pfs3_alloc_inode(struct super_block *sb)
{
    struct ifs_pfs3_inode_info *info;

    (void)sb;
    info = alloc_inode_sb(sb, ifs_pfs3_inode_cache, GFP_KERNEL);
    if (!info)
        return NULL;
    return &info->vfs_inode;
}

static void pfs3_free_inode(struct inode *inode)
{
    kmem_cache_free(ifs_pfs3_inode_cache, IFS_PFS3_I(inode));
}

static void pfs3_inode_init_once(void *object)
{
    struct ifs_pfs3_inode_info *info = object;

    inode_init_once(&info->vfs_inode);
}

static void pfs3_put_super(struct super_block *sb)
{
    kfree(sb->s_fs_info);
    sb->s_fs_info = NULL;
}

static const struct super_operations pfs3_sops = {
    .alloc_inode = pfs3_alloc_inode,
    .free_inode = pfs3_free_inode,
    .put_super = pfs3_put_super,
    .statfs = pfs3_statfs,
};

static int pfs3_load_root(struct super_block *sb)
{
    struct ifs_pfs3_sb_info *sbi = IFS_PFS3_SB(sb);
    u8 *root_sector;
    u8 *extension = NULL;
    u32 i;
    int rc;

    if (!sb_set_blocksize(sb, IFS_PFS3_SECTOR_SIZE))
        return -EINVAL;

    root_sector = kmalloc(IFS_PFS3_SECTOR_SIZE, GFP_KERNEL);
    if (!root_sector)
        return -ENOMEM;

    rc = pfs3_read_sector(sb, IFS_PFS3_ROOT_SECTOR, root_sector);
    if (rc)
        goto out;

    if (ifs_pfs3_decode_root(
            root_sector, IFS_PFS3_SECTOR_SIZE, &sbi->root) != 0) {
        rc = -EINVAL;
        goto out;
    }

    if (ifs_pfs3_classify_disk_type(sbi->root.disk_type) ==
            IFS_PFS3_FORMAT_INVALID ||
        (sbi->root.options & (IFS_PFS3_MODE_HARDDISK |
                               IFS_PFS3_MODE_SIZEFIELD)) !=
            (IFS_PFS3_MODE_HARDDISK |
             IFS_PFS3_MODE_SIZEFIELD) ||
        ((sbi->root.options & IFS_PFS3_MODE_LARGEFILE) != 0U &&
         (sbi->root.options & IFS_PFS3_MODE_DIR_EXTENSION) == 0U)) {
        rc = -EUCLEAN;
        goto out;
    }

    if (ifs_pfs3_validate_root_record(
            &sbi->root, IFS_PFS3_SECTOR_SIZE,
            (u32)(bdev_nr_sectors(sb->s_bdev))) != IFS_PFS3_MEDIA_OK) {
        rc = -EUCLEAN;
        goto out;
    }

    if (sbi->root.reserved_block_size < IFS_PFS3_SECTOR_SIZE ||
        sbi->root.reserved_block_size > 4096U ||
        sbi->root.reserved_block_size % IFS_PFS3_SECTOR_SIZE != 0U) {
        rc = -EUCLEAN;
        goto out;
    }
    sbi->sectors_per_reserved =
        sbi->root.reserved_block_size / IFS_PFS3_SECTOR_SIZE;
    sbi->filename_size = IFS_PFS3_DEFAULT_FILENAME_SIZE;

    for (i = 0U; i < IFS_PFS3_MAX_DIRECT_INDEX; ++i)
        sbi->index_blocks[i] =
            pfs3_be32(root_sector + 116U + i * 4U);

    if ((sbi->root.options & IFS_PFS3_MODE_EXTENSION) != 0U) {
        IfsPfs3ExtensionRecord ext;

        extension = kmalloc(sbi->root.reserved_block_size, GFP_KERNEL);
        if (!extension) {
            rc = -ENOMEM;
            goto out;
        }
        rc = pfs3_read_reserved(sb, sbi->root.extension, extension);
        if (rc)
            goto out;
        if (ifs_pfs3_decode_extension(
                extension, sbi->root.reserved_block_size, &ext) != 0 ||
            ifs_pfs3_validate_extension(
                &ext,
                (sbi->root.last_reserved - sbi->root.first_reserved + 1U) /
                    sbi->sectors_per_reserved,
                &sbi->filename_size) != 0) {
            rc = -EUCLEAN;
            goto out;
        }

        if ((sbi->root.options & IFS_PFS3_MODE_SUPERINDEX) != 0U) {
            if (sbi->root.reserved_block_size <
                64U + IFS_PFS3_MAX_SUPER_INDEX * 4U) {
                rc = -EUCLEAN;
                goto out;
            }
            for (i = 0U; i < IFS_PFS3_MAX_SUPER_INDEX; ++i) {
                sbi->superindex_blocks[i] =
                    pfs3_be32(extension + 64U + i * 4U);
                if (sbi->superindex_blocks[i] != 0U &&
                    !pfs3_reserved_pointer_valid(
                        sb, sbi->superindex_blocks[i])) {
                    rc = -EUCLEAN;
                    goto out;
                }
            }
        }
    } else if ((sbi->root.options & IFS_PFS3_MODE_SUPERINDEX) != 0U) {
        rc = -EUCLEAN;
        goto out;
    }

    rc = 0;
out:
    kfree(extension);
    kfree(root_sector);
    return rc;
}

static int pfs3_fill_super_data(struct super_block *sb, int silent)
{
    struct ifs_pfs3_sb_info *sbi;
    struct inode *root;
    int rc;

    (void)silent;

    sbi = kzalloc(sizeof(*sbi), GFP_KERNEL);
    if (!sbi)
        return -ENOMEM;
    mutex_init(&sbi->lock);
    sb->s_fs_info = sbi;

    rc = pfs3_load_root(sb);
    if (rc)
        goto fail;

    sb->s_magic = IFS_PFS3_MAGIC;
    sb->s_flags |= SB_RDONLY | SB_NODEV | SB_NOSUID;
    sb->s_maxbytes = 0x0000FFFFFFFFFFFFLL;
    sb->s_op = &pfs3_sops;

    root = pfs3_get_root_inode(sb);
    if (IS_ERR(root)) {
        rc = PTR_ERR(root);
        goto fail;
    }
    sb->s_root = d_make_root(root);
    if (!sb->s_root) {
        rc = -ENOMEM;
        goto fail;
    }
    return 0;
fail:
    pfs3_put_super(sb);
    return rc;
}

#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
static int pfs3_fill_super(struct super_block *sb, struct fs_context *fc)
{
    return pfs3_fill_super_data(
        sb, (fc->sb_flags & SB_SILENT) != 0);
}

static int pfs3_get_tree(struct fs_context *fc)
{
    fc->sb_flags |= SB_RDONLY;
    return get_tree_bdev(fc, pfs3_fill_super);
}

static const struct fs_context_operations pfs3_context_ops = {
    .get_tree = pfs3_get_tree,
};

static int pfs3_init_fs_context(struct fs_context *fc)
{
    fc->sb_flags |= SB_RDONLY;
    fc->ops = &pfs3_context_ops;
    return 0;
}
#else
static int pfs3_fill_super_legacy(struct super_block *sb, void *data, int silent)
{
    (void)data;
    return pfs3_fill_super_data(sb, silent);
}

static struct dentry *pfs3_mount(struct file_system_type *type, int flags,
                                 const char *dev_name, void *data)
{
    return mount_bdev(
        type, flags | SB_RDONLY, dev_name, data, pfs3_fill_super_legacy);
}
#endif

static struct file_system_type pfs3_type = {
    .owner = THIS_MODULE,
    .name = IFS_PFS3_FS_NAME,
#if LINUX_VERSION_CODE >= KERNEL_VERSION(7, 0, 0)
    .init_fs_context = pfs3_init_fs_context,
#else
    .mount = pfs3_mount,
#endif
    .kill_sb = kill_block_super,
    .fs_flags = FS_REQUIRES_DEV,
};

static int __init pfs3_init(void)
{
    int rc;

    ifs_pfs3_inode_cache = kmem_cache_create(
        "pfs3_inode_cache", sizeof(struct ifs_pfs3_inode_info), 0,
        SLAB_RECLAIM_ACCOUNT | SLAB_ACCOUNT, pfs3_inode_init_once);
    if (!ifs_pfs3_inode_cache)
        return -ENOMEM;

    rc = register_filesystem(&pfs3_type);
    if (rc) {
        kmem_cache_destroy(ifs_pfs3_inode_cache);
        ifs_pfs3_inode_cache = NULL;
    }
    return rc;
}

static void __exit pfs3_exit(void)
{
    unregister_filesystem(&pfs3_type);
    rcu_barrier();
    kmem_cache_destroy(ifs_pfs3_inode_cache);
    ifs_pfs3_inode_cache = NULL;
}

MODULE_DESCRIPTION("Infiltrator PFS3 native read-only filesystem support");
MODULE_ALIAS_FS(IFS_PFS3_FS_NAME);
MODULE_LICENSE("GPL");
module_init(pfs3_init);
module_exit(pfs3_exit);
