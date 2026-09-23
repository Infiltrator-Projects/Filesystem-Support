/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Canonical EXT3 format policy shared by all host adapters.
 */

#include "ext3_core.h"

ifs_ext3_u32 ifs_ext3_unsupported_incompat_features(
    const ifs_ext3_u32 feature_incompat)
{
    return feature_incompat & ~IFS_EXT3_FEATURE_INCOMPAT_SUPPORTED;
}

ifs_ext3_u32 ifs_ext3_unsupported_ro_compat_features(
    const ifs_ext3_u32 feature_ro_compat)
{
    return feature_ro_compat & ~IFS_EXT3_FEATURE_RO_COMPAT_SUPPORTED;
}
