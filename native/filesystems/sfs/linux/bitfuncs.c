#include <linux/types.h>
#include <asm/byteorder.h>
#include "bitfuncs.h"

static int sfs_bitmap_find(
    const u32 *bitmap, int longs, int bitoffset, bool want_set)
{
    int word_index;
    int local_bit;

    if (!bitmap || longs <= 0 || bitoffset < 0 ||
        bitoffset >= longs * 32)
        return -1;

    word_index = bitoffset / 32;
    local_bit = bitoffset % 32;

    while (word_index < longs) {
        const u32 word = be32_to_cpu(bitmap[word_index]);
        const int found = want_set
            ? ifs_sfs_bitmap_word_find_set(word, (ifs_sfs_u32)local_bit)
            : ifs_sfs_bitmap_word_find_zero(word, (ifs_sfs_u32)local_bit);

        if (found >= 0)
            return word_index * 32 + found;

        word_index++;
        local_bit = 0;
    }

    return -1;
}

static int sfs_bitmap_modify(
    u32 *bitmap, int longs, int bitoffset, int bits, bool set_bits)
{
    int remaining;
    int position;
    int changed = 0;

    if (!bitmap || longs <= 0 || bitoffset < 0 || bits <= 0 ||
        bitoffset >= longs * 32)
        return 0;

    remaining = bits;
    position = bitoffset;

    while (remaining > 0 && position < longs * 32) {
        const int word_index = position / 32;
        const int local_bit = position % 32;
        int chunk = 32 - local_bit;
        u32 word;

        if (chunk > remaining)
            chunk = remaining;

        word = be32_to_cpu(bitmap[word_index]);
        word = set_bits
            ? ifs_sfs_bitmap_word_set(
                word, (ifs_sfs_u32)local_bit, (ifs_sfs_u32)chunk)
            : ifs_sfs_bitmap_word_clear(
                word, (ifs_sfs_u32)local_bit, (ifs_sfs_u32)chunk);
        bitmap[word_index] = cpu_to_be32(word);

        position += chunk;
        remaining -= chunk;
        changed += chunk;
    }

    return changed;
}

int bmffo(u32 *bitmap, int longs, int bitoffset)
{
    return sfs_bitmap_find(bitmap, longs, bitoffset, true);
}

int bmffz(u32 *bitmap, int longs, int bitoffset)
{
    return sfs_bitmap_find(bitmap, longs, bitoffset, false);
}

int bmclr(u32 *bitmap, int longs, int bitoffset, int bits)
{
    return sfs_bitmap_modify(bitmap, longs, bitoffset, bits, false);
}

int bmset(u32 *bitmap, int longs, int bitoffset, int bits)
{
    return sfs_bitmap_modify(bitmap, longs, bitoffset, bits, true);
}
