// SPDX-License-Identifier: GPL-3.0-or-later
#include "ext2_driver.h"

/*
 * Filesystem Support EXT2 is a conservative synchronous native IFS adapter.
 * All EXT2 on-disk decisions are delegated to the canonical shared engine.
 *
 * The Windows adapter deliberately remains one WDK translation unit for this
 * recut so proven static-helper and object-lifetime behaviour does not change.
 * Its implementation is nevertheless cut by responsibility. These fragments
 * contain Windows integration only and must not grow a second EXT2 engine.
 */
#include "adapter_support.inc"
#include "name_translation.inc"
#include "file_dispatch.inc"
#include "directory_dispatch.inc"
#include "volume_lifecycle.inc"
#include "driver_entry.inc"
