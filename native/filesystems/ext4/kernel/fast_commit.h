/* SPDX-License-Identifier: GPL-2.0 */

/*
 * EXT4 — Fast-commit record format
 *
 * Purpose:
 *   Defines private fast-commit state and record structures used by commit and replay.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Persistent record formats require strict length/type validation because recovery treats them as untrusted disk input.
 *
 * Project rules:
 *   - Register and implement EXT4 only; do not route EXT2 or EXT3 mounts through this module.
 *   - Preserve every valid EXT4 feature path supported by the pinned implementation.
 *   - Treat journaling, extents, allocation, checksums, recovery and feature negotiation as correctness-critical state machines.
 *
 * Commentary policy:
 *   Comments explain invariants, ownership, persistence ordering and
 *   non-obvious design intent. They deliberately avoid restating C syntax.
 */

#ifndef __FAST_COMMIT_H__
#define __FAST_COMMIT_H__


#define EXT4_FC_TAG_ADD_RANGE		0x0001
#define EXT4_FC_TAG_DEL_RANGE		0x0002
#define EXT4_FC_TAG_CREAT		0x0003
#define EXT4_FC_TAG_LINK		0x0004
#define EXT4_FC_TAG_UNLINK		0x0005
#define EXT4_FC_TAG_INODE		0x0006
#define EXT4_FC_TAG_PAD			0x0007
#define EXT4_FC_TAG_TAIL		0x0008
#define EXT4_FC_TAG_HEAD		0x0009

#define EXT4_FC_SUPPORTED_FEATURES	0x0


/**
 * struct ext4_fc_tl - Private EXT4 state/data structure used by fast-commit record format.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fc_tl {
	__le16 fc_tag;
	__le16 fc_len;
};


/**
 * struct ext4_fc_head - Private EXT4 state/data structure used by fast-commit record format.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fc_head {
	__le32 fc_features;
	__le32 fc_tid;
};


/**
 * struct ext4_fc_add_range - Private EXT4 state/data structure used by fast-commit record format.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fc_add_range {
	__le32 fc_ino;
	__u8 fc_ex[12];
};


/**
 * struct ext4_fc_del_range - Private EXT4 state/data structure used by fast-commit record format.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fc_del_range {
	__le32 fc_ino;
	__le32 fc_lblk;
	__le32 fc_len;
};


/**
 * struct ext4_fc_dentry_info - Private EXT4 state/data structure used by fast-commit record format.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fc_dentry_info {
	__le32 fc_parent_ino;
	__le32 fc_ino;
	__u8 fc_dname[];
};


/**
 * struct ext4_fc_inode - Private EXT4 state/data structure used by fast-commit record format.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fc_inode {
	__le32 fc_ino;
	__u8 fc_raw_inode[];
};


/**
 * struct ext4_fc_tail - Private EXT4 state/data structure used by fast-commit record format.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fc_tail {
	__le32 fc_tid;
	__le32 fc_crc;
};


#define EXT4_FC_TAG_BASE_LEN (sizeof(struct ext4_fc_tl))


/**
 * EXT4_FC_TAG_BASE_LEN - Implements the EXT4 FC TAG BASE LEN operation within the fast-commit record format subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
enum {
	EXT4_FC_STATUS_OK = 0,
	EXT4_FC_STATUS_INELIGIBLE,
	EXT4_FC_STATUS_SKIPPED,
	EXT4_FC_STATUS_FAILED,
};


enum {
	EXT4_FC_REASON_XATTR = 0,
	EXT4_FC_REASON_CROSS_RENAME,
	EXT4_FC_REASON_JOURNAL_FLAG_CHANGE,
	EXT4_FC_REASON_NOMEM,
	EXT4_FC_REASON_SWAP_BOOT,
	EXT4_FC_REASON_RESIZE,
	EXT4_FC_REASON_RENAME_DIR,
	EXT4_FC_REASON_FALLOC_RANGE,
	EXT4_FC_REASON_INODE_JOURNAL_DATA,
	EXT4_FC_REASON_ENCRYPTED_FILENAME,
	EXT4_FC_REASON_MAX
};

#ifdef __KERNEL__


/**
 * struct ext4_fc_dentry_update - Private EXT4 state/data structure used by fast-commit record format.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fc_dentry_update {
	int fcd_op;
	int fcd_parent;
	int fcd_ino;
	struct qstr fcd_name;
	unsigned char fcd_iname[DNAME_INLINE_LEN];
	struct list_head fcd_list;
	struct list_head fcd_dilist;
};

/**
 * struct ext4_fc_stats - Private EXT4 state/data structure used by fast-commit record format.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fc_stats {
	unsigned int fc_ineligible_reason_count[EXT4_FC_REASON_MAX];
	unsigned long fc_num_commits;
	unsigned long fc_ineligible_commits;
	unsigned long fc_failed_commits;
	unsigned long fc_skipped_commits;
	unsigned long fc_numblks;
	u64 s_fc_avg_commit_time;
};

#define EXT4_FC_REPLAY_REALLOC_INCREMENT	4


/**
 * struct ext4_fc_alloc_region - Private EXT4 state/data structure used by fast-commit record format.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fc_alloc_region {
	ext4_lblk_t lblk;
	ext4_fsblk_t pblk;
	int ino, len;
};


/**
 * struct ext4_fc_replay_state - Private EXT4 state/data structure used by fast-commit record format.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fc_replay_state {
	int fc_replay_num_tags;
	int fc_replay_expected_off;
	int fc_current_pass;
	int fc_cur_tag;
	int fc_crc;
	struct ext4_fc_alloc_region *fc_regions;
	int fc_regions_size, fc_regions_used, fc_regions_valid;
	int *fc_modified_inodes;
	int fc_modified_inodes_used, fc_modified_inodes_size;
};

#define region_last(__region) (((__region)->lblk) + ((__region)->len) - 1)
#endif

/**
 * tag2str - Implements the tag2str operation within the fast-commit record format subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem; propagate an error or leave state recoverable when the
 * operation cannot complete.
 */
static inline const char *tag2str(__u16 tag)
{
	switch (tag) {
	case EXT4_FC_TAG_LINK:
		return "ADD_ENTRY";
	case EXT4_FC_TAG_UNLINK:
		return "DEL_ENTRY";
	case EXT4_FC_TAG_ADD_RANGE:
		return "ADD_RANGE";
	case EXT4_FC_TAG_CREAT:
		return "CREAT_DENTRY";
	case EXT4_FC_TAG_DEL_RANGE:
		return "DEL_RANGE";
	case EXT4_FC_TAG_INODE:
		return "INODE";
	case EXT4_FC_TAG_PAD:
		return "PAD";
	case EXT4_FC_TAG_TAIL:
		return "TAIL";
	case EXT4_FC_TAG_HEAD:
		return "HEAD";
	default:
		return "ERROR";
	}
}

#endif
