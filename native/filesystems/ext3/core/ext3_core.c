/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Canonical EXT3 format policy shared by all host adapters.
 */

#include "ext3_core.h"

ifs_ext3_u32 ifs_ext3_unsupported_incompat_features(
    const ifs_ext3_u32 feature_incompat)
{
    return feature_incompat & ~IFS_EXT3_FEATURE_INCOMPAT_SUPPORTED;
}

ifs_ext3_u32 ifs_ext3_unsupported_ro_compat_features(
    const ifs_ext3_u32 feature_ro_compat)
{
    return feature_ro_compat & ~IFS_EXT3_FEATURE_RO_COMPAT_SUPPORTED;
}

IfsExt3GeometryStatus ifs_ext3_validate_geometry(
    const ifs_ext3_u32 block_size,
    const ifs_ext3_u32 inode_size,
    const ifs_ext3_u32 fragment_size,
    const ifs_ext3_u32 blocks_per_group,
    const ifs_ext3_u32 fragments_per_group,
    const ifs_ext3_u32 inodes_per_group)
{
    const ifs_ext3_u32 bitmap_capacity = block_size * 8U;

    if (inode_size < 128U || inode_size > block_size ||
        (inode_size & (inode_size - 1U)) != 0U)
        return IFS_EXT3_GEOMETRY_INVALID_INODE_SIZE;

    if (fragment_size != block_size)
        return IFS_EXT3_GEOMETRY_FRAGMENT_SIZE_MISMATCH;

    if (blocks_per_group == 0U || fragments_per_group == 0U ||
        inodes_per_group == 0U)
        return IFS_EXT3_GEOMETRY_ZERO_GROUP_VALUE;

    if (blocks_per_group > bitmap_capacity)
        return IFS_EXT3_GEOMETRY_BLOCKS_PER_GROUP_TOO_LARGE;

    if (fragments_per_group > bitmap_capacity)
        return IFS_EXT3_GEOMETRY_FRAGMENTS_PER_GROUP_TOO_LARGE;

    if (inodes_per_group > bitmap_capacity)
        return IFS_EXT3_GEOMETRY_INODES_PER_GROUP_TOO_LARGE;

    return IFS_EXT3_GEOMETRY_OK;
}

IfsExt3LayoutStatus ifs_ext3_compute_group_count(
    const ifs_ext3_u32 blocks_count,
    const ifs_ext3_u32 first_data_block,
    const ifs_ext3_u32 blocks_per_group,
    ifs_ext3_u32 *const group_count)
{
    if (group_count == 0)
        return IFS_EXT3_LAYOUT_INVALID_BLOCKS_PER_GROUP;

    if (blocks_per_group == 0U)
        return IFS_EXT3_LAYOUT_INVALID_BLOCKS_PER_GROUP;

    if (first_data_block >= blocks_count)
        return IFS_EXT3_LAYOUT_INVALID_FIRST_DATA_BLOCK;

    *group_count =
        ((blocks_count - first_data_block - 1U) / blocks_per_group) + 1U;
    return IFS_EXT3_LAYOUT_OK;
}



ifs_ext3_u32 ifs_ext3_directory_record_length_from_disk(
    const ifs_ext3_u16 encoded_length,
    const ifs_ext3_u32 maximum_record_length)
{
    if (encoded_length == 0xFFFFU &&
        maximum_record_length >= IFS_EXT3_MAX_DIRECTORY_RECORD_LENGTH)
        return IFS_EXT3_MAX_DIRECTORY_RECORD_LENGTH;
    return encoded_length;
}

int ifs_ext3_directory_record_length_to_disk(
    const ifs_ext3_u32 record_length,
    const ifs_ext3_u32 maximum_record_length,
    ifs_ext3_u16 *const encoded_length)
{
    if (encoded_length == 0)
        return -1;
    if (record_length == 0U ||
        record_length > maximum_record_length ||
        (record_length & 3U) != 0U)
        return -1;

    if (record_length == IFS_EXT3_MAX_DIRECTORY_RECORD_LENGTH &&
        maximum_record_length >= IFS_EXT3_MAX_DIRECTORY_RECORD_LENGTH) {
        *encoded_length = 0xFFFFU;
        return 0;
    }
    if (record_length > 0xFFFFU)
        return -1;

    *encoded_length = (ifs_ext3_u16)record_length;
    return 0;
}

IfsExt3DirectoryRecordStatus ifs_ext3_validate_directory_record(
    const ifs_ext3_u32 record_offset,
    const ifs_ext3_u32 record_length,
    const ifs_ext3_u32 name_length,
    const ifs_ext3_u32 inode_number,
    const ifs_ext3_u32 block_size,
    const ifs_ext3_u32 maximum_inode)
{
    const ifs_ext3_u32 minimum_length = (name_length + 11U) & ~3U;

    if (record_length < 12U)
        return IFS_EXT3_DIRECTORY_RECORD_TOO_SHORT;
    if ((record_length & 3U) != 0U)
        return IFS_EXT3_DIRECTORY_RECORD_UNALIGNED;
    if (record_length < minimum_length)
        return IFS_EXT3_DIRECTORY_RECORD_NAME_TOO_LONG;
    if (block_size == 0U ||
        record_offset >= block_size ||
        record_length > block_size - record_offset)
        return IFS_EXT3_DIRECTORY_RECORD_CROSSES_BLOCK;
    if (inode_number > maximum_inode)
        return IFS_EXT3_DIRECTORY_RECORD_INODE_RANGE;
    return IFS_EXT3_DIRECTORY_RECORD_OK;
}

const char *ifs_ext3_directory_record_status_string(
    const IfsExt3DirectoryRecordStatus status)
{
    switch (status) {
    case IFS_EXT3_DIRECTORY_RECORD_OK: return "ok";
    case IFS_EXT3_DIRECTORY_RECORD_TOO_SHORT: return "rec_len is smaller than minimal";
    case IFS_EXT3_DIRECTORY_RECORD_UNALIGNED: return "rec_len % 4 != 0";
    case IFS_EXT3_DIRECTORY_RECORD_NAME_TOO_LONG: return "rec_len is too small for name_len";
    case IFS_EXT3_DIRECTORY_RECORD_CROSSES_BLOCK: return "directory entry across blocks";
    case IFS_EXT3_DIRECTORY_RECORD_INODE_RANGE: return "inode out of bounds";
    }
    return "invalid EXT3 directory record";
}


static ifs_ext3_u32 ifs_ext3_rol32(
    const ifs_ext3_u32 value, const unsigned int shift)
{
    return (value << shift) | (value >> (32U - shift));
}

static ifs_ext3_u32 ifs_ext3_md4_f(
    const ifs_ext3_u32 x, const ifs_ext3_u32 y, const ifs_ext3_u32 z)
{
    return z ^ (x & (y ^ z));
}

static ifs_ext3_u32 ifs_ext3_md4_g(
    const ifs_ext3_u32 x, const ifs_ext3_u32 y, const ifs_ext3_u32 z)
{
    return (x & y) + (((x ^ y) & z));
}

static ifs_ext3_u32 ifs_ext3_md4_h(
    const ifs_ext3_u32 x, const ifs_ext3_u32 y, const ifs_ext3_u32 z)
{
    return x ^ y ^ z;
}

static ifs_ext3_u32 ifs_ext3_md4_step(
    ifs_ext3_u32 (*function)(ifs_ext3_u32, ifs_ext3_u32, ifs_ext3_u32),
    ifs_ext3_u32 accumulator,
    const ifs_ext3_u32 b,
    const ifs_ext3_u32 c,
    const ifs_ext3_u32 d,
    const ifs_ext3_u32 word,
    const unsigned int shift)
{
    return ifs_ext3_rol32(
        accumulator + function(b, c, d) + word, shift);
}

static void ifs_ext3_half_md4(
    ifs_ext3_u32 state[4], const ifs_ext3_u32 words[8])
{
    const ifs_ext3_u32 k2 = 0x5a827999U;
    const ifs_ext3_u32 k3 = 0x6ed9eba1U;
    ifs_ext3_u32 a = state[0];
    ifs_ext3_u32 b = state[1];
    ifs_ext3_u32 c = state[2];
    ifs_ext3_u32 d = state[3];

#define IFS_EXT3_MD4_STEP(fn, reg, b_, c_, d_, word_, shift_) \
    (reg) = ifs_ext3_md4_step((fn), (reg), (b_), (c_), (d_), (word_), (shift_))

    IFS_EXT3_MD4_STEP(ifs_ext3_md4_f, a, b, c, d, words[0], 3U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_f, d, a, b, c, words[1], 7U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_f, c, d, a, b, words[2], 11U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_f, b, c, d, a, words[3], 19U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_f, a, b, c, d, words[4], 3U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_f, d, a, b, c, words[5], 7U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_f, c, d, a, b, words[6], 11U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_f, b, c, d, a, words[7], 19U);

    IFS_EXT3_MD4_STEP(ifs_ext3_md4_g, a, b, c, d, words[1] + k2, 3U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_g, d, a, b, c, words[3] + k2, 5U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_g, c, d, a, b, words[5] + k2, 9U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_g, b, c, d, a, words[7] + k2, 13U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_g, a, b, c, d, words[0] + k2, 3U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_g, d, a, b, c, words[2] + k2, 5U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_g, c, d, a, b, words[4] + k2, 9U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_g, b, c, d, a, words[6] + k2, 13U);

    IFS_EXT3_MD4_STEP(ifs_ext3_md4_h, a, b, c, d, words[3] + k3, 3U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_h, d, a, b, c, words[7] + k3, 9U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_h, c, d, a, b, words[2] + k3, 11U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_h, b, c, d, a, words[6] + k3, 15U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_h, a, b, c, d, words[1] + k3, 3U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_h, d, a, b, c, words[5] + k3, 9U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_h, c, d, a, b, words[0] + k3, 11U);
    IFS_EXT3_MD4_STEP(ifs_ext3_md4_h, b, c, d, a, words[4] + k3, 15U);

#undef IFS_EXT3_MD4_STEP

    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

static void ifs_ext3_tea(
    ifs_ext3_u32 state[4], const ifs_ext3_u32 words[4])
{
    ifs_ext3_u32 left = state[0];
    ifs_ext3_u32 right = state[1];
    ifs_ext3_u32 sum = 0U;
    unsigned int round;

    for (round = 0U; round < 16U; ++round) {
        sum += 0x9e3779b9U;
        left += ((right << 4) + words[0]) ^
                (right + sum) ^
                ((right >> 5) + words[1]);
        right += ((left << 4) + words[2]) ^
                 (left + sum) ^
                 ((left >> 5) + words[3]);
    }

    state[0] += left;
    state[1] += right;
}

static void ifs_ext3_pack_hash_words(
    const ifs_ext3_u8 *name,
    const ifs_ext3_u32 length,
    ifs_ext3_u32 *words,
    const unsigned int word_count,
    const int signed_bytes)
{
    const ifs_ext3_u32 pad_byte = length & 0xffU;
    const ifs_ext3_u32 pad =
        pad_byte | (pad_byte << 8) | (pad_byte << 16) | (pad_byte << 24);
    const ifs_ext3_u32 limit =
        length < word_count * 4U ? length : word_count * 4U;
    unsigned int word_index;

    for (word_index = 0U; word_index < word_count; ++word_index)
        words[word_index] = pad;

    for (word_index = 0U; word_index < limit; ++word_index) {
        const unsigned int target = word_index / 4U;
        const int byte_value = signed_bytes != 0 && (int)name[word_index] >= 0x80
            ? (int)name[word_index] - 256
            : (int)name[word_index];

        if ((word_index & 3U) == 0U)
            words[target] = pad;
        words[target] =
            (ifs_ext3_u32)byte_value + (words[target] << 8);
    }
}

static ifs_ext3_u32 ifs_ext3_legacy_hash(
    const ifs_ext3_u8 *name,
    const ifs_ext3_u32 length,
    const int signed_bytes)
{
    ifs_ext3_u32 previous = 0x12a3fe2dU;
    ifs_ext3_u32 older = 0x37abe8f9U;
    ifs_ext3_u32 index;

    for (index = 0U; index < length; ++index) {
        const int byte_value = signed_bytes != 0 && (int)name[index] >= 0x80
            ? (int)name[index] - 256
            : (int)name[index];
        ifs_ext3_u32 next =
            older + (previous ^ ((ifs_ext3_u32)byte_value * 7152373U));

        if ((next & 0x80000000U) != 0U)
            next -= 0x7fffffffU;
        older = previous;
        previous = next;
    }

    return previous << 1;
}

int ifs_ext3_directory_hash(
    const ifs_ext3_u8 *name,
    const ifs_ext3_u32 length,
    const int version,
    const ifs_ext3_u32 seed[4],
    ifs_ext3_u32 *const major_hash,
    ifs_ext3_u32 *const minor_hash)
{
    ifs_ext3_u32 state[4] = {
        0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U
    };
    ifs_ext3_u32 major = 0U;
    ifs_ext3_u32 minor = 0U;
    ifs_ext3_u32 offset = 0U;
    int signed_bytes = 1;

    if ((name == 0 && length != 0U) ||
        major_hash == 0 || minor_hash == 0)
        return -1;

    if (seed != 0 &&
        (seed[0] != 0U || seed[1] != 0U ||
         seed[2] != 0U || seed[3] != 0U)) {
        state[0] = seed[0];
        state[1] = seed[1];
        state[2] = seed[2];
        state[3] = seed[3];
    }

    switch (version) {
    case IFS_EXT3_HASH_LEGACY_UNSIGNED:
        signed_bytes = 0;
        major = ifs_ext3_legacy_hash(name, length, signed_bytes);
        break;
    case IFS_EXT3_HASH_LEGACY:
        major = ifs_ext3_legacy_hash(name, length, signed_bytes);
        break;
    case IFS_EXT3_HASH_HALF_MD4_UNSIGNED:
        signed_bytes = 0;
        /* fall through */
    case IFS_EXT3_HASH_HALF_MD4:
        while (offset < length) {
            ifs_ext3_u32 words[8];
            const ifs_ext3_u32 remaining = length - offset;
            ifs_ext3_pack_hash_words(
                name + offset, remaining, words, 8U, signed_bytes);
            ifs_ext3_half_md4(state, words);
            offset += remaining < 32U ? remaining : 32U;
        }
        major = state[1];
        minor = state[2];
        break;
    case IFS_EXT3_HASH_TEA_UNSIGNED:
        signed_bytes = 0;
        /* fall through */
    case IFS_EXT3_HASH_TEA:
        while (offset < length) {
            ifs_ext3_u32 words[4];
            const ifs_ext3_u32 remaining = length - offset;
            ifs_ext3_pack_hash_words(
                name + offset, remaining, words, 4U, signed_bytes);
            ifs_ext3_tea(state, words);
            offset += remaining < 16U ? remaining : 16U;
        }
        major = state[0];
        minor = state[1];
        break;
    default:
        return -1;
    }

    major &= ~1U;
    if (major == 0xfffffffeU)
        major = 0xfffffffcU;

    *major_hash = major;
    *minor_hash = minor;
    return 0;
}

int ifs_ext3_indirect_block_path(
    ifs_ext3_u64 logical_block,
    const ifs_ext3_u32 pointers_per_block,
    const ifs_ext3_u32 pointer_bits,
    ifs_ext3_u32 offsets[4],
    ifs_ext3_u32 *const boundary)
{
    ifs_ext3_u64 remaining = logical_block;
    const ifs_ext3_u64 indirect_blocks = pointers_per_block;
    ifs_ext3_u64 double_blocks;
    ifs_ext3_u64 triple_blocks;
    ifs_ext3_u32 final = 0U;
    int depth = 0;

    if (offsets == 0 || pointers_per_block == 0U ||
        pointer_bits >= 32U ||
        ((ifs_ext3_u64)1U << pointer_bits) != pointers_per_block) {
        if (boundary != 0)
            *boundary = 0U;
        return 0;
    }

    double_blocks = indirect_blocks * indirect_blocks;
    triple_blocks = double_blocks * indirect_blocks;

    if (remaining < IFS_EXT3_NDIR_BLOCKS) {
        offsets[depth++] = (ifs_ext3_u32)remaining;
        final = IFS_EXT3_NDIR_BLOCKS;
    } else {
        remaining -= IFS_EXT3_NDIR_BLOCKS;
        if (remaining < indirect_blocks) {
            offsets[depth++] = IFS_EXT3_IND_BLOCK;
            offsets[depth++] = (ifs_ext3_u32)remaining;
            final = pointers_per_block;
        } else {
            remaining -= indirect_blocks;
            if (remaining < double_blocks) {
                offsets[depth++] = IFS_EXT3_DIND_BLOCK;
                offsets[depth++] = (ifs_ext3_u32)(remaining >> pointer_bits);
                offsets[depth++] = (ifs_ext3_u32)(remaining & (pointers_per_block - 1U));
                final = pointers_per_block;
            } else {
                remaining -= double_blocks;
                if (remaining < triple_blocks) {
                    offsets[depth++] = IFS_EXT3_TIND_BLOCK;
                    offsets[depth++] = (ifs_ext3_u32)(remaining >> (pointer_bits * 2U));
                    offsets[depth++] = (ifs_ext3_u32)((remaining >> pointer_bits) &
                                                      (pointers_per_block - 1U));
                    offsets[depth++] = (ifs_ext3_u32)(remaining & (pointers_per_block - 1U));
                    final = pointers_per_block;
                }
            }
        }
    }

    if (boundary != 0) {
        if (depth == 0)
            *boundary = 0U;
        else
            *boundary = final - 1U -
                ((ifs_ext3_u32)remaining & (pointers_per_block - 1U));
    }
    return depth;
}


IfsExt3JournalStatus ifs_ext3_validate_journal_header(
    const ifs_ext3_u32 magic,
    const ifs_ext3_u32 block_type,
    const ifs_ext3_u32 sequence)
{
    if (magic != IFS_EXT3_JOURNAL_MAGIC)
        return IFS_EXT3_JOURNAL_BAD_MAGIC;

    switch (block_type) {
    case IFS_EXT3_JOURNAL_DESCRIPTOR_BLOCK:
    case IFS_EXT3_JOURNAL_COMMIT_BLOCK:
    case IFS_EXT3_JOURNAL_SUPERBLOCK_V1:
    case IFS_EXT3_JOURNAL_SUPERBLOCK_V2:
    case IFS_EXT3_JOURNAL_REVOKE_BLOCK:
        break;
    default:
        return IFS_EXT3_JOURNAL_BAD_TYPE;
    }

    if (sequence == 0U &&
        block_type != IFS_EXT3_JOURNAL_SUPERBLOCK_V1 &&
        block_type != IFS_EXT3_JOURNAL_SUPERBLOCK_V2)
        return IFS_EXT3_JOURNAL_BAD_ARGUMENT;

    return IFS_EXT3_JOURNAL_OK;
}

IfsExt3JournalStatus ifs_ext3_validate_journal_superblock(
    const ifs_ext3_u32 block_size,
    const ifs_ext3_u32 max_length,
    const ifs_ext3_u32 first_block,
    const ifs_ext3_u32 start_block,
    const ifs_ext3_u32 incompat_features)
{
    if (block_size < 1024U || block_size > 65536U ||
        (block_size & (block_size - 1U)) != 0U)
        return IFS_EXT3_JOURNAL_BAD_BLOCK_SIZE;

    if (max_length < 2U || first_block == 0U ||
        first_block >= max_length ||
        (start_block != 0U &&
         (start_block < first_block || start_block >= max_length)))
        return IFS_EXT3_JOURNAL_BAD_GEOMETRY;

    if ((incompat_features &
         ~IFS_EXT3_JOURNAL_FEATURE_INCOMPAT_REVOKE) != 0U)
        return IFS_EXT3_JOURNAL_UNSUPPORTED_FEATURE;

    return IFS_EXT3_JOURNAL_OK;
}

