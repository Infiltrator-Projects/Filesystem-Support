#ifndef INFILTRATR_EXT2_CORE_H
#define INFILTRATR_EXT2_CORE_H

/*
 * Canonical EXT2 on-disk semantics shared by every operating-system adapter.
 *
 * This interface deliberately contains no Linux VFS or Windows IFS types.
 * The same source is compiled into the Linux EXT2 module, userspace
 * qualification tools and the Windows EXT2 driver.
 */

#if defined(__KERNEL__)
#include <linux/types.h>
typedef u8 ifs_ext2_u8;
typedef u16 ifs_ext2_u16;
typedef u32 ifs_ext2_u32;
typedef u64 ifs_ext2_u64;
typedef size_t ifs_ext2_size_t;
#elif defined(IFS_EXT2_WINDOWS_KERNEL)
/*
 * Keep the canonical engine free of the user-mode MSVC CRT. WDK kernel
 * translation units define this contract explicitly and use fundamental C
 * integer types whose widths are fixed by the supported x64/ARM64 ABIs.
 */
typedef unsigned char ifs_ext2_u8;
typedef unsigned short ifs_ext2_u16;
typedef unsigned int ifs_ext2_u32;
typedef unsigned long long ifs_ext2_u64;
typedef unsigned long long ifs_ext2_size_t;
#else
#include <stddef.h>
#include <stdint.h>
typedef uint8_t ifs_ext2_u8;
typedef uint16_t ifs_ext2_u16;
typedef uint32_t ifs_ext2_u32;
typedef uint64_t ifs_ext2_u64;
typedef size_t ifs_ext2_size_t;
#endif

#define IFS_EXT2_SUPERBLOCK_SIZE 1024U
#define IFS_EXT2_SUPER_MAGIC 0xEF53U
#define IFS_EXT2_MIN_BLOCK_SIZE 1024U
#define IFS_EXT2_MAX_BLOCK_SIZE 65536U
#define IFS_EXT2_GOOD_OLD_INODE_SIZE 128U
#define IFS_EXT2_GOOD_OLD_FIRST_INO 11U
#define IFS_EXT2_MAX_REVISION 1U
#define IFS_EXT2_NULL ((void *)0)

#define IFS_EXT2_NDIR_BLOCKS 12U
#define IFS_EXT2_IND_BLOCK 12U
#define IFS_EXT2_DIND_BLOCK 13U
#define IFS_EXT2_TIND_BLOCK 14U
#define IFS_EXT2_N_BLOCKS 15U

#define IFS_EXT2_XATTR_MAGIC 0xEA020000U
#define IFS_EXT2_XATTR_REFCOUNT_MAX 1024U
#define IFS_EXT2_XATTR_HEADER_SIZE 32U
#define IFS_EXT2_XATTR_ENTRY_FIXED_SIZE 16U
#define IFS_EXT2_XATTR_SENTINEL_SIZE 4U
#define IFS_EXT2_XATTR_ALIGNMENT 4U

#define IFS_EXT2_ACL_HEADER_SIZE 4U
#define IFS_EXT2_ACL_SHORT_ENTRY_SIZE 4U
#define IFS_EXT2_ACL_FULL_ENTRY_SIZE 8U

#define IFS_EXT2_FEATURE_COMPAT_HAS_JOURNAL 0x0004U
#define IFS_EXT2_FEATURE_COMPAT_EXT_ATTR 0x0008U

#define IFS_EXT2_FEATURE_INCOMPAT_FILETYPE 0x0002U
#define IFS_EXT2_FEATURE_INCOMPAT_RECOVER 0x0004U
#define IFS_EXT2_FEATURE_INCOMPAT_JOURNAL_DEV 0x0008U
#define IFS_EXT2_FEATURE_INCOMPAT_META_BG 0x0010U
#define IFS_EXT2_FEATURE_INCOMPAT_SUPPORTED \
    (IFS_EXT2_FEATURE_INCOMPAT_FILETYPE | IFS_EXT2_FEATURE_INCOMPAT_META_BG)

#define IFS_EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER 0x0001U
#define IFS_EXT2_FEATURE_RO_COMPAT_LARGE_FILE 0x0002U
#define IFS_EXT2_FEATURE_RO_COMPAT_BTREE_DIR 0x0004U
#define IFS_EXT2_FEATURE_RO_COMPAT_SUPPORTED \
    (IFS_EXT2_FEATURE_RO_COMPAT_SPARSE_SUPER | \
     IFS_EXT2_FEATURE_RO_COMPAT_LARGE_FILE | \
     IFS_EXT2_FEATURE_RO_COMPAT_BTREE_DIR)

typedef enum IfsExt2Status {
    IFS_EXT2_OK = 0,
    IFS_EXT2_ERROR_ARGUMENT,
    IFS_EXT2_ERROR_MAGIC,
    IFS_EXT2_ERROR_REVISION,
    IFS_EXT2_ERROR_JOURNALLED,
    IFS_EXT2_ERROR_FEATURES,
    IFS_EXT2_ERROR_BLOCK_SIZE,
    IFS_EXT2_ERROR_FRAGMENT_SIZE,
    IFS_EXT2_ERROR_INODE_SIZE,
    IFS_EXT2_ERROR_GEOMETRY,
    IFS_EXT2_ERROR_RANGE,
    IFS_EXT2_ERROR_CORRUPT,
    IFS_EXT2_ERROR_IO,
    IFS_EXT2_ERROR_UNSUPPORTED,
    IFS_EXT2_ERROR_BUFFER_TOO_SMALL,
    IFS_EXT2_ERROR_NOT_FOUND,
    IFS_EXT2_ERROR_NOT_DIRECTORY,
    IFS_EXT2_ERROR_IS_DIRECTORY,
    IFS_EXT2_ERROR_NO_SPACE,
    IFS_EXT2_STOP
} IfsExt2Status;

typedef struct IfsExt2Superblock {
    ifs_ext2_u32 inodes_count;
    ifs_ext2_u32 blocks_count;
    ifs_ext2_u32 reserved_blocks_count;
    ifs_ext2_u32 free_blocks_count;
    ifs_ext2_u32 free_inodes_count;
    ifs_ext2_u32 first_data_block;
    ifs_ext2_u32 log_block_size;
    ifs_ext2_u32 log_fragment_size;
    ifs_ext2_u32 blocks_per_group;
    ifs_ext2_u32 fragments_per_group;
    ifs_ext2_u32 inodes_per_group;
    ifs_ext2_u16 state;
    ifs_ext2_u16 errors;
    ifs_ext2_u32 revision;
    ifs_ext2_u32 first_inode;
    ifs_ext2_u16 inode_size;
    ifs_ext2_u32 feature_compat;
    ifs_ext2_u32 feature_incompat;
    ifs_ext2_u32 feature_ro_compat;
    ifs_ext2_u32 first_meta_bg;
    ifs_ext2_u32 block_size;
    ifs_ext2_u32 inodes_per_block;
    ifs_ext2_u32 inode_table_blocks_per_group;
    ifs_ext2_u32 group_count;
} IfsExt2Superblock;

typedef struct IfsExt2GroupDescriptor {
    ifs_ext2_u32 block_bitmap;
    ifs_ext2_u32 inode_bitmap;
    ifs_ext2_u32 inode_table;
    ifs_ext2_u16 free_blocks_count;
    ifs_ext2_u16 free_inodes_count;
    ifs_ext2_u16 used_dirs_count;
} IfsExt2GroupDescriptor;

typedef struct IfsExt2BlockPath {
    ifs_ext2_u32 offsets[4];
    ifs_ext2_u32 depth;
    ifs_ext2_u32 boundary;
} IfsExt2BlockPath;


typedef enum IfsExt2DirectoryRecordStatus {
    IFS_EXT2_DIRECTORY_RECORD_OK = 0,
    IFS_EXT2_DIRECTORY_RECORD_TOO_SHORT,
    IFS_EXT2_DIRECTORY_RECORD_UNALIGNED,
    IFS_EXT2_DIRECTORY_RECORD_NAME_TOO_LONG,
    IFS_EXT2_DIRECTORY_RECORD_CROSSES_BLOCK,
    IFS_EXT2_DIRECTORY_RECORD_INODE_RANGE
} IfsExt2DirectoryRecordStatus;

IfsExt2Status ifs_ext2_decode_superblock(
    const void *raw_superblock,
    ifs_ext2_size_t raw_size,
    IfsExt2Superblock *superblock);

IfsExt2Status ifs_ext2_validate_superblock(
    IfsExt2Superblock *superblock,
    ifs_ext2_u64 device_blocks,
    int writable);

IfsExt2Status ifs_ext2_decode_group_descriptor(
    const void *raw_descriptor,
    ifs_ext2_size_t raw_size,
    IfsExt2GroupDescriptor *descriptor);

IfsExt2Status ifs_ext2_group_bounds(
    const IfsExt2Superblock *superblock,
    ifs_ext2_u32 group,
    ifs_ext2_u64 *first_block,
    ifs_ext2_u64 *last_block);

IfsExt2Status ifs_ext2_block_group_position(
    ifs_ext2_u32 first_data_block,
    ifs_ext2_u32 blocks_per_group,
    ifs_ext2_u32 blocks_count,
    ifs_ext2_u32 block,
    ifs_ext2_u32 *group,
    ifs_ext2_u32 *offset);

int ifs_ext2_sparse_super_group(ifs_ext2_u32 group);
int ifs_ext2_group_has_super(int sparse_super_enabled, ifs_ext2_u32 group);

IfsExt2Status ifs_ext2_validate_group_descriptor(
    const IfsExt2Superblock *superblock,
    ifs_ext2_u32 group,
    const IfsExt2GroupDescriptor *descriptor);

IfsExt2Status ifs_ext2_block_to_path(
    ifs_ext2_u32 block_size,
    ifs_ext2_u64 logical_block,
    IfsExt2BlockPath *path);

IfsExt2Status ifs_ext2_validate_group_metadata_bitmap(
    const IfsExt2Superblock *superblock,
    ifs_ext2_u32 group,
    const IfsExt2GroupDescriptor *descriptor,
    const void *block_bitmap,
    ifs_ext2_u32 bitmap_size);

int ifs_ext2_data_block_range_valid(
    ifs_ext2_u32 first_data_block,
    ifs_ext2_u64 blocks_count,
    ifs_ext2_u64 superblock_block,
    ifs_ext2_u64 start,
    ifs_ext2_u32 count);

IfsExt2Status ifs_ext2_validate_xattr_block(
    const void *block,
    ifs_ext2_u32 block_size);

int ifs_ext2_xattr_name_compare(
    ifs_ext2_u8 left_index,
    const void *left_name,
    ifs_ext2_u32 left_length,
    ifs_ext2_u8 right_index,
    const void *right_name,
    ifs_ext2_u32 right_length);

IfsExt2Status ifs_ext2_xattr_packed_layout(
    ifs_ext2_u32 block_size,
    ifs_ext2_u32 entry_offset,
    ifs_ext2_u32 name_length,
    ifs_ext2_u32 value_length,
    ifs_ext2_u32 value_cursor,
    ifs_ext2_u32 *next_entry_offset,
    ifs_ext2_u32 *next_value_cursor);

ifs_ext2_u32 ifs_ext2_xattr_entry_hash(
    const void *name,
    ifs_ext2_u32 name_length,
    const void *value,
    ifs_ext2_u32 value_length);

IfsExt2Status ifs_ext2_xattr_block_hash(
    const void *block,
    ifs_ext2_u32 block_size,
    ifs_ext2_u32 *hash);

ifs_ext2_size_t ifs_ext2_acl_size(int count);
int ifs_ext2_acl_count(ifs_ext2_size_t size);

ifs_ext2_u32 ifs_ext2_directory_record_required_length(
    ifs_ext2_u32 name_length);

int ifs_ext2_directory_record_can_insert(
    ifs_ext2_u32 record_length,
    ifs_ext2_u32 existing_name_length,
    ifs_ext2_u32 existing_inode_number,
    ifs_ext2_u32 requested_name_length,
    ifs_ext2_u32 *occupied_length);

IfsExt2Status ifs_ext2_directory_initial_layout(
    ifs_ext2_u32 block_size,
    ifs_ext2_u32 *dot_record_length,
    ifs_ext2_u32 *dotdot_record_length);

IfsExt2Status ifs_ext2_directory_delete_span(
    ifs_ext2_u32 target_offset,
    ifs_ext2_u32 target_record_length,
    int has_previous,
    ifs_ext2_u32 previous_offset,
    ifs_ext2_u32 block_size,
    ifs_ext2_u32 *span_offset,
    ifs_ext2_u32 *span_length);

ifs_ext2_u32 ifs_ext2_directory_record_length_from_disk(
    ifs_ext2_u16 encoded_length,
    ifs_ext2_u32 maximum_record_length);

IfsExt2Status ifs_ext2_directory_record_length_to_disk(
    ifs_ext2_u32 record_length,
    ifs_ext2_u32 maximum_record_length,
    ifs_ext2_u16 *encoded_length);

IfsExt2DirectoryRecordStatus ifs_ext2_validate_directory_record(
    ifs_ext2_u32 record_offset,
    ifs_ext2_u32 record_length,
    ifs_ext2_u32 name_length,
    ifs_ext2_u32 inode_number,
    ifs_ext2_u32 block_size,
    ifs_ext2_u32 maximum_inode);

const char *ifs_ext2_directory_record_status_string(
    IfsExt2DirectoryRecordStatus status);

const char *ifs_ext2_status_string(IfsExt2Status status);

#endif
