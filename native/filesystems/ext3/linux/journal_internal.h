/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Infiltrator Filesystem Support — EXT3 journal compatibility contract.
 *
 * EXT3 uses the kernel JBD2 service as its journal transport.  This header
 * deliberately contains only the narrow compatibility surface required by
 * the EXT3 implementation; no private copy of JBD/JBD2 is retained.
 */

#ifndef INFILTRATOR_EXT3_JOURNAL_INTERNAL_H
#define INFILTRATOR_EXT3_JOURNAL_INTERNAL_H

#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/jbd2.h>
#include <linux/mm.h>
#include <linux/pagemap.h>

#define JBD_DEFAULT_MAX_COMMIT_AGE JBD2_DEFAULT_MAX_COMMIT_AGE
#define JFS_ABORT                   JBD2_ABORT
#define JFS_BARRIER                 JBD2_BARRIER
#define JFS_FEATURE_INCOMPAT_REVOKE JBD2_FEATURE_INCOMPAT_REVOKE

/*
 * Historical EXT3 exposed a policy bit for abort-on-sync-data-error.
 * JBD2 no longer owns that policy bit, so EXT3 keeps it outside JBD2 state.
 */
#define JFS_ABORT_ON_SYNCDATA_ERR 0UL

#ifndef CONFIG_JBD_DEBUG
#define jbd_debug(level, fmt, ...) do { } while (0)
#endif

#define journal_start                    jbd2_journal_start
#define journal_restart                  jbd2_journal_restart
#define journal_get_write_access         jbd2_journal_get_write_access
#define journal_get_create_access        jbd2_journal_get_create_access
#define journal_get_undo_access          jbd2_journal_get_undo_access
#define journal_dirty_metadata           jbd2_journal_dirty_metadata
#define journal_forget                   jbd2_journal_forget
#define journal_stop                     jbd2_journal_stop
#define journal_lock_updates             jbd2_journal_lock_updates
#define journal_unlock_updates           jbd2_journal_unlock_updates
#define journal_init_dev                 jbd2_journal_init_dev
#define journal_init_inode               jbd2_journal_init_inode
#define journal_update_format            jbd2_journal_update_format
#define journal_check_used_features      jbd2_journal_check_used_features
#define journal_check_available_features jbd2_journal_check_available_features
#define journal_set_features             jbd2_journal_set_features
#define journal_load                     jbd2_journal_load
#define journal_destroy                  jbd2_journal_destroy
#define journal_wipe                     jbd2_journal_wipe
#define journal_abort                    jbd2_journal_abort
#define journal_errno                    jbd2_journal_errno
#define journal_ack_err                  jbd2_journal_ack_err
#define journal_clear_err                jbd2_journal_clear_err
#define journal_force_commit             jbd2_journal_force_commit
#define journal_start_commit             jbd2_journal_start_commit
#define journal_force_commit_nested      jbd2_journal_force_commit_nested
#define journal_trans_will_send_data_barrier jbd2_trans_will_send_data_barrier
#define journal_revoke                   jbd2_journal_revoke
#define journal_current_handle           journal_current_handle

static inline int journal_extend(handle_t *handle, int blocks)
{
	return jbd2_journal_extend(handle, blocks, 0);
}

static inline int journal_flush(journal_t *journal)
{
	return jbd2_journal_flush(journal, 0);
}

static inline void journal_release_buffer(handle_t *handle,
					  struct buffer_head *bh)
{
	(void)handle;
	(void)bh;
}

static inline void jbd_lock_bh_state(struct buffer_head *bh)
{
	struct journal_head *jh = bh2jh(bh);

	J_ASSERT_JH(jh, jh != NULL);
	spin_lock(&jh->b_state_lock);
}

static inline void jbd_unlock_bh_state(struct buffer_head *bh)
{
	struct journal_head *jh = bh2jh(bh);

	J_ASSERT_JH(jh, jh != NULL);
	spin_unlock(&jh->b_state_lock);
}

static inline int journal_blocks_per_page(struct inode *inode)
{
	return PAGE_SIZE >> inode->i_sb->s_blocksize_bits;
}

static inline void journal_invalidatepage(journal_t *journal,
					  struct folio *folio,
					  size_t offset,
					  size_t length)
{
	jbd2_journal_invalidate_folio(journal, folio, offset, length);
}

static inline int journal_try_to_free_buffers(journal_t *journal,
					      struct folio *folio,
					      gfp_t gfp_mask)
{
	(void)gfp_mask;
	return jbd2_journal_try_to_free_buffers(journal, folio) ? 1 : 0;
}

int journal_dirty_data(handle_t *handle, struct buffer_head *bh);
int journal_create(journal_t *journal);

#endif
