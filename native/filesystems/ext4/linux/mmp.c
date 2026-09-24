#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/kthread.h>
#include <linux/random.h>
#include <linux/utsname.h>

#include "ext4.h"

static __le32 ext4_mmp_checksum(struct super_block *sb,
				const struct mmp_struct *mmp)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	const int length = offsetof(struct mmp_struct, mmp_checksum);

	return cpu_to_le32(
		ext4_chksum(sbi, sbi->s_csum_seed, (const char *)mmp, length));
}

static bool ext4_mmp_checksum_valid(struct super_block *sb,
				    const struct mmp_struct *mmp)
{
	return !ext4_has_metadata_csum(sb) ||
	       mmp->mmp_checksum == ext4_mmp_checksum(sb, mmp);
}

static void ext4_mmp_update_checksum(struct super_block *sb,
				     struct mmp_struct *mmp)
{
	if (ext4_has_metadata_csum(sb))
		mmp->mmp_checksum = ext4_mmp_checksum(sb, mmp);
}

static int ext4_mmp_write_unfrozen(struct super_block *sb,
				    struct buffer_head *bh)
{
	struct mmp_struct *mmp = (struct mmp_struct *)bh->b_data;

	ext4_mmp_update_checksum(sb, mmp);
	lock_buffer(bh);
	bh->b_end_io = end_buffer_write_sync;
	get_bh(bh);
	submit_bh(REQ_OP_WRITE | REQ_SYNC | REQ_META | REQ_PRIO, bh);
	wait_on_buffer(bh);
	return buffer_uptodate(bh) ? 0 : -EIO;
}

static int ext4_mmp_write(struct super_block *sb, struct buffer_head *bh)
{
	int err;

	sb_start_write(sb);
	err = ext4_mmp_write_unfrozen(sb, bh);
	sb_end_write(sb);
	return err;
}

static int ext4_mmp_read(struct super_block *sb, struct buffer_head **bh,
			 ext4_fsblk_t block)
{
	struct mmp_struct *mmp;
	int err;

	if (*bh)
		clear_buffer_uptodate(*bh);
	else {
		*bh = sb_getblk(sb, block);
		if (!*bh)
			return -ENOMEM;
	}

	lock_buffer(*bh);
	err = ext4_read_bh(*bh, REQ_META | REQ_PRIO, NULL, false);
	if (err)
		goto fail;

	mmp = (struct mmp_struct *)(*bh)->b_data;
	if (le32_to_cpu(mmp->mmp_magic) != EXT4_MMP_MAGIC) {
		err = -EFSCORRUPTED;
		goto fail;
	}
	if (!ext4_mmp_checksum_valid(sb, mmp)) {
		err = -EFSBADCRC;
		goto fail;
	}

	return 0;

fail:
	brelse(*bh);
	*bh = NULL;
	ext4_warning(sb, "error %d while reading MMP block %llu", err, block);
	return err;
}

void __dump_mmp_msg(struct super_block *sb, struct mmp_struct *mmp,
		    const char *function, unsigned int line,
		    const char *msg)
{
	__ext4_warning(sb, function, line, "%s", msg);
	__ext4_warning(
		sb, function, line,
		"MMP last update: time=%llu node=%.*s device=%.*s",
		(unsigned long long)le64_to_cpu(mmp->mmp_time),
		(int)sizeof(mmp->mmp_nodename), mmp->mmp_nodename,
		(int)sizeof(mmp->mmp_bdevname), mmp->mmp_bdevname);
}

static unsigned int ext4_mmp_random_sequence(void)
{
	return get_random_u32_below(EXT4_MMP_SEQ_MAX + 1U);
}

static unsigned int ext4_mmp_check_interval(
	unsigned int configured, unsigned long elapsed_jiffies)
{
	unsigned int measured = EXT4_MMP_CHECK_MULT *
				(unsigned int)(elapsed_jiffies / HZ);

	if (measured < EXT4_MMP_MIN_CHECK_INTERVAL)
		measured = EXT4_MMP_MIN_CHECK_INTERVAL;
	if (measured > EXT4_MMP_MAX_CHECK_INTERVAL)
		measured = EXT4_MMP_MAX_CHECK_INTERVAL;

	return max(configured, measured);
}

static int ext4_mmp_thread(void *data)
{
	struct super_block *sb = data;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_super_block *es = sbi->s_es;
	struct buffer_head *bh = sbi->s_mmp_bh;
	struct mmp_struct *mmp = (struct mmp_struct *)bh->b_data;
	const ext4_fsblk_t block = le64_to_cpu(es->s_mmp_block);
	const unsigned int update_interval =
		max_t(unsigned int,
		      le16_to_cpu(es->s_mmp_update_interval),
		      EXT4_MMP_MIN_CHECK_INTERVAL);
	unsigned int check_interval =
		max(EXT4_MMP_CHECK_MULT * update_interval,
		    EXT4_MMP_MIN_CHECK_INTERVAL);
	unsigned int sequence = 0;
	unsigned long failed_writes = 0;
	int err = 0;

	mmp->mmp_time = cpu_to_le64(ktime_get_real_seconds());
	mmp->mmp_check_interval = cpu_to_le16(check_interval);
	memcpy(mmp->mmp_nodename, init_utsname()->nodename,
	       sizeof(mmp->mmp_nodename));

	while (!kthread_should_stop() && !ext4_forced_shutdown(sb)) {
		unsigned long started;
		unsigned long elapsed;

		if (!ext4_has_feature_mmp(sb)) {
			ext4_warning(sb, "MMP feature disabled while heartbeat active");
			break;
		}

		if (++sequence > EXT4_MMP_SEQ_MAX)
			sequence = 1;

		mmp->mmp_seq = cpu_to_le32(sequence);
		mmp->mmp_time = cpu_to_le64(ktime_get_real_seconds());
		started = jiffies;

		err = ext4_mmp_write(sb, bh);
		if (err && (failed_writes++ % 60UL) == 0)
			ext4_error_err(sb, -err, "error writing MMP heartbeat");

		elapsed = jiffies - started;
		if (elapsed < update_interval * HZ)
			schedule_timeout_interruptible(
				update_interval * HZ - elapsed);

		elapsed = jiffies - started;
		if (elapsed > check_interval * HZ) {
			struct buffer_head *verify_bh = NULL;
			struct mmp_struct *verify;

			err = ext4_mmp_read(sb, &verify_bh, block);
			if (err)
				break;

			verify = (struct mmp_struct *)verify_bh->b_data;
			if (mmp->mmp_seq != verify->mmp_seq ||
			    memcmp(mmp->mmp_nodename,
				   verify->mmp_nodename,
				   sizeof(mmp->mmp_nodename)) != 0) {
				dump_mmp_msg(
					sb, verify,
					"MMP heartbeat changed by another writer");
				put_bh(verify_bh);
				err = -EBUSY;
				break;
			}
			put_bh(verify_bh);
		}

		check_interval =
			ext4_mmp_check_interval(update_interval, elapsed);
		mmp->mmp_check_interval = cpu_to_le16(check_interval);
	}

	if (!ext4_forced_shutdown(sb)) {
		mmp->mmp_seq = cpu_to_le32(EXT4_MMP_SEQ_CLEAN);
		mmp->mmp_time = cpu_to_le64(ktime_get_real_seconds());
		if (!err)
			err = ext4_mmp_write(sb, bh);
	}

	while (!kthread_should_stop()) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (!kthread_should_stop())
			schedule();
	}
	__set_current_state(TASK_RUNNING);
	return err;
}

void ext4_stop_mmpd(struct ext4_sb_info *sbi)
{
	if (!sbi->s_mmp_tsk)
		return;

	kthread_stop(sbi->s_mmp_tsk);
	brelse(sbi->s_mmp_bh);
	sbi->s_mmp_bh = NULL;
	sbi->s_mmp_tsk = NULL;
}

int ext4_multi_mount_protect(struct super_block *sb, ext4_fsblk_t mmp_block)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_super_block *es = sbi->s_es;
	struct buffer_head *bh = NULL;
	struct mmp_struct *mmp;
	unsigned int check_interval;
	unsigned int wait_seconds;
	u32 observed_sequence;
	u32 claimed_sequence;
	int err;

	if (mmp_block < le32_to_cpu(es->s_first_data_block) ||
	    mmp_block >= ext4_blocks_count(es))
		return -EINVAL;

	err = ext4_mmp_read(sb, &bh, mmp_block);
	if (err)
		return err;

	mmp = (struct mmp_struct *)bh->b_data;
	check_interval = max_t(
		unsigned int,
		le16_to_cpu(es->s_mmp_update_interval),
		EXT4_MMP_MIN_CHECK_INTERVAL);
	check_interval = max_t(
		unsigned int,
		check_interval,
		le16_to_cpu(mmp->mmp_check_interval));

	observed_sequence = le32_to_cpu(mmp->mmp_seq);
	if (observed_sequence == EXT4_MMP_SEQ_FSCK) {
		dump_mmp_msg(sb, mmp, "filesystem check is active");
		err = -EBUSY;
		goto fail;
	}

	wait_seconds = min(check_interval * 2U + 1U, check_interval + 60U);

	if (observed_sequence != EXT4_MMP_SEQ_CLEAN) {
		if (schedule_timeout_interruptible(HZ * wait_seconds) != 0) {
			err = -ETIMEDOUT;
			goto fail;
		}

		err = ext4_mmp_read(sb, &bh, mmp_block);
		if (err)
			goto fail;

		mmp = (struct mmp_struct *)bh->b_data;
		if (observed_sequence != le32_to_cpu(mmp->mmp_seq)) {
			dump_mmp_msg(sb, mmp, "filesystem is active elsewhere");
			err = -EBUSY;
			goto fail;
		}
	}

	claimed_sequence = ext4_mmp_random_sequence();
	mmp->mmp_seq = cpu_to_le32(claimed_sequence);
	err = ext4_mmp_write_unfrozen(sb, bh);
	if (err)
		goto fail;

	if (schedule_timeout_interruptible(HZ * wait_seconds) != 0) {
		err = -ETIMEDOUT;
		goto fail;
	}

	err = ext4_mmp_read(sb, &bh, mmp_block);
	if (err)
		goto fail;

	mmp = (struct mmp_struct *)bh->b_data;
	if (claimed_sequence != le32_to_cpu(mmp->mmp_seq)) {
		dump_mmp_msg(sb, mmp, "MMP claim was overwritten");
		err = -EBUSY;
		goto fail;
	}

	sbi->s_mmp_bh = bh;
	BUILD_BUG_ON(sizeof(mmp->mmp_bdevname) < BDEVNAME_SIZE);
	snprintf(mmp->mmp_bdevname, sizeof(mmp->mmp_bdevname),
		 "%pg", bh->b_bdev);

	sbi->s_mmp_tsk = kthread_run(
		ext4_mmp_thread, sb, "kmmpd-%.*s",
		(int)sizeof(mmp->mmp_bdevname), mmp->mmp_bdevname);
	if (IS_ERR(sbi->s_mmp_tsk)) {
		err = PTR_ERR(sbi->s_mmp_tsk);
		sbi->s_mmp_tsk = NULL;
		sbi->s_mmp_bh = NULL;
		goto fail;
	}

	return 0;

fail:
	brelse(bh);
	return err;
}
