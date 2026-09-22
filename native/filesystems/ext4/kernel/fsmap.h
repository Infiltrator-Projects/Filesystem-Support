// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright (C) 2017 Oracle.  All Rights Reserved.
 *
 * Author: Darrick J. Wong <darrick.wong@oracle.com>
 */

/*
 * EXT4 — Filesystem-map interfaces
 *
 * Purpose:
 *   Defines internal fsmap query state and callbacks used while walking EXT4 physical allocation metadata.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Range endpoints use filesystem block units and require overflow-safe conversion at user/kernel boundaries.
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

#ifndef __EXT4_FSMAP_H__
#define	__EXT4_FSMAP_H__

struct fsmap;


/**
 * struct ext4_fsmap - Private EXT4 state/data structure used by filesystem-map interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fsmap {
	struct list_head	fmr_list;
	dev_t		fmr_device;
	uint32_t	fmr_flags;
	uint64_t	fmr_physical;
	uint64_t	fmr_owner;
	uint64_t	fmr_length;
};

/**
 * struct ext4_fsmap_head - Private EXT4 state/data structure used by filesystem-map interfaces.
 *
 * Treat fields that mirror persistent media or cross subsystem boundaries
 * as interface contracts rather than incidental layout.
 */
struct ext4_fsmap_head {
	uint32_t	fmh_iflags;
	uint32_t	fmh_oflags;
	unsigned int	fmh_count;
	unsigned int	fmh_entries;

	struct ext4_fsmap fmh_keys[2];
};

void ext4_fsmap_from_internal(struct super_block *sb, struct fsmap *dest,
		struct ext4_fsmap *src);
void ext4_fsmap_to_internal(struct super_block *sb, struct ext4_fsmap *dest,
		struct fsmap *src);


typedef int (*ext4_fsmap_format_t)(struct ext4_fsmap *, void *);

int ext4_getfsmap(struct super_block *sb, struct ext4_fsmap_head *head,
		ext4_fsmap_format_t formatter, void *arg);

#define EXT4_QUERY_RANGE_ABORT		1
#define EXT4_QUERY_RANGE_CONTINUE	0


#define EXT4_FMR_OWN_FREE	FMR_OWN_FREE
#define EXT4_FMR_OWN_UNKNOWN	FMR_OWN_UNKNOWN
#define EXT4_FMR_OWN_FS		FMR_OWNER('X', 1)
#define EXT4_FMR_OWN_LOG	FMR_OWNER('X', 2)
#define EXT4_FMR_OWN_INODES	FMR_OWNER('X', 5)
#define EXT4_FMR_OWN_GDT	FMR_OWNER('f', 1)
#define EXT4_FMR_OWN_RESV_GDT	FMR_OWNER('f', 2)
#define EXT4_FMR_OWN_BLKBM	FMR_OWNER('f', 3)
#define EXT4_FMR_OWN_INOBM	FMR_OWNER('f', 4)

#endif
