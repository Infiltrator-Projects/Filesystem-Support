#ifndef INFILTRATOR_AMIGADOS_DISK_LAYOUT_H
#define INFILTRATOR_AMIGADOS_DISK_LAYOUT_H

#include <linux/types.h>

#define FS_OFS          0x444f5300U
#define FS_FFS          0x444f5301U
#define FS_INTLOFS      0x444f5302U
#define FS_INTLFFS      0x444f5303U
#define FS_DCOFS        0x444f5304U
#define FS_DCFFS        0x444f5305U

#define MUFS_FS         0x6d754653U
#define MUFS_OFS        0x6d754600U
#define MUFS_FFS        0x6d754601U
#define MUFS_INTLOFS    0x6d754602U
#define MUFS_INTLFFS    0x6d754603U
#define MUFS_DCOFS      0x6d754604U
#define MUFS_DCFFS      0x6d754605U

#define T_SHORT         2
#define T_LIST          16
#define T_DATA          8

#define ST_LINKFILE     (-4)
#define ST_FILE         (-3)
#define ST_ROOT         1
#define ST_USERDIR      2
#define ST_SOFTLINK     3
#define ST_LINKDIR      4

#define AFFS_ROOT_BMAPS 25U
#define AFFS_EPOCH_DELTA ((8LL * 365LL + 2LL) * 86400LL)

struct affs_date {
    __be32 days;
    __be32 mins;
    __be32 ticks;
};

struct affs_short_date {
    __be16 days;
    __be16 mins;
    __be16 ticks;
};

struct affs_root_head {
    __be32 ptype;
    __be32 reserved0;
    __be32 reserved1;
    __be32 hash_size;
    __be32 reserved2;
    __be32 checksum;
    __be32 table[];
};

struct affs_root_tail {
    __be32 bitmap_valid;
    __be32 bitmap_blocks[AFFS_ROOT_BMAPS];
    __be32 bitmap_extension;
    struct affs_date root_change;
    u8 disk_name[32];
    __be32 reserved0;
    __be32 reserved1;
    struct affs_date disk_change;
    struct affs_date disk_create;
    __be32 reserved2;
    __be32 reserved3;
    __be32 directory_cache;
    __be32 stype;
};

/*
 * Compatibility field aliases keep the Linux adapter readable while the
 * canonical core is migrated away from direct structure access.
 */
#define bm_flag bitmap_valid
#define bm_blk bitmap_blocks
#define bm_ext bitmap_extension
#define dcache directory_cache

struct affs_head {
    __be32 ptype;
    __be32 key;
    __be32 block_count;
    __be32 reserved;
    __be32 first_data;
    __be32 checksum;
    __be32 table[];
};

struct affs_tail {
    __be32 reserved0;
    __be16 uid;
    __be16 gid;
    __be32 protect;
    __be32 size;
    u8 comment[92];
    struct affs_date change;
    u8 name[32];
    __be32 reserved1;
    __be32 original;
    __be32 link_chain;
    __be32 reserved2[5];
    __be32 hash_chain;
    __be32 parent;
    __be32 extension;
    __be32 stype;
};

struct slink_front {
    __be32 ptype;
    __be32 key;
    __be32 reserved[3];
    __be32 checksum;
    u8 symname[];
};

struct affs_data_head {
    __be32 ptype;
    __be32 key;
    __be32 sequence;
    __be32 size;
    __be32 next;
    __be32 checksum;
    u8 data[];
};

#define FIBF_OTR_READ       0x8000U
#define FIBF_OTR_WRITE      0x4000U
#define FIBF_OTR_EXECUTE    0x2000U
#define FIBF_OTR_DELETE     0x1000U
#define FIBF_GRP_READ       0x0800U
#define FIBF_GRP_WRITE      0x0400U
#define FIBF_GRP_EXECUTE    0x0200U
#define FIBF_GRP_DELETE     0x0100U
#define FIBF_HIDDEN         0x0080U
#define FIBF_SCRIPT         0x0040U
#define FIBF_PURE           0x0020U
#define FIBF_ARCHIVED       0x0010U
#define FIBF_NOREAD         0x0008U
#define FIBF_NOWRITE        0x0004U
#define FIBF_NOEXECUTE      0x0002U
#define FIBF_NODELETE       0x0001U

#define FIBF_OWNER          0x000fU
#define FIBF_MASK           0xee0eU

#endif
