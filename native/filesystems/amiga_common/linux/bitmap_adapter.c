/*
 * Shared project-authored Linux adapter for the AmigaDOS allocation bitmap
 * used by OFS and FFS. On-disk arithmetic and bit-selection rules are owned by
 * the canonical amiga_dos_core; this file binds those rules to Linux buffers,
 * locking and dirty-state handling.
 */

#include <linux/slab.h>

static void ifs_amiga_bitmap_drop_cache(struct affs_sb_info *sbi)
{
    affs_brelse(sbi->s_bmap_bh);
    sbi->s_bmap_bh = NULL;
    sbi->s_last_bmap = ~0U;
}

static struct buffer_head *ifs_amiga_bitmap_get(
    struct super_block *sb, const u32 bitmap_index)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct buffer_head *bh;

    if (!sbi->s_bitmap || bitmap_index >= sbi->s_bmap_count)
        return NULL;

    if (sbi->s_last_bmap == bitmap_index && sbi->s_bmap_bh)
        return sbi->s_bmap_bh;

    ifs_amiga_bitmap_drop_cache(sbi);
    bh = affs_bread(sb, sbi->s_bitmap[bitmap_index].bm_key);
    if (!bh)
        return NULL;

    sbi->s_bmap_bh = bh;
    sbi->s_last_bmap = bitmap_index;
    return bh;
}

static u32 ifs_amiga_bitmap_valid_bits(
    const struct affs_sb_info *sbi, const u32 bitmap_index)
{
    const u32 data_blocks =
        (u32)sbi->s_partition_size - (u32)sbi->s_reserved;
    const u32 base = bitmap_index * sbi->s_bmap_bits;
    const u32 remaining = data_blocks - base;

    return remaining < sbi->s_bmap_bits ? remaining : sbi->s_bmap_bits;
}

u32 affs_count_free_blocks(struct super_block *sb)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    u32 total = 0U;
    u32 index;

    if (sb_rdonly(sb) || !sbi->s_bitmap)
        return 0U;

    mutex_lock(&sbi->s_bmlock);
    for (index = 0U; index < sbi->s_bmap_count; ++index)
        total += sbi->s_bitmap[index].bm_free;
    mutex_unlock(&sbi->s_bmlock);

    return total;
}

void affs_free_block(struct super_block *sb, u32 block)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct affs_bm_info *bm;
    struct buffer_head *bh;
    __be32 *words;
    u32 bitmap_index;
    u32 bit_index;
    u32 word_index;
    u32 mask;
    u32 value;
    u32 checksum;

    if (ifs_amiga_bitmap_location(
            block, (u32)sbi->s_reserved, (u32)sbi->s_partition_size,
            sbi->s_bmap_bits, &bitmap_index, &bit_index) != 0) {
        affs_error(sb, "affs_free_block",
                   "Block %u outside partition", block);
        return;
    }

    mutex_lock(&sbi->s_bmlock);

    if (!sbi->s_bitmap || bitmap_index >= sbi->s_bmap_count) {
        affs_error(sb, "affs_free_block",
                   "Bitmap index %u is invalid", bitmap_index);
        goto out_unlock;
    }

    bm = &sbi->s_bitmap[bitmap_index];
    bh = ifs_amiga_bitmap_get(sb, bitmap_index);
    if (!bh) {
        affs_error(sb, "affs_free_block",
                   "Cannot read bitmap block %u", bm->bm_key);
        goto out_unlock;
    }

    word_index = 1U + bit_index / 32U;
    if (word_index >= sb->s_blocksize / sizeof(__be32)) {
        affs_error(sb, "affs_free_block",
                   "Bitmap bit %u escapes bitmap block", bit_index);
        goto out_unlock;
    }

    words = (__be32 *)bh->b_data;
    mask = ifs_amiga_bitmap_bit_mask(bit_index);
    value = be32_to_cpu(words[word_index]);

    if ((value & mask) != 0U) {
        affs_warning(sb, "affs_free_block",
                     "Block %u is already free", block);
        goto out_unlock;
    }

    words[word_index] = cpu_to_be32(value | mask);
    checksum = be32_to_cpu(words[0]);
    words[0] = cpu_to_be32(checksum - mask);
    bm->bm_free++;

    mark_buffer_dirty(bh);
    affs_mark_sb_dirty(sb);

out_unlock:
    mutex_unlock(&sbi->s_bmlock);
}

static int ifs_amiga_allocate_bitmap_range(
    struct inode *inode,
    const u32 bitmap_index,
    const u32 first_bit,
    const u32 end_bit,
    u32 *const allocated_block)
{
    struct super_block *sb = inode->i_sb;
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct affs_bm_info *bm = &sbi->s_bitmap[bitmap_index];
    struct buffer_head *bh;
    __be32 *words;
    u32 first_word;
    u32 last_word;
    u32 word_index;
    u32 bitmap_base;

    if (allocated_block == NULL || first_bit >= end_bit)
        return 0;

    bh = ifs_amiga_bitmap_get(sb, bitmap_index);
    if (!bh)
        return -EIO;

    words = (__be32 *)bh->b_data;
    first_word = first_bit / 32U;
    last_word = (end_bit - 1U) / 32U;
    bitmap_base = bitmap_index * sbi->s_bmap_bits;

    for (word_index = first_word; word_index <= last_word; ++word_index) {
        const u32 word_base = word_index * 32U;
        const u32 start =
            first_bit > word_base ? first_bit - word_base : 0U;
        const u32 range_end =
            end_bit - word_base < 32U ? end_bit - word_base : 32U;
        const u32 disk_word_index = word_index + 1U;
        u32 value;
        u32 first;
        u32 run_mask;
        u32 run_length;
        u32 checksum;
        u32 absolute;

        if (disk_word_index >= sb->s_blocksize / sizeof(__be32))
            return -EUCLEAN;

        value = be32_to_cpu(words[disk_word_index]);
        if (ifs_amiga_bitmap_select_free_run(
                value, start, range_end,
                &first, &run_mask, &run_length) != 0)
            continue;

        absolute = (u32)sbi->s_reserved + bitmap_base +
                   word_base + first;
        if (!affs_validblock(sb, absolute))
            return -EUCLEAN;

        if (bm->bm_free < run_length) {
            run_length = 1U;
            run_mask = ifs_amiga_bitmap_bit_mask(first);
        }

        words[disk_word_index] = cpu_to_be32(value & ~run_mask);
        checksum = be32_to_cpu(words[0]);
        words[0] = cpu_to_be32(checksum + run_mask);

        bm->bm_free -= run_length;
        AFFS_I(inode)->i_lastalloc = absolute;
        AFFS_I(inode)->i_pa_cnt = (int)run_length - 1;

        mark_buffer_dirty(bh);
        affs_mark_sb_dirty(sb);
        *allocated_block = absolute;
        return 1;
    }

    return 0;
}

u32 affs_alloc_block(struct inode *inode, u32 goal)
{
    struct super_block *sb = inode->i_sb;
    struct affs_sb_info *sbi = AFFS_SB(sb);
    u32 start_bitmap;
    u32 start_bit;
    u32 allocated = 0U;
    u32 step;
    int result;

    if (AFFS_I(inode)->i_pa_cnt > 0) {
        const u32 next = AFFS_I(inode)->i_lastalloc + 1U;

        if (affs_validblock(sb, next)) {
            AFFS_I(inode)->i_lastalloc = next;
            AFFS_I(inode)->i_pa_cnt--;
            return next;
        }
        AFFS_I(inode)->i_pa_cnt = 0;
    }

    if (!sbi->s_bitmap || sbi->s_bmap_count == 0U ||
        sbi->s_bmap_bits == 0U)
        return 0U;

    if (!affs_validblock(sb, goal))
        goal = (u32)sbi->s_reserved;

    if (ifs_amiga_bitmap_location(
            goal, (u32)sbi->s_reserved, (u32)sbi->s_partition_size,
            sbi->s_bmap_bits, &start_bitmap, &start_bit) != 0)
        return 0U;

    mutex_lock(&sbi->s_bmlock);

    for (step = 0U; step < sbi->s_bmap_count; ++step) {
        const u32 bitmap_index =
            (start_bitmap + step) % sbi->s_bmap_count;
        const u32 valid_bits =
            ifs_amiga_bitmap_valid_bits(sbi, bitmap_index);
        const u32 begin =
            step == 0U ? start_bit : 0U;

        if (sbi->s_bitmap[bitmap_index].bm_free == 0U ||
            begin >= valid_bits)
            continue;

        result = ifs_amiga_allocate_bitmap_range(
            inode, bitmap_index, begin, valid_bits, &allocated);
        if (result < 0) {
            affs_error(sb, "affs_alloc_block",
                       "Cannot use bitmap block %u",
                       sbi->s_bitmap[bitmap_index].bm_key);
            goto out;
        }
        if (result > 0)
            goto out;
    }

    if (start_bit > 0U && sbi->s_bitmap[start_bitmap].bm_free != 0U) {
        result = ifs_amiga_allocate_bitmap_range(
            inode, start_bitmap, 0U, start_bit, &allocated);
        if (result < 0) {
            affs_error(sb, "affs_alloc_block",
                       "Cannot wrap within bitmap block %u",
                       sbi->s_bitmap[start_bitmap].bm_key);
            allocated = 0U;
        }
    }

out:
    mutex_unlock(&sbi->s_bmlock);
    return allocated;
}

static void ifs_amiga_bitmap_discard(struct affs_sb_info *sbi)
{
    ifs_amiga_bitmap_drop_cache(sbi);
    kfree(sbi->s_bitmap);
    sbi->s_bitmap = NULL;
    sbi->s_bmap_count = 0U;
    sbi->s_bmap_bits = 0U;
}

static int ifs_amiga_bitmap_normalize_tail(
    struct super_block *sb,
    struct buffer_head *bh,
    struct affs_bm_info *last_bitmap,
    const u32 valid_bits)
{
    __be32 *words = (__be32 *)bh->b_data;
    const u32 data_word_count =
        (u32)(sb->s_blocksize / sizeof(__be32)) - 1U;
    const u32 full_words = valid_bits / 32U;
    const u32 partial_bits = valid_bits % 32U;
    u32 index;
    bool changed = false;

    for (index = 0U; index < data_word_count; ++index) {
        u32 valid_mask;
        const u32 old_value = be32_to_cpu(words[index + 1U]);
        u32 new_value;

        if (index < full_words)
            valid_mask = 0xffffffffU;
        else if (index == full_words && partial_bits != 0U)
            valid_mask = ifs_amiga_bitmap_valid_word_mask(partial_bits);
        else
            valid_mask = 0U;

        new_value = old_value & valid_mask;
        if (new_value != old_value) {
            words[index + 1U] = cpu_to_be32(new_value);
            changed = true;
        }
    }

    if (changed) {
        words[0] = 0;
        words[0] = cpu_to_be32(0U - affs_checksum_block(sb, bh));
        mark_buffer_dirty(bh);
    }

    last_bitmap->bm_free =
        memweight(bh->b_data + sizeof(__be32),
                  sb->s_blocksize - sizeof(__be32));
    return 0;
}

int affs_init_bitmap(struct super_block *sb, int *flags)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);
    struct affs_bm_info *bitmap;
    struct buffer_head *bitmap_bh = NULL;
    struct buffer_head *extension_bh = NULL;
    __be32 *key_words;
    u32 key_index;
    u32 key_end;
    u32 words_per_block;
    u32 index;
    u32 data_blocks;
    u32 last_valid_bits;
    int result = 0;

    if ((*flags & SB_RDONLY) != 0)
        return 0;

    if (be32_to_cpu(AFFS_ROOT_TAIL(sb, sbi->s_root_bh)->bm_flag) == 0U) {
        pr_notice("Bitmap invalid - mounting %s read only\n", sb->s_id);
        *flags |= SB_RDONLY;
        return 0;
    }

    ifs_amiga_bitmap_drop_cache(sbi);

    if (ifs_amiga_bitmap_geometry(
            (u32)sb->s_blocksize, (u32)sbi->s_reserved,
            (u32)sbi->s_partition_size,
            &sbi->s_bmap_bits, &sbi->s_bmap_count) != 0) {
        pr_err("Invalid AmigaDOS bitmap geometry\n");
        return -EINVAL;
    }

    bitmap = kcalloc(sbi->s_bmap_count, sizeof(*bitmap), GFP_KERNEL);
    if (!bitmap)
        return -ENOMEM;
    sbi->s_bitmap = bitmap;

    words_per_block = (u32)(sb->s_blocksize / sizeof(__be32));
    if (words_per_block < 50U) {
        result = -EUCLEAN;
        goto fail;
    }

    key_words = (__be32 *)sbi->s_root_bh->b_data;
    key_index = words_per_block - 49U;
    key_end = key_index + AFFS_ROOT_BMAPS;

    for (index = 0U; index < sbi->s_bmap_count; ++index) {
        u32 key;

        affs_brelse(bitmap_bh);
        bitmap_bh = NULL;

        if (key_index >= key_end) {
            const u32 extension_key = be32_to_cpu(key_words[key_index]);

            affs_brelse(extension_bh);
            extension_bh = affs_bread(sb, extension_key);
            if (!extension_bh) {
                result = -EIO;
                goto fail;
            }

            key_words = (__be32 *)extension_bh->b_data;
            key_index = 0U;
            key_end = words_per_block - 1U;
        }

        key = be32_to_cpu(key_words[key_index++]);
        bitmap[index].bm_key = key;
        bitmap_bh = affs_bread(sb, key);
        if (!bitmap_bh) {
            result = -EIO;
            goto fail;
        }

        if (affs_checksum_block(sb, bitmap_bh) != 0U) {
            pr_warn("Bitmap %u invalid - mounting %s read only\n",
                    key, sb->s_id);
            *flags |= SB_RDONLY;
            result = 0;
            goto readonly;
        }

        bitmap[index].bm_free =
            memweight(bitmap_bh->b_data + sizeof(__be32),
                      sb->s_blocksize - sizeof(__be32));
    }

    data_blocks = (u32)sbi->s_partition_size - (u32)sbi->s_reserved;
    last_valid_bits =
        data_blocks - (sbi->s_bmap_count - 1U) * sbi->s_bmap_bits;

    if (last_valid_bits == 0U || last_valid_bits > sbi->s_bmap_bits) {
        result = -EUCLEAN;
        goto fail;
    }

    result = ifs_amiga_bitmap_normalize_tail(
        sb, bitmap_bh, &bitmap[sbi->s_bmap_count - 1U],
        last_valid_bits);
    goto out;

readonly:
    ifs_amiga_bitmap_discard(sbi);
    goto out;

fail:
    ifs_amiga_bitmap_discard(sbi);

out:
    affs_brelse(bitmap_bh);
    affs_brelse(extension_bh);
    return result;
}

void affs_free_bitmap(struct super_block *sb)
{
    struct affs_sb_info *sbi = AFFS_SB(sb);

    ifs_amiga_bitmap_discard(sbi);
}
