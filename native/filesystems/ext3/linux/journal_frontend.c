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
