/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFILTRATOR_SFS2_CORE_H
#define INFILTRATOR_SFS2_CORE_H

#if defined(__KERNEL__)
#include <linux/types.h>
typedef u16 ifs_sfs2_u16;
typedef u32 ifs_sfs2_u32;
typedef u64 ifs_sfs2_u64;
#else
#include <stdint.h>
typedef uint16_t ifs_sfs2_u16;
typedef uint32_t ifs_sfs2_u32;
typedef uint64_t ifs_sfs2_u64;
#endif

#define IFS_SFS2_ROOT_ID 0x53465302U
#define IFS_SFS2_STRUCTURE_VERSION 4U
#define IFS_SFS2_OBJECT_FIXED_SIZE 27U
#define IFS_SFS2_EXTENT_NODE_SIZE 16U
#define IFS_SFS2_MAX_FILE_SIZE 0x0000FFFFFFFFFFFFULL
#define IFS_SFS2_MAX_FILENAME 107U
#define IFS_SFS2_BLOCK_HEADER_SIZE 12U
#define IFS_SFS2_ROOT_BYTES 128U

typedef struct IfsSfs2RootRecord {
    ifs_sfs2_u32 block_id;
    ifs_sfs2_u32 block_checksum;
    ifs_sfs2_u32 block_self_pointer;
    ifs_sfs2_u16 version;
    ifs_sfs2_u16 sequence;
    ifs_sfs2_u32 date_created;
    unsigned char bits;
    ifs_sfs2_u64 first_byte;
    ifs_sfs2_u64 last_byte;
    ifs_sfs2_u32 total_blocks;
    ifs_sfs2_u32 block_size;
    ifs_sfs2_u32 bitmap_base;
    ifs_sfs2_u32 adminspace_container;
    ifs_sfs2_u32 root_object_container;
    ifs_sfs2_u32 extent_bnode_root;
    ifs_sfs2_u32 object_node_root;
} IfsSfs2RootRecord;

typedef enum IfsSfs2RootStatus {
    IFS_SFS2_ROOT_OK = 0,
    IFS_SFS2_ROOT_BAD_ID,
    IFS_SFS2_ROOT_BAD_VERSION,
    IFS_SFS2_ROOT_INVALID_BLOCK_SIZE,
    IFS_SFS2_ROOT_INVALID_TOTAL_BLOCKS,
    IFS_SFS2_ROOT_BLOCK_REFERENCE_OUT_OF_RANGE
} IfsSfs2RootStatus;

int ifs_sfs2_decode_root(
    const unsigned char *bytes,
    ifs_sfs2_u32 byte_count,
    IfsSfs2RootRecord *root);

int ifs_sfs2_validate_root_record(const IfsSfs2RootRecord *root);

IfsSfs2RootStatus ifs_sfs2_validate_root_layout(
    ifs_sfs2_u32 id,
    ifs_sfs2_u32 version,
    ifs_sfs2_u32 block_size,
    ifs_sfs2_u32 total_blocks,
    ifs_sfs2_u32 bitmap_base,
    ifs_sfs2_u32 adminspace_container,
    ifs_sfs2_u32 root_object_container,
    ifs_sfs2_u32 extent_bnode_root,
    ifs_sfs2_u32 object_node_root);

ifs_sfs2_u64 ifs_sfs2_decode_file_size(
    ifs_sfs2_u32 high_32,
    ifs_sfs2_u16 low_16);

int ifs_sfs2_encode_file_size(
    ifs_sfs2_u64 file_size,
    ifs_sfs2_u32 *high_32,
    ifs_sfs2_u16 *low_16);

const char *ifs_sfs2_root_status_string(IfsSfs2RootStatus status);

int ifs_sfs2_select_root_copy(
    int primary_valid,
    ifs_sfs2_u32 primary_sequence,
    int backup_valid,
    ifs_sfs2_u32 backup_sequence);

ifs_sfs2_u32 ifs_sfs2_calculate_block_checksum(
    const unsigned char *block,
    ifs_sfs2_u32 block_size);

int ifs_sfs2_validate_block_header(
    const unsigned char *block,
    ifs_sfs2_u32 block_size,
    ifs_sfs2_u32 expected_block_number,
    ifs_sfs2_u32 expected_block_id);

typedef enum IfsSfs2ObjectRecordStatus {
    IFS_SFS2_OBJECT_RECORD_OK = 0,
    IFS_SFS2_OBJECT_RECORD_INVALID_ARGUMENT,
    IFS_SFS2_OBJECT_RECORD_NAME_UNTERMINATED,
    IFS_SFS2_OBJECT_RECORD_NAME_TOO_LONG,
    IFS_SFS2_OBJECT_RECORD_COMMENT_UNTERMINATED,
    IFS_SFS2_OBJECT_RECORD_SIZE_OVERFLOW
} IfsSfs2ObjectRecordStatus;

IfsSfs2ObjectRecordStatus ifs_sfs2_object_record_layout(
    const unsigned char *name_and_comment,
    ifs_sfs2_u32 available_tail_bytes,
    ifs_sfs2_u32 *record_bytes,
    ifs_sfs2_u32 *name_bytes);

int ifs_sfs2_validate_extent(
    ifs_sfs2_u32 key,
    ifs_sfs2_u32 next,
    ifs_sfs2_u32 block_count,
    ifs_sfs2_u32 total_blocks);

#endif
