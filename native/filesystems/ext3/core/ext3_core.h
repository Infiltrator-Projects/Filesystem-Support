/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef INFILTRATR_EXT3_CORE_H
#define INFILTRATR_EXT3_CORE_H

/*
 * Canonical EXT3 filesystem semantics shared by every operating-system
 * adapter.  This interface contains no Linux VFS or Windows IFS types.
 */

#if defined(__KERNEL__)
#include <linux/types.h>
typedef u32 ifs_ext3_u32;
#elif defined(IFS_EXT3_WINDOWS_KERNEL)
typedef unsigned int ifs_ext3_u32;
#else
#include <stdint.h>
typedef uint32_t ifs_ext3_u32;
#endif

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

#endif
