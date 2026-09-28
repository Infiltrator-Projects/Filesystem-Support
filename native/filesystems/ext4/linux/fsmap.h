#ifndef INFILTRATR_EXT4_FSMAP_H
#define INFILTRATR_EXT4_FSMAP_H

struct fsmap;

/*
 * Internal physical-map representation. Values remain in filesystem block
 * units until the Linux adapter converts them at the user/kernel boundary.
 */
struct ext4_fsmap {
	struct list_head fmr_list;
	dev_t fmr_device;
	uint32_t fmr_flags;
	uint64_t fmr_physical;
	uint64_t fmr_owner;
	uint64_t fmr_length;
};

struct ext4_fsmap_head {
	uint32_t fmh_iflags;
	uint32_t fmh_oflags;
	unsigned int fmh_count;
	unsigned int fmh_entries;
	struct ext4_fsmap fmh_keys[2];
};

typedef int (*ext4_fsmap_format_t)(struct ext4_fsmap *, void *);

void ext4_fsmap_from_internal(struct super_block *sb, struct fsmap *dest,
			     struct ext4_fsmap *src);
void ext4_fsmap_to_internal(struct super_block *sb, struct ext4_fsmap *dest,
			   struct fsmap *src);
int ext4_getfsmap(struct super_block *sb, struct ext4_fsmap_head *head,
		  ext4_fsmap_format_t formatter, void *arg);

#define EXT4_QUERY_RANGE_CONTINUE 0
#define EXT4_QUERY_RANGE_ABORT    1

#define EXT4_FMR_OWN_FREE      FMR_OWN_FREE
#define EXT4_FMR_OWN_UNKNOWN   FMR_OWN_UNKNOWN
#define EXT4_FMR_OWN_FS        FMR_OWNER('X', 1)
#define EXT4_FMR_OWN_LOG       FMR_OWNER('X', 2)
#define EXT4_FMR_OWN_INODES    FMR_OWNER('X', 5)
#define EXT4_FMR_OWN_GDT       FMR_OWNER('f', 1)
#define EXT4_FMR_OWN_RESV_GDT  FMR_OWNER('f', 2)
#define EXT4_FMR_OWN_BLKBM     FMR_OWNER('f', 3)
#define EXT4_FMR_OWN_INOBM     FMR_OWNER('f', 4)

#endif
