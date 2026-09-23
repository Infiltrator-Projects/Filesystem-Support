/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ext4_core.h"

#include <stdio.h>

static int fail(const char *message)
{
    fprintf(stderr, "ext4 core test: %s\n", message);
    return 1;
}

int main(void)
{
    if (ifs_ext4_unsupported_incompat_features(
            IFS_EXT4_FEATURE_INCOMPAT_SUPPORTED) != 0U)
        return fail("supported incompat features were rejected");

    if (ifs_ext4_unsupported_incompat_features(
            IFS_EXT4_FEATURE_INCOMPAT_SUPPORTED | 0x80000000U) !=
        0x80000000U)
        return fail("unknown incompat feature was not reported");

    if (ifs_ext4_unsupported_ro_compat_features(
            IFS_EXT4_FEATURE_RO_COMPAT_SUPPORTED |
            IFS_EXT4_FEATURE_RO_COMPAT_READONLY) != 0U)
        return fail("supported read-only-compatible features were rejected");

    if (!ifs_ext4_requires_readonly(IFS_EXT4_FEATURE_RO_COMPAT_READONLY))
        return fail("read-only filesystem feature was not recognised");

    if (ifs_ext4_validate_bigalloc(
            0U, IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC, 0U) !=
        IFS_EXT4_BIGALLOC_REQUIRES_EXTENTS)
        return fail("bigalloc without extents was accepted");

    if (ifs_ext4_validate_bigalloc(
            IFS_EXT4_FEATURE_INCOMPAT_EXTENTS,
            IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC, 1U) !=
        IFS_EXT4_BIGALLOC_INVALID_FIRST_DATA_BLOCK)
        return fail("invalid bigalloc first_data_block was accepted");

    if (ifs_ext4_validate_bigalloc(
            IFS_EXT4_FEATURE_INCOMPAT_EXTENTS,
            IFS_EXT4_FEATURE_RO_COMPAT_BIGALLOC, 0U) !=
        IFS_EXT4_BIGALLOC_OK)
        return fail("valid bigalloc feature combination was rejected");

    return 0;
}
