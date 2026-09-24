#ifndef INFILTRATR_EXT3_CORE_H
#define INFILTRATR_EXT3_CORE_H

/*
 * Canonical EXT3 filesystem semantics shared by every operating-system
 * adapter.  This interface contains no Linux VFS or Windows IFS types.
 */

#if defined(__KERNEL__)
#include <linux/types.h>
typedef u8 ifs_ext3_u8;
typedef u16 ifs_ext3_u16;
typedef u32 ifs_ext3_u32;
typedef u64 ifs_ext3_u64;
#elif defined(IFS_EXT3_WINDOWS_KERNEL)
typedef unsigned char ifs_ext3_u8;
typedef unsigned short ifs_ext3_u16;
typedef unsigned int ifs_ext3_u32;
typedef unsigned long long ifs_ext3_u64;
#else
#include <stdint.h>
typedef uint8_t ifs_ext3_u8;
typedef uint16_t ifs_ext3_u16;
typedef uint32_t ifs_ext3_u32;
typedef uint64_t ifs_ext3_u64;
#endif

#define IFS_EXT3_JOURNAL_MAGIC 0xc03b3998U
#define IFS_EXT3_JOURNAL_DESCRIPTOR_BLOCK 1U
#define IFS_EXT3_JOURNAL_COMMIT_BLOCK 2U
#define IFS_EXT3_JOURNAL_SUPERBLOCK_V1 3U
#define IFS_EXT3_JOURNAL_SUPERBLOCK_V2 4U
#define IFS_EXT3_JOURNAL_REVOKE_BLOCK 5U
#define IFS_EXT3_JOURNAL_FEATURE_INCOMPAT_REVOKE 0x00000001U

typedef enum IfsExt3JournalStatus {
    IFS_EXT3_JOURNAL_OK = 0,
    IFS_EXT3_JOURNAL_BAD_ARGUMENT,
    IFS_EXT3_JOURNAL_BAD_MAGIC,
    IFS_EXT3_JOURNAL_BAD_TYPE,
    IFS_EXT3_JOURNAL_BAD_BLOCK_SIZE,
    IFS_EXT3_JOURNAL_BAD_GEOMETRY,
    IFS_EXT3_JOURNAL_UNSUPPORTED_FEATURE
} IfsExt3JournalStatus;

IfsExt3JournalStatus ifs_ext3_validate_journal_header(
    ifs_ext3_u32 magic,
    ifs_ext3_u32 block_type,
    ifs_ext3_u32 sequence);

IfsExt3JournalStatus ifs_ext3_validate_journal_superblock(
    ifs_ext3_u32 block_size,
    ifs_ext3_u32 max_length,
    ifs_ext3_u32 first_block,
    ifs_ext3_u32 start_block,
    ifs_ext3_u32 incompat_features);

#define IFS_EXT3_FEATURE_INCOMPAT_FILETYPE 0x0002U
#define IFS_EXT3_FEATURE_INCOMPAT_RECOVER  0x0004U
#define IFS_EXT3_FEATURE_INCOMPAT_META_BG  0x0010U
#define IFS_EXT3_FEATURE_INCOMPAT_SUPPORTED     (IFS_EXT3_FEATURE_INCOMPAT_FILETYPE |      IFS_EXT3_FEATURE_INCOMPAT_RECOVER |      IFS_EXT3_FEATURE_INCOMPAT_META_BG)

#define IFS_EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001U
#define IFS_EXT3_FEATURE_RO_COMPAT_LARGE_FILE   0x0002U
#define IFS_EXT3_FEATURE_RO_COMPAT_BTREE_DIR    0x0004U
#define IFS_EXT3_FEATURE_RO_COMPAT_SUPPORTED     (IFS_EXT3_FEATURE_RO_COMPAT_SPARSE_SUPER |      IFS_EXT3_FEATURE_RO_COMPAT_LARGE_FILE |      IFS_EXT3_FEATURE_RO_COMPAT_BTREE_DIR)

ifs_ext3_u32 ifs_ext3_unsupported_incompat_features(
    ifs_ext3_u32 feature_incompat);

ifs_ext3_u32 ifs_ext3_unsupported_ro_compat_features(
    ifs_ext3_u32 feature_ro_compat);

typedef enum IfsExt3GeometryStatus {
    IFS_EXT3_GEOMETRY_OK = 0,
    IFS_EXT3_GEOMETRY_INVALID_INODE_SIZE,
    IFS_EXT3_GEOMETRY_FRAGMENT_SIZE_MISMATCH,
    IFS_EXT3_GEOMETRY_ZERO_GROUP_VALUE,
    IFS_EXT3_GEOMETRY_BLOCKS_PER_GROUP_TOO_LARGE,
    IFS_EXT3_GEOMETRY_FRAGMENTS_PER_GROUP_TOO_LARGE,
    IFS_EXT3_GEOMETRY_INODES_PER_GROUP_TOO_LARGE
} IfsExt3GeometryStatus;

IfsExt3GeometryStatus ifs_ext3_validate_geometry(
    ifs_ext3_u32 block_size,
    ifs_ext3_u32 inode_size,
    ifs_ext3_u32 fragment_size,
    ifs_ext3_u32 blocks_per_group,
    ifs_ext3_u32 fragments_per_group,
    ifs_ext3_u32 inodes_per_group);

typedef enum IfsExt3LayoutStatus {
    IFS_EXT3_LAYOUT_OK = 0,
    IFS_EXT3_LAYOUT_INVALID_FIRST_DATA_BLOCK,
    IFS_EXT3_LAYOUT_INVALID_BLOCKS_PER_GROUP
} IfsExt3LayoutStatus;

IfsExt3LayoutStatus ifs_ext3_compute_group_count(
    ifs_ext3_u32 blocks_count,
    ifs_ext3_u32 first_data_block,
    ifs_ext3_u32 blocks_per_group,
    ifs_ext3_u32 *group_count);

typedef enum IfsExt3BlockGroupStatus {
    IFS_EXT3_BLOCK_GROUP_OK = 0,
    IFS_EXT3_BLOCK_GROUP_INVALID_ARGUMENT,
    IFS_EXT3_BLOCK_GROUP_INVALID_GEOMETRY,
    IFS_EXT3_BLOCK_GROUP_OUT_OF_RANGE
} IfsExt3BlockGroupStatus;

IfsExt3BlockGroupStatus ifs_ext3_block_group_position(
    ifs_ext3_u64 block,
    ifs_ext3_u32 first_data_block,
    ifs_ext3_u32 blocks_per_group,
    ifs_ext3_u32 group_count,
    ifs_ext3_u32 *group,
    ifs_ext3_u32 *block_offset);

IfsExt3BlockGroupStatus ifs_ext3_group_bounds(
    ifs_ext3_u32 group,
    ifs_ext3_u32 first_data_block,
    ifs_ext3_u32 blocks_per_group,
    ifs_ext3_u64 blocks_count,
    ifs_ext3_u64 *first_block,
    ifs_ext3_u64 *last_block);

int ifs_ext3_sparse_super_group(ifs_ext3_u32 group);
int ifs_ext3_group_has_super(int sparse_super_enabled, ifs_ext3_u32 group);

ifs_ext3_u32 ifs_ext3_meta_gdb_count(
    ifs_ext3_u32 group,
    ifs_ext3_u32 descriptors_per_block);

#define IFS_EXT3_MAX_DIRECTORY_RECORD_LENGTH 65536U

typedef enum IfsExt3DirectoryRecordStatus {
    IFS_EXT3_DIRECTORY_RECORD_OK = 0,
    IFS_EXT3_DIRECTORY_RECORD_TOO_SHORT,
    IFS_EXT3_DIRECTORY_RECORD_UNALIGNED,
    IFS_EXT3_DIRECTORY_RECORD_NAME_TOO_LONG,
    IFS_EXT3_DIRECTORY_RECORD_CROSSES_BLOCK,
    IFS_EXT3_DIRECTORY_RECORD_INODE_RANGE
} IfsExt3DirectoryRecordStatus;

ifs_ext3_u32 ifs_ext3_directory_record_length_from_disk(
    ifs_ext3_u16 encoded_length,
    ifs_ext3_u32 maximum_record_length);

int ifs_ext3_directory_record_length_to_disk(
    ifs_ext3_u32 record_length,
    ifs_ext3_u32 maximum_record_length,
    ifs_ext3_u16 *encoded_length);

IfsExt3DirectoryRecordStatus ifs_ext3_validate_directory_record(
    ifs_ext3_u32 record_offset,
    ifs_ext3_u32 record_length,
    ifs_ext3_u32 name_length,
    ifs_ext3_u32 inode_number,
    ifs_ext3_u32 block_size,
    ifs_ext3_u32 maximum_inode);

const char *ifs_ext3_directory_record_status_string(
    IfsExt3DirectoryRecordStatus status);


#define IFS_EXT3_HASH_LEGACY 0
#define IFS_EXT3_HASH_HALF_MD4 1
#define IFS_EXT3_HASH_TEA 2
#define IFS_EXT3_HASH_LEGACY_UNSIGNED 3
#define IFS_EXT3_HASH_HALF_MD4_UNSIGNED 4
#define IFS_EXT3_HASH_TEA_UNSIGNED 5

int ifs_ext3_directory_hash(
    const ifs_ext3_u8 *name,
    ifs_ext3_u32 length,
    int version,
    const ifs_ext3_u32 seed[4],
    ifs_ext3_u32 *major_hash,
    ifs_ext3_u32 *minor_hash);

#define IFS_EXT3_NDIR_BLOCKS 12U
#define IFS_EXT3_IND_BLOCK   12U
#define IFS_EXT3_DIND_BLOCK  13U
#define IFS_EXT3_TIND_BLOCK  14U

int ifs_ext3_indirect_block_path(
    ifs_ext3_u64 logical_block,
    ifs_ext3_u32 pointers_per_block,
    ifs_ext3_u32 pointer_bits,
    ifs_ext3_u32 offsets[4],
    ifs_ext3_u32 *boundary);

#endif
