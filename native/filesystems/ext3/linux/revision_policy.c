/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Infiltrator Filesystem Support — EXT3 superblock revision policy.
 *
 * This unit owns the transition from the original EXT revision to the
 * dynamic revision required when feature fields become meaningful.
 * The policy is intentionally small: it updates only the revision-dependent
 * superblock defaults and leaves feature selection and persistence to callers.
 */

#include <linux/fs.h>

#include "linux_adapter.h"

void ext3_update_dynamic_rev(struct super_block *sb)
{
	struct ext3_super_block *disk_super = EXT3_SB(sb)->s_es;
	u32 revision = le32_to_cpu(disk_super->s_rev_level);

	if (revision > EXT3_GOOD_OLD_REV)
		return;

	disk_super->s_first_ino = cpu_to_le32(EXT3_GOOD_OLD_FIRST_INO);
	disk_super->s_inode_size = cpu_to_le16(EXT3_GOOD_OLD_INODE_SIZE);
	disk_super->s_rev_level = cpu_to_le32(EXT3_DYNAMIC_REV);

	ext3_msg(sb, KERN_WARNING,
		 "superblock upgraded to dynamic revision %u; filesystem check recommended",
		 EXT3_DYNAMIC_REV);
}
