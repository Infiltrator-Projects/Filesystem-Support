#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include "../core/ffs_disk_layout.h"
#ifndef INFILTRATOR_FFS_LINUX_ADAPTER_H
#define INFILTRATOR_FFS_LINUX_ADAPTER_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/buffer_head.h>
#include <linux/mutex.h>
#include <linux/workqueue.h>
#include <linux/errno.h>
#include "../core/ffs_primitives.h"
#include "../../../platform/linux_kernel/vfs_compat.h"

#define IFS_FFS_AOPS_WRITE_CONTEXT IFS_LINUX_AOPS_WRITE_CONTEXT
#define IFS_FFS_AOPS_WRITE_CONTEXT_ARG IFS_LINUX_AOPS_WRITE_CONTEXT_ARG
#define IFS_FFS_INODE_IS_NEW(inode) IFS_LINUX_INODE_IS_NEW(inode)
#define IFS_FFS_MKDIR_RETURN IFS_LINUX_MKDIR_RETURN
#define IFS_FFS_MKDIR_FAILURE(error) IFS_LINUX_MKDIR_FAILURE(error)
#define IFS_FFS_MKDIR_SUCCESS IFS_LINUX_MKDIR_SUCCESS

#define IFS_AMIGA_MKDIR_RETURN IFS_FFS_MKDIR_RETURN
#define IFS_AMIGA_BLOCK_VALID(sb, block) \
    (ifs_ffs_data_block_valid( \
        (ifs_ffs_u32)(block), \
        (ifs_ffs_u32)AFFS_SB(sb)->s_reserved, \
        (ifs_ffs_u32)AFFS_SB(sb)->s_partition_size) != 0)
#include "affs_compat.h"
#undef IFS_AMIGA_BLOCK_VALID
#undef IFS_AMIGA_MKDIR_RETURN

#define AFFSNAMEMAX IFS_FFS_DOS_NAME_MAX
#define ifs_ffs_chain_budget ifs_amiga_chain_budget

#endif
