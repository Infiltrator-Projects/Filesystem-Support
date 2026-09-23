// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef FILESYSTEM_SUPPORT_EXT2_COMPAT_H
#define FILESYSTEM_SUPPORT_EXT2_COMPAT_H

/*
 * Temporary naming facade for the audited ExtFS Windows lifecycle code.
 * It contains no filesystem algorithms: every extfs_* operation below maps
 * directly to the canonical Filesystem Support EXT2 engine.
 */
#include "../../../native/filesystems/ext2/core/ext2_engine.h"

typedef ifs_ext2_u8 extfs_u8;
typedef ifs_ext2_u16 extfs_u16;
typedef ifs_ext2_u32 extfs_u32;
typedef ifs_ext2_u64 extfs_u64;
typedef long long extfs_s64;

typedef IfsExt2Status extfs_status;
typedef IfsExt2Io extfs_io;
typedef IfsExt2Volume extfs_volume;
typedef IfsExt2Inode extfs_inode;
typedef IfsExt2NodeType extfs_node_type;
typedef IfsExt2DirectoryCallback extfs_directory_callback;

#define EXTFS_OK IFS_EXT2_OK
#define EXTFS_STOP IFS_EXT2_STOP
#define EXTFS_ERR_INVALID_ARGUMENT IFS_EXT2_ERROR_ARGUMENT
#define EXTFS_ERR_IO IFS_EXT2_ERROR_IO
#define EXTFS_ERR_NOT_EXT IFS_EXT2_ERROR_MAGIC
#define EXTFS_ERR_CORRUPT IFS_EXT2_ERROR_CORRUPT
#define EXTFS_ERR_UNSUPPORTED IFS_EXT2_ERROR_UNSUPPORTED
#define EXTFS_ERR_BUFFER_TOO_SMALL IFS_EXT2_ERROR_BUFFER_TOO_SMALL
#define EXTFS_ERR_RANGE IFS_EXT2_ERROR_RANGE
#define EXTFS_ERR_NOT_FOUND IFS_EXT2_ERROR_NOT_FOUND
#define EXTFS_ERR_NOT_DIRECTORY IFS_EXT2_ERROR_NOT_DIRECTORY
#define EXTFS_ERR_IS_DIRECTORY IFS_EXT2_ERROR_IS_DIRECTORY
#define EXTFS_ERR_NO_SPACE IFS_EXT2_ERROR_NO_SPACE

#define EXTFS_MAX_NAME_LENGTH IFS_EXT2_MAX_NAME_LENGTH
#define EXTFS_ROOT_INODE IFS_EXT2_ROOT_INODE

#define EXTFS_NODE_UNKNOWN IFS_EXT2_NODE_UNKNOWN
#define EXTFS_NODE_REGULAR IFS_EXT2_NODE_REGULAR
#define EXTFS_NODE_DIRECTORY IFS_EXT2_NODE_DIRECTORY
#define EXTFS_NODE_SYMLINK IFS_EXT2_NODE_SYMLINK
#define EXTFS_NODE_CHARACTER IFS_EXT2_NODE_CHARACTER
#define EXTFS_NODE_BLOCK IFS_EXT2_NODE_BLOCK
#define EXTFS_NODE_FIFO IFS_EXT2_NODE_FIFO
#define EXTFS_NODE_SOCKET IFS_EXT2_NODE_SOCKET

#define extfs_open ifs_ext2_open
#define extfs_readonly_assess ifs_ext2_readonly_assess
#define extfs_write_assess ifs_ext2_write_assess
#define extfs_inode_write_assess ifs_ext2_inode_write_assess
#define extfs_read_inode ifs_ext2_read_inode
#define extfs_inode_type ifs_ext2_inode_type
#define extfs_read_file ifs_ext2_read_file
#define extfs_write_file_existing ifs_ext2_write_file_existing
#define extfs_iterate_directory ifs_ext2_iterate_directory
#define extfs_lookup ifs_ext2_lookup

#endif
