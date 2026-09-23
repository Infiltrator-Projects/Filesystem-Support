/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Canonical EXT4 format policy shared by all host adapters.
 */

#include "ext4_core.h"

ifs_ext4_u32 ifs_ext4_unsupported_incompat_features(
    const ifs_ext4_u32 feature_incompat)
{
    return feature_incompat & ~IFS_EXT4_FEATURE_INCOMPAT_SUPPORTED;
}

ifs_ext4_u32 ifs_ext4_unsupported_ro_compat_features(
    const ifs_ext4_u32 feature_ro_compat)
{
    return feature_ro_compat & ~IFS_EXT4_FEATURE_RO_COMPAT_SUPPORTED &
           ~IFS_EXT4_FEATURE_RO_COMPAT_READONLY;
}

int ifs_ext4_requires_readonly(const ifs_ext4_u32 feature_ro_compat)
{
    return (feature_ro_compat & IFS_EXT4_FEATURE_RO_COMPAT_READONLY) != 0U;
}

IfsExt4BigallocStatus ifs_ext4_validate_bigalloc(
    const ifs_ext4_u32 feature_incompat,
    const ifs_ext4_u32 feature_ro_compat,
    const ifs_ext4_u32 first_data_block)
{
    if ((feature_ro_compat & IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC) == 0U)
        return IFS_EXT4_BIGALLOC_OK;

    if ((feature_incompat & IFS_EXT4_FEATURE_INCOMPAT_EXTENTS) == 0U)
        return IFS_EXT4_BIGALLOC_REQUIRES_EXTENTS;

    if (first_data_block != 0U)
        return IFS_EXT4_BIGALLOC_INVALID_FIRST_DATA_BLOCK;

    return IFS_EXT4_BIGALLOC_OK;
}
