/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "ext3_core.h"

#include <stdio.h>

static int fail(const char *message)
{
    fprintf(stderr, "ext3 core test: %s\n", message);
    return 1;
}

int main(void)
{
    if (ifs_ext3_unsupported_incompat_features(
            IFS_EXT3_FEATURE_INCOMPAT_SUPPORTED) != 0U)
        return fail("supported incompat features were rejected");

    if (ifs_ext3_unsupported_incompat_features(
            IFS_EXT3_FEATURE_INCOMPAT_SUPPORTED | 0x80000000U) !=
        0x80000000U)
        return fail("unknown incompat feature was not reported");

    if (ifs_ext3_unsupported_ro_compat_features(
            IFS_EXT3_FEATURE_RO_COMPAT_SUPPORTED) != 0U)
        return fail("supported read-only-compatible features were rejected");

    if (ifs_ext3_unsupported_ro_compat_features(
            IFS_EXT3_FEATURE_RO_COMPAT_SUPPORTED | 0x40000000U) !=
        0x40000000U)
        return fail("unknown read-only-compatible feature was not reported");

    return 0;
}
