/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Canonical EXT4 format policy shared by all host adapters.
 */

#include "ext4_core.h"

ifs_ext4_u32 ifs_ext4_unsupported_incompat_features(
    const ifs_ext4_u32 feature_incompat)
{
    return feature_incompat & ~IFS_EXT4_FEATURE_INCOMPAT_SUPPORTED;
}

ifs_ext4_u32 ifs_ext4_unsupported_ro_compat_features(
    const ifs_ext4_u32 feature_ro_compat)
{
    return feature_ro_compat & ~IFS_EXT4_FEATURE_RO_COMPAT_SUPPORTED &
           ~IFS_EXT4_FEATURE_RO_COMPAT_READONLY;
}

int ifs_ext4_requires_readonly(const ifs_ext4_u32 feature_ro_compat)
{
    return (feature_ro_compat & IFS_EXT4_FEATURE_RO_COMPAT_READONLY) != 0U;
}

IfsExt4BigallocStatus ifs_ext4_validate_bigalloc(
    const ifs_ext4_u32 feature_incompat,
    const ifs_ext4_u32 feature_ro_compat,
    const ifs_ext4_u32 first_data_block)
{
    if ((feature_ro_compat & IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC) == 0U)
        return IFS_EXT4_BIGALLOC_OK;

    if ((feature_incompat & IFS_EXT4_FEATURE_INCOMPAT_EXTENTS) == 0U)
        return IFS_EXT4_BIGALLOC_REQUIRES_EXTENTS;

    if (first_data_block != 0U)
        return IFS_EXT4_BIGALLOC_INVALID_FIRST_DATA_BLOCK;

    return IFS_EXT4_BIGALLOC_OK;
}

IfsExt4InodeGeometryStatus ifs_ext4_validate_inode_geometry(
    const ifs_ext4_u32 block_size,
    const ifs_ext4_u32 inode_size,
    const ifs_ext4_u32 first_inode)
{
    if (first_inode < 11U)
        return IFS_EXT4_INODE_GEOMETRY_INVALID_FIRST_INODE;

    if (inode_size < 128U || inode_size > block_size ||
        (inode_size & (inode_size - 1U)) != 0U)
        return IFS_EXT4_INODE_GEOMETRY_INVALID_INODE_SIZE;

    return IFS_EXT4_INODE_GEOMETRY_OK;
}

IfsExt4GroupGeometryStatus ifs_ext4_validate_group_geometry(
    const ifs_ext4_u32 block_size,
    const ifs_ext4_u32 inode_size,
    const ifs_ext4_u32 descriptor_size,
    const int has_64bit,
    const ifs_ext4_u32 blocks_per_group,
    const ifs_ext4_u32 inodes_per_group)
{
    const ifs_ext4_u32 inodes_per_block =
        inode_size != 0U ? block_size / inode_size : 0U;

    if (has_64bit &&
        (descriptor_size < 64U || descriptor_size > 1024U ||
         (descriptor_size & (descriptor_size - 1U)) != 0U))
        return IFS_EXT4_GROUP_GEOMETRY_INVALID_DESCRIPTOR_SIZE;

    if (inodes_per_block == 0U || blocks_per_group == 0U)
        return IFS_EXT4_GROUP_GEOMETRY_ZERO_VALUE;

    if (inodes_per_group < inodes_per_block ||
        inodes_per_group > block_size * 8U)
        return IFS_EXT4_GROUP_GEOMETRY_INVALID_INODES_PER_GROUP;

    return IFS_EXT4_GROUP_GEOMETRY_OK;
}

IfsExt4ClusterGeometryStatus ifs_ext4_validate_cluster_geometry(
    const ifs_ext4_u32 block_size,
    const ifs_ext4_u32 cluster_size,
    const int has_bigalloc,
    const ifs_ext4_u32 blocks_per_group,
    const ifs_ext4_u32 clusters_per_group)
{
    if (block_size == 0U || cluster_size == 0U)
        return IFS_EXT4_CLUSTER_GEOMETRY_GROUP_RATIO_MISMATCH;

    if (has_bigalloc) {
        if (cluster_size < block_size)
            return IFS_EXT4_CLUSTER_GEOMETRY_CLUSTER_SMALLER_THAN_BLOCK;
    } else {
        if (cluster_size != block_size)
            return IFS_EXT4_CLUSTER_GEOMETRY_CLUSTER_BLOCK_MISMATCH;
        if (blocks_per_group > block_size * 8U)
            return IFS_EXT4_CLUSTER_GEOMETRY_BLOCKS_PER_GROUP_TOO_LARGE;
    }

    if (clusters_per_group > block_size * 8U)
        return IFS_EXT4_CLUSTER_GEOMETRY_CLUSTERS_PER_GROUP_TOO_LARGE;

    if ((ifs_ext4_u64)blocks_per_group !=
        (ifs_ext4_u64)clusters_per_group *
            ((ifs_ext4_u64)cluster_size / block_size))
        return IFS_EXT4_CLUSTER_GEOMETRY_GROUP_RATIO_MISMATCH;

    return IFS_EXT4_CLUSTER_GEOMETRY_OK;
}

IfsExt4LayoutStatus ifs_ext4_validate_layout(
    const ifs_ext4_u32 block_size,
    const ifs_ext4_u32 reserved_gdt_blocks,
    const ifs_ext4_u64 blocks_count,
    const ifs_ext4_u32 first_data_block,
    const ifs_ext4_u32 log_block_size,
    const ifs_ext4_u32 cluster_ratio,
    const ifs_ext4_u32 blocks_per_group,
    const ifs_ext4_u32 descriptors_per_block,
    const ifs_ext4_u32 inodes_per_group,
    const ifs_ext4_u32 inodes_count,
    ifs_ext4_u64 *const group_count)
{
    if (reserved_gdt_blocks > block_size / 4U)
        return IFS_EXT4_LAYOUT_RESERVED_GDT_TOO_LARGE;

    if (blocks_per_group == 0U || group_count == 0)
        return IFS_EXT4_LAYOUT_GROUP_COUNT_TOO_LARGE;

    if ((ifs_ext4_u64)first_data_block >= blocks_count)
        return IFS_EXT4_LAYOUT_INVALID_FIRST_DATA_BLOCK;

    if (first_data_block == 0U && log_block_size == 0U &&
        cluster_ratio == 1U)
        return IFS_EXT4_LAYOUT_INVALID_1K_FIRST_DATA_BLOCK;

    {
        const ifs_ext4_u64 groups =
            (blocks_count - first_data_block + blocks_per_group - 1U) /
            blocks_per_group;
        const ifs_ext4_u64 max_groups =
            0x100000000ULL - descriptors_per_block;

        *group_count = groups;

        if (groups > max_groups)
            return IFS_EXT4_LAYOUT_GROUP_COUNT_TOO_LARGE;

        if (groups * inodes_per_group != inodes_count)
            return IFS_EXT4_LAYOUT_INVALID_INODE_COUNT;
    }

    return IFS_EXT4_LAYOUT_OK;
}



ifs_ext4_u32 ifs_ext4_directory_record_min_length(
    const ifs_ext4_u32 name_length,
    const int has_hash)
{
    ifs_ext4_u32 length = name_length + 11U;

    if (has_hash != 0)
        length += 8U;
    return length & ~3U;
}

ifs_ext4_u32 ifs_ext4_directory_record_length_from_disk(
    const ifs_ext4_u16 encoded_length,
    const ifs_ext4_u32 block_size)
{
    const ifs_ext4_u32 length = encoded_length;

    if (block_size >= 65536U) {
        if (length == 0xFFFFU || length == 0U)
            return block_size;
        return (length & 65532U) | ((length & 3U) << 16);
    }
    return length;
}

int ifs_ext4_directory_record_length_to_disk(
    const ifs_ext4_u32 record_length,
    const ifs_ext4_u32 block_size,
    ifs_ext4_u16 *const encoded_length)
{
    if (encoded_length == 0)
        return -1;
    if (record_length == 0U || record_length > block_size ||
        block_size > (1U << 18) || (record_length & 3U) != 0U)
        return -1;

    if (block_size >= 65536U) {
        if (record_length < 65536U) {
            *encoded_length = (ifs_ext4_u16)record_length;
            return 0;
        }
        if (record_length == block_size) {
            *encoded_length = block_size == 65536U ? 0xFFFFU : 0U;
            return 0;
        }
        *encoded_length = (ifs_ext4_u16)(
            (record_length & 65532U) | ((record_length >> 16) & 3U));
        return 0;
    }

    if (record_length > 0xFFFFU)
        return -1;
    *encoded_length = (ifs_ext4_u16)record_length;
    return 0;
}

IfsExt4DirectoryRecordStatus ifs_ext4_validate_directory_record(
    const ifs_ext4_u32 record_offset,
    const ifs_ext4_u32 record_length,
    const ifs_ext4_u32 name_length,
    const ifs_ext4_u32 inode_number,
    const ifs_ext4_u32 buffer_size,
    const ifs_ext4_u32 maximum_inode,
    const int entry_has_hash,
    const int trailing_entry_has_hash,
    const int dot_entry)
{
    const ifs_ext4_u32 minimum =
        ifs_ext4_directory_record_min_length(1U, entry_has_hash);
    const ifs_ext4_u32 minimum_for_name =
        ifs_ext4_directory_record_min_length(name_length, entry_has_hash);
    const ifs_ext4_u32 trailing_minimum =
        ifs_ext4_directory_record_min_length(1U, trailing_entry_has_hash);
    ifs_ext4_u64 next_offset;

    if (record_length < minimum)
        return IFS_EXT4_DIRECTORY_RECORD_TOO_SHORT;
    if ((record_length & 3U) != 0U)
        return IFS_EXT4_DIRECTORY_RECORD_UNALIGNED;
    if (record_length < minimum_for_name)
        return IFS_EXT4_DIRECTORY_RECORD_NAME_TOO_LONG;

    next_offset = (ifs_ext4_u64)record_offset + record_length;
    if (next_offset > buffer_size)
        return IFS_EXT4_DIRECTORY_RECORD_OVERRUN;
    if (next_offset != buffer_size &&
        (ifs_ext4_u64)buffer_size - next_offset < trailing_minimum)
        return IFS_EXT4_DIRECTORY_RECORD_TOO_CLOSE_TO_END;
    if (inode_number > maximum_inode)
        return IFS_EXT4_DIRECTORY_RECORD_INODE_RANGE;
    if (next_offset == buffer_size && dot_entry != 0)
        return IFS_EXT4_DIRECTORY_RECORD_DOT_LAST;
    return IFS_EXT4_DIRECTORY_RECORD_OK;
}

const char *ifs_ext4_directory_record_status_string(
    const IfsExt4DirectoryRecordStatus status)
{
    switch (status) {
    case IFS_EXT4_DIRECTORY_RECORD_OK: return "ok";
    case IFS_EXT4_DIRECTORY_RECORD_TOO_SHORT: return "rec_len is smaller than minimal";
    case IFS_EXT4_DIRECTORY_RECORD_UNALIGNED: return "rec_len % 4 != 0";
    case IFS_EXT4_DIRECTORY_RECORD_NAME_TOO_LONG: return "rec_len is too small for name_len";
    case IFS_EXT4_DIRECTORY_RECORD_OVERRUN: return "directory entry overrun";
    case IFS_EXT4_DIRECTORY_RECORD_TOO_CLOSE_TO_END: return "directory entry too close to block end";
    case IFS_EXT4_DIRECTORY_RECORD_INODE_RANGE: return "inode out of bounds";
    case IFS_EXT4_DIRECTORY_RECORD_DOT_LAST: return "'.' directory cannot be the last in data block";
    }
    return "invalid EXT4 directory record";
}

int ifs_ext4_indirect_block_path(
    ifs_ext4_u64 logical_block,
    const ifs_ext4_u32 pointers_per_block,
    const ifs_ext4_u32 pointer_bits,
    ifs_ext4_u32 offsets[4],
    ifs_ext4_u32 *const boundary)
{
    ifs_ext4_u64 remaining = logical_block;
    const ifs_ext4_u64 indirect_blocks = pointers_per_block;
    ifs_ext4_u64 double_blocks;
    ifs_ext4_u64 triple_blocks;
    ifs_ext4_u32 final = 0U;
    int depth = 0;

    if (offsets == 0 || pointers_per_block == 0U ||
        pointer_bits >= 32U ||
        ((ifs_ext4_u64)1U << pointer_bits) != pointers_per_block) {
        if (boundary != 0)
            *boundary = 0U;
        return 0;
    }

    double_blocks = indirect_blocks * indirect_blocks;
    triple_blocks = double_blocks * indirect_blocks;

    if (remaining < IFS_EXT4_NDIR_BLOCKS) {
        offsets[depth++] = (ifs_ext4_u32)remaining;
        final = IFS_EXT4_NDIR_BLOCKS;
    } else {
        remaining -= IFS_EXT4_NDIR_BLOCKS;
        if (remaining < indirect_blocks) {
            offsets[depth++] = IFS_EXT4_IND_BLOCK;
            offsets[depth++] = (ifs_ext4_u32)remaining;
            final = pointers_per_block;
        } else {
            remaining -= indirect_blocks;
            if (remaining < double_blocks) {
                offsets[depth++] = IFS_EXT4_DIND_BLOCK;
                offsets[depth++] = (ifs_ext4_u32)(remaining >> pointer_bits);
                offsets[depth++] = (ifs_ext4_u32)(remaining & (pointers_per_block - 1U));
                final = pointers_per_block;
            } else {
                remaining -= double_blocks;
                if (remaining < triple_blocks) {
                    offsets[depth++] = IFS_EXT4_TIND_BLOCK;
                    offsets[depth++] = (ifs_ext4_u32)(remaining >> (pointer_bits * 2U));
                    offsets[depth++] = (ifs_ext4_u32)((remaining >> pointer_bits) &
                                                      (pointers_per_block - 1U));
                    offsets[depth++] = (ifs_ext4_u32)(remaining & (pointers_per_block - 1U));
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
                ((ifs_ext4_u32)remaining & (pointers_per_block - 1U));
    }
    return depth;
}


IfsExt4BlockGroupStatus ifs_ext4_block_group_position(
    const ifs_ext4_u64 block,
    const ifs_ext4_u32 first_data_block,
    const ifs_ext4_u32 blocks_per_group,
    const ifs_ext4_u32 cluster_bits,
    const ifs_ext4_u32 group_count,
    ifs_ext4_u32 *const group,
    ifs_ext4_u32 *const cluster_offset)
{
    ifs_ext4_u64 relative;
    ifs_ext4_u64 computed_group;
    ifs_ext4_u64 offset;

    if (group == 0 || cluster_offset == 0)
        return IFS_EXT4_BLOCK_GROUP_INVALID_ARGUMENT;
    if (blocks_per_group == 0U || cluster_bits >= 32U)
        return IFS_EXT4_BLOCK_GROUP_INVALID_GEOMETRY;
    if (block < first_data_block)
        return IFS_EXT4_BLOCK_GROUP_OUT_OF_RANGE;

    relative = block - first_data_block;
    computed_group = relative / blocks_per_group;
    if (computed_group >= group_count || computed_group > 0xffffffffULL)
        return IFS_EXT4_BLOCK_GROUP_OUT_OF_RANGE;

    offset = relative % blocks_per_group;
    offset >>= cluster_bits;
    if (offset > 0xffffffffULL)
        return IFS_EXT4_BLOCK_GROUP_INVALID_GEOMETRY;

    *group = (ifs_ext4_u32)computed_group;
    *cluster_offset = (ifs_ext4_u32)offset;
    return IFS_EXT4_BLOCK_GROUP_OK;
}

IfsExt4BlockGroupStatus ifs_ext4_group_bounds(
    const ifs_ext4_u32 group,
    const ifs_ext4_u32 first_data_block,
    const ifs_ext4_u32 blocks_per_group,
    const ifs_ext4_u64 blocks_count,
    ifs_ext4_u64 *const first_block,
    ifs_ext4_u64 *const last_block)
{
    ifs_ext4_u64 first;
    ifs_ext4_u64 last;

    if (first_block == 0 || last_block == 0)
        return IFS_EXT4_BLOCK_GROUP_INVALID_ARGUMENT;
    if (blocks_per_group == 0U || blocks_count <= first_data_block)
        return IFS_EXT4_BLOCK_GROUP_INVALID_GEOMETRY;

    first = (ifs_ext4_u64)first_data_block +
            (ifs_ext4_u64)group * blocks_per_group;
    if (first >= blocks_count)
        return IFS_EXT4_BLOCK_GROUP_OUT_OF_RANGE;

    last = first + blocks_per_group - 1U;
    if (last < first || last >= blocks_count)
        last = blocks_count - 1U;

    *first_block = first;
    *last_block = last;
    return IFS_EXT4_BLOCK_GROUP_OK;
}

static int ifs_ext4_is_power_of(
    ifs_ext4_u32 value, const ifs_ext4_u32 base)
{
    if (value < 1U || base < 2U)
        return 0;
    while (value > 1U && value % base == 0U)
        value /= base;
    return value == 1U;
}

int ifs_ext4_sparse_super_group(const ifs_ext4_u32 group)
{
    if (group <= 1U)
        return 1;
    return ifs_ext4_is_power_of(group, 3U) ||
           ifs_ext4_is_power_of(group, 5U) ||
           ifs_ext4_is_power_of(group, 7U);
}

int ifs_ext4_group_has_super(
    const int sparse_super_enabled, const ifs_ext4_u32 group)
{
    return sparse_super_enabled == 0 ||
           ifs_ext4_sparse_super_group(group);
}


int ifs_ext4_group_has_super_ex(
    const int sparse_super_enabled,
    const int sparse_super2_enabled,
    const ifs_ext4_u32 backup_group0,
    const ifs_ext4_u32 backup_group1,
    const ifs_ext4_u32 group)
{
    if (group == 0U)
        return 1;

    if (sparse_super2_enabled != 0)
        return group == backup_group0 || group == backup_group1;

    return ifs_ext4_group_has_super(sparse_super_enabled, group);
}


static ifs_ext4_u32 ifs_ext4_rol32(const ifs_ext4_u32 value,
                                    const unsigned int shift)
{
    return (value << shift) | (value >> (32U - shift));
}

static ifs_ext4_u32 ifs_ext4_hash_boolean(
    const unsigned int round,
    const ifs_ext4_u32 x,
    const ifs_ext4_u32 y,
    const ifs_ext4_u32 z)
{
    switch (round) {
    case 0U:
        return z ^ (x & (y ^ z));
    case 1U:
        return (x & y) + ((x ^ y) & z);
    default:
        return x ^ y ^ z;
    }
}

static void ifs_ext4_half_md4_rounds(
    ifs_ext4_u32 state[4], const ifs_ext4_u32 words[8])
{
    static const unsigned char word_order[3][8] = {
        { 0U, 1U, 2U, 3U, 4U, 5U, 6U, 7U },
        { 1U, 3U, 5U, 7U, 0U, 2U, 4U, 6U },
        { 3U, 7U, 2U, 6U, 1U, 5U, 0U, 4U }
    };
    static const unsigned char rotations[3][4] = {
        { 3U, 7U, 11U, 19U },
        { 3U, 5U, 9U, 13U },
        { 3U, 9U, 11U, 15U }
    };
    static const ifs_ext4_u32 constants[3] = {
        0U, 0x5a827999U, 0x6ed9eba1U
    };
    static const unsigned char target_order[4] = { 0U, 3U, 2U, 1U };
    ifs_ext4_u32 work[4];
    unsigned int round;
    unsigned int step;

    work[0] = state[0];
    work[1] = state[1];
    work[2] = state[2];
    work[3] = state[3];

    for (round = 0U; round < 3U; ++round) {
        for (step = 0U; step < 8U; ++step) {
            const unsigned int target = target_order[step & 3U];
            const unsigned int x_index = (target + 1U) & 3U;
            const unsigned int y_index = (target + 2U) & 3U;
            const unsigned int z_index = (target + 3U) & 3U;
            const ifs_ext4_u32 mixed = ifs_ext4_hash_boolean(
                round, work[x_index], work[y_index], work[z_index]);
            const ifs_ext4_u32 sum =
                work[target] + mixed +
                words[word_order[round][step]] + constants[round];

            work[target] = ifs_ext4_rol32(
                sum, rotations[round][step & 3U]);
        }
    }

    state[0] += work[0];
    state[1] += work[1];
    state[2] += work[2];
    state[3] += work[3];
}

static void ifs_ext4_tea_hash_rounds(
    ifs_ext4_u32 state[4], const ifs_ext4_u32 words[4])
{
    ifs_ext4_u32 left = state[0];
    ifs_ext4_u32 right = state[1];
    ifs_ext4_u32 sum = 0U;
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

static void ifs_ext4_pack_hash_words(
    const unsigned char *name,
    const ifs_ext4_u32 remaining,
    ifs_ext4_u32 *words,
    const unsigned int word_count,
    const int signed_bytes)
{
    const ifs_ext4_u32 capacity = (ifs_ext4_u32)word_count * 4U;
    const ifs_ext4_u32 used = remaining < capacity ? remaining : capacity;
    ifs_ext4_u32 pad = remaining | (remaining << 8);
    ifs_ext4_u32 value;
    unsigned int word = 0U;
    ifs_ext4_u32 index;

    pad |= pad << 16;
    value = pad;

    for (index = 0U; index < used; ++index) {
        int byte_value;

        if (signed_bytes != 0)
            byte_value = (int)(signed char)name[index];
        else
            byte_value = (int)name[index];

        value = (ifs_ext4_u32)byte_value + (value << 8);
        if ((index & 3U) == 3U) {
            words[word++] = value;
            value = pad;
        }
    }

    if (word < word_count)
        words[word++] = value;
    while (word < word_count)
        words[word++] = pad;
}

static ifs_ext4_u32 ifs_ext4_legacy_directory_hash(
    const unsigned char *name,
    const ifs_ext4_u32 name_length,
    const int signed_bytes)
{
    ifs_ext4_u32 previous = 0x37abe8f9U;
    ifs_ext4_u32 current = 0x12a3fe2dU;
    ifs_ext4_u32 index;

    for (index = 0U; index < name_length; ++index) {
        int byte_value;
        ifs_ext4_u32 next;

        if (signed_bytes != 0)
            byte_value = (int)(signed char)name[index];
        else
            byte_value = (int)name[index];

        next = previous +
               (current ^ ((ifs_ext4_u32)byte_value * 7152373U));
        if ((next & 0x80000000U) != 0U)
            next -= 0x7fffffffU;

        previous = current;
        current = next;
    }

    return current << 1;
}

int ifs_ext4_directory_hash(
    const unsigned char *name,
    const ifs_ext4_u32 name_length,
    const ifs_ext4_u32 hash_version,
    const ifs_ext4_u32 seed[4],
    ifs_ext4_u32 *major_hash,
    ifs_ext4_u32 *minor_hash)
{
    ifs_ext4_u32 state[4] = {
        0x67452301U, 0xefcdab89U, 0x98badcfeU, 0x10325476U
    };
    ifs_ext4_u32 major = 0U;
    ifs_ext4_u32 minor = 0U;
    ifs_ext4_u32 offset = 0U;
    int signed_bytes;

    if (name == 0 || major_hash == 0 || minor_hash == 0)
        return -1;

    if (seed != 0 &&
        (seed[0] != 0U || seed[1] != 0U ||
         seed[2] != 0U || seed[3] != 0U)) {
        state[0] = seed[0];
        state[1] = seed[1];
        state[2] = seed[2];
        state[3] = seed[3];
    }

    switch (hash_version) {
    case IFS_EXT4_HASH_LEGACY:
    case IFS_EXT4_HASH_LEGACY_UNSIGNED:
        signed_bytes = hash_version == IFS_EXT4_HASH_LEGACY;
        major = ifs_ext4_legacy_directory_hash(
            name, name_length, signed_bytes);
        break;

    case IFS_EXT4_HASH_HALF_MD4:
    case IFS_EXT4_HASH_HALF_MD4_UNSIGNED:
        signed_bytes = hash_version == IFS_EXT4_HASH_HALF_MD4;
        while (offset < name_length) {
            ifs_ext4_u32 words[8];
            const ifs_ext4_u32 remaining = name_length - offset;

            ifs_ext4_pack_hash_words(
                name + offset, remaining, words, 8U, signed_bytes);
            ifs_ext4_half_md4_rounds(state, words);
            offset += remaining > 32U ? 32U : remaining;
        }
        major = state[1];
        minor = state[2];
        break;

    case IFS_EXT4_HASH_TEA:
    case IFS_EXT4_HASH_TEA_UNSIGNED:
        signed_bytes = hash_version == IFS_EXT4_HASH_TEA;
        while (offset < name_length) {
            ifs_ext4_u32 words[4];
            const ifs_ext4_u32 remaining = name_length - offset;

            ifs_ext4_pack_hash_words(
                name + offset, remaining, words, 4U, signed_bytes);
            ifs_ext4_tea_hash_rounds(state, words);
            offset += remaining > 16U ? 16U : remaining;
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
