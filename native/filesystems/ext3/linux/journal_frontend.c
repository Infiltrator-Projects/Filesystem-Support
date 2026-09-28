/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Infiltrator Filesystem Support — EXT3 journal/VFS front-end.
 *
 * This unit owns the small boundary between EXT3 VFS operations and the
 * filesystem's private journal engine.  It contains no journal algorithm:
 * journal state transitions remain inside the EXT3 journal implementation.
 */

#include <linux/err.h>
#include <linux/fs.h>
#include <linux/kernel.h>

#include "linux_adapter.h"

handle_t *ext3_journal_start_sb(struct super_block *sb, int nblocks)
{
	journal_t *journal;

	if (sb->s_flags & SB_RDONLY)
		return ERR_PTR(-EROFS);

	journal = EXT3_SB(sb)->s_journal;
	if (is_journal_aborted(journal)) {
		ext3_abort(sb, __func__, "journal is aborted");
		return ERR_PTR(-EROFS);
	}

	return journal_start(journal, nblocks);
}

int __ext3_journal_stop(const char *where, handle_t *handle)
{
	struct super_block *sb;
	int operation_error;
	int stop_error;

	sb = handle->h_transaction->t_journal->j_private;
	operation_error = handle->h_err;
	stop_error = journal_stop(handle);

	if (!operation_error)
		operation_error = stop_error;
	if (operation_error)
		__ext3_std_error(sb, where, operation_error);

	return operation_error;
}

void ext3_msg(struct super_block *sb, const char *prefix,
	      const char *fmt, ...)
{
	struct va_format message;
	va_list args;

	va_start(args, fmt);
	message.fmt = fmt;
	message.va = &args;
	printk("%sEXT3-fs (%s): %pV\n", prefix, sb->s_id, &message);
	va_end(args);
}


/*
 * All wrappers in this section have one policy: the journal operation owns
 * the media-state transition, while EXT3 owns propagation of any failure.
 * A failed journal request is therefore reported through the common EXT3
 * abort path before its exact errno is returned to the caller.
 */
static int ext3_finish_journal_buffer_op(const char *where,
					 const char *operation,
					 struct buffer_head *bh,
					 handle_t *handle,
					 int error)
{
	if (error)
		ext3_journal_abort_handle(where, operation, bh, handle, error);
	return error;
}

int __ext3_journal_get_undo_access(const char *where, handle_t *handle,
				   struct buffer_head *bh)
{
	return ext3_finish_journal_buffer_op(
		where, __func__, bh, handle,
		journal_get_undo_access(handle, bh));
}

int __ext3_journal_get_write_access(const char *where, handle_t *handle,
				    struct buffer_head *bh)
{
	return ext3_finish_journal_buffer_op(
		where, __func__, bh, handle,
		journal_get_write_access(handle, bh));
}

int __ext3_journal_forget(const char *where, handle_t *handle,
			  struct buffer_head *bh)
{
	return ext3_finish_journal_buffer_op(
		where, __func__, bh, handle,
		journal_forget(handle, bh));
}

int __ext3_journal_revoke(const char *where, handle_t *handle,
			  unsigned long blocknr, struct buffer_head *bh)
{
	return ext3_finish_journal_buffer_op(
		where, __func__, bh, handle,
		journal_revoke(handle, blocknr, bh));
}

int __ext3_journal_get_create_access(const char *where, handle_t *handle,
				     struct buffer_head *bh)
{
	return ext3_finish_journal_buffer_op(
		where, __func__, bh, handle,
		journal_get_create_access(handle, bh));
}

int __ext3_journal_dirty_metadata(const char *where, handle_t *handle,
				  struct buffer_head *bh)
{
	return ext3_finish_journal_buffer_op(
		where, __func__, bh, handle,
		journal_dirty_metadata(handle, bh));
}
