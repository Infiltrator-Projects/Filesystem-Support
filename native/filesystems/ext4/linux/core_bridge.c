/*
 * Copyright (C) 2026 Shannon Smith
 *
 * Infiltrator Filesystem Support EXT4 Linux adapter: core_bridge.c.
 * Project-maintained implementation for the canonical EXT4 driver.
 */

/*
 * Linux build bridge for the canonical EXT4 filesystem core.
 *
 * The implementation lives in ../core.  This translation unit only makes the
 * same core part of ext4.ko; it does not create a Linux-specific copy.
 */
#include "../core/ext4_core.c"
