// SPDX-License-Identifier: GPL-2.0

/*
 * EXT4 — Multiple-mount protection
 *
 * Purpose:
 *   Implements the on-disk heartbeat used to detect concurrent read-write mounts of the same EXT4 filesystem.
 *
 * Filesystem model:
 *   This file belongs to a full-featured EXT4 VFS implementation with JBD2 embedded in ext4.ko.
 *
 * Correctness focus:
 *   Heartbeat timing and sequence updates are safety signals; stale or ambiguous state must be handled conservatively.
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

#include <linux/fs.h>
#include <linux/random.h>
#include <linux/buffer_head.h>
#include <linux/utsname.h>
#include <linux/kthread.h>

#include "ext4.h"


/**
 * ext4_mmp_csum - Implements the mmp csum operation within the multiple-mount protection subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static __le32 ext4_mmp_csum(struct super_block *sb, struct mmp_struct *mmp)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	int offset = offsetof(struct mmp_struct, mmp_checksum);
	__u32 csum;

	csum = ext4_chksum(sbi, sbi->s_csum_seed, (char *)mmp, offset);

	return cpu_to_le32(csum);
}

/**
 * ext4_mmp_csum_verify - Validates state before it is trusted by the remainder of the filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int ext4_mmp_csum_verify(struct super_block *sb, struct mmp_struct *mmp)
{
	if (!ext4_has_metadata_csum(sb))
		return 1;

	return mmp->mmp_checksum == ext4_mmp_csum(sb, mmp);
}

/**
 * ext4_mmp_csum_set - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static void ext4_mmp_csum_set(struct super_block *sb, struct mmp_struct *mmp)
{
	if (!ext4_has_metadata_csum(sb))
		return;

	mmp->mmp_checksum = ext4_mmp_csum(sb, mmp);
}


/**
 * write_mmp_block_thawed - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int write_mmp_block_thawed(struct super_block *sb,
				  struct buffer_head *bh)
{
	struct mmp_struct *mmp = (struct mmp_struct *)(bh->b_data);

	ext4_mmp_csum_set(sb, mmp);
	lock_buffer(bh);
	bh->b_end_io = end_buffer_write_sync;
	get_bh(bh);
	submit_bh(REQ_OP_WRITE | REQ_SYNC | REQ_META | REQ_PRIO, bh);
	wait_on_buffer(bh);
	if (unlikely(!buffer_uptodate(bh)))
		return -EIO;
	return 0;
}

/**
 * write_mmp_block - Updates filesystem state under the ordering and persistence rules of the surrounding subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int write_mmp_block(struct super_block *sb, struct buffer_head *bh)
{
	int err;


	sb_start_write(sb);
	err = write_mmp_block_thawed(sb, bh);
	sb_end_write(sb);
	return err;
}


/**
 * read_mmp_block - Retrieves or materialises filesystem state for validation or higher-level processing without changing ownership by default.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int read_mmp_block(struct super_block *sb, struct buffer_head **bh,
			  ext4_fsblk_t mmp_block)
{
	struct mmp_struct *mmp;
	int ret;

	if (*bh)
		clear_buffer_uptodate(*bh);


	if (!*bh) {
		*bh = sb_getblk(sb, mmp_block);
		if (!*bh) {
			ret = -ENOMEM;
			goto warn_exit;
		}
	}

	lock_buffer(*bh);
	ret = ext4_read_bh(*bh, REQ_META | REQ_PRIO, NULL, false);
	if (ret)
		goto warn_exit;

	mmp = (struct mmp_struct *)((*bh)->b_data);
	if (le32_to_cpu(mmp->mmp_magic) != EXT4_MMP_MAGIC) {
		ret = -EFSCORRUPTED;
		goto warn_exit;
	}
	if (!ext4_mmp_csum_verify(sb, mmp)) {
		ret = -EFSBADCRC;
		goto warn_exit;
	}
	return 0;
warn_exit:
	brelse(*bh);
	*bh = NULL;
	ext4_warning(sb, "Error %d while reading MMP block %llu",
		     ret, mmp_block);
	return ret;
}


/**
 * __dump_mmp_msg - Implements the dump mmp msg operation within the multiple-mount protection subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void __dump_mmp_msg(struct super_block *sb, struct mmp_struct *mmp,
		    const char *function, unsigned int line, const char *msg)
{
	__ext4_warning(sb, function, line, "%s", msg);
	__ext4_warning(sb, function, line,
		       "MMP failure info: last update time: %llu, last update node: %.*s, last update device: %.*s",
		       (unsigned long long)le64_to_cpu(mmp->mmp_time),
		       (int)sizeof(mmp->mmp_nodename), mmp->mmp_nodename,
		       (int)sizeof(mmp->mmp_bdevname), mmp->mmp_bdevname);
}


/**
 * kmmpd - Implements the kmmpd operation within the multiple-mount protection subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static int kmmpd(void *data)
{
	struct super_block *sb = data;
	struct ext4_super_block *es = EXT4_SB(sb)->s_es;
	struct buffer_head *bh = EXT4_SB(sb)->s_mmp_bh;
	struct mmp_struct *mmp;
	ext4_fsblk_t mmp_block;
	u32 seq = 0;
	unsigned long failed_writes = 0;
	int mmp_update_interval = le16_to_cpu(es->s_mmp_update_interval);
	unsigned mmp_check_interval;
	unsigned long last_update_time;
	unsigned long diff;
	int retval = 0;

	mmp_block = le64_to_cpu(es->s_mmp_block);
	mmp = (struct mmp_struct *)(bh->b_data);
	mmp->mmp_time = cpu_to_le64(ktime_get_real_seconds());


	mmp_check_interval = max(EXT4_MMP_CHECK_MULT * mmp_update_interval,
				 EXT4_MMP_MIN_CHECK_INTERVAL);
	mmp->mmp_check_interval = cpu_to_le16(mmp_check_interval);

	memcpy(mmp->mmp_nodename, init_utsname()->nodename,
	       sizeof(mmp->mmp_nodename));

	while (!kthread_should_stop() && !ext4_forced_shutdown(sb)) {
		if (!ext4_has_feature_mmp(sb)) {
			ext4_warning(sb, "kmmpd being stopped since MMP feature"
				     " has been disabled.");
			goto wait_to_exit;
		}
		if (++seq > EXT4_MMP_SEQ_MAX)
			seq = 1;

		mmp->mmp_seq = cpu_to_le32(seq);
		mmp->mmp_time = cpu_to_le64(ktime_get_real_seconds());
		last_update_time = jiffies;

		retval = write_mmp_block(sb, bh);


		if (retval) {
			if ((failed_writes % 60) == 0) {
				ext4_error_err(sb, -retval,
					       "Error writing to MMP block");
			}
			failed_writes++;
		}

		diff = jiffies - last_update_time;
		if (diff < mmp_update_interval * HZ)
			schedule_timeout_interruptible(mmp_update_interval *
						       HZ - diff);


		diff = jiffies - last_update_time;
		if (diff > mmp_check_interval * HZ) {
			struct buffer_head *bh_check = NULL;
			struct mmp_struct *mmp_check;

			retval = read_mmp_block(sb, &bh_check, mmp_block);
			if (retval) {
				ext4_error_err(sb, -retval,
					       "error reading MMP data: %d",
					       retval);
				goto wait_to_exit;
			}

			mmp_check = (struct mmp_struct *)(bh_check->b_data);
			if (mmp->mmp_seq != mmp_check->mmp_seq ||
			    memcmp(mmp->mmp_nodename, mmp_check->mmp_nodename,
				   sizeof(mmp->mmp_nodename))) {
				dump_mmp_msg(sb, mmp_check,
					     "Error while updating MMP info. "
					     "The filesystem seems to have been"
					     " multiply mounted.");
				ext4_error_err(sb, EBUSY, "abort");
				put_bh(bh_check);
				retval = -EBUSY;
				goto wait_to_exit;
			}
			put_bh(bh_check);
		}


		mmp_check_interval = max(min(EXT4_MMP_CHECK_MULT * diff / HZ,
					     EXT4_MMP_MAX_CHECK_INTERVAL),
					 EXT4_MMP_MIN_CHECK_INTERVAL);
		mmp->mmp_check_interval = cpu_to_le16(mmp_check_interval);
	}


	mmp->mmp_seq = cpu_to_le32(EXT4_MMP_SEQ_CLEAN);
	mmp->mmp_time = cpu_to_le64(ktime_get_real_seconds());

	retval = write_mmp_block(sb, bh);

wait_to_exit:
	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop())
			schedule();
	}
	set_current_state(TASK_RUNNING);
	return retval;
}

/**
 * ext4_stop_mmpd - Implements the stop mmpd operation within the multiple-mount protection subsystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
void ext4_stop_mmpd(struct ext4_sb_info *sbi)
{
	if (sbi->s_mmp_tsk) {
		kthread_stop(sbi->s_mmp_tsk);
		brelse(sbi->s_mmp_bh);
		sbi->s_mmp_tsk = NULL;
	}
}


/**
 * mmp_new_seq - Allocates or reserves filesystem state while maintaining the owning allocator's accounting invariants.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
static unsigned int mmp_new_seq(void)
{
	return get_random_u32_below(EXT4_MMP_SEQ_MAX + 1);
}


/**
 * ext4_multi_mount_protect - Implements a mount-path operation for the owning filesystem.
 *
 * Correctness contract: preserve the locking, lifetime, range and
 * transaction preconditions established by the surrounding EXT4
 * subsystem. Failure handling must follow that subsystem's established
 * rollback, abort or retry policy.
 */
int ext4_multi_mount_protect(struct super_block *sb,
				    ext4_fsblk_t mmp_block)
{
	struct ext4_super_block *es = EXT4_SB(sb)->s_es;
	struct buffer_head *bh = NULL;
	struct mmp_struct *mmp = NULL;
	u32 seq;
	unsigned int mmp_check_interval = le16_to_cpu(es->s_mmp_update_interval);
	unsigned int wait_time = 0;
	int retval;

	if (mmp_block < le32_to_cpu(es->s_first_data_block) ||
	    mmp_block >= ext4_blocks_count(es)) {
		ext4_warning(sb, "Invalid MMP block in superblock");
		retval = -EINVAL;
		goto failed;
	}

	retval = read_mmp_block(sb, &bh, mmp_block);
	if (retval)
		goto failed;

	mmp = (struct mmp_struct *)(bh->b_data);

	if (mmp_check_interval < EXT4_MMP_MIN_CHECK_INTERVAL)
		mmp_check_interval = EXT4_MMP_MIN_CHECK_INTERVAL;


	if (le16_to_cpu(mmp->mmp_check_interval) > mmp_check_interval)
		mmp_check_interval = le16_to_cpu(mmp->mmp_check_interval);

	seq = le32_to_cpu(mmp->mmp_seq);
	if (seq == EXT4_MMP_SEQ_CLEAN)
		goto skip;

	if (seq == EXT4_MMP_SEQ_FSCK) {
		dump_mmp_msg(sb, mmp, "fsck is running on the filesystem");
		retval = -EBUSY;
		goto failed;
	}

	wait_time = min(mmp_check_interval * 2 + 1,
			mmp_check_interval + 60);


	if (wait_time > EXT4_MMP_MIN_CHECK_INTERVAL * 4)
		ext4_warning(sb, "MMP interval %u higher than expected, please"
			     " wait.\n", wait_time * 2);

	if (schedule_timeout_interruptible(HZ * wait_time) != 0) {
		ext4_warning(sb, "MMP startup interrupted, failing mount\n");
		retval = -ETIMEDOUT;
		goto failed;
	}

	retval = read_mmp_block(sb, &bh, mmp_block);
	if (retval)
		goto failed;
	mmp = (struct mmp_struct *)(bh->b_data);
	if (seq != le32_to_cpu(mmp->mmp_seq)) {
		dump_mmp_msg(sb, mmp,
			     "Device is already active on another node.");
		retval = -EBUSY;
		goto failed;
	}

skip:


	seq = mmp_new_seq();
	mmp->mmp_seq = cpu_to_le32(seq);


	retval = write_mmp_block_thawed(sb, bh);
	if (retval)
		goto failed;


	if (schedule_timeout_interruptible(HZ * wait_time) != 0) {
		ext4_warning(sb, "MMP startup interrupted, failing mount");
		retval = -ETIMEDOUT;
		goto failed;
	}

	retval = read_mmp_block(sb, &bh, mmp_block);
	if (retval)
		goto failed;
	mmp = (struct mmp_struct *)(bh->b_data);
	if (seq != le32_to_cpu(mmp->mmp_seq)) {
		dump_mmp_msg(sb, mmp,
			     "Device is already active on another node.");
		retval = -EBUSY;
		goto failed;
	}

	EXT4_SB(sb)->s_mmp_bh = bh;

	BUILD_BUG_ON(sizeof(mmp->mmp_bdevname) < BDEVNAME_SIZE);
	snprintf(mmp->mmp_bdevname, sizeof(mmp->mmp_bdevname),
		 "%pg", bh->b_bdev);


	EXT4_SB(sb)->s_mmp_tsk = kthread_run(kmmpd, sb, "kmmpd-%.*s",
					     (int)sizeof(mmp->mmp_bdevname),
					     mmp->mmp_bdevname);
	if (IS_ERR(EXT4_SB(sb)->s_mmp_tsk)) {
		EXT4_SB(sb)->s_mmp_tsk = NULL;
		ext4_warning(sb, "Unable to create kmmpd thread for %s.",
			     sb->s_id);
		retval = -ENOMEM;
		goto failed;
	}

	return 0;

failed:
	brelse(bh);
	return retval;
}
