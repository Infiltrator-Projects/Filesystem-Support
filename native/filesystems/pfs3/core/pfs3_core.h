/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef INFILTRATOR_PFS3_CORE_H
#define INFILTRATOR_PFS3_CORE_H

#if defined(__KERNEL__)
#include <linux/types.h>
typedef u16 ifs_pfs3_u16;
typedef u32 ifs_pfs3_u32;
#else
#include <stdint.h>
typedef uint16_t ifs_pfs3_u16;
typedef uint32_t ifs_pfs3_u32;
#endif

#define IFS_PFS3_DISK_PFS1 0x50465301U
#define IFS_PFS3_DISK_PFS2 0x50465302U
#define IFS_PFS3_MAX_RESERVED_BLOCKS (4096U + 255U * 1024U * 8U)
#define IFS_PFS3_MAX_ROOT_CLUSTER 521U
#define IFS_PFS3_DISK_NAME_BYTES 32U
#define IFS_PFS3_MAX_DISK_NAME 31U
#define IFS_PFS3_DEFAULT_FILENAME_SIZE 32U
#define IFS_PFS3_MIN_FILENAME_SIZE 30U
#define IFS_PFS3_MAX_FILENAME_SIZE 107U
#define IFS_PFS3_ROOT_MIN_BYTES 96U
#define IFS_PFS3_EXTENSION_ID 0x4558U
#define IFS_PFS3_EXTENSION_MIN_BYTES 58U
#define IFS_PFS3_MAX_DELDIR_BLOCKS 32U
#define IFS_PFS3_DELDIR_ENTRIES_PER_BLOCK 31U
#define IFS_PFS3_DIRENTRY_BYTES 20U
#define IFS_PFS3_DIRENTRY_NAME_OFFSET 18U
#define IFS_PFS3_EXTRA_FIELD_WORDS 11U
#define IFS_PFS3_DIRBLOCK_ID 0x4442U
#define IFS_PFS3_DIRBLOCK_HEADER_BYTES 20U
#define IFS_PFS3_ANODEBLOCK_ID 0x4142U
#define IFS_PFS3_ANODEBLOCK_HEADER_BYTES 16U
#define IFS_PFS3_ANODE_BYTES 12U

#define IFS_PFS3_MODE_HARDDISK        0x0001U
#define IFS_PFS3_MODE_SPLITTED_ANODES 0x0002U
#define IFS_PFS3_MODE_DIR_EXTENSION   0x0004U
#define IFS_PFS3_MODE_DELDIR          0x0008U
#define IFS_PFS3_MODE_SIZEFIELD       0x0010U
#define IFS_PFS3_MODE_EXTENSION       0x0020U
#define IFS_PFS3_MODE_DATESTAMP       0x0040U
#define IFS_PFS3_MODE_SUPERINDEX      0x0080U
#define IFS_PFS3_MODE_SUPERDELDIR     0x0100U
#define IFS_PFS3_MODE_EXTROVING       0x0200U
#define IFS_PFS3_MODE_LONGFN          0x0400U
#define IFS_PFS3_MODE_LARGEFILE       0x0800U
#define IFS_PFS3_MODE_STORED_GEOM     0x1000U

typedef struct IfsPfs3RootRecord {
    ifs_pfs3_u32 disk_type;
    ifs_pfs3_u32 options;
    ifs_pfs3_u32 datestamp;
    ifs_pfs3_u16 creation_day;
    ifs_pfs3_u16 creation_minute;
    ifs_pfs3_u16 creation_tick;
    ifs_pfs3_u16 protection;
    unsigned char disk_name[IFS_PFS3_DISK_NAME_BYTES];
    ifs_pfs3_u32 last_reserved;
    ifs_pfs3_u32 first_reserved;
    ifs_pfs3_u32 reserved_free;
    ifs_pfs3_u16 reserved_block_size;
    ifs_pfs3_u16 root_block_cluster;
    ifs_pfs3_u32 blocks_free;
    ifs_pfs3_u32 always_free;
    ifs_pfs3_u32 roving_pointer;
    ifs_pfs3_u32 delete_directory;
    ifs_pfs3_u32 disk_size;
    ifs_pfs3_u32 extension;
} IfsPfs3RootRecord;

typedef struct IfsPfs3ExtensionRecord {
    ifs_pfs3_u16 id;
    ifs_pfs3_u32 extension_options;
    ifs_pfs3_u32 datestamp;
    ifs_pfs3_u32 format_version;
    ifs_pfs3_u32 reserved_roving;
    ifs_pfs3_u16 roving_bit;
    ifs_pfs3_u16 current_anode_sequence;
    ifs_pfs3_u16 delete_directory_roving;
    ifs_pfs3_u16 delete_directory_size;
    ifs_pfs3_u16 filename_size;
} IfsPfs3ExtensionRecord;

typedef enum IfsPfs3DirEntryStatus {
    IFS_PFS3_DIRENTRY_OK = 0,
    IFS_PFS3_DIRENTRY_END,
    IFS_PFS3_DIRENTRY_TRUNCATED,
    IFS_PFS3_DIRENTRY_ODD_SIZE,
    IFS_PFS3_DIRENTRY_NAME_TOO_LONG,
    IFS_PFS3_DIRENTRY_COMMENT_OVERRUN,
    IFS_PFS3_DIRENTRY_EXTRA_FIELDS_OVERRUN,
    IFS_PFS3_DIRENTRY_UNKNOWN_EXTRA_FIELDS
} IfsPfs3DirEntryStatus;

typedef struct IfsPfs3AnodeRecord {
    ifs_pfs3_u32 cluster_size;
    ifs_pfs3_u32 block_number;
    ifs_pfs3_u32 next_anode;
} IfsPfs3AnodeRecord;

typedef struct IfsPfs3AnodeBlockView {
    ifs_pfs3_u32 datestamp;
    ifs_pfs3_u32 sequence;
    ifs_pfs3_u32 node_count;
} IfsPfs3AnodeBlockView;

typedef struct IfsPfs3DirBlockView {
    ifs_pfs3_u32 datestamp;
    ifs_pfs3_u32 directory_anode;
    ifs_pfs3_u32 parent_anode;
} IfsPfs3DirBlockView;

typedef struct IfsPfs3DirEntryView {
    ifs_pfs3_u16 record_bytes;
    signed char type;
    ifs_pfs3_u32 anode;
    ifs_pfs3_u32 file_size_low;
    ifs_pfs3_u16 creation_day;
    ifs_pfs3_u16 creation_minute;
    ifs_pfs3_u16 creation_tick;
    unsigned char protection;
    unsigned char name_length;
    unsigned char comment_length;
    ifs_pfs3_u16 extra_flags;
    ifs_pfs3_u16 extra_word_count;
} IfsPfs3DirEntryView;

typedef enum IfsPfs3Format {
    IFS_PFS3_FORMAT_INVALID = 0,
    IFS_PFS3_FORMAT_PFS1,
    IFS_PFS3_FORMAT_PFS2
} IfsPfs3Format;

typedef enum IfsPfs3MediaStatus {
    IFS_PFS3_MEDIA_OK = 0,
    IFS_PFS3_MEDIA_INVALID_ROOT,
    IFS_PFS3_MEDIA_INVALID_NAME,
    IFS_PFS3_MEDIA_INVALID_ALLOCATION_COUNTS,
    IFS_PFS3_MEDIA_RESERVED_RANGE_OUTSIDE_MEDIA,
    IFS_PFS3_MEDIA_SIZE_FIELD_MISMATCH,
    IFS_PFS3_MEDIA_EXTENSION_OUT_OF_RANGE
} IfsPfs3MediaStatus;

typedef enum IfsPfs3RootStatus {
    IFS_PFS3_ROOT_OK = 0,
    IFS_PFS3_ROOT_BAD_DISK_TYPE,
    IFS_PFS3_ROOT_ZERO_OPTIONS,
    IFS_PFS3_ROOT_INVALID_LOGICAL_BLOCK_SIZE,
    IFS_PFS3_ROOT_INVALID_RESERVED_BLOCK_SIZE,
    IFS_PFS3_ROOT_INVALID_ROOT_CLUSTER,
    IFS_PFS3_ROOT_CLASSIC_FEATURE_CONFLICT,
    IFS_PFS3_ROOT_INVALID_RESERVED_RANGE,
    IFS_PFS3_ROOT_RESERVED_COUNT_OUT_OF_RANGE,
    IFS_PFS3_ROOT_INVALID_ROOT_CLUSTER_ALIGNMENT,
    IFS_PFS3_ROOT_RESERVED_FREE_OUT_OF_RANGE
} IfsPfs3RootStatus;

int ifs_pfs3_decode_anode(
    const unsigned char *bytes,
    ifs_pfs3_u32 byte_count,
    IfsPfs3AnodeRecord *anode);

int ifs_pfs3_validate_anode_extent(
    const IfsPfs3AnodeRecord *anode,
    ifs_pfs3_u32 media_block_count);

int ifs_pfs3_decode_anode_block(
    const unsigned char *bytes,
    ifs_pfs3_u32 block_bytes,
    IfsPfs3AnodeBlockView *block);

int ifs_pfs3_decode_directory_block(
    const unsigned char *bytes,
    ifs_pfs3_u32 block_bytes,
    int directory_extensions,
    IfsPfs3DirBlockView *block,
    ifs_pfs3_u32 *entry_count);

IfsPfs3DirEntryStatus ifs_pfs3_decode_directory_entry(
    const unsigned char *bytes,
    ifs_pfs3_u32 available_bytes,
    int directory_extensions,
    IfsPfs3DirEntryView *entry);

int ifs_pfs3_decode_extension(
    const unsigned char *bytes,
    ifs_pfs3_u32 byte_count,
    IfsPfs3ExtensionRecord *extension);

int ifs_pfs3_validate_extension(
    const IfsPfs3ExtensionRecord *extension,
    ifs_pfs3_u32 reserved_block_count,
    ifs_pfs3_u16 *effective_filename_size);

int ifs_pfs3_decode_root(
    const unsigned char *bytes,
    ifs_pfs3_u32 byte_count,
    IfsPfs3RootRecord *root);

IfsPfs3Format ifs_pfs3_classify_disk_type(ifs_pfs3_u32 disk_type);

IfsPfs3MediaStatus ifs_pfs3_validate_root_record(
    const IfsPfs3RootRecord *root,
    ifs_pfs3_u32 logical_block_size,
    ifs_pfs3_u32 media_block_count);

IfsPfs3RootStatus ifs_pfs3_validate_root_geometry(
    ifs_pfs3_u32 disk_type,
    ifs_pfs3_u32 options,
    ifs_pfs3_u32 logical_block_size,
    ifs_pfs3_u32 reserved_block_size,
    ifs_pfs3_u32 root_block_cluster,
    ifs_pfs3_u32 first_reserved,
    ifs_pfs3_u32 last_reserved,
    ifs_pfs3_u32 reserved_free);

int ifs_pfs3_effective_filename_size(
    ifs_pfs3_u16 stored_filename_size,
    ifs_pfs3_u16 *effective_filename_size);

int ifs_pfs3_validate_allocation_counts(
    ifs_pfs3_u32 blocks_free,
    ifs_pfs3_u32 always_free);

int ifs_pfs3_validate_disk_name(
    const unsigned char disk_name[IFS_PFS3_DISK_NAME_BYTES]);

const char *ifs_pfs3_root_status_string(IfsPfs3RootStatus status);

#endif
