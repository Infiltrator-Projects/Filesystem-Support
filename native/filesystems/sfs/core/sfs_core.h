#ifndef INFILTRATOR_SFS_CORE_H
#define INFILTRATOR_SFS_CORE_H
#if defined(__KERNEL__)
#include <linux/types.h>
typedef u8 ifs_sfs_u8;
typedef u16 ifs_sfs_u16;
typedef s32 ifs_sfs_i32;
typedef s64 ifs_sfs_i64;
typedef u32 ifs_sfs_u32;
typedef u64 ifs_sfs_u64;
#else
#include <stdint.h>
typedef uint8_t ifs_sfs_u8;
typedef uint16_t ifs_sfs_u16;
typedef int32_t ifs_sfs_i32;
typedef int64_t ifs_sfs_i64;
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
IfsSfsRootStatus ifs_sfs_validate_root_probe(
    ifs_sfs_u32 id,
    ifs_sfs_u32 version,
    ifs_sfs_u32 block_size,
    ifs_sfs_u32 total_blocks);

IfsSfsRootStatus ifs_sfs_validate_root_layout(
    ifs_sfs_u32 id, ifs_sfs_u32 version, ifs_sfs_u32 block_size,
    ifs_sfs_u32 total_blocks, ifs_sfs_u32 bitmap_base,
    ifs_sfs_u32 adminspace_container, ifs_sfs_u32 root_object_container,
    ifs_sfs_u32 extent_bnode_root, ifs_sfs_u32 object_node_root);
int ifs_sfs_compute_bitmap_layout(
    ifs_sfs_u32 block_size, ifs_sfs_u32 total_blocks,
    ifs_sfs_u32 *blocks_per_bitmap, ifs_sfs_u32 *bitmap_block_count);
const char *ifs_sfs_root_status_string(IfsSfsRootStatus status);

/*
 * SFS stores redundant root blocks. A valid copy with the numerically
 * highest sequence number is current. Return 0 for primary, 1 for backup,
 * and -1 when neither copy is valid.
 */
int ifs_sfs_select_root_copy(
    int primary_valid,
    ifs_sfs_u32 primary_sequence,
    int backup_valid,
    ifs_sfs_u32 backup_sequence);

#define IFS_SFS_MAX_FILENAME 105U

typedef enum IfsSfsNameStatus {
    IFS_SFS_NAME_OK = 0,
    IFS_SFS_NAME_TOO_LONG,
    IFS_SFS_NAME_INVALID_CHARACTER
} IfsSfsNameStatus;

IfsSfsNameStatus ifs_sfs_validate_name(
    const ifs_sfs_u8 *name,
    ifs_sfs_u32 length);

ifs_sfs_u8 ifs_sfs_fold_character(ifs_sfs_u8 character);
ifs_sfs_u8 ifs_sfs_lower_character(ifs_sfs_u8 character);

ifs_sfs_u16 ifs_sfs_component_hash(
    const ifs_sfs_u8 *name,
    int case_sensitive);


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

int ifs_sfs_free_count_after_allocate(
    ifs_sfs_u32 current_free,
    ifs_sfs_u32 allocated_blocks,
    ifs_sfs_u32 *new_free);

int ifs_sfs_free_count_after_release(
    ifs_sfs_u32 current_free,
    ifs_sfs_u32 released_blocks,
    ifs_sfs_u32 total_blocks,
    ifs_sfs_u32 *new_free);

ifs_sfs_u32 ifs_sfs_calculate_block_checksum(
    const unsigned char *block,
    ifs_sfs_u32 block_size);

int ifs_sfs_validate_block_header(
    const unsigned char *block,
    ifs_sfs_u32 block_size,
    ifs_sfs_u32 expected_block_number,
    ifs_sfs_u32 expected_block_id);

#define IFS_SFS_NODE_CONTAINER_FIXED_SIZE 20U
#define IFS_SFS_OBJECT_NODE_SIZE 10U
#define IFS_SFS_NODE_INDEX_ENTRY_SIZE 4U

int ifs_sfs_node_leaf_slot(
    ifs_sfs_u32 block_size,
    ifs_sfs_u32 base_node,
    ifs_sfs_u32 target_node,
    ifs_sfs_u32 *slot);

int ifs_sfs_node_index_slot(
    ifs_sfs_u32 block_size,
    ifs_sfs_u32 base_node,
    ifs_sfs_u32 nodes_per_entry,
    ifs_sfs_u32 target_node,
    ifs_sfs_u32 *slot);

#define IFS_SFS_BNODE_CONTAINER_FIXED_SIZE 16U
#define IFS_SFS_BTREE_INTERNAL_NODE_MIN_SIZE 8U
#define IFS_SFS_BTREE_EXTENT_NODE_MIN_SIZE 14U

int ifs_sfs_validate_btree_layout(
    ifs_sfs_u32 block_size,
    ifs_sfs_u32 node_count,
    ifs_sfs_u32 node_size,
    int is_leaf,
    ifs_sfs_u32 *capacity);

int ifs_sfs_validate_extent(
    ifs_sfs_u32 key,
    ifs_sfs_u32 next,
    ifs_sfs_u32 block_count,
    ifs_sfs_u32 total_blocks);

int ifs_sfs_adjust_counter(
    ifs_sfs_u32 current_value,
    ifs_sfs_i32 delta,
    ifs_sfs_u32 *result);
#endif
