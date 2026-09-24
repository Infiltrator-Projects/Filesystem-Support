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
		       le32_to_cpu(chain[depth - 1].slot + count) ==
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
