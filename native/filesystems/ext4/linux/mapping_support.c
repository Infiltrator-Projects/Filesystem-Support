/* Infiltrator Filesystem Support — EXT4 Linux mapping support.
 * Legacy indirect mapping, extent migration/movement and read-side page I/O
 * are grouped as host integration around canonical EXT4 mapping semantics.
 */

#include "ext4_jbd2.h"
#include "truncate.h"

#include <linux/dax.h>
#include <linux/uio.h>
#include <trace/events/ext4.h>

struct ext4_indirect_cursor {
	__le32 *slot;
	__le32 value;
	struct buffer_head *bh;
};

static void ext4_indirect_set_cursor(struct ext4_indirect_cursor *cursor,
				     struct buffer_head *bh,
				     __le32 *slot)
{
	cursor->slot = slot;
	cursor->value = *slot;
	cursor->bh = bh;
}

static int ext4_indirect_path(struct inode *inode, ext4_lblk_t logical,
			      ext4_lblk_t offsets[4], int *boundary)
{
	ifs_ext4_u32 core_offsets[4] = { 0 };
	ifs_ext4_u32 core_boundary = 0;
	const ifs_ext4_u32 per_block =
		EXT4_ADDR_PER_BLOCK(inode->i_sb);
	const ifs_ext4_u32 per_block_bits =
		EXT4_ADDR_PER_BLOCK_BITS(inode->i_sb);
	int depth;
	int i;

	depth = ifs_ext4_indirect_block_path(
		(ifs_ext4_u64)logical, per_block, per_block_bits,
		core_offsets, boundary ? &core_boundary : NULL);
	if (!depth) {
		ext4_warning(inode->i_sb,
			     "logical block %u exceeds indirect-map capacity",
			     logical);
		return 0;
	}

	for (i = 0; i < depth; ++i)
		offsets[i] = core_offsets[i];
	if (boundary)
		*boundary = (int)core_boundary;
	return depth;
}

static struct ext4_indirect_cursor *
ext4_indirect_walk(struct inode *inode, int depth,
		   ext4_lblk_t offsets[4],
		   struct ext4_indirect_cursor chain[4],
		   int *err)
{
	struct ext4_indirect_cursor *cursor = chain;
	struct buffer_head *bh;
	ext4_fsblk_t block;

	*err = 0;
	ext4_indirect_set_cursor(
		cursor, NULL, EXT4_I(inode)->i_data + offsets[0]);
	if (!cursor->value)
		return cursor;

	while (--depth) {
		block = le32_to_cpu(cursor->value);
		if (block >= ext4_blocks_count(EXT4_SB(inode->i_sb)->s_es)) {
			*err = -EFSCORRUPTED;
			return cursor;
		}

		bh = sb_getblk(inode->i_sb, block);
		if (!bh) {
			*err = -ENOMEM;
			return cursor;
		}

		if (!bh_uptodate_or_lock(bh)) {
			if (ext4_read_bh(bh, 0, NULL, false) < 0) {
				put_bh(bh);
				*err = -EIO;
				return cursor;
			}
			if (ext4_check_indirect_blockref(inode, bh)) {
				put_bh(bh);
				*err = -EFSCORRUPTED;
				return cursor;
			}
		}

		cursor++;
		ext4_indirect_set_cursor(
			cursor, bh,
			(__le32 *)bh->b_data + offsets[cursor - chain]);
		if (!cursor->value)
			return cursor;
	}

	return NULL;
}

static void ext4_indirect_release_chain(
	struct ext4_indirect_cursor *first,
	struct ext4_indirect_cursor *last)
{
	while (last > first) {
		brelse(last->bh);
		last--;
	}
}

static ext4_fsblk_t
ext4_indirect_allocation_goal(struct inode *inode,
			      struct ext4_indirect_cursor *missing)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	__le32 *start = missing->bh ?
		(__le32 *)missing->bh->b_data : ei->i_data;
	__le32 *slot;

	for (slot = missing->slot; slot > start;) {
		slot--;
		if (*slot)
			return le32_to_cpu(*slot) & EXT4_MAX_BLOCK_FILE_PHYS;
	}

	if (missing->bh)
		return missing->bh->b_blocknr & EXT4_MAX_BLOCK_FILE_PHYS;

	return ext4_inode_to_goal_block(inode) & EXT4_MAX_BLOCK_FILE_PHYS;
}

static unsigned int
ext4_indirect_data_count(struct ext4_indirect_cursor *missing,
			 int metadata_levels,
			 unsigned int requested,
			 int boundary)
{
	unsigned int count;

	if (metadata_levels > 0)
		return min_t(unsigned int, requested, boundary + 1);

	count = 1;
	while (count < requested &&
	       count <= boundary &&
	       le32_to_cpu(*(missing->slot + count)) == 0)
		count++;
	return count;
}

static int ext4_indirect_allocate_branch(
	handle_t *handle,
	struct ext4_allocation_request *request,
	int metadata_levels,
	const ext4_lblk_t *offsets,
	struct ext4_indirect_cursor *branch)
{
	ext4_fsblk_t allocated[4] = { 0 };
	struct buffer_head *bh;
	int created = -1;
	int level;
	int err = 0;

	for (level = 0; level <= metadata_levels; ++level) {
		if (level == metadata_levels) {
			allocated[level] =
				ext4_mb_new_blocks(handle, request, &err);
		} else {
			allocated[level] = ext4_new_meta_blocks(
				handle, request->inode, request->goal,
				request->flags & EXT4_MB_DELALLOC_RESERVED,
				NULL, &err);
			request->goal = allocated[level];
		}
		if (err)
			goto rollback;

		created = level;
		branch[level].value = cpu_to_le32(allocated[level]);

		if (level == 0)
			continue;

		bh = sb_getblk(
			request->inode->i_sb, allocated[level - 1]);
		if (!bh) {
			err = -ENOMEM;
			goto rollback;
		}
		branch[level].bh = bh;

		lock_buffer(bh);
		err = ext4_journal_get_create_access(
			handle, request->inode->i_sb, bh, EXT4_JTR_NONE);
		if (err) {
			unlock_buffer(bh);
			goto rollback;
		}

		memset(bh->b_data, 0, bh->b_size);
		branch[level].slot =
			(__le32 *)bh->b_data + offsets[level];

		if (level == metadata_levels) {
			ext4_fsblk_t block = allocated[level];
			unsigned int i;

			for (i = 0; i < request->len; ++i)
				branch[level].slot[i] =
					cpu_to_le32(block++);
		} else {
			*branch[level].slot =
				cpu_to_le32(allocated[level]);
		}

		set_buffer_uptodate(bh);
		unlock_buffer(bh);

		err = ext4_handle_dirty_metadata(
			handle, request->inode, bh);
		if (err)
			goto rollback;
	}

	return 0;

rollback:
	if (created == metadata_levels && created >= 0) {
		ext4_free_blocks(
			handle, request->inode, NULL,
			allocated[created], request->len, 0);
		created--;
	}

	for (; created >= 0; --created) {
		struct buffer_head *owner_bh =
			branch[created + 1].bh;

		ext4_free_blocks(
			handle, request->inode, owner_bh,
			allocated[created], 1,
			owner_bh ? EXT4_FREE_BLOCKS_FORGET : 0);
	}
	return err;
}

static int ext4_indirect_publish_branch(
	handle_t *handle,
	struct ext4_allocation_request *request,
	struct ext4_indirect_cursor *where,
	int metadata_levels)
{
	int err;
	int i;

	if (where->bh) {
		err = ext4_journal_get_write_access(
			handle, request->inode->i_sb,
			where->bh, EXT4_JTR_NONE);
		if (err)
			goto rollback;
	}

	*where->slot = where->value;
	if (metadata_levels == 0 && request->len > 1) {
		ext4_fsblk_t block = le32_to_cpu(where->value) + 1;

		for (i = 1; i < request->len; ++i)
			where->slot[i] = cpu_to_le32(block++);
	}

	if (where->bh)
		err = ext4_handle_dirty_metadata(
			handle, request->inode, where->bh);
	else
		err = ext4_mark_inode_dirty(handle, request->inode);

	if (!err)
		return 0;

rollback:
	/*
	 * Nothing may remain reachable after a failed publication. Release the
	 * freshly created metadata chain from the leaves upward, then the data
	 * extent allocated at the final cursor.
	 */
	for (i = metadata_levels; i > 0; --i) {
		if (where[i].bh)
			ext4_free_blocks(
				handle, request->inode, where[i].bh,
				where[i].bh->b_blocknr, 1,
				EXT4_FREE_BLOCKS_FORGET |
				EXT4_FREE_BLOCKS_METADATA);
	}

	ext4_free_blocks(
		handle, request->inode, NULL,
		le32_to_cpu(where[metadata_levels].value),
		request->len, 0);
	return err;
}

int ext4_ind_map_blocks(handle_t *handle, struct inode *inode,
			struct ext4_map_blocks *map, int flags)
{
	struct ext4_allocation_request request;
	struct ext4_indirect_cursor chain[4] = { 0 };
	struct ext4_indirect_cursor *missing;
	ext4_lblk_t offsets[4];
	ext4_fsblk_t first;
	int boundary = 0;
	int depth;
	int metadata_levels;
	int err = -EIO;
	u64 count = 0;

	trace_ext4_ind_map_blocks_enter(
		inode, map->m_lblk, map->m_len, flags);

	ASSERT(!ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS));
	ASSERT(handle || !(flags & EXT4_GET_BLOCKS_CREATE));

	depth = ext4_indirect_path(
		inode, map->m_lblk, offsets, &boundary);
	if (!depth)
		goto out;

	missing = ext4_indirect_walk(
		inode, depth, offsets, chain, &err);

	if (!missing) {
		first = le32_to_cpu(chain[depth - 1].value);
		count = 1;

		while (count < map->m_len &&
		       count <= boundary &&
		       le32_to_cpu(*(chain[depth - 1].slot + count)) ==
			       first + count)
			count++;

		map->m_flags |= EXT4_MAP_MAPPED;
		map->m_pblk = first;
		map->m_len = count;
		if (count > boundary)
			map->m_flags |= EXT4_MAP_BOUNDARY;
		err = count;
		missing = chain + depth - 1;
		goto cleanup;
	}

	if (!(flags & EXT4_GET_BLOCKS_CREATE)) {
		const unsigned int per_block =
			inode->i_sb->s_blocksize / sizeof(u32);
		int level;

		for (level = missing - chain + 1;
		     level < depth; ++level)
			count = count * per_block +
				(per_block - offsets[level] - 1);
		count++;

		map->m_pblk = 0;
		map->m_len = min_t(u64, map->m_len, count);
		err = 0;
		goto cleanup;
	}

	if (err == -EIO)
		goto cleanup;

	if (ext4_has_feature_bigalloc(inode->i_sb)) {
		EXT4_ERROR_INODE(
			inode,
			"legacy indirect allocation is invalid with bigalloc");
		err = -EFSCORRUPTED;
		goto cleanup;
	}

	memset(&request, 0, sizeof(request));
	request.inode = inode;
	request.logical = map->m_lblk;
	if (S_ISREG(inode->i_mode))
		request.flags = EXT4_MB_HINT_DATA;
	if (flags & EXT4_GET_BLOCKS_DELALLOC_RESERVE)
		request.flags |= EXT4_MB_DELALLOC_RESERVED;
	if (flags & EXT4_GET_BLOCKS_METADATA_NOFAIL)
		request.flags |= EXT4_MB_USE_RESERVED;

	request.goal = ext4_indirect_allocation_goal(inode, missing);
	metadata_levels = chain + depth - missing - 1;
	request.len = ext4_indirect_data_count(
		missing, metadata_levels, map->m_len, boundary);

	err = ext4_indirect_allocate_branch(
		handle, &request, metadata_levels,
		offsets + (missing - chain), missing);
	if (err)
		goto cleanup;

	err = ext4_indirect_publish_branch(
		handle, &request, missing, metadata_levels);
	if (err)
		goto cleanup;

	ext4_update_inode_fsync_trans(handle, inode, 1);
	map->m_flags |= EXT4_MAP_NEW | EXT4_MAP_MAPPED;
	map->m_pblk = le32_to_cpu(chain[depth - 1].value);
	map->m_len = request.len;
	if (request.len > boundary)
		map->m_flags |= EXT4_MAP_BOUNDARY;
	err = request.len;

cleanup:
	ext4_indirect_release_chain(chain, missing);
out:
	trace_ext4_ind_map_blocks_exit(inode, flags, map, err);
	return err;
}

int ext4_ind_trans_blocks(struct inode *inode, int blocks)
{
	return DIV_ROUND_UP(
		blocks, EXT4_ADDR_PER_BLOCK(inode->i_sb)) + 4;
}

static int ext4_indirect_restart_transaction(
	handle_t *handle, struct inode *inode,
	struct buffer_head *bh, int *dropped)
{
	int err;

	if (bh) {
		err = ext4_handle_dirty_metadata(handle, inode, bh);
		if (err)
			return err;
	}

	err = ext4_mark_inode_dirty(handle, inode);
	if (err)
		return err;

	BUG_ON(!EXT4_JOURNAL(inode));
	ext4_discard_preallocations(inode);
	up_write(&EXT4_I(inode)->i_data_sem);
	*dropped = 1;
	return 0;
}

static int ext4_indirect_ensure_truncate_credits(
	handle_t *handle, struct inode *inode,
	struct buffer_head *bh, int revoke_credits)
{
	int dropped = 0;
	int err;

	err = ext4_journal_ensure_credits_fn(
		handle, EXT4_RESERVE_TRANS_BLOCKS,
		ext4_blocks_for_truncate(inode), revoke_credits,
		ext4_indirect_restart_transaction(
			handle, inode, bh, &dropped));

	if (dropped)
		down_write(&EXT4_I(inode)->i_data_sem);
	if (err <= 0)
		return err;

	if (!bh)
		return 0;

	return ext4_journal_get_write_access(
		handle, inode->i_sb, bh, EXT4_JTR_NONE);
}

static bool ext4_indirect_slots_empty(__le32 *first, __le32 *last)
{
	while (first < last)
		if (*first++)
			return false;
	return true;
}

static struct ext4_indirect_cursor *
ext4_indirect_find_shared(struct inode *inode, int depth,
			  ext4_lblk_t offsets[4],
			  struct ext4_indirect_cursor chain[4],
			  __le32 *top)
{
	struct ext4_indirect_cursor *partial;
	struct ext4_indirect_cursor *cursor;
	int err;
	int walk_depth;

	*top = 0;
	for (walk_depth = depth;
	     walk_depth > 1 && offsets[walk_depth - 1] == 0;
	     --walk_depth)
		;

	partial = ext4_indirect_walk(
		inode, walk_depth, offsets, chain, &err);
	if (!partial)
		partial = chain + walk_depth - 1;

	if (!partial->value && *partial->slot)
		return partial;

	for (cursor = partial;
	     cursor > chain &&
	     ext4_indirect_slots_empty(
		     (__le32 *)cursor->bh->b_data, cursor->slot);
	     --cursor)
		;

	if (cursor == chain + walk_depth - 1 && cursor > chain)
		cursor->slot--;
	else
		*top = *cursor->slot;

	while (partial > cursor) {
		brelse(partial->bh);
		partial--;
	}
	return partial;
}

static int ext4_indirect_free_data_run(
	handle_t *handle, struct inode *inode,
	struct buffer_head *parent,
	ext4_fsblk_t first_block, unsigned long count,
	__le32 *first_slot, __le32 *after_last)
{
	int flags = EXT4_FREE_BLOCKS_VALIDATED;
	__le32 *slot;
	int err;

	if (S_ISDIR(inode->i_mode) ||
	    S_ISLNK(inode->i_mode) ||
	    ext4_test_inode_flag(inode, EXT4_INODE_EA_INODE))
		flags |= EXT4_FREE_BLOCKS_FORGET |
			 EXT4_FREE_BLOCKS_METADATA;
	else if (ext4_should_journal_data(inode))
		flags |= EXT4_FREE_BLOCKS_FORGET;

	if (!ext4_inode_block_valid(inode, first_block, count)) {
		EXT4_ERROR_INODE(
			inode,
			"invalid data run %llu length %lu",
			(unsigned long long)first_block, count);
		return -EFSCORRUPTED;
	}

	err = ext4_indirect_ensure_truncate_credits(
		handle, inode, parent,
		ext4_free_data_revoke_credits(inode, count));
	if (err)
		return err;

	for (slot = first_slot; slot < after_last; ++slot)
		*slot = 0;

	ext4_free_blocks(
		handle, inode, NULL, first_block, count, flags);
	return 0;
}

static int ext4_indirect_free_data(
	handle_t *handle, struct inode *inode,
	struct buffer_head *parent,
	__le32 *first, __le32 *last)
{
	ext4_fsblk_t run_start = 0;
	unsigned long run_length = 0;
	__le32 *run_slot = NULL;
	__le32 *slot;
	int err = 0;

	if (parent) {
		err = ext4_journal_get_write_access(
			handle, inode->i_sb, parent, EXT4_JTR_NONE);
		if (err)
			return err;
	}

	for (slot = first; slot < last; ++slot) {
		const ext4_fsblk_t block = le32_to_cpu(*slot);

		if (!block)
			continue;

		if (!run_length) {
			run_start = block;
			run_slot = slot;
			run_length = 1;
			continue;
		}
		if (block == run_start + run_length) {
			run_length++;
			continue;
		}

		err = ext4_indirect_free_data_run(
			handle, inode, parent,
			run_start, run_length, run_slot, slot);
		if (err)
			return err;

		run_start = block;
		run_slot = slot;
		run_length = 1;
	}

	if (run_length) {
		err = ext4_indirect_free_data_run(
			handle, inode, parent,
			run_start, run_length, run_slot, last);
		if (err)
			return err;
	}

	if (parent) {
		if (!EXT4_JOURNAL(inode) || bh2jh(parent))
			err = ext4_handle_dirty_metadata(
				handle, inode, parent);
		else {
			EXT4_ERROR_INODE(
				inode,
				"circular indirect block at %llu",
				(unsigned long long)parent->b_blocknr);
			err = -EFSCORRUPTED;
		}
	}

	return err;
}

static int ext4_indirect_free_branches(
	handle_t *handle, struct inode *inode,
	struct buffer_head *parent,
	__le32 *first, __le32 *last, int depth)
{
	const int per_block = EXT4_ADDR_PER_BLOCK(inode->i_sb);
	__le32 *slot;
	int err = 0;

	if (ext4_handle_is_aborted(handle))
		return -EROFS;

	if (depth == 0)
		return ext4_indirect_free_data(
			handle, inode, parent, first, last);

	for (slot = last; slot > first;) {
		struct buffer_head *bh;
		ext4_fsblk_t block;

		slot--;
		block = le32_to_cpu(*slot);
		if (!block)
			continue;
		if (!ext4_inode_block_valid(inode, block, 1))
			return -EFSCORRUPTED;

		bh = ext4_sb_bread_nofail(inode->i_sb, block);
		if (IS_ERR(bh))
			return PTR_ERR(bh);

		err = ext4_indirect_free_branches(
			handle, inode, bh,
			(__le32 *)bh->b_data,
			(__le32 *)bh->b_data + per_block,
			depth - 1);
		brelse(bh);
		if (err)
			return err;

		err = ext4_indirect_ensure_truncate_credits(
			handle, inode, NULL,
			ext4_free_metadata_revoke_credits(
				inode->i_sb, 1));
		if (err)
			return err;

		ext4_free_blocks(
			handle, inode, NULL, block, 1,
			EXT4_FREE_BLOCKS_METADATA |
			EXT4_FREE_BLOCKS_FORGET);

		if (parent) {
			err = ext4_journal_get_write_access(
				handle, inode->i_sb,
				parent, EXT4_JTR_NONE);
			if (err)
				return err;
			*slot = 0;
			err = ext4_handle_dirty_metadata(
				handle, inode, parent);
			if (err)
				return err;
		}
	}

	return 0;
}

void ext4_ind_truncate(handle_t *handle, struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	__le32 *data = ei->i_data;
	const int per_block = EXT4_ADDR_PER_BLOCK(inode->i_sb);
	const unsigned int block_size = inode->i_sb->s_blocksize;
	ext4_lblk_t offsets[4] = { 0 };
	struct ext4_indirect_cursor chain[4] = { 0 };
	struct ext4_indirect_cursor *partial;
	ext4_lblk_t last;
	ext4_lblk_t maximum;
	__le32 top = 0;
	int depth = 0;

	last = (inode->i_size + block_size - 1) >>
	       EXT4_BLOCK_SIZE_BITS(inode->i_sb);
	maximum = (EXT4_SB(inode->i_sb)->s_bitmap_maxbytes +
		   block_size - 1) >>
		  EXT4_BLOCK_SIZE_BITS(inode->i_sb);

	if (last != maximum) {
		depth = ext4_indirect_path(
			inode, last, offsets, NULL);
		if (!depth)
			return;
	}

	ext4_es_remove_extent(
		inode, last, EXT_MAX_BLOCKS - last);
	ei->i_disksize = inode->i_size;

	if (last == maximum)
		return;

	if (depth == 1) {
		ext4_indirect_free_data(
			handle, inode, NULL,
			data + offsets[0],
			data + EXT4_NDIR_BLOCKS);
		goto free_higher_levels;
	}

	partial = ext4_indirect_find_shared(
		inode, depth, offsets, chain, &top);

	if (top) {
		if (partial == chain) {
			ext4_indirect_free_branches(
				handle, inode, NULL,
				&top, &top + 1,
				(chain + depth - 1) - partial);
			*partial->slot = 0;
		} else {
			ext4_indirect_free_branches(
				handle, inode, partial->bh,
				partial->slot, partial->slot + 1,
				(chain + depth - 1) - partial);
		}
	}

	while (partial > chain) {
		ext4_indirect_free_branches(
			handle, inode, partial->bh,
			partial->slot + 1,
			(__le32 *)partial->bh->b_data + per_block,
			(chain + depth - 1) - partial);
		brelse(partial->bh);
		partial--;
	}

free_higher_levels:
	switch (offsets[0]) {
	default:
		top = data[EXT4_IND_BLOCK];
		if (top) {
			ext4_indirect_free_branches(
				handle, inode, NULL, &top, &top + 1, 1);
			data[EXT4_IND_BLOCK] = 0;
		}
		fallthrough;
	case EXT4_IND_BLOCK:
		top = data[EXT4_DIND_BLOCK];
		if (top) {
			ext4_indirect_free_branches(
				handle, inode, NULL, &top, &top + 1, 2);
			data[EXT4_DIND_BLOCK] = 0;
		}
		fallthrough;
	case EXT4_DIND_BLOCK:
		top = data[EXT4_TIND_BLOCK];
		if (top) {
			ext4_indirect_free_branches(
				handle, inode, NULL, &top, &top + 1, 3);
			data[EXT4_TIND_BLOCK] = 0;
		}
		fallthrough;
	case EXT4_TIND_BLOCK:
		break;
	}
}

int ext4_ind_remove_space(handle_t *handle, struct inode *inode,
			  ext4_lblk_t start, ext4_lblk_t end)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	__le32 *data = ei->i_data;
	const int per_block = EXT4_ADDR_PER_BLOCK(inode->i_sb);
	const unsigned int block_size = inode->i_sb->s_blocksize;
	ext4_lblk_t offsets[4] = { 0 };
	ext4_lblk_t end_offsets[4] = { 0 };
	struct ext4_indirect_cursor left_chain[4] = { 0 };
	struct ext4_indirect_cursor right_chain[4] = { 0 };
	struct ext4_indirect_cursor *left = NULL;
	struct ext4_indirect_cursor *right = NULL;
	struct ext4_indirect_cursor *left_release = NULL;
	struct ext4_indirect_cursor *right_release = NULL;
	ext4_lblk_t maximum;
	__le32 left_top = 0;
	__le32 right_top = 0;
	int left_depth;
	int right_depth;
	int err = 0;

	maximum = (EXT4_SB(inode->i_sb)->s_bitmap_maxbytes +
		   block_size - 1) >>
		  EXT4_BLOCK_SIZE_BITS(inode->i_sb);
	if (end > maximum)
		end = maximum;
	if (start >= end || start > maximum)
		return 0;

	left_depth = ext4_indirect_path(
		inode, start, offsets, NULL);
	right_depth = ext4_indirect_path(
		inode, end, end_offsets, NULL);
	if (!left_depth || !right_depth || left_depth > right_depth)
		return -EFSCORRUPTED;

	if (left_depth == 1 && right_depth == 1)
		return ext4_indirect_free_data(
			handle, inode, NULL,
			data + offsets[0],
			data + end_offsets[0]);

	left = ext4_indirect_find_shared(
		inode, left_depth, offsets,
		left_chain, &left_top);
	left_release = left;

	right = ext4_indirect_find_shared(
		inode, right_depth, end_offsets,
		right_chain, &right_top);
	right_release = right;

	/*
	 * Free complete branch tails on the left and complete branch prefixes
	 * on the right. If both cursors meet in the same indirect block, free
	 * exactly the interval between them once.
	 */
	while (left > left_chain || right > right_chain) {
		const int left_level =
			(left_chain + left_depth - 1) - left;
		const int right_level =
			(right_chain + right_depth - 1) - right;

		if (left > left_chain &&
		    right > right_chain &&
		    left->bh->b_blocknr == right->bh->b_blocknr) {
			err = ext4_indirect_free_branches(
				handle, inode, left->bh,
				left->slot + 1, right->slot,
				left_level);
			break;
		}

		if (left > left_chain && left_level <= right_level) {
			err = ext4_indirect_free_branches(
				handle, inode, left->bh,
				left->slot + 1,
				(__le32 *)left->bh->b_data + per_block,
				left_level);
			if (err)
				break;
			left--;
		}

		if (right > right_chain && right_level <= left_level) {
			err = ext4_indirect_free_branches(
				handle, inode, right->bh,
				(__le32 *)right->bh->b_data,
				right->slot,
				right_level);
			if (err)
				break;
			right--;
		}
	}

	ext4_indirect_release_chain(left_chain, left_release);
	ext4_indirect_release_chain(right_chain, right_release);
	return err;
}


/* ===== extent migration ===== */
#include <linux/slab.h>

#include "ext4_extents.h"
#include "ext4_jbd2.h"

struct ext4_migrate_run {
	ext4_lblk_t logical_start;
	ext4_lblk_t logical_last;
	ext4_lblk_t logical_cursor;
	ext4_fsblk_t physical_start;
	ext4_fsblk_t physical_last;
};

static int ext4_migrate_flush_run(handle_t *handle, struct inode *inode,
				  struct ext4_migrate_run *run)
{
	struct ext4_extent extent;
	struct ext4_ext_path *path;
	unsigned int length;
	int credits;
	int err = 0;

	if (!run->physical_start)
		return 0;

	length = run->logical_last - run->logical_start + 1;
	memset(&extent, 0, sizeof(extent));
	extent.ee_block = cpu_to_le32(run->logical_start);
	extent.ee_len = cpu_to_le16(length);
	ext4_ext_store_pblock(&extent, run->physical_start);

	down_write(&EXT4_I(inode)->i_data_sem);
	path = ext4_find_extent(inode, run->logical_start, NULL, 0);
	if (IS_ERR(path)) {
		err = PTR_ERR(path);
		path = NULL;
		goto out_unlock;
	}

	credits = ext4_ext_calc_credits_for_single_extent(
		inode, length, path);
	err = ext4_datasem_ensure_credits(
		handle, inode, credits, credits, 0);
	if (err)
		goto out_unlock;

	path = ext4_ext_insert_extent(
		handle, inode, path, &extent, 0);
	if (IS_ERR(path)) {
		err = PTR_ERR(path);
		path = NULL;
	}

out_unlock:
	up_write(&EXT4_I(inode)->i_data_sem);
	ext4_free_ext_path(path);
	run->physical_start = 0;
	return err;
}

static int ext4_migrate_add_block(handle_t *handle, struct inode *inode,
				  ext4_fsblk_t physical,
				  struct ext4_migrate_run *run)
{
	int err;

	if (run->physical_start &&
	    run->physical_last + 1 == physical &&
	    run->logical_last + 1 == run->logical_cursor) {
		run->physical_last = physical;
		run->logical_last = run->logical_cursor++;
		return 0;
	}

	err = ext4_migrate_flush_run(handle, inode, run);
	if (err)
		return err;

	run->physical_start = physical;
	run->physical_last = physical;
	run->logical_start = run->logical_cursor;
	run->logical_last = run->logical_cursor++;
	return 0;
}

static int ext4_migrate_scan_indirect(handle_t *handle, struct inode *inode,
				      ext4_fsblk_t block,
				      unsigned int depth,
				      struct ext4_migrate_run *run)
{
	struct buffer_head *bh;
	__le32 *entries;
	const unsigned int per_block = inode->i_sb->s_blocksize >> 2;
	u64 hole_advance = 1;
	unsigned int i;
	int err = 0;

	if (depth == 0)
		return ext4_migrate_add_block(handle, inode, block, run);

	for (i = 1; i < depth; ++i)
		hole_advance *= per_block;

	bh = ext4_sb_bread(inode->i_sb, block, 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	entries = (__le32 *)bh->b_data;
	for (i = 0; i < per_block; ++i) {
		if (entries[i]) {
			err = ext4_migrate_scan_indirect(
				handle, inode, le32_to_cpu(entries[i]),
				depth - 1, run);
			if (err)
				break;
		} else {
			if (hole_advance > EXT4_MAX_LOGICAL_BLOCK -
			    run->logical_cursor) {
				err = -EFSCORRUPTED;
				break;
			}
			run->logical_cursor += hole_advance;
		}
	}

	put_bh(bh);
	return err;
}

static int ext4_migrate_free_indirect_tree(handle_t *handle,
					    struct inode *inode,
					    ext4_fsblk_t block,
					    unsigned int depth)
{
	struct buffer_head *bh;
	__le32 *entries;
	const unsigned int per_block = inode->i_sb->s_blocksize >> 2;
	unsigned int i;
	int err;

	if (!block)
		return 0;

	if (depth > 1) {
		bh = ext4_sb_bread(inode->i_sb, block, 0);
		if (IS_ERR(bh))
			return PTR_ERR(bh);

		entries = (__le32 *)bh->b_data;
		for (i = 0; i < per_block; ++i) {
			if (!entries[i])
				continue;
			err = ext4_migrate_free_indirect_tree(
				handle, inode, le32_to_cpu(entries[i]),
				depth - 1);
			if (err) {
				put_bh(bh);
				return err;
			}
		}
		put_bh(bh);
	}

	err = ext4_journal_ensure_credits(
		handle, EXT4_RESERVE_TRANS_BLOCKS,
		ext4_free_metadata_revoke_credits(inode->i_sb, 1));
	if (err)
		return err;

	ext4_free_blocks(
		handle, inode, NULL, block, 1,
		EXT4_FREE_BLOCKS_METADATA | EXT4_FREE_BLOCKS_FORGET);
	return 0;
}

static int ext4_migrate_free_old_indirects(handle_t *handle,
					    struct inode *inode,
					    const __le32 saved[3])
{
	int err;

	err = ext4_migrate_free_indirect_tree(
		handle, inode, le32_to_cpu(saved[0]), 1);
	if (err)
		return err;
	err = ext4_migrate_free_indirect_tree(
		handle, inode, le32_to_cpu(saved[1]), 2);
	if (err)
		return err;
	return ext4_migrate_free_indirect_tree(
		handle, inode, le32_to_cpu(saved[2]), 3);
}

static int ext4_migrate_swap_to_extents(handle_t *handle,
					 struct inode *inode,
					 struct inode *temp)
{
	struct ext4_inode_info *dst = EXT4_I(inode);
	struct ext4_inode_info *src = EXT4_I(temp);
	__le32 saved[3];
	int err;
	int mark_err;

	err = ext4_journal_ensure_credits(handle, 1, 0);
	if (err)
		return err;

	saved[0] = dst->i_data[EXT4_IND_BLOCK];
	saved[1] = dst->i_data[EXT4_DIND_BLOCK];
	saved[2] = dst->i_data[EXT4_TIND_BLOCK];

	down_write(&dst->i_data_sem);
	if (!ext4_test_inode_state(inode, EXT4_STATE_EXT_MIGRATE)) {
		up_write(&dst->i_data_sem);
		return -EAGAIN;
	}
	ext4_clear_inode_state(inode, EXT4_STATE_EXT_MIGRATE);

	ext4_set_inode_flag(inode, EXT4_INODE_EXTENTS);
	memcpy(dst->i_data, src->i_data, sizeof(dst->i_data));
	spin_lock(&inode->i_lock);
	inode->i_blocks += temp->i_blocks;
	spin_unlock(&inode->i_lock);
	up_write(&dst->i_data_sem);

	err = ext4_migrate_free_old_indirects(handle, inode, saved);
	mark_err = ext4_mark_inode_dirty(handle, inode);
	if (!err)
		err = mark_err;
	return err;
}

static int ext4_migrate_free_extent_index(handle_t *handle,
					   struct inode *inode,
					   struct ext4_extent_idx *index)
{
	struct buffer_head *bh;
	struct ext4_extent_header *header;
	struct ext4_extent_idx *child;
	ext4_fsblk_t block = ext4_idx_pblock(index);
	unsigned int i;
	int err;

	bh = ext4_sb_bread(inode->i_sb, block, 0);
	if (IS_ERR(bh))
		return PTR_ERR(bh);

	header = (struct ext4_extent_header *)bh->b_data;
	if (le16_to_cpu(header->eh_depth) != 0) {
		child = EXT_FIRST_INDEX(header);
		for (i = 0; i < le16_to_cpu(header->eh_entries); ++i, ++child) {
			err = ext4_migrate_free_extent_index(
				handle, inode, child);
			if (err) {
				put_bh(bh);
				return err;
			}
		}
	}
	put_bh(bh);

	err = ext4_journal_ensure_credits(
		handle, EXT4_RESERVE_TRANS_BLOCKS,
		ext4_free_metadata_revoke_credits(inode->i_sb, 1));
	if (err)
		return err;

	ext4_free_blocks(
		handle, inode, NULL, block, 1,
		EXT4_FREE_BLOCKS_METADATA | EXT4_FREE_BLOCKS_FORGET);
	return 0;
}

static int ext4_migrate_free_extent_tree(handle_t *handle,
					  struct inode *inode)
{
	struct ext4_extent_header *header = ext_inode_hdr(inode);
	struct ext4_extent_idx *index;
	unsigned int i;
	int err;

	if (le16_to_cpu(header->eh_depth) == 0)
		return 0;

	index = EXT_FIRST_INDEX(header);
	for (i = 0; i < le16_to_cpu(header->eh_entries); ++i, ++index) {
		err = ext4_migrate_free_extent_index(handle, inode, index);
		if (err)
			return err;
	}
	return 0;
}

static int ext4_migrate_build_extent_tree(handle_t *handle,
					   struct inode *source,
					   struct inode *temp)
{
	struct ext4_inode_info *ei = EXT4_I(source);
	struct ext4_migrate_run run = { 0 };
	const unsigned int per_block = source->i_sb->s_blocksize >> 2;
	unsigned int i;
	int err;

	for (i = 0; i < EXT4_NDIR_BLOCKS; ++i) {
		if (ei->i_data[i])
			err = ext4_migrate_add_block(
				handle, temp, le32_to_cpu(ei->i_data[i]), &run);
		else {
			run.logical_cursor++;
			err = 0;
		}
		if (err)
			return err;
	}

	if (ei->i_data[EXT4_IND_BLOCK])
		err = ext4_migrate_scan_indirect(
			handle, temp,
			le32_to_cpu(ei->i_data[EXT4_IND_BLOCK]), 1, &run);
	else {
		run.logical_cursor += per_block;
		err = 0;
	}
	if (err)
		return err;

	if (ei->i_data[EXT4_DIND_BLOCK])
		err = ext4_migrate_scan_indirect(
			handle, temp,
			le32_to_cpu(ei->i_data[EXT4_DIND_BLOCK]), 2, &run);
	else {
		run.logical_cursor += (ext4_lblk_t)per_block * per_block;
		err = 0;
	}
	if (err)
		return err;

	if (ei->i_data[EXT4_TIND_BLOCK]) {
		err = ext4_migrate_scan_indirect(
			handle, temp,
			le32_to_cpu(ei->i_data[EXT4_TIND_BLOCK]), 3, &run);
		if (err)
			return err;
	}

	return ext4_migrate_flush_run(handle, temp, &run);
}

int ext4_ext_migrate(struct inode *inode)
{
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct inode *temp = NULL;
	handle_t *handle;
	uid_t owner[2];
	__u32 goal;
	__u32 original_temp_seed;
	int writepages_ctx;
	int err;

	if (!ext4_has_feature_extents(inode->i_sb) ||
	    ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS) ||
	    ext4_has_inline_data(inode))
		return -EINVAL;
	if (S_ISLNK(inode->i_mode) && inode->i_blocks == 0)
		return 0;

	writepages_ctx = ext4_writepages_down_write(inode->i_sb);
	handle = ext4_journal_start(
		inode, EXT4_HT_MIGRATE,
		3 + EXT4_MAXQUOTAS_TRANS_BLOCKS(inode->i_sb));
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out_unlock;
	}

	goal = ((inode->i_ino - 1) /
		EXT4_INODES_PER_GROUP(inode->i_sb)) *
		EXT4_INODES_PER_GROUP(inode->i_sb) + 1;
	owner[0] = i_uid_read(inode);
	owner[1] = i_gid_read(inode);

	temp = ext4_new_inode(
		handle, d_inode(inode->i_sb->s_root),
		S_IFREG, NULL, goal, owner, 0);
	if (IS_ERR(temp)) {
		err = PTR_ERR(temp);
		temp = NULL;
		ext4_journal_stop(handle);
		goto out_unlock;
	}

	original_temp_seed = EXT4_I(temp)->i_csum_seed;
	EXT4_I(temp)->i_csum_seed = ei->i_csum_seed;
	i_size_write(temp, i_size_read(inode));
	clear_nlink(temp);
	ext4_ext_tree_init(handle, temp);
	ext4_journal_stop(handle);

	down_write(&ei->i_data_sem);
	ext4_set_inode_state(inode, EXT4_STATE_EXT_MIGRATE);
	up_write(&ei->i_data_sem);

	handle = ext4_journal_start(inode, EXT4_HT_MIGRATE, 1);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out_temp;
	}

	err = ext4_migrate_build_extent_tree(handle, inode, temp);
	if (!err)
		err = ext4_migrate_swap_to_extents(handle, inode, temp);
	if (err)
		ext4_migrate_free_extent_tree(handle, temp);

	if (!ext4_journal_ensure_credits(handle, 1, 0)) {
		i_size_write(temp, 0);
		temp->i_blocks = 0;
		EXT4_I(temp)->i_csum_seed = original_temp_seed;
		ext4_ext_tree_init(handle, temp);
	}
	ext4_journal_stop(handle);

out_temp:
	if (temp) {
		unlock_new_inode(temp);
		iput(temp);
	}
out_unlock:
	ext4_writepages_up_write(inode->i_sb, writepages_ctx);
	return err;
}

int ext4_ind_migrate(struct inode *inode)
{
	struct ext4_sb_info *sbi = EXT4_SB(inode->i_sb);
	struct ext4_inode_info *ei = EXT4_I(inode);
	struct ext4_extent_header *header;
	struct ext4_extent *extent;
	handle_t *handle;
	ext4_lblk_t logical_start = 0;
	ext4_lblk_t logical_end = 0;
	ext4_fsblk_t physical = 0;
	unsigned int length = 0;
	unsigned int i;
	int writepages_ctx;
	int err;
	int mark_err;

	if (!ext4_has_feature_extents(inode->i_sb) ||
	    !ext4_test_inode_flag(inode, EXT4_INODE_EXTENTS))
		return -EINVAL;
	if (ext4_has_feature_bigalloc(inode->i_sb))
		return -EOPNOTSUPP;

	if (test_opt(inode->i_sb, DELALLOC))
		ext4_alloc_da_blocks(inode);

	writepages_ctx = ext4_writepages_down_write(inode->i_sb);
	handle = ext4_journal_start(inode, EXT4_HT_MIGRATE, 1);
	if (IS_ERR(handle)) {
		err = PTR_ERR(handle);
		goto out_unlock;
	}

	down_write(&ei->i_data_sem);
	err = ext4_ext_check_inode(inode);
	if (err)
		goto out_sem;

	header = ext_inode_hdr(inode);
	if (ext4_blocks_count(sbi->s_es) > EXT4_MAX_BLOCK_FILE_PHYS ||
	    le16_to_cpu(header->eh_depth) != 0 ||
	    le16_to_cpu(header->eh_entries) > 1) {
		err = -EOPNOTSUPP;
		goto out_sem;
	}

	if (le16_to_cpu(header->eh_entries) == 1) {
		extent = EXT_FIRST_EXTENT(header);
		length = le16_to_cpu(extent->ee_len);
		physical = ext4_ext_pblock(extent);
		logical_start = le32_to_cpu(extent->ee_block);
		logical_end = logical_start + length - 1;
		if (logical_end >= EXT4_NDIR_BLOCKS) {
			err = -EOPNOTSUPP;
			goto out_sem;
		}
	}

	ext4_clear_inode_flag(inode, EXT4_INODE_EXTENTS);
	memset(ei->i_data, 0, sizeof(ei->i_data));
	if (length) {
		for (i = logical_start; i <= logical_end; ++i)
			ei->i_data[i] = cpu_to_le32(physical++);
	}

	mark_err = ext4_mark_inode_dirty(handle, inode);
	if (!err)
		err = mark_err;

out_sem:
	up_write(&ei->i_data_sem);
	ext4_journal_stop(handle);
out_unlock:
	ext4_writepages_up_write(inode->i_sb, writepages_ctx);
	return err;
}


/* ===== extent movement ===== */
#include <linux/fs.h>
#include <linux/quotaops.h>
#include <linux/sched/mm.h>
#include <linux/slab.h>

#include "ext4.h"
#include "ext4_extents.h"
#include "ext4_jbd2.h"

static struct ext4_ext_path *
ext4_move_find_path(struct inode *inode, ext4_lblk_t block,
		    struct ext4_ext_path *path)
{
	path = ext4_find_extent(inode, block, path, EXT4_EX_NOCACHE);
	if (IS_ERR(path))
		return path;

	if (!path[path->p_depth].p_ext) {
		ext4_free_ext_path(path);
		return ERR_PTR(-ENODATA);
	}
	return path;
}

void ext4_double_down_write_data_sem(struct inode *first, struct inode *second)
{
	if (first < second) {
		down_write(&EXT4_I(first)->i_data_sem);
		down_write_nested(
			&EXT4_I(second)->i_data_sem, I_DATA_SEM_OTHER);
	} else {
		down_write(&EXT4_I(second)->i_data_sem);
		down_write_nested(
			&EXT4_I(first)->i_data_sem, I_DATA_SEM_OTHER);
	}
}

void ext4_double_up_write_data_sem(struct inode *first, struct inode *second)
{
	up_write(&EXT4_I(first)->i_data_sem);
	up_write(&EXT4_I(second)->i_data_sem);
}

static int ext4_move_same_extent_state(struct inode *inode,
				       ext4_lblk_t start,
				       ext4_lblk_t count,
				       bool unwritten,
				       int *err)
{
	struct ext4_ext_path *path = NULL;
	const ext4_lblk_t end = start + count;
	int covered = 0;

	while (start < end) {
		struct ext4_extent *extent;
		ext4_lblk_t extent_start;
		ext4_lblk_t extent_end;

		path = ext4_move_find_path(inode, start, path);
		if (IS_ERR(path)) {
			*err = PTR_ERR(path);
			return covered;
		}

		extent = path[path->p_depth].p_ext;
		if (unwritten != ext4_ext_is_unwritten(extent))
			goto out;

		extent_start = le32_to_cpu(extent->ee_block);
		extent_end = extent_start + ext4_ext_get_actual_len(extent);
		if (start < extent_start) {
			*err = -ENODATA;
			goto out;
		}
		start = extent_end;
	}

	covered = 1;
out:
	ext4_free_ext_path(path);
	return covered;
}

static int ext4_move_lock_folios(struct inode *first_inode,
				  struct inode *second_inode,
				  pgoff_t first_index,
				  pgoff_t second_index,
				  struct folio *folios[2])
{
	struct address_space *mapping[2];
	unsigned int nofs;

	if (first_inode < second_inode) {
		mapping[0] = first_inode->i_mapping;
		mapping[1] = second_inode->i_mapping;
	} else {
		swap(first_index, second_index);
		mapping[0] = second_inode->i_mapping;
		mapping[1] = first_inode->i_mapping;
	}

	nofs = memalloc_nofs_save();
	folios[0] = __filemap_get_folio(
		mapping[0], first_index, FGP_WRITEBEGIN,
		mapping_gfp_mask(mapping[0]));
	if (IS_ERR(folios[0])) {
		memalloc_nofs_restore(nofs);
		return PTR_ERR(folios[0]);
	}

	folios[1] = __filemap_get_folio(
		mapping[1], second_index, FGP_WRITEBEGIN,
		mapping_gfp_mask(mapping[1]));
	memalloc_nofs_restore(nofs);
	if (IS_ERR(folios[1])) {
		folio_unlock(folios[0]);
		folio_put(folios[0]);
		return PTR_ERR(folios[1]);
	}

	folio_wait_writeback(folios[0]);
	folio_wait_writeback(folios[1]);
	if (first_inode > second_inode)
		swap(folios[0], folios[1]);
	return 0;
}

static int ext4_move_ensure_folio_uptodate(struct folio *folio,
					   size_t from, size_t to)
{
	struct inode *inode = folio->mapping->host;
	const unsigned int block_size = i_blocksize(inode);
	struct buffer_head *head;
	struct buffer_head *bh;
	sector_t block;
	unsigned int block_start = 0;
	bool partial = false;
	int reads = 0;

	BUG_ON(!folio_test_locked(folio));
	BUG_ON(folio_test_writeback(folio));
	if (folio_test_uptodate(folio))
		return 0;

	head = folio_buffers(folio);
	if (!head)
		head = create_empty_buffers(folio, block_size, 0);

	block = folio_pos(folio) >> inode->i_blkbits;
	bh = head;
	do {
		const unsigned int block_end = block_start + block_size;

		if (block_end <= from || block_start >= to) {
			if (!buffer_uptodate(bh))
				partial = true;
			goto advance;
		}
		if (buffer_uptodate(bh))
			goto advance;

		if (!buffer_mapped(bh)) {
			int err = ext4_get_block(inode, block, bh, 0);

			if (err)
				return err;
			if (!buffer_mapped(bh)) {
				folio_zero_range(folio, block_start, block_size);
				set_buffer_uptodate(bh);
				goto advance;
			}
		}

		lock_buffer(bh);
		if (buffer_uptodate(bh)) {
			unlock_buffer(bh);
			goto advance;
		}
		ext4_read_bh_nowait(bh, 0, NULL, false);
		reads++;

advance:
		block++;
		block_start += block_size;
		bh = bh->b_this_page;
	} while (bh != head);

	if (reads) {
		bh = head;
		do {
			if (bh_offset(bh) + block_size <= from)
				goto next_wait;
			if (bh_offset(bh) >= to)
				break;
			wait_on_buffer(bh);
			if (!buffer_uptodate(bh))
				return -EIO;
next_wait:
			bh = bh->b_this_page;
		} while (bh != head);
	}

	if (!partial)
		folio_mark_uptodate(folio);
	return 0;
}

static void ext4_move_unlock_folios(struct folio *folios[2])
{
	unsigned int i;

	for (i = 0; i < 2; ++i) {
		if (!folios[i])
			continue;
		folio_unlock(folios[i]);
		folio_put(folios[i]);
		folios[i] = NULL;
	}
}

static int ext4_move_one_folio(struct file *file,
			       struct inode *donor,
			       pgoff_t source_page,
			       pgoff_t donor_page,
			       int block_offset,
			       int block_count,
			       bool unwritten,
			       int *err)
{
	struct inode *source = file_inode(file);
	struct super_block *sb = source->i_sb;
	struct folio *folios[2] = { NULL, NULL };
	const unsigned int blocks_per_page =
		PAGE_SIZE >> source->i_blkbits;
	const unsigned int block_size = 1U << source->i_blkbits;
	const ext4_lblk_t source_block =
		source_page * blocks_per_page + block_offset;
	const ext4_lblk_t donor_block =
		donor_page * blocks_per_page + block_offset;
	const int from = block_offset << source->i_blkbits;
	unsigned int data_size;
	unsigned int replaced_size;
	handle_t *handle;
	struct buffer_head *bh;
	int journal_blocks;
	int retries = 0;
	int replaced = 0;
	int i;

retry:
	*err = 0;
	journal_blocks = ext4_writepage_trans_blocks(source) * 2;
	handle = ext4_journal_start(
		source, EXT4_HT_MOVE_EXTENTS, journal_blocks);
	if (IS_ERR(handle)) {
		*err = PTR_ERR(handle);
		return 0;
	}

	if (source_block + block_count - 1 ==
	    (source->i_size - 1) >> source->i_blkbits) {
		const unsigned int tail = source->i_size & (block_size - 1);
		data_size = (tail ? tail : block_size) +
			((block_count - 1) << source->i_blkbits);
	} else {
		data_size = block_count << source->i_blkbits;
	}
	replaced_size = data_size;

	*err = ext4_move_lock_folios(
		source, donor, source_page, donor_page, folios);
	if (*err)
		goto stop;

	VM_BUG_ON_FOLIO(folio_test_large(folios[0]), folios[0]);
	VM_BUG_ON_FOLIO(folio_test_large(folios[1]), folios[1]);

	if (unwritten) {
		bool both_unwritten;

		ext4_double_down_write_data_sem(source, donor);
		both_unwritten = ext4_move_same_extent_state(
			source, source_block, block_count, true, err);
		if (!*err)
			both_unwritten &=
				ext4_move_same_extent_state(
					donor, donor_block,
					block_count, true, err);
		if (*err) {
			ext4_double_up_write_data_sem(source, donor);
			goto unlock;
		}

		if (both_unwritten) {
			if (!filemap_release_folio(folios[0], 0) ||
			    !filemap_release_folio(folios[1], 0)) {
				*err = -EBUSY;
				ext4_double_up_write_data_sem(source, donor);
				goto unlock;
			}
			replaced = ext4_swap_extents(
				handle, source, donor,
				source_block, donor_block,
				block_count, 1, err);
			ext4_double_up_write_data_sem(source, donor);
			goto unlock;
		}
		ext4_double_up_write_data_sem(source, donor);
	}

	*err = ext4_move_ensure_folio_uptodate(
		folios[0], from, from + replaced_size);
	if (*err)
		goto unlock;

	if (!filemap_release_folio(folios[0], 0) ||
	    !filemap_release_folio(folios[1], 0)) {
		*err = -EBUSY;
		goto unlock;
	}

	ext4_double_down_write_data_sem(source, donor);
	replaced = ext4_swap_extents(
		handle, source, donor,
		source_block, donor_block,
		block_count, 1, err);
	ext4_double_up_write_data_sem(source, donor);

	if (*err && replaced == 0)
		goto unlock;
	if (*err) {
		block_count = replaced;
		replaced_size = block_count << source->i_blkbits;
	}

	bh = folio_buffers(folios[0]);
	if (!bh)
		bh = create_empty_buffers(
			folios[0], 1U << source->i_blkbits, 0);
	for (i = 0; i < block_offset; ++i)
		bh = bh->b_this_page;

	for (i = 0; i < block_count; ++i) {
		*err = ext4_get_block(source, source_block + i, bh, 0);
		if (*err)
			goto rollback;
		bh = bh->b_this_page;
	}

	block_commit_write(&folios[0]->page, from, from + replaced_size);
	*err = ext4_jbd2_inode_add_write(
		handle, source,
		(loff_t)source_page << PAGE_SHIFT,
		replaced_size);
	goto unlock;

rollback:
	{
		int rollback_err = 0;
		int restored;

		ext4_double_down_write_data_sem(source, donor);
		restored = ext4_swap_extents(
			handle, donor, source,
			source_block, donor_block,
			block_count, 0, &rollback_err);
		ext4_double_up_write_data_sem(source, donor);

		if (restored != block_count) {
			ext4_error_inode_block(
				source, source_block, EIO,
				"extent relocation rollback incomplete");
			*err = -EIO;
		}
		replaced = 0;
	}

unlock:
	ext4_move_unlock_folios(folios);
stop:
	ext4_journal_stop(handle);

	if (*err == -ENOSPC &&
	    ext4_should_retry_alloc(sb, &retries))
		goto retry;

	if (*err == -EBUSY && retries++ < 4 &&
	    EXT4_SB(sb)->s_journal &&
	    jbd2_journal_force_commit_nested(EXT4_SB(sb)->s_journal))
		goto retry;

	return replaced;
}

static int ext4_move_validate(struct inode *source, struct inode *donor,
			      __u64 source_start, __u64 donor_start,
			      __u64 *length)
{
	const unsigned int block_bits = source->i_blkbits;
	const unsigned int block_size = 1U << block_bits;
	const __u64 source_eof =
		(i_size_read(source) + block_size - 1) >> block_bits;
	const __u64 donor_eof =
		(i_size_read(donor) + block_size - 1) >> block_bits;
	const __u64 page_block_mask = ~(PAGE_MASK >> block_bits);

	if (donor->i_mode & (S_ISUID | S_ISGID))
		return -EINVAL;
	if (IS_IMMUTABLE(donor) || IS_APPEND(donor))
		return -EPERM;
	if (IS_SWAPFILE(source) || IS_SWAPFILE(donor))
		return -ETXTBSY;
	if (ext4_is_quota_file(source) || ext4_is_quota_file(donor))
		return -EOPNOTSUPP;
	if (!ext4_test_inode_flag(source, EXT4_INODE_EXTENTS) ||
	    !ext4_test_inode_flag(donor, EXT4_INODE_EXTENTS))
		return -EOPNOTSUPP;
	if (!source->i_size || !donor->i_size)
		return -EINVAL;
	if ((source_start & page_block_mask) !=
	    (donor_start & page_block_mask))
		return -EINVAL;

	if (source_start >= EXT_MAX_BLOCKS ||
	    donor_start >= EXT_MAX_BLOCKS ||
	    *length > EXT_MAX_BLOCKS ||
	    source_start > EXT_MAX_BLOCKS - *length ||
	    donor_start > EXT_MAX_BLOCKS - *length)
		return -EINVAL;

	if (source_eof <= source_start || donor_eof <= donor_start)
		return -EINVAL;
	if (*length > source_eof - source_start)
		*length = source_eof - source_start;
	if (*length > donor_eof - donor_start)
		*length = donor_eof - donor_start;

	return *length ? 0 : -EINVAL;
}

int ext4_move_extents(struct file *source_file, struct file *donor_file,
		      __u64 source_block, __u64 donor_block,
		      __u64 length, __u64 *moved)
{
	struct inode *source = file_inode(source_file);
	struct inode *donor = file_inode(donor_file);
	struct ext4_ext_path *path = NULL;
	const int blocks_per_page = PAGE_SIZE >> source->i_blkbits;
	ext4_lblk_t source_cursor = source_block;
	ext4_lblk_t donor_cursor = donor_block;
	ext4_lblk_t source_end;
	int err = 0;

	if (source->i_sb != donor->i_sb ||
	    source == donor ||
	    !S_ISREG(source->i_mode) ||
	    !S_ISREG(donor->i_mode))
		return -EINVAL;
	if (ext4_should_journal_data(source) ||
	    ext4_should_journal_data(donor))
		return -EOPNOTSUPP;
	if (IS_ENCRYPTED(source) || IS_ENCRYPTED(donor))
		return -EOPNOTSUPP;

	lock_two_nondirectories(source, donor);
	inode_dio_wait(source);
	inode_dio_wait(donor);
	ext4_double_down_write_data_sem(source, donor);

	err = ext4_move_validate(
		source, donor, source_block, donor_block, &length);
	if (err)
		goto out;

	source_end = source_cursor + length;
	*moved = 0;

	while (source_cursor < source_end) {
		struct ext4_extent *extent;
		ext4_lblk_t extent_start;
		ext4_lblk_t next;
		int extent_length;
		int block_offset;
		pgoff_t source_page;
		pgoff_t donor_page;
		bool unwritten;

		path = ext4_move_find_path(source, source_cursor, path);
		if (IS_ERR(path)) {
			err = PTR_ERR(path);
			path = NULL;
			goto out;
		}

		extent = path[path->p_depth].p_ext;
		extent_start = le32_to_cpu(extent->ee_block);
		extent_length = ext4_ext_get_actual_len(extent);

		if (extent_start + extent_length <= source_cursor) {
			next = ext4_ext_next_allocated_block(path);
			if (next == EXT_MAX_BLOCKS) {
				err = -ENODATA;
				goto out;
			}
			donor_cursor += next - source_cursor;
			source_cursor = next;
			continue;
		}

		if (extent_start > source_cursor) {
			donor_cursor += extent_start - source_cursor;
			source_cursor = extent_start;
			if (source_cursor >= source_end)
				break;
		} else {
			extent_length -= source_cursor - extent_start;
		}

		if (extent_length > source_end - source_cursor)
			extent_length = source_end - source_cursor;

		unwritten = ext4_ext_is_unwritten(extent);
		source_page = source_cursor >>
			(PAGE_SHIFT - source->i_blkbits);
		donor_page = donor_cursor >>
			(PAGE_SHIFT - donor->i_blkbits);
		block_offset = source_cursor % blocks_per_page;
		if (extent_length > blocks_per_page - block_offset)
			extent_length = blocks_per_page - block_offset;

		ext4_double_up_write_data_sem(source, donor);
		*moved += ext4_move_one_folio(
			source_file, donor,
			source_page, donor_page,
			block_offset, extent_length,
			unwritten, &err);
		ext4_double_down_write_data_sem(source, donor);

		if (err)
			break;

		source_cursor += extent_length;
		donor_cursor += extent_length;
	}

out:
	if (*moved) {
		ext4_discard_preallocations(source);
		ext4_discard_preallocations(donor);
	}

	ext4_free_ext_path(path);
	ext4_double_up_write_data_sem(source, donor);
	unlock_two_nondirectories(source, donor);
	return err;
}


/* ===== read-side page I/O ===== */
#include <linux/backing-dev.h>
#include <linux/bio.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/gfp.h>
#include <linux/highmem.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/mpage.h>
#include <linux/pagevec.h>
#include <linux/prefetch.h>
#include <linux/writeback.h>

#include "ext4.h"

#define EXT4_POST_READ_RESERVE 128

enum ext4_post_read_stage {
	EXT4_POST_READ_INITIAL = 0,
	EXT4_POST_READ_DECRYPT,
	EXT4_POST_READ_VERITY,
	EXT4_POST_READ_DONE,
};

struct ext4_post_read_ctx {
	struct bio *bio;
	struct work_struct work;
	unsigned int stage;
	unsigned int enabled;
};

static struct kmem_cache *ext4_post_read_cache;
static mempool_t *ext4_post_read_pool;

static void ext4_finish_read_bio(struct bio *bio)
{
	struct folio_iter iter;

	bio_for_each_folio_all(iter, bio)
		folio_end_read(iter.folio, bio->bi_status == 0);

	if (bio->bi_private)
		mempool_free(bio->bi_private, ext4_post_read_pool);
	bio_put(bio);
}

static void ext4_continue_post_read(struct ext4_post_read_ctx *ctx);

static void ext4_decrypt_read_work(struct work_struct *work)
{
	struct ext4_post_read_ctx *ctx =
		container_of(work, struct ext4_post_read_ctx, work);

	if (fscrypt_decrypt_bio(ctx->bio))
		ext4_continue_post_read(ctx);
	else
		ext4_finish_read_bio(ctx->bio);
}

static void ext4_verify_read_work(struct work_struct *work)
{
	struct ext4_post_read_ctx *ctx =
		container_of(work, struct ext4_post_read_ctx, work);
	struct bio *bio = ctx->bio;

	mempool_free(ctx, ext4_post_read_pool);
	bio->bi_private = NULL;
	fsverity_verify_bio(bio);
	ext4_finish_read_bio(bio);
}

static void ext4_continue_post_read(struct ext4_post_read_ctx *ctx)
{
	for (;;) {
		ctx->stage++;

		if (ctx->stage == EXT4_POST_READ_DECRYPT &&
		    (ctx->enabled & BIT(EXT4_POST_READ_DECRYPT))) {
			INIT_WORK(&ctx->work, ext4_decrypt_read_work);
			fscrypt_enqueue_decrypt_work(&ctx->work);
			return;
		}

		if (ctx->stage == EXT4_POST_READ_VERITY &&
		    (ctx->enabled & BIT(EXT4_POST_READ_VERITY))) {
			INIT_WORK(&ctx->work, ext4_verify_read_work);
			fsverity_enqueue_verify_work(&ctx->work);
			return;
		}

		if (ctx->stage >= EXT4_POST_READ_DONE) {
			ext4_finish_read_bio(ctx->bio);
			return;
		}
	}
}

static void ext4_read_bio_end_io(struct bio *bio)
{
	struct ext4_post_read_ctx *ctx = bio->bi_private;

	if (ctx && !bio->bi_status) {
		ctx->stage = EXT4_POST_READ_INITIAL;
		ext4_continue_post_read(ctx);
		return;
	}

	ext4_finish_read_bio(bio);
}

static bool ext4_folio_needs_verity(const struct inode *inode, pgoff_t index)
{
	return fsverity_active(inode) &&
	       index < DIV_ROUND_UP(i_size_read(inode), PAGE_SIZE);
}

static void ext4_prepare_post_read(struct bio *bio,
				   const struct inode *inode,
				   pgoff_t first_index)
{
	unsigned int enabled = 0;
	struct ext4_post_read_ctx *ctx;

	if (fscrypt_inode_uses_fs_layer_crypto(inode))
		enabled |= BIT(EXT4_POST_READ_DECRYPT);
	if (ext4_folio_needs_verity(inode, first_index))
		enabled |= BIT(EXT4_POST_READ_VERITY);
	if (!enabled)
		return;

	ctx = mempool_alloc(ext4_post_read_pool, GFP_NOFS);
	ctx->bio = bio;
	ctx->stage = EXT4_POST_READ_INITIAL;
	ctx->enabled = enabled;
	bio->bi_private = ctx;
}

static loff_t ext4_read_limit(const struct inode *inode)
{
	if (IS_ENABLED(CONFIG_FS_VERITY) && IS_VERITY(inode))
		return inode->i_sb->s_maxbytes;
	return i_size_read(inode);
}

static void ext4_zero_failed_read(struct folio *folio)
{
	folio_zero_segment(folio, 0, folio_size(folio));
	folio_unlock(folio);
}

int ext4_mpage_readpages(struct inode *inode,
			 struct readahead_control *rac,
			 struct folio *folio)
{
	const unsigned int block_bits = inode->i_blkbits;
	const unsigned int blocks_per_folio = PAGE_SIZE >> block_bits;
	const unsigned int block_size = 1U << block_bits;
	struct block_device *bdev = inode->i_sb->s_bdev;
	struct ext4_map_blocks map = { 0 };
	struct bio *bio = NULL;
	sector_t bio_last_block = 0;
	unsigned int pages = rac ? readahead_count(rac) : 1;

	while (pages--) {
		sector_t logical;
		sector_t next_logical;
		sector_t last_logical;
		sector_t file_last;
		sector_t first_physical = 0;
		unsigned int folio_block = 0;
		unsigned int first_hole = blocks_per_folio;
		unsigned int relative = 0;
		bool fully_mapped = true;
		unsigned int length;

		if (rac)
			folio = readahead_folio(rac);
		prefetchw(&folio->flags);

		if (folio_buffers(folio))
			goto fallback;

		logical = next_logical =
			(sector_t)folio->index << (PAGE_SHIFT - block_bits);
		last_logical = logical +
			(sector_t)(pages + 1) * blocks_per_folio;
		file_last = (ext4_read_limit(inode) + block_size - 1) >>
			    block_bits;
		if (last_logical > file_last)
			last_logical = file_last;

		if ((map.m_flags & EXT4_MAP_MAPPED) &&
		    logical > map.m_lblk &&
		    logical < map.m_lblk + map.m_len) {
			unsigned int offset = logical - map.m_lblk;
			unsigned int available = map.m_len - offset;

			first_physical = map.m_pblk + offset;
			for (relative = 0;
			     relative < available &&
			     folio_block < blocks_per_folio;
			     ++relative) {
				folio_block++;
				logical++;
			}
			if (relative == available)
				map.m_flags &= ~EXT4_MAP_MAPPED;
		}

		while (folio_block < blocks_per_folio) {
			int mapped;

			if (logical < last_logical) {
				map.m_lblk = logical;
				map.m_len = last_logical - logical;
				mapped = ext4_map_blocks(NULL, inode, &map, 0);
				if (mapped < 0) {
					ext4_zero_failed_read(folio);
					goto next_folio;
				}
			} else {
				map.m_flags &= ~EXT4_MAP_MAPPED;
			}

			if (!(map.m_flags & EXT4_MAP_MAPPED)) {
				fully_mapped = false;
				if (first_hole == blocks_per_folio)
					first_hole = folio_block;
				folio_block++;
				logical++;
				continue;
			}

			if (first_hole != blocks_per_folio)
				goto fallback;

			if (folio_block == 0)
				first_physical = map.m_pblk;
			else if (first_physical + folio_block != map.m_pblk)
				goto fallback;

			for (relative = 0;
			     relative < map.m_len &&
			     folio_block < blocks_per_folio;
			     ++relative) {
				folio_block++;
				logical++;
			}
			if (relative == map.m_len)
				map.m_flags &= ~EXT4_MAP_MAPPED;
		}

		if (first_hole != blocks_per_folio) {
			folio_zero_segment(
				folio, first_hole << block_bits,
				folio_size(folio));
			if (first_hole == 0) {
				if (ext4_folio_needs_verity(
					    inode, folio->index) &&
				    !fsverity_verify_folio(folio)) {
					ext4_zero_failed_read(folio);
					goto next_folio;
				}
				folio_end_read(folio, true);
				goto next_folio;
			}
		} else if (fully_mapped) {
			folio_set_mappedtodisk(folio);
		}

		if (bio &&
		    (bio_last_block + 1 != first_physical ||
		     !fscrypt_mergeable_bio(
			     bio, inode, next_logical))) {
			submit_bio(bio);
			bio = NULL;
		}

allocate_bio:
		if (!bio) {
			bio = bio_alloc(
				bdev, bio_max_segs(pages + 1),
				REQ_OP_READ, GFP_KERNEL);
			fscrypt_set_bio_crypt_ctx(
				bio, inode, next_logical, GFP_KERNEL);
			ext4_prepare_post_read(
				bio, inode, folio->index);
			bio->bi_iter.bi_sector =
				first_physical << (block_bits - 9);
			bio->bi_end_io = ext4_read_bio_end_io;
			if (rac)
				bio->bi_opf |= REQ_RAHEAD;
		}

		length = first_hole << block_bits;
		if (!bio_add_folio(bio, folio, length, 0)) {
			submit_bio(bio);
			bio = NULL;
			goto allocate_bio;
		}

		if (((map.m_flags & EXT4_MAP_BOUNDARY) &&
		     relative == map.m_len) ||
		    first_hole != blocks_per_folio) {
			submit_bio(bio);
			bio = NULL;
		} else {
			bio_last_block =
				first_physical + blocks_per_folio - 1;
		}
		goto next_folio;

fallback:
		if (bio) {
			submit_bio(bio);
			bio = NULL;
		}
		if (!folio_test_uptodate(folio))
			block_read_full_folio(folio, ext4_get_block);
		else
			folio_unlock(folio);

next_folio:
		;
	}

	if (bio)
		submit_bio(bio);
	return 0;
}

int __init ext4_init_post_read_processing(void)
{
	ext4_post_read_cache =
		KMEM_CACHE(ext4_post_read_ctx, SLAB_RECLAIM_ACCOUNT);
	if (!ext4_post_read_cache)
		return -ENOMEM;

	ext4_post_read_pool = mempool_create_slab_pool(
		EXT4_POST_READ_RESERVE, ext4_post_read_cache);
	if (!ext4_post_read_pool) {
		kmem_cache_destroy(ext4_post_read_cache);
		ext4_post_read_cache = NULL;
		return -ENOMEM;
	}

	return 0;
}

void ext4_exit_post_read_processing(void)
{
	mempool_destroy(ext4_post_read_pool);
	kmem_cache_destroy(ext4_post_read_cache);
	ext4_post_read_pool = NULL;
	ext4_post_read_cache = NULL;
}

