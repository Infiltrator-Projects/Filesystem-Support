/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef INFILTRATOR_SFS_CORE_H
#define INFILTRATOR_SFS_CORE_H
#if defined(__KERNEL__)
#include <linux/types.h>
typedef u32 ifs_sfs_u32;
typedef u64 ifs_sfs_u64;
#else
#include <stdint.h>
typedef uint32_t ifs_sfs_u32;
typedef uint64_t ifs_sfs_u64;
#endif
#define IFS_SFS_ROOT_ID 0x53465300U
#define IFS_SFS_STRUCTURE_VERSION 3U
#define IFS_SFS_MIN_BLOCK_SIZE 512U
#define IFS_SFS_BLOCK_HEADER_SIZE 12U
typedef enum IfsSfsRootStatus {
    IFS_SFS_ROOT_OK = 0,
    IFS_SFS_ROOT_BAD_ID,
    IFS_SFS_ROOT_BAD_VERSION,
    IFS_SFS_ROOT_INVALID_BLOCK_SIZE,
    IFS_SFS_ROOT_INVALID_TOTAL_BLOCKS,
    IFS_SFS_ROOT_BLOCK_REFERENCE_OUT_OF_RANGE,
    IFS_SFS_ROOT_TRANSACTION_BLOCK_OUT_OF_RANGE
} IfsSfsRootStatus;
IfsSfsRootStatus ifs_sfs_validate_root_layout(
    ifs_sfs_u32 id, ifs_sfs_u32 version, ifs_sfs_u32 block_size,
    ifs_sfs_u32 total_blocks, ifs_sfs_u32 bitmap_base,
    ifs_sfs_u32 adminspace_container, ifs_sfs_u32 root_object_container,
    ifs_sfs_u32 extent_bnode_root, ifs_sfs_u32 object_node_root);
int ifs_sfs_compute_bitmap_layout(
    ifs_sfs_u32 block_size, ifs_sfs_u32 total_blocks,
    ifs_sfs_u32 *blocks_per_bitmap, ifs_sfs_u32 *bitmap_block_count);
const char *ifs_sfs_root_status_string(IfsSfsRootStatus status);

#define IFS_SFS_MAX_FILENAME 105U

typedef enum IfsSfsObjectRecordStatus {
    IFS_SFS_OBJECT_RECORD_OK = 0,
    IFS_SFS_OBJECT_RECORD_INVALID_ARGUMENT,
    IFS_SFS_OBJECT_RECORD_NAME_UNTERMINATED,
    IFS_SFS_OBJECT_RECORD_NAME_TOO_LONG,
    IFS_SFS_OBJECT_RECORD_COMMENT_UNTERMINATED,
    IFS_SFS_OBJECT_RECORD_SIZE_OVERFLOW
} IfsSfsObjectRecordStatus;

IfsSfsObjectRecordStatus ifs_sfs_object_record_layout(
    const unsigned char *name_and_comment,
    ifs_sfs_u32 available_tail_bytes,
    ifs_sfs_u32 fixed_prefix_bytes,
    ifs_sfs_u32 *record_bytes,
    ifs_sfs_u32 *name_bytes);

const char *ifs_sfs_object_record_status_string(
    IfsSfsObjectRecordStatus status);

int ifs_sfs_has_allocation_headroom(
    ifs_sfs_u32 free_blocks,
    ifs_sfs_u32 requested_blocks,
    ifs_sfs_u32 always_free_blocks);

int ifs_sfs_adminspace_block_mask(
    ifs_sfs_u32 area_start,
    ifs_sfs_u32 block,
    ifs_sfs_u32 *mask);

int ifs_sfs_bitmap_word_find_set(
    ifs_sfs_u32 word,
    ifs_sfs_u32 start_bit);

int ifs_sfs_bitmap_word_find_zero(
    ifs_sfs_u32 word,
    ifs_sfs_u32 start_bit);

ifs_sfs_u32 ifs_sfs_bitmap_word_set(
    ifs_sfs_u32 word,
    ifs_sfs_u32 start_bit,
    ifs_sfs_u32 bit_count);

ifs_sfs_u32 ifs_sfs_bitmap_word_clear(
    ifs_sfs_u32 word,
    ifs_sfs_u32 start_bit,
    ifs_sfs_u32 bit_count);
#endif
