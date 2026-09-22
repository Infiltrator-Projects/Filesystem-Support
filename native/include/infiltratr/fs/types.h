// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATR_FS_TYPES_H
#define INFILTRATR_FS_TYPES_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u8 ifs_u8;
typedef u16 ifs_u16;
typedef u32 ifs_u32;
typedef u64 ifs_u64;
typedef s64 ifs_s64;
#else
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint8_t ifs_u8;
typedef uint16_t ifs_u16;
typedef uint32_t ifs_u32;
typedef uint64_t ifs_u64;
typedef int64_t ifs_s64;
#endif

#endif
