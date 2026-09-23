/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Linux build bridge for the canonical EXT3 filesystem core.
 *
 * The implementation lives in ../core.  This translation unit only makes the
 * same core part of ext3.ko; it does not create a Linux-specific copy.
 */
#include "../core/ext3_core.c"
