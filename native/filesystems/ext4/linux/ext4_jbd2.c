#include "ext4_jbd2.h"

#include <trace/events/ext4.h>

int ext4_inode_journal_mode(struct inode *inode)
{
	if (!EXT4_JOURNAL(inode))
		return EXT4_INODE_WRITEBACK_DATA_MODE;

	if (!S_ISREG(inode->i_mode) ||
	    ext4_test_inode_flag(inode, EXT4_INODE_EA_INODE) ||
	    test_opt(inode->i_sb, DATA_FLAGS) == EXT4_MOUNT_JOURNAL_DATA ||
	    (ext4_test_inode_flag(inode, EXT4_INODE_JOURNAL_DATA) &&
	     !test_opt(inode->i_sb, DELALLOC))) {
		if (S_ISREG(inode->i_mode) && IS_ENCRYPTED(inode))
			return EXT4_INODE_ORDERED_DATA_MODE;
		return EXT4_INODE_JOURNAL_DATA_MODE;
	}

	if (test_opt(inode->i_sb, DATA_FLAGS) == EXT4_MOUNT_ORDERED_DATA)
		return EXT4_INODE_ORDERED_DATA_MODE;
	if (test_opt(inode->i_sb, DATA_FLAGS) == EXT4_MOUNT_WRITEBACK_DATA)
		return EXT4_INODE_WRITEBACK_DATA_MODE;

	BUG();
}

static handle_t *ext4_nojournal_get(void)
{
	unsigned long refs = (unsigned long)current->journal_info;

	BUG_ON(refs >= EXT4_NOJOURNAL_MAX_REF_COUNT);
	refs++;
	current->journal_info = (handle_t *)refs;
	return (handle_t *)refs;
}

static void ext4_nojournal_put(handle_t *handle)
{
	unsigned long refs = (unsigned long)handle;

	BUG_ON(refs == 0);
	current->journal_info = (handle_t *)(refs - 1);
}

static int ext4_journal_can_start(struct super_block *sb)
{
	journal_t *journal;

	might_sleep();
	if (unlikely(ext4_forced_shutdown(sb)))
		return -EIO;
	if (WARN_ON_ONCE(sb_rdonly(sb)))
		return -EROFS;

	WARN_ON(sb->s_writers.frozen == SB_FREEZE_COMPLETE);
	journal = EXT4_SB(sb)->s_journal;
	if (journal && is_journal_aborted(journal)) {
		ext4_abort(sb, -journal->j_errno, "aborted journal");
		return -EROFS;
	}

	return 0;
}

handle_t *__ext4_journal_start_sb(struct inode *inode,
				  struct super_block *sb,
				  unsigned int line, int type,
				  int blocks, int reserved_blocks,
				  int revoke_credits)
{
	journal_t *journal;
	int err;

	if (inode)
		trace_ext4_journal_start_inode(
			inode, blocks, reserved_blocks,
			revoke_credits, type, _RET_IP_);
	else
		trace_ext4_journal_start_sb(
			sb, blocks, reserved_blocks,
			revoke_credits, type, _RET_IP_);

	err = ext4_journal_can_start(sb);
	if (err)
		return ERR_PTR(err);

	journal = EXT4_SB(sb)->s_journal;
	if (!journal || (EXT4_SB(sb)->s_mount_state & EXT4_FC_REPLAY))
		return ext4_nojournal_get();

	return jbd2__journal_start(
		journal, blocks, reserved_blocks, revoke_credits,
		GFP_NOFS, type, line);
}

int __ext4_journal_stop(const char *where, unsigned int line,
			handle_t *handle)
{
	struct super_block *sb;
	int stored_err;
	int stop_err;

	if (!ext4_handle_valid(handle)) {
		ext4_nojournal_put(handle);
		return 0;
	}

	stored_err = handle->h_err;
	if (!handle->h_transaction) {
		stop_err = jbd2_journal_stop(handle);
		return stored_err ? stored_err : stop_err;
	}

	sb = handle->h_transaction->t_journal->j_private;
	stop_err = jbd2_journal_stop(handle);
	if (!stored_err)
		stored_err = stop_err;
	if (stored_err)
		__ext4_std_error(sb, where, line, stored_err);
	return stored_err;
}

handle_t *__ext4_journal_start_reserved(handle_t *handle,
					unsigned int line, int type)
{
	struct super_block *sb;
	int err;

	if (!ext4_handle_valid(handle))
		return ext4_nojournal_get();

	sb = handle->h_journal->j_private;
	trace_ext4_journal_start_reserved(
		sb, jbd2_handle_buffer_credits(handle), _RET_IP_);

	err = ext4_journal_can_start(sb);
	if (err) {
		jbd2_journal_free_reserved(handle);
		return ERR_PTR(err);
	}

	err = jbd2_journal_start_reserved(handle, type, line);
	return err ? ERR_PTR(err) : handle;
}

int __ext4_journal_ensure_credits(handle_t *handle, int check_credits,
				  int extend_credits, int revoke_credits)
{
	if (!ext4_handle_valid(handle))
		return 0;
	if (is_handle_aborted(handle))
		return -EROFS;
	if (jbd2_handle_buffer_credits(handle) >= check_credits &&
	    handle->h_revoke_credits >= revoke_credits)
		return 0;

	extend_credits = max(
		0, extend_credits - jbd2_handle_buffer_credits(handle));
	revoke_credits = max(
		0, revoke_credits - handle->h_revoke_credits);
	return ext4_journal_extend(
		handle, extend_credits, revoke_credits);
}

static void ext4_abort_handle(const char *where, unsigned int line,
			      const char *operation,
			      struct buffer_head *bh,
			      handle_t *handle, int err)
{
	char error_text[16];

	BUG_ON(!ext4_handle_valid(handle));
	if (bh)
		BUFFER_TRACE(bh, "abort");
	if (!handle->h_err)
		handle->h_err = err;
	if (is_handle_aborted(handle))
		return;

	pr_err("EXT4-fs: %s:%u: aborting transaction: %s in %s\n",
	       where, line,
	       ext4_decode_error(NULL, err, error_text), operation);
	jbd2_journal_abort_handle(handle);
}

static void ext4_check_device_writeback(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct address_space *mapping = sb->s_bdev->bd_mapping;
	int err;

	if (!errseq_check(&mapping->wb_err, READ_ONCE(sbi->s_bdev_wb_err)))
		return;

	spin_lock(&sbi->s_bdev_wb_lock);
	err = errseq_check_and_advance(
		&mapping->wb_err, &sbi->s_bdev_wb_err);
	spin_unlock(&sbi->s_bdev_wb_lock);

	if (err)
		ext4_error_err(
			sb, -err, "asynchronous metadata writeback failed");
}

static void ext4_set_journal_trigger(struct super_block *sb,
				     struct buffer_head *bh,
				     enum ext4_journal_trigger_type type)
{
	if (type == EXT4_JTR_NONE || !ext4_has_metadata_csum(sb))
		return;

	BUG_ON(type >= EXT4_JOURNAL_TRIGGER_COUNT);
	jbd2_journal_set_triggers(
		bh, &EXT4_SB(sb)->s_journal_triggers[type].tr_triggers);
}

int __ext4_journal_get_write_access(const char *where, unsigned int line,
				    handle_t *handle,
				    struct super_block *sb,
				    struct buffer_head *bh,
				    enum ext4_journal_trigger_type type)
{
	int err;

	might_sleep();
	if (ext4_handle_valid(handle)) {
		err = jbd2_journal_get_write_access(handle, bh);
		if (err) {
			ext4_abort_handle(
				where, line, __func__, bh, handle, err);
			return err;
		}
	} else {
		ext4_check_device_writeback(sb);
	}

	ext4_set_journal_trigger(sb, bh, type);
	return 0;
}

int __ext4_forget(const char *where, unsigned int line, handle_t *handle,
		  int metadata, struct inode *inode,
		  struct buffer_head *bh, ext4_fsblk_t block)
{
	int err;

	might_sleep();
	trace_ext4_forget(inode, metadata, block);

	if (!ext4_handle_valid(handle)) {
		if (bh) {
			clear_buffer_dirty(bh);
			wait_on_buffer(bh);
			__bforget(bh);
		}
		return 0;
	}

	if (test_opt(inode->i_sb, DATA_FLAGS) == EXT4_MOUNT_JOURNAL_DATA ||
	    (!metadata && !ext4_should_journal_data(inode))) {
		if (!bh)
			return 0;
		err = jbd2_journal_forget(handle, bh);
		if (err)
			ext4_abort_handle(
				where, line, __func__, bh, handle, err);
		return err;
	}

	err = jbd2_journal_revoke(handle, block, bh);
	if (err) {
		ext4_abort_handle(
			where, line, __func__, bh, handle, err);
		__ext4_error(
			inode->i_sb, where, line, true, -err, 0,
			"journal revoke failed: %d", err);
	}
	return err;
}

int __ext4_journal_get_create_access(
	const char *where, unsigned int line,
	handle_t *handle, struct super_block *sb,
	struct buffer_head *bh,
	enum ext4_journal_trigger_type type)
{
	int err;

	if (!ext4_handle_valid(handle))
		return 0;

	err = jbd2_journal_get_create_access(handle, bh);
	if (err) {
		ext4_abort_handle(
			where, line, __func__, bh, handle, err);
		return err;
	}

	ext4_set_journal_trigger(sb, bh, type);
	return 0;
}

int __ext4_handle_dirty_metadata(const char *where, unsigned int line,
				 handle_t *handle, struct inode *inode,
				 struct buffer_head *bh)
{
	int err = 0;

	might_sleep();
	set_buffer_meta(bh);
	set_buffer_prio(bh);
	set_buffer_uptodate(bh);

	if (ext4_handle_valid(handle)) {
		err = jbd2_journal_dirty_metadata(handle, bh);
		if (!is_handle_aborted(handle) && WARN_ON_ONCE(err)) {
			ext4_abort_handle(
				where, line, __func__, bh, handle, err);
			if (inode)
				ext4_error_inode(
					inode, where, line, bh->b_blocknr,
					"journal metadata dirty failed: %d",
					err);
			else
				pr_err(
					"EXT4: journal metadata dirty failed: %d\n",
					err);
		}
		return err;
	}

	if (inode)
		mark_buffer_dirty_inode(bh, inode);
	else
		mark_buffer_dirty(bh);

	if (inode && inode_needs_sync(inode)) {
		sync_dirty_buffer(bh);
		if (buffer_req(bh) && !buffer_uptodate(bh)) {
			ext4_error_inode_err(
				inode, where, line, bh->b_blocknr, EIO,
				"metadata sync failed");
			err = -EIO;
		}
	}

	return err;
}
