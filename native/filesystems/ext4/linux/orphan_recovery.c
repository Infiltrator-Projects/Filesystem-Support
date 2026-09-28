/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Infiltrator Filesystem Support — EXT4 orphan recovery.
 *
 * This implementation is organized around the two on-disk EXT4 orphan
 * representations: the legacy superblock/inode chain and the COMPAT_ORPHAN_FILE
 * block array.  The Linux adapter owns transaction integration; the media
 * format rules are kept explicit here so recovery remains independently
 * testable.
 */

#include <linux/atomic.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/quotaops.h>
#include <linux/slab.h>

#include "ext4.h"
#include "ext4_jbd2.h"

#define IFS_EXT4_ORPHAN_FILE_MAX_BLOCKS 512U

static struct ext4_orphan_block_tail *
ifs_ext4_orphan_tail(struct super_block *sb, struct buffer_head *bh)
{
	return (struct ext4_orphan_block_tail *)
		(bh->b_data + sb->s_blocksize -
		 sizeof(struct ext4_orphan_block_tail));
}

static __u32 ifs_ext4_orphan_block_checksum(struct super_block *sb,
					     struct buffer_head *bh,
					     const void *entries)
{
	struct ext4_orphan_info *oi = &EXT4_SB(sb)->s_orphan_info;
	__le64 block_number = cpu_to_le64(bh->b_blocknr);
	unsigned int bytes =
		ext4_inodes_per_orphan_block(sb) * sizeof(__le32);
	__u32 checksum;

	checksum = ext4_chksum(EXT4_SB(sb), oi->of_csum_seed,
			      (const __u8 *)&block_number,
			      sizeof(block_number));
	return ext4_chksum(EXT4_SB(sb), checksum,
			  (const __u8 *)entries, bytes);
}

static bool ifs_ext4_orphan_block_valid(struct super_block *sb,
					struct buffer_head *bh)
{
	struct ext4_orphan_block_tail *tail = ifs_ext4_orphan_tail(sb, bh);

	if (le32_to_cpu(tail->ob_magic) != EXT4_ORPHAN_BLOCK_MAGIC)
		return false;
	if (!ext4_has_metadata_csum(sb))
		return true;

	return le32_to_cpu(tail->ob_checksum) ==
	       ifs_ext4_orphan_block_checksum(sb, bh, bh->b_data);
}

static int ifs_ext4_orphan_file_claim(handle_t *handle, struct inode *inode)
{
	struct ext4_orphan_info *oi = &EXT4_SB(inode->i_sb)->s_orphan_info;
	unsigned int entries = ext4_inodes_per_orphan_block(inode->i_sb);
	unsigned int first_block;
	unsigned int block_step;

	if (!oi->of_binfo || oi->of_blocks <= 0 || !entries)
		return -ENOSPC;

	first_block = (unsigned int)(inode->i_ino % (unsigned long)oi->of_blocks);

	for (block_step = 0; block_step < (unsigned int)oi->of_blocks;
	     ++block_step) {
		unsigned int block =
			(first_block + block_step) % (unsigned int)oi->of_blocks;
		struct ext4_orphan_block *info = &oi->of_binfo[block];
		struct buffer_head *bh = info->ob_bh;
		__le32 *slots;
		unsigned int first_slot;
		unsigned int slot_step;
		int err;

		if (atomic_dec_if_positive(&info->ob_free_entries) < 0)
			continue;

		err = ext4_journal_get_write_access(handle, inode->i_sb, bh,
						   EXT4_JTR_ORPHAN_FILE);
		if (err) {
			atomic_inc(&info->ob_free_entries);
			return err;
		}

		slots = (__le32 *)bh->b_data;
		first_slot =
			(unsigned int)((inode->i_ino + block_step) % entries);

		for (slot_step = 0; slot_step < entries; ++slot_step) {
			unsigned int slot = (first_slot + slot_step) % entries;
			__le32 value = cpu_to_le32(inode->i_ino);

			if (cmpxchg(&slots[slot], cpu_to_le32(0), value) !=
			    cpu_to_le32(0))
				continue;

			EXT4_I(inode)->i_orphan_idx = block * entries + slot;
			ext4_set_inode_state(inode, EXT4_STATE_ORPHAN_FILE);

			err = ext4_handle_dirty_metadata(handle, NULL, bh);
			if (!err)
				return 0;

			cmpxchg(&slots[slot], value, cpu_to_le32(0));
			ext4_clear_inode_state(inode, EXT4_STATE_ORPHAN_FILE);
			INIT_LIST_HEAD(&EXT4_I(inode)->i_orphan);
			atomic_inc(&info->ob_free_entries);
			return err;
		}

		/*
		 * The free counter is an admission hint.  A concurrent claimant
		 * may consume the observed slot before our compare/exchange.
		 */
		atomic_inc(&info->ob_free_entries);
	}

	return -ENOSPC;
}

static int ifs_ext4_legacy_orphan_add(handle_t *handle, struct inode *inode)
{
	struct super_block *sb = inode->i_sb;
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_iloc iloc;
	__u32 inode_count = le32_to_cpu(sbi->s_es->s_inodes_count);
	bool changed_link = false;
	int err;
	int inode_err;

	err = ext4_journal_get_write_access(handle, sb, sbi->s_sbh,
					    EXT4_JTR_NONE);
	if (err)
		return err;

	err = ext4_reserve_inode_write(handle, inode, &iloc);
	if (err)
		return err;

	mutex_lock(&sbi->s_orphan_lock);

	/*
	 * i_dtime is the legacy next-orphan pointer while the inode is on the
	 * orphan chain.  Values outside the inode-number domain are stale
	 * deletion-time data and are replaced when the inode is enrolled.
	 */
	if (!NEXT_ORPHAN(inode) || NEXT_ORPHAN(inode) > inode_count) {
		NEXT_ORPHAN(inode) = le32_to_cpu(sbi->s_es->s_last_orphan);

		lock_buffer(sbi->s_sbh);
		sbi->s_es->s_last_orphan = cpu_to_le32(inode->i_ino);
		ext4_superblock_csum_set(sb);
		unlock_buffer(sbi->s_sbh);
		changed_link = true;
	}

	list_add(&EXT4_I(inode)->i_orphan, &sbi->s_orphan);
	mutex_unlock(&sbi->s_orphan_lock);

	if (!changed_link) {
		brelse(iloc.bh);
		return 0;
	}

	err = ext4_handle_dirty_metadata(handle, NULL, sbi->s_sbh);
	inode_err = ext4_mark_iloc_dirty(handle, inode, &iloc);
	if (!err)
		err = inode_err;

	if (err) {
		mutex_lock(&sbi->s_orphan_lock);
		if (!list_empty(&EXT4_I(inode)->i_orphan))
			list_del_init(&EXT4_I(inode)->i_orphan);
		mutex_unlock(&sbi->s_orphan_lock);
	}

	return err;
}

int ext4_orphan_add(handle_t *handle, struct inode *inode)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	int err;

	if (!sbi->s_journal || is_bad_inode(inode))
		return 0;

	WARN_ON_ONCE(!(inode->i_state & (I_NEW | I_FREEING)) &&
		     !inode_is_locked(inode));

	if (ext4_inode_orphan_tracked(inode))
		return 0;

	ASSERT(S_ISREG(inode->i_mode) || S_ISDIR(inode->i_mode) ||
	       S_ISLNK(inode->i_mode) || inode->i_nlink == 0);

	if (sbi->s_orphan_info.of_blocks) {
		err = ifs_ext4_orphan_file_claim(handle, inode);
		if (err != -ENOSPC) {
			ext4_std_error(inode->i_sb, err);
			return err;
		}
	}

	err = ifs_ext4_legacy_orphan_add(handle, inode);
	ext4_std_error(inode->i_sb, err);
	return err;
}

static int ifs_ext4_orphan_file_release(handle_t *handle, struct inode *inode)
{
	struct ext4_orphan_info *oi = &EXT4_SB(inode->i_sb)->s_orphan_info;
	unsigned int entries = ext4_inodes_per_orphan_block(inode->i_sb);
	unsigned int index = EXT4_I(inode)->i_orphan_idx;
	unsigned int block;
	unsigned int slot;
	int err = 0;

	if (!entries || !oi->of_binfo || oi->of_blocks <= 0)
		goto finish;

	block = index / entries;
	slot = index % entries;

	if (block >= (unsigned int)oi->of_blocks) {
		WARN_ON_ONCE(1);
		err = -EFSCORRUPTED;
		goto finish;
	}

	if (!handle)
		goto finish;

	err = ext4_journal_get_write_access(handle, inode->i_sb,
					    oi->of_binfo[block].ob_bh,
					    EXT4_JTR_ORPHAN_FILE);
	if (!err) {
		__le32 *slots =
			(__le32 *)oi->of_binfo[block].ob_bh->b_data;
		__le32 expected = cpu_to_le32(inode->i_ino);

		if (cmpxchg(&slots[slot], expected, cpu_to_le32(0)) !=
		    expected) {
			err = -EFSCORRUPTED;
		} else {
			atomic_inc(&oi->of_binfo[block].ob_free_entries);
			err = ext4_handle_dirty_metadata(
				handle, NULL, oi->of_binfo[block].ob_bh);
		}
	}

finish:
	ext4_clear_inode_state(inode, EXT4_STATE_ORPHAN_FILE);
	INIT_LIST_HEAD(&EXT4_I(inode)->i_orphan);
	return err;
}

static int ifs_ext4_legacy_orphan_del(handle_t *handle, struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_iloc inode_loc;
	struct list_head *previous;
	__u32 next;
	int err = 0;

	if (list_empty(&ei->i_orphan))
		return 0;

	if (handle) {
		err = ext4_reserve_inode_write(handle, inode, &inode_loc);
		if (err)
			handle = NULL;
	}

	mutex_lock(&sbi->s_orphan_lock);
	previous = ei->i_orphan.prev;
	list_del_init(&ei->i_orphan);

	if (!handle) {
		mutex_unlock(&sbi->s_orphan_lock);
		return err;
	}

	next = NEXT_ORPHAN(inode);

	if (previous == &sbi->s_orphan) {
		err = ext4_journal_get_write_access(handle, inode->i_sb,
						    sbi->s_sbh,
						    EXT4_JTR_NONE);
		if (!err) {
			lock_buffer(sbi->s_sbh);
			sbi->s_es->s_last_orphan = cpu_to_le32(next);
			ext4_superblock_csum_set(inode->i_sb);
			unlock_buffer(sbi->s_sbh);
		}
		mutex_unlock(&sbi->s_orphan_lock);

		if (!err)
			err = ext4_handle_dirty_metadata(handle, NULL,
							 sbi->s_sbh);
	} else {
		struct ext4_inode_info *previous_info =
			list_entry(previous, struct ext4_inode_info, i_orphan);
		struct inode *previous_inode = &previous_info->vfs_inode;
		struct ext4_iloc previous_loc;

		err = ext4_reserve_inode_write(handle, previous_inode,
					       &previous_loc);
		if (!err) {
			NEXT_ORPHAN(previous_inode) = next;
			err = ext4_mark_iloc_dirty(handle, previous_inode,
						  &previous_loc);
		}
		mutex_unlock(&sbi->s_orphan_lock);
	}

	if (!err) {
		NEXT_ORPHAN(inode) = 0;
		err = ext4_mark_iloc_dirty(handle, inode, &inode_loc);
	} else {
		brelse(inode_loc.bh);
	}

	return err;
}

int ext4_orphan_del(handle_t *handle, struct inode *inode)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	int err;

	if (!sbi->s_journal && !(sbi->s_mount_state & EXT4_ORPHAN_FS))
		return 0;

	WARN_ON_ONCE(!(inode->i_state & (I_NEW | I_FREEING)) &&
		     !inode_is_locked(inode));

	if (ext4_test_inode_state(inode, EXT4_STATE_ORPHAN_FILE))
		err = ifs_ext4_orphan_file_release(handle, inode);
	else
		err = ifs_ext4_legacy_orphan_del(handle, inode);

	ext4_std_error(inode->i_sb, err);
	return err;
}

#ifdef CONFIG_QUOTA
static bool ifs_ext4_enable_cleanup_quotas(struct super_block *sb,
					   bool temporarily_writable)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	bool enabled = false;
	int type;

	if (ext4_has_feature_quota(sb) && temporarily_writable) {
		int err = ext4_enable_quotas(sb);

		if (!err)
			enabled = true;
		else
			ext4_msg(sb, KERN_ERR,
				 "Cannot enable quota metadata for orphan cleanup: %d",
				 err);
	}

	for (type = 0; type < EXT4_MAXQUOTAS; ++type) {
		char *name;

		name = rcu_dereference_protected(
			sbi->s_qf_names[type],
			lockdep_is_held(&sb->s_umount));
		if (name) {
			int err = dquot_quota_on_mount(
				sb, name, sbi->s_jquota_fmt, type);

			if (!err)
				enabled = true;
			else
				ext4_msg(sb, KERN_ERR,
					 "Cannot enable journaled quota type %d: %d",
					 type, err);
		}
	}

	return enabled;
}

static void ifs_ext4_disable_cleanup_quotas(struct super_block *sb,
					    bool enabled)
{
	int type;

	if (!enabled)
		return;

	for (type = 0; type < EXT4_MAXQUOTAS; ++type)
		if (sb_dqopt(sb)->files[type])
			dquot_quota_off(sb, type);
}
#endif

static void ifs_ext4_finish_orphan(struct inode *inode,
				   int *truncates, int *unlinked)
{
	struct super_block *sb = inode->i_sb;

	dquot_initialize(inode);

	if (inode->i_nlink) {
		int err;

		inode_lock(inode);
		truncate_inode_pages(inode->i_mapping, inode->i_size);
		err = ext4_truncate(inode);
		if (err) {
			ext4_orphan_del(NULL, inode);
			ext4_std_error(sb, err);
		}
		inode_unlock(inode);
		++*truncates;
	} else {
		++*unlinked;
	}

	iput(inode);
}

void ext4_orphan_cleanup(struct super_block *sb, struct ext4_super_block *es)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_orphan_info *oi = &sbi->s_orphan_info;
	unsigned int saved_flags = sb->s_flags;
	bool was_readonly = (saved_flags & SB_RDONLY) != 0;
	int entries = ext4_inodes_per_orphan_block(sb);
	int truncates = 0;
	int unlinked = 0;
	int block;

#ifdef CONFIG_QUOTA
	bool cleanup_quotas = false;
#endif

	if (!es->s_last_orphan && !oi->of_blocks)
		return;

	if (bdev_read_only(sb->s_bdev)) {
		ext4_msg(sb, KERN_ERR,
			 "write access unavailable; orphan cleanup skipped");
		return;
	}

	if (!ext4_feature_set_ok(sb, 0)) {
		ext4_msg(sb, KERN_INFO,
			 "orphan cleanup skipped because of unsupported ro-compat features");
		return;
	}

	if (sbi->s_mount_state & EXT4_ERROR_FS) {
		if (es->s_last_orphan && !was_readonly)
			es->s_last_orphan = 0;
		return;
	}

	if (was_readonly)
		sb->s_flags &= ~SB_RDONLY;

#ifdef CONFIG_QUOTA
	cleanup_quotas = ifs_ext4_enable_cleanup_quotas(sb, was_readonly);
#endif

	while (es->s_last_orphan) {
		struct inode *inode;

		if (sbi->s_mount_state & EXT4_ERROR_FS) {
			es->s_last_orphan = 0;
			break;
		}

		inode = ext4_orphan_get(
			sb, le32_to_cpu(es->s_last_orphan));
		if (IS_ERR(inode)) {
			es->s_last_orphan = 0;
			break;
		}

		list_add(&EXT4_I(inode)->i_orphan, &sbi->s_orphan);
		ifs_ext4_finish_orphan(inode, &truncates, &unlinked);
	}

	for (block = 0; block < oi->of_blocks; ++block) {
		__le32 *slots = (__le32 *)oi->of_binfo[block].ob_bh->b_data;
		int slot;

		for (slot = 0; slot < entries; ++slot) {
			unsigned long ino;
			struct inode *inode;

			if (!slots[slot])
				continue;

			ino = le32_to_cpu(slots[slot]);
			inode = ext4_orphan_get(sb, ino);
			if (IS_ERR(inode))
				continue;

			EXT4_I(inode)->i_orphan_idx = block * entries + slot;
			ext4_set_inode_state(inode, EXT4_STATE_ORPHAN_FILE);
			ifs_ext4_finish_orphan(inode, &truncates, &unlinked);
		}
	}

	if (unlinked)
		ext4_msg(sb, KERN_INFO, "%d orphan inode%s deleted",
			 unlinked, unlinked == 1 ? "" : "s");
	if (truncates)
		ext4_msg(sb, KERN_INFO, "%d truncate%s recovered",
			 truncates, truncates == 1 ? "" : "s");

#ifdef CONFIG_QUOTA
	ifs_ext4_disable_cleanup_quotas(sb, cleanup_quotas);
#endif

	sb->s_flags = saved_flags;
}

void ext4_release_orphan_info(struct super_block *sb)
{
	struct ext4_orphan_info *oi = &EXT4_SB(sb)->s_orphan_info;
	int block;

	if (oi->of_binfo) {
		for (block = 0; block < oi->of_blocks; ++block)
			if (oi->of_binfo[block].ob_bh)
				brelse(oi->of_binfo[block].ob_bh);
		kvfree(oi->of_binfo);
	}

	oi->of_binfo = NULL;
	oi->of_blocks = 0;
	oi->of_csum_seed = 0;
}

void ext4_orphan_file_block_trigger(struct jbd2_buffer_trigger_type *triggers,
				    struct buffer_head *bh,
				    void *data, size_t size)
{
	struct super_block *sb = EXT4_TRIGGER(triggers)->sb;
	struct ext4_orphan_block_tail *tail;

	(void)size;
	tail = ifs_ext4_orphan_tail(sb, bh);
	tail->ob_checksum =
		cpu_to_le32(ifs_ext4_orphan_block_checksum(sb, bh, data));
}

int ext4_init_orphan_info(struct super_block *sb)
{
	struct ext4_sb_info *sbi = EXT4_SB(sb);
	struct ext4_orphan_info *oi = &sbi->s_orphan_info;
	ino_t orphan_ino = le32_to_cpu(sbi->s_es->s_orphan_file_inum);
	struct inode *inode;
	loff_t size;
	unsigned int entries;
	unsigned int blocks;
	unsigned int loaded = 0;
	int err = 0;

	if (!ext4_has_feature_orphan_file(sb))
		return 0;

	inode = ext4_iget(sb, orphan_ino, EXT4_IGET_SPECIAL);
	if (IS_ERR(inode)) {
		ext4_msg(sb, KERN_ERR,
			 "cannot load orphan-file inode %lu",
			 (unsigned long)orphan_ino);
		return PTR_ERR(inode);
	}

	size = i_size_read(inode);
	if (size < sb->s_blocksize ||
	    size > ((loff_t)IFS_EXT4_ORPHAN_FILE_MAX_BLOCKS <<
		    inode->i_blkbits) ||
	    (size & (sb->s_blocksize - 1))) {
		ext4_error(sb, "invalid orphan-file size %lld",
			   (long long)size);
		err = -EFSCORRUPTED;
		goto out_inode;
	}

	blocks = (unsigned int)(size >> sb->s_blocksize_bits);
	entries = ext4_inodes_per_orphan_block(sb);
	if (!blocks || !entries) {
		err = -EFSCORRUPTED;
		goto out_inode;
	}

	oi->of_binfo = kvmalloc_array(blocks, sizeof(*oi->of_binfo),
				     GFP_KERNEL | __GFP_ZERO);
	if (!oi->of_binfo) {
		err = -ENOMEM;
		goto out_inode;
	}

	oi->of_blocks = blocks;
	oi->of_csum_seed = EXT4_I(inode)->i_csum_seed;

	for (loaded = 0; loaded < blocks; ++loaded) {
		struct buffer_head *bh;
		__le32 *slots;
		unsigned int slot;
		int free_slots = 0;

		bh = ext4_bread(NULL, inode, loaded, 0);
		if (IS_ERR(bh)) {
			err = PTR_ERR(bh);
			break;
		}
		if (!bh) {
			err = -EIO;
			break;
		}

		oi->of_binfo[loaded].ob_bh = bh;
		if (!ifs_ext4_orphan_block_valid(sb, bh)) {
			ext4_error(sb,
				   "orphan-file block %u failed format/checksum validation",
				   loaded);
			err = -EFSBADCRC;
			break;
		}

		slots = (__le32 *)bh->b_data;
		for (slot = 0; slot < entries; ++slot)
			if (!slots[slot])
				++free_slots;

		atomic_set(&oi->of_binfo[loaded].ob_free_entries,
			   free_slots);
	}

	if (err) {
		unsigned int release = loaded;

		if (loaded < blocks && oi->of_binfo[loaded].ob_bh)
			++release;
		while (release) {
			--release;
			if (oi->of_binfo[release].ob_bh)
				brelse(oi->of_binfo[release].ob_bh);
		}
		kvfree(oi->of_binfo);
		oi->of_binfo = NULL;
		oi->of_blocks = 0;
		oi->of_csum_seed = 0;
	}

out_inode:
	iput(inode);
	return err;
}

int ext4_orphan_file_empty(struct super_block *sb)
{
	struct ext4_orphan_info *oi = &EXT4_SB(sb)->s_orphan_info;
	int entries = ext4_inodes_per_orphan_block(sb);
	int block;

	if (!ext4_has_feature_orphan_file(sb))
		return 1;
	if (!oi->of_binfo || oi->of_blocks <= 0)
		return 1;

	for (block = 0; block < oi->of_blocks; ++block)
		if (atomic_read(&oi->of_binfo[block].ob_free_entries) !=
		    entries)
			return 0;

	return 1;
}
