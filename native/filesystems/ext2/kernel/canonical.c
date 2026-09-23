/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Kbuild compiles the exact canonical EXT2 source. This file is intentionally
 * only an inclusion boundary so there is no second Linux copy of the
 * filesystem-format algorithms.
 */
#include "../core/ext2_core.c"
