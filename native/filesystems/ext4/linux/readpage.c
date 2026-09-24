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
