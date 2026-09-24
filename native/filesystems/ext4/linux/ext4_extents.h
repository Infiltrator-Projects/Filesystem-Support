#ifndef INFILTRATR_EXT4_EXTENTS_H
#define INFILTRATR_EXT4_EXTENTS_H

#include "ext4.h"

#define AGGRESSIVE_TEST_
#define EXTENTS_STATS__
#define CHECK_BINSEARCH__
#define EXT_STATS_

struct ext4_extent_tail {
	__le32 et_checksum;
};

struct ext4_extent {
	__le32 ee_block;
	__le16 ee_len;
	__le16 ee_start_hi;
	__le32 ee_start_lo;
};

struct ext4_extent_idx {
	__le32 ei_block;
	__le32 ei_leaf_lo;
	__le16 ei_leaf_hi;
	__u16 ei_unused;
};

struct ext4_extent_header {
	__le16 eh_magic;
	__le16 eh_entries;
	__le16 eh_max;
	__le16 eh_depth;
	__le32 eh_generation;
};

#define EXT4_EXT_MAGIC cpu_to_le16(0xf30a)
#define EXT4_MAX_EXTENT_DEPTH 5
#define EXT4_EXTENT_TAIL_OFFSET(header) 	(sizeof(struct ext4_extent_header) + 	 sizeof(struct ext4_extent) * le16_to_cpu((header)->eh_max))

static inline struct ext4_extent_tail *
find_ext4_extent_tail(struct ext4_extent_header *header)
{
	return (struct ext4_extent_tail *)
		((char *)header + EXT4_EXTENT_TAIL_OFFSET(header));
}

struct ext4_ext_path {
	ext4_fsblk_t p_block;
	__u16 p_depth;
	__u16 p_maxdepth;
	struct ext4_extent *p_ext;
	struct ext4_extent_idx *p_idx;
	struct ext4_extent_header *p_hdr;
	struct buffer_head *p_bh;
};

enum ext4_partial_cluster_state {
	EXT4_PARTIAL_CLUSTER_INITIAL = 0,
	EXT4_PARTIAL_CLUSTER_TO_FREE,
	EXT4_PARTIAL_CLUSTER_NO_FREE,
};

struct partial_cluster {
	ext4_fsblk_t pclu;
	ext4_lblk_t lblk;
	enum {
		initial = EXT4_PARTIAL_CLUSTER_INITIAL,
		tofree = EXT4_PARTIAL_CLUSTER_TO_FREE,
		nofree = EXT4_PARTIAL_CLUSTER_NO_FREE,
	} state;
};

#define EXT_INIT_MAX_LEN (1UL << 15)
#define EXT_UNWRITTEN_MAX_LEN (EXT_INIT_MAX_LEN - 1)

#define EXT_FIRST_EXTENT(header) 	((struct ext4_extent *)((char *)(header) + 	 sizeof(struct ext4_extent_header)))
#define EXT_FIRST_INDEX(header) 	((struct ext4_extent_idx *)((char *)(header) + 	 sizeof(struct ext4_extent_header)))
#define EXT_HAS_FREE_INDEX(path) 	(le16_to_cpu((path)->p_hdr->eh_entries) < 	 le16_to_cpu((path)->p_hdr->eh_max))
#define EXT_LAST_EXTENT(header) 	(EXT_FIRST_EXTENT(header) + 	 le16_to_cpu((header)->eh_entries) - 1)
#define EXT_LAST_INDEX(header) 	(EXT_FIRST_INDEX(header) + 	 le16_to_cpu((header)->eh_entries) - 1)
#define EXT_MAX_EXTENT(header) 	(le16_to_cpu((header)->eh_max) ? 	 EXT_FIRST_EXTENT(header) + 	 le16_to_cpu((header)->eh_max) - 1 : NULL)
#define EXT_MAX_INDEX(header) 	(le16_to_cpu((header)->eh_max) ? 	 EXT_FIRST_INDEX(header) + 	 le16_to_cpu((header)->eh_max) - 1 : NULL)

static inline struct ext4_extent_header *
ext_inode_hdr(struct inode *inode)
{
	return (struct ext4_extent_header *)EXT4_I(inode)->i_data;
}

static inline struct ext4_extent_header *
ext_block_hdr(struct buffer_head *bh)
{
	return (struct ext4_extent_header *)bh->b_data;
}

static inline unsigned short ext_depth(struct inode *inode)
{
	return le16_to_cpu(ext_inode_hdr(inode)->eh_depth);
}

static inline bool ext4_ext_is_unwritten(const struct ext4_extent *extent)
{
	return le16_to_cpu(extent->ee_len) > EXT_INIT_MAX_LEN;
}

static inline unsigned int
ext4_ext_get_actual_len(const struct ext4_extent *extent)
{
	const unsigned int encoded = le16_to_cpu(extent->ee_len);

	return encoded <= EXT_INIT_MAX_LEN ?
		encoded : encoded - EXT_INIT_MAX_LEN;
}

static inline void ext4_ext_mark_unwritten(struct ext4_extent *extent)
{
	const unsigned int length = ext4_ext_get_actual_len(extent);

	BUG_ON(length == 0 || length > EXT_UNWRITTEN_MAX_LEN);
	extent->ee_len = cpu_to_le16(length + EXT_INIT_MAX_LEN);
}

static inline void ext4_ext_mark_initialized(struct ext4_extent *extent)
{
	extent->ee_len =
		cpu_to_le16(ext4_ext_get_actual_len(extent));
}

static inline ext4_fsblk_t
ext4_ext_pblock(const struct ext4_extent *extent)
{
	return (ext4_fsblk_t)le32_to_cpu(extent->ee_start_lo) |
	       ((ext4_fsblk_t)le16_to_cpu(extent->ee_start_hi) << 32);
}

static inline ext4_fsblk_t
ext4_idx_pblock(const struct ext4_extent_idx *index)
{
	return (ext4_fsblk_t)le32_to_cpu(index->ei_leaf_lo) |
	       ((ext4_fsblk_t)le16_to_cpu(index->ei_leaf_hi) << 32);
}

static inline void ext4_ext_store_pblock(
	struct ext4_extent *extent, ext4_fsblk_t block)
{
	extent->ee_start_lo = cpu_to_le32((u32)block);
	extent->ee_start_hi = cpu_to_le16((u16)(block >> 32));
}

static inline void ext4_idx_store_pblock(
	struct ext4_extent_idx *index, ext4_fsblk_t block)
{
	index->ei_leaf_lo = cpu_to_le32((u32)block);
	index->ei_leaf_hi = cpu_to_le16((u16)(block >> 32));
}

#endif
