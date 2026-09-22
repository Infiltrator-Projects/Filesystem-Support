// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILTRATR_FS_ENDIAN_H
#define INFILTRATR_FS_ENDIAN_H

#include "types.h"

#ifdef __KERNEL__
#include <asm/unaligned.h>

static inline ifs_u16 ifs_load_le16(const void *bytes) { return get_unaligned_le16(bytes); }
static inline ifs_u32 ifs_load_le32(const void *bytes) { return get_unaligned_le32(bytes); }
static inline ifs_u64 ifs_load_le64(const void *bytes) { return get_unaligned_le64(bytes); }
static inline ifs_u16 ifs_load_be16(const void *bytes) { return get_unaligned_be16(bytes); }
static inline ifs_u32 ifs_load_be32(const void *bytes) { return get_unaligned_be32(bytes); }
static inline ifs_u64 ifs_load_be64(const void *bytes) { return get_unaligned_be64(bytes); }
#else
#include <infiltratr/endian.h>

static inline ifs_u16 ifs_load_le16(const void *bytes) { return infiltratr_load_le16(bytes); }
static inline ifs_u32 ifs_load_le32(const void *bytes) { return infiltratr_load_le32(bytes); }
static inline ifs_u64 ifs_load_le64(const void *bytes) { return infiltratr_load_le64(bytes); }
static inline ifs_u16 ifs_load_be16(const void *bytes) { return infiltratr_load_be16(bytes); }
static inline ifs_u32 ifs_load_be32(const void *bytes) { return infiltratr_load_be32(bytes); }
static inline ifs_u64 ifs_load_be64(const void *bytes) { return infiltratr_load_be64(bytes); }
#endif

#endif
