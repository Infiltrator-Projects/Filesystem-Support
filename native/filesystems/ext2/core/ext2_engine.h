/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef INFILTRATR_EXT2_ENGINE_H
#define INFILTRATR_EXT2_ENGINE_H

#include "ext2_core.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IFS_EXT2_MAX_NAME_LENGTH 255U
#define IFS_EXT2_ROOT_INODE 2U

#define IFS_EXT2_READONLY_RISK_DIRTY 0x00000001U
#define IFS_EXT2_READONLY_RISK_ERROR_STATE 0x00000002U
#define IFS_EXT2_READONLY_RISK_UNSUPPORTED_LAYOUT 0x00000004U

#define IFS_EXT2_WRITE_RISK_NO_WRITER 0x00000001U
#define IFS_EXT2_WRITE_RISK_READONLY_POLICY 0x00000002U
#define IFS_EXT2_WRITE_RISK_UNSUPPORTED_LAYOUT 0x00000004U

typedef int (*IfsExt2ReadAtFn)(
    void *user, ifs_ext2_u64 offset, void *destination, ifs_ext2_u32 count);
typedef int (*IfsExt2WriteAtFn)(
    void *user, ifs_ext2_u64 offset, const void *source, ifs_ext2_u32 count);
typedef int (*IfsExt2FlushFn)(void *user);

typedef struct IfsExt2Io {
    IfsExt2ReadAtFn read_at;
    IfsExt2WriteAtFn write_at;
    IfsExt2FlushFn flush;
    void *user;
} IfsExt2Io;

typedef enum IfsExt2NodeType {
    IFS_EXT2_NODE_UNKNOWN = 0,
    IFS_EXT2_NODE_REGULAR,
    IFS_EXT2_NODE_DIRECTORY,
    IFS_EXT2_NODE_SYMLINK,
    IFS_EXT2_NODE_CHARACTER,
    IFS_EXT2_NODE_BLOCK,
    IFS_EXT2_NODE_FIFO,
    IFS_EXT2_NODE_SOCKET
} IfsExt2NodeType;

typedef struct IfsExt2Volume {
    IfsExt2Io io;
    IfsExt2Superblock super;
    ifs_ext2_u32 block_size;
    ifs_ext2_u32 blocks_per_group;
    ifs_ext2_u32 inodes_per_group;
    ifs_ext2_u32 first_data_block;
    ifs_ext2_u32 total_inodes;
    ifs_ext2_u64 total_blocks;
    ifs_ext2_u64 free_blocks;
    ifs_ext2_u64 byte_size;
    ifs_ext2_u32 group_count;
    ifs_ext2_u16 inode_size;
    ifs_ext2_u16 descriptor_size;
    ifs_ext2_u16 state;
    ifs_ext2_u32 revision;
    ifs_ext2_u32 feature_compat;
    ifs_ext2_u32 feature_incompat;
    ifs_ext2_u32 feature_ro_compat;
    ifs_ext2_u8 uuid[16];
    char label[17];
} IfsExt2Volume;

typedef struct IfsExt2Inode {
    ifs_ext2_u32 number;
    ifs_ext2_u16 mode;
    ifs_ext2_u16 links_count;
    ifs_ext2_u32 uid;
    ifs_ext2_u32 gid;
    ifs_ext2_u32 flags;
    ifs_ext2_u32 generation;
    ifs_ext2_u64 size;
    ifs_ext2_u64 access_time;
    ifs_ext2_u64 change_time;
    ifs_ext2_u64 modification_time;
    ifs_ext2_u64 creation_time;
    ifs_ext2_u32 access_time_nanoseconds;
    ifs_ext2_u32 change_time_nanoseconds;
    ifs_ext2_u32 modification_time_nanoseconds;
    ifs_ext2_u32 creation_time_nanoseconds;
    ifs_ext2_u8 block_map[60];
} IfsExt2Inode;

typedef int (*IfsExt2DirectoryCallback)(
    void *user,
    ifs_ext2_u32 inode_number,
    IfsExt2NodeType type,
    const char *name,
    ifs_ext2_u8 name_length);

IfsExt2Status ifs_ext2_open(IfsExt2Volume *volume, const IfsExt2Io *io);
IfsExt2Status ifs_ext2_readonly_assess(
    const IfsExt2Volume *volume, ifs_ext2_u32 *risk_flags);
IfsExt2Status ifs_ext2_write_assess(
    const IfsExt2Volume *volume, ifs_ext2_u32 *risk_flags);
IfsExt2Status ifs_ext2_inode_write_assess(
    const IfsExt2Volume *volume, const IfsExt2Inode *inode);
IfsExt2Status ifs_ext2_read_inode(
    const IfsExt2Volume *volume,
    ifs_ext2_u32 inode_number,
    IfsExt2Inode *inode,
    void *scratch,
    ifs_ext2_u32 scratch_size);
IfsExt2Status ifs_ext2_map_file_block(
    const IfsExt2Volume *volume,
    const IfsExt2Inode *inode,
    ifs_ext2_u32 logical_block,
    ifs_ext2_u64 *physical_block,
    int *is_hole,
    void *scratch,
    ifs_ext2_u32 scratch_size);
IfsExt2Status ifs_ext2_read_file(
    const IfsExt2Volume *volume,
    const IfsExt2Inode *inode,
    ifs_ext2_u64 byte_offset,
    void *destination,
    ifs_ext2_u32 byte_count,
    void *scratch,
    ifs_ext2_u32 scratch_size,
    ifs_ext2_u32 *bytes_read);
IfsExt2Status ifs_ext2_write_file_existing(
    const IfsExt2Volume *volume,
    const IfsExt2Inode *inode,
    ifs_ext2_u64 byte_offset,
    const void *source,
    ifs_ext2_u32 byte_count,
    void *scratch,
    ifs_ext2_u32 scratch_size,
    ifs_ext2_u32 *bytes_written);
IfsExt2Status ifs_ext2_iterate_directory(
    const IfsExt2Volume *volume,
    const IfsExt2Inode *directory,
    IfsExt2DirectoryCallback callback,
    void *callback_user,
    void *scratch,
    ifs_ext2_u32 scratch_size);
IfsExt2Status ifs_ext2_lookup(
    const IfsExt2Volume *volume,
    const IfsExt2Inode *directory,
    const char *name,
    ifs_ext2_u8 name_length,
    ifs_ext2_u32 *inode_number,
    void *scratch,
    ifs_ext2_u32 scratch_size);
IfsExt2NodeType ifs_ext2_inode_type(const IfsExt2Inode *inode);

#ifdef __cplusplus
}
#endif

#endif
