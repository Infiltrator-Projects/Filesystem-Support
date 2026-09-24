#ifndef INFILTRATR_EXT4_JOURNAL_H
#define INFILTRATR_EXT4_JOURNAL_H

#include <linux/fs.h>
#include <linux/jbd2.h>

#include "ext4.h"
#include "embedded_jbd2.h"

#define EXT4_JOURNAL(inode) (EXT4_SB((inode)->i_sb)->s_journal)

#define EXT4_SINGLEDATA_TRANS_BLOCKS(sb) 	(ext4_has_feature_extents(sb) ? 20U : 8U)
#define EXT4_XATTR_TRANS_BLOCKS 6U
#define EXT4_DATA_TRANS_BLOCKS(sb) 	(EXT4_SINGLEDATA_TRANS_BLOCKS(sb) + 	 EXT4_XATTR_TRANS_BLOCKS - 2U + 	 EXT4_MAXQUOTAS_TRANS_BLOCKS(sb))
#define EXT4_META_TRANS_BLOCKS(sb) 	(EXT4_XATTR_TRANS_BLOCKS + EXT4_MAXQUOTAS_TRANS_BLOCKS(sb))
#define EXT4_MAX_TRANS_DATA 64U
#define EXT4_RESERVE_TRANS_BLOCKS 12U
#define EXT4_INDEX_EXTRA_TRANS_BLOCKS 12U

#ifdef CONFIG_QUOTA
#define EXT4_QUOTA_TRANS_BLOCKS(sb) 	(ext4_quota_capable(sb) ? 1 : 0)
#define EXT4_QUOTA_INIT_BLOCKS(sb) 	(ext4_quota_capable(sb) ? 	 (DQUOT_INIT_ALLOC * (EXT4_SINGLEDATA_TRANS_BLOCKS(sb) - 3) + 	  3 + DQUOT_INIT_REWRITE) : 0)
#define EXT4_QUOTA_DEL_BLOCKS(sb) 	(ext4_quota_capable(sb) ? 	 (DQUOT_DEL_ALLOC * (EXT4_SINGLEDATA_TRANS_BLOCKS(sb) - 3) + 	  3 + DQUOT_DEL_REWRITE) : 0)
#else
#define EXT4_QUOTA_TRANS_BLOCKS(sb) 0
#define EXT4_QUOTA_INIT_BLOCKS(sb) 0
#define EXT4_QUOTA_DEL_BLOCKS(sb) 0
#endif

#define EXT4_MAXQUOTAS_TRANS_BLOCKS(sb) 	(EXT4_MAXQUOTAS * EXT4_QUOTA_TRANS_BLOCKS(sb))
#define EXT4_MAXQUOTAS_INIT_BLOCKS(sb) 	(EXT4_MAXQUOTAS * EXT4_QUOTA_INIT_BLOCKS(sb))
#define EXT4_MAXQUOTAS_DEL_BLOCKS(sb) 	(EXT4_MAXQUOTAS * EXT4_QUOTA_DEL_BLOCKS(sb))

enum ext4_journal_handle_type {
	EXT4_HT_MISC = 0,
	EXT4_HT_INODE,
	EXT4_HT_WRITE_PAGE,
	EXT4_HT_MAP_BLOCKS,
	EXT4_HT_DIR,
	EXT4_HT_TRUNCATE,
	EXT4_HT_QUOTA,
	EXT4_HT_RESIZE,
	EXT4_HT_MIGRATE,
	EXT4_HT_MOVE_EXTENTS,
	EXT4_HT_XATTR,
	EXT4_HT_EXT_CONVERT,
	EXT4_HT_MAX,
};

struct ext4_journal_cb_entry {
	struct list_head jce_list;
	void (*jce_func)(struct super_block *,
			 struct ext4_journal_cb_entry *, int);
};

static inline void ext4_journal_callback_add(
	handle_t *handle,
	void (*func)(struct super_block *,
		     struct ext4_journal_cb_entry *, int),
	struct ext4_journal_cb_entry *entry)
{
	struct ext4_sb_info *sbi =
		EXT4_SB(handle->h_transaction->t_journal->j_private);

	entry->jce_func = func;
	spin_lock(&sbi->s_md_lock);
	list_add_tail(
		&entry->jce_list,
		&handle->h_transaction->t_private_list);
	spin_unlock(&sbi->s_md_lock);
}

static inline bool ext4_journal_callback_try_del(
	handle_t *handle, struct ext4_journal_cb_entry *entry)
{
	struct ext4_sb_info *sbi =
		EXT4_SB(handle->h_transaction->t_journal->j_private);
	bool present;

	spin_lock(&sbi->s_md_lock);
	present = !list_empty(&entry->jce_list);
	if (present)
		list_del_init(&entry->jce_list);
	spin_unlock(&sbi->s_md_lock);
	return present;
}

int ext4_mark_iloc_dirty(handle_t *handle, struct inode *inode,
			 struct ext4_iloc *iloc);
int ext4_reserve_inode_write(handle_t *handle, struct inode *inode,
			     struct ext4_iloc *iloc);
int __ext4_mark_inode_dirty(handle_t *handle, struct inode *inode,
			    const char *func, unsigned int line);
#define ext4_mark_inode_dirty(handle, inode) 	__ext4_mark_inode_dirty((handle), (inode), __func__, __LINE__)

int ext4_expand_extra_isize(struct inode *inode,
			    unsigned int new_extra_isize,
			    struct ext4_iloc *iloc);

int __ext4_journal_get_write_access(
	const char *where, unsigned int line,
	handle_t *handle, struct super_block *sb,
	struct buffer_head *bh,
	enum ext4_journal_trigger_type trigger_type);
int __ext4_forget(
	const char *where, unsigned int line,
	handle_t *handle, int metadata,
	struct inode *inode, struct buffer_head *bh,
	ext4_fsblk_t block);
int __ext4_journal_get_create_access(
	const char *where, unsigned int line,
	handle_t *handle, struct super_block *sb,
	struct buffer_head *bh,
	enum ext4_journal_trigger_type trigger_type);
int __ext4_handle_dirty_metadata(
	const char *where, unsigned int line,
	handle_t *handle, struct inode *inode,
	struct buffer_head *bh);

#define ext4_journal_get_write_access(handle, sb, bh, trigger) 	__ext4_journal_get_write_access( 		__func__, __LINE__, (handle), (sb), (bh), (trigger))
#define ext4_forget(handle, metadata, inode, bh, block) 	__ext4_forget( 		__func__, __LINE__, (handle), (metadata), 		(inode), (bh), (block))
#define ext4_journal_get_create_access(handle, sb, bh, trigger) 	__ext4_journal_get_create_access( 		__func__, __LINE__, (handle), (sb), (bh), (trigger))
#define ext4_handle_dirty_metadata(handle, inode, bh) 	__ext4_handle_dirty_metadata( 		__func__, __LINE__, (handle), (inode), (bh))

#define EXT4_NOJOURNAL_MAX_REF_COUNT 4096UL

handle_t *__ext4_journal_start_sb(
	struct inode *inode, struct super_block *sb,
	unsigned int line, int type, int blocks,
	int reserved_blocks, int revoke_credits);
int __ext4_journal_stop(
	const char *where, unsigned int line,
	handle_t *handle);
handle_t *__ext4_journal_start_reserved(
	handle_t *handle, unsigned int line, int type);
int __ext4_journal_ensure_credits(
	handle_t *handle, int check_credits,
	int extend_credits, int revoke_credits);

static inline bool ext4_handle_valid(handle_t *handle)
{
	return (unsigned long)handle >= EXT4_NOJOURNAL_MAX_REF_COUNT;
}

static inline void ext4_handle_sync(handle_t *handle)
{
	if (ext4_handle_valid(handle))
		handle->h_sync = 1;
}

static inline bool ext4_handle_is_aborted(handle_t *handle)
{
	return ext4_handle_valid(handle) && is_handle_aborted(handle);
}

static inline int ext4_free_metadata_revoke_credits(
	struct super_block *sb, int blocks)
{
	return blocks * EXT4_SB(sb)->s_cluster_ratio;
}

static inline int ext4_trans_default_revoke_credits(
	struct super_block *sb)
{
	return ext4_free_metadata_revoke_credits(sb, 8);
}

static inline handle_t *__ext4_journal_start(
	struct inode *inode, unsigned int line, int type,
	int blocks, int reserved_blocks, int revoke_credits)
{
	return __ext4_journal_start_sb(
		inode, inode->i_sb, line, type,
		blocks, reserved_blocks, revoke_credits);
}

#define ext4_journal_start_sb(sb, type, blocks) 	__ext4_journal_start_sb( 		NULL, (sb), __LINE__, (type), (blocks), 0, 		ext4_trans_default_revoke_credits(sb))
#define ext4_journal_start(inode, type, blocks) 	__ext4_journal_start( 		(inode), __LINE__, (type), (blocks), 0, 		ext4_trans_default_revoke_credits((inode)->i_sb))
#define ext4_journal_start_with_reserve(inode, type, blocks, reserved) 	__ext4_journal_start( 		(inode), __LINE__, (type), (blocks), (reserved), 		ext4_trans_default_revoke_credits((inode)->i_sb))
#define ext4_journal_start_with_revoke(inode, type, blocks, revoke) 	__ext4_journal_start( 		(inode), __LINE__, (type), (blocks), 0, (revoke))
#define ext4_journal_stop(handle) 	__ext4_journal_stop(__func__, __LINE__, (handle))
#define ext4_journal_start_reserved(handle, type) 	__ext4_journal_start_reserved((handle), __LINE__, (type))

static inline handle_t *ext4_journal_current_handle(void)
{
	return journal_current_handle();
}

static inline int ext4_journal_extend(
	handle_t *handle, int blocks, int revoke)
{
	return ext4_handle_valid(handle) ?
		jbd2_journal_extend(handle, blocks, revoke) : 0;
}

static inline int ext4_journal_restart(
	handle_t *handle, int blocks, int revoke)
{
	return ext4_handle_valid(handle) ?
		jbd2__journal_restart(handle, blocks, revoke, GFP_NOFS) : 0;
}

#define ext4_journal_ensure_credits_fn( 	handle, check_credits, extend_credits, revoke_credits, restart_fn) ({ 	int __err = __ext4_journal_ensure_credits( 		(handle), (check_credits), 		(extend_credits), (revoke_credits)); 	if (__err > 0) { 		__err = (restart_fn); 		if (__err >= 0) { 			__err = ext4_journal_restart( 				(handle), (extend_credits), 				(revoke_credits)); 			if (__err == 0) 				__err = 1; 		} 	} 	__err; })

static inline int ext4_journal_ensure_credits(
	handle_t *handle, int credits, int revoke_credits)
{
	return ext4_journal_ensure_credits_fn(
		handle, credits, credits, revoke_credits, 0);
}

static inline int ext4_journal_blocks_per_page(struct inode *inode)
{
	return EXT4_JOURNAL(inode) ?
		jbd2_journal_blocks_per_page(inode) : 0;
}

static inline int ext4_journal_force_commit(journal_t *journal)
{
	return journal ? jbd2_journal_force_commit(journal) : 0;
}

static inline int ext4_jbd2_inode_add_write(
	handle_t *handle, struct inode *inode,
	loff_t start_byte, loff_t length)
{
	return ext4_handle_valid(handle) ?
		jbd2_journal_inode_ranged_write(
			handle, EXT4_I(inode)->jinode,
			start_byte, length) : 0;
}

static inline int ext4_jbd2_inode_add_wait(
	handle_t *handle, struct inode *inode,
	loff_t start_byte, loff_t length)
{
	return ext4_handle_valid(handle) ?
		jbd2_journal_inode_ranged_wait(
			handle, EXT4_I(inode)->jinode,
			start_byte, length) : 0;
}

static inline void ext4_update_inode_fsync_trans(
	handle_t *handle, struct inode *inode, int datasync)
{
	struct ext4_inode_info *ei = EXT4_I(inode);

	if (!ext4_handle_valid(handle) || is_handle_aborted(handle))
		return;

	ei->i_sync_tid = handle->h_transaction->t_tid;
	if (datasync)
		ei->i_datasync_tid = handle->h_transaction->t_tid;
}

int ext4_force_commit(struct super_block *sb);

#define EXT4_INODE_JOURNAL_DATA_MODE 0x01
#define EXT4_INODE_ORDERED_DATA_MODE 0x02
#define EXT4_INODE_WRITEBACK_DATA_MODE 0x04

int ext4_inode_journal_mode(struct inode *inode);

static inline bool ext4_should_journal_data(struct inode *inode)
{
	return ext4_inode_journal_mode(inode) &
	       EXT4_INODE_JOURNAL_DATA_MODE;
}

static inline bool ext4_should_order_data(struct inode *inode)
{
	return ext4_inode_journal_mode(inode) &
	       EXT4_INODE_ORDERED_DATA_MODE;
}

static inline bool ext4_should_writeback_data(struct inode *inode)
{
	return ext4_inode_journal_mode(inode) &
	       EXT4_INODE_WRITEBACK_DATA_MODE;
}

static inline int ext4_free_data_revoke_credits(
	struct inode *inode, int blocks)
{
	if (test_opt(inode->i_sb, DATA_FLAGS) ==
	    EXT4_MOUNT_JOURNAL_DATA)
		return 0;
	if (!ext4_should_journal_data(inode))
		return 0;

	return blocks +
	       2 * (EXT4_SB(inode->i_sb)->s_cluster_ratio - 1);
}

static inline bool ext4_should_dioread_nolock(struct inode *inode)
{
	return test_opt(inode->i_sb, DIOREAD_NOLOCK) &&
	       S_ISREG(inode->i_mode) &&
	       ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS) &&
	       !ext4_should_journal_data(inode) &&
	       test_opt(inode->i_sb, DELALLOC);
}

static inline int ext4_journal_destroy(
	struct ext4_sb_info *sbi, journal_t *journal)
{
	int err;

	ext4_set_mount_flag(sbi->s_sb, EXT4_MF_JOURNAL_DESTROY);
	ext4_force_commit(sbi->s_sb);
	flush_work(&sbi->s_sb_upd_work);

	err = jbd2_journal_destroy(journal);
	sbi->s_journal = NULL;
	return err;
}

#endif
