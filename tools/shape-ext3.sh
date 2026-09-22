#!/usr/bin/env bash
set -euo pipefail

root="${1:-native/filesystems/ext3/kernel}"

for required in \
  Makefile ext3.h acl.c acl.h balloc.c bitmap.c dir.c ext3_jbd.c file.c \
  fsync.c hash.c ialloc.c inode.c ioctl.c namei.c namei.h resize.c super.c \
  symlink.c xattr.c xattr.h xattr_security.c xattr_trusted.c xattr_user.c \
  mbcache.c include/linux/mbcache.h include/linux/jbd.h \
  jbd_checkpoint.c jbd_commit.c jbd_journal.c jbd_recovery.c \
  jbd_revoke.c jbd_transaction.c; do
  test -f "$root/$required"
done

append_without_includes() {
  local target="$1"
  local source="$2"
  local title="$3"
  {
    printf '\n/* ---- %s (merged into this translation unit) ---- */\n' "$title"
    sed '/^[[:space:]]*#include[[:space:]]/d' "$source"
  } >> "$target"
}

# EXT3 owns the complete final standalone Linux EXT3 feature set.  Tiny
# organisational translation units are folded into the subsystem that owns
# them; large allocator, journal, resize and namespace units remain separate.
append_without_includes "$root/balloc.c" "$root/bitmap.c" "EXT3 bitmap helpers"
append_without_includes "$root/dir.c" "$root/hash.c" "EXT3 indexed-directory hash"
append_without_includes "$root/file.c" "$root/fsync.c" "EXT3 fsync"
append_without_includes "$root/file.c" "$root/ioctl.c" "EXT3 file ioctls"
append_without_includes "$root/inode.c" "$root/symlink.c" "EXT3 symlink inode operations"

append_without_includes "$root/xattr.c" "$root/xattr_user.c" "EXT3 user xattr handler"
append_without_includes "$root/xattr.c" "$root/xattr_trusted.c" "EXT3 trusted xattr handler"
append_without_includes "$root/xattr.c" "$root/xattr_security.c" "EXT3 security xattr handler"
append_without_includes "$root/xattr.c" "$root/acl.c" "EXT3 POSIX ACL implementation"
append_without_includes "$root/xattr.c" "$root/mbcache.c" "EXT3 private metadata-block cache"

# The small EXT3-to-JBD wrapper belongs to the filesystem super/journal
# boundary and does not justify another object.
append_without_includes "$root/super.c" "$root/ext3_jbd.c" "EXT3 journal adapter"

# Consolidate private EXT3 declarations into ext3.h.  JBD stays in journal.h
# because its types are needed by ext3.h itself.
{
  printf '\n/* ---- EXT3 ACL declarations ---- */\n'
  cat "$root/acl.h"
  printf '\n/* ---- EXT3 namei declarations ---- */\n'
  cat "$root/namei.h"
  printf '\n/* ---- EXT3 extended-attribute declarations ---- */\n'
  cat "$root/xattr.h"
  printf '\n/* ---- EXT3 private metadata-cache declarations ---- */\n'
  cat "$root/include/linux/mbcache.h"
} >> "$root/ext3.h"

mv "$root/include/linux/jbd.h" "$root/journal.h"

# Every EXT3 and embedded JBD translation unit uses this filesystem-private
# journal header.  No separate JBD module ABI exists.
find "$root" -type f \( -name '*.c' -o -name '*.h' \) -exec \
  sed -i \
    -e 's#<linux/jbd.h>#"journal.h"#g' \
    -e '/^[[:space:]]*#include[[:space:]]*"acl\.h"/d' \
    -e '/^[[:space:]]*#include[[:space:]]*"namei\.h"/d' \
    -e '/^[[:space:]]*#include[[:space:]]*"xattr\.h"/d' \
    -e '/^[[:space:]]*#include[[:space:]]*<linux\/mbcache\.h>/d' \
    {} +

# JBD is implementation code inside ext3.ko, not a second module.  Keep the
# journal engine but remove its exported-module surface and diagnostic trace
# subsystem.
for source in "$root"/jbd_*.c; do
  sed -i \
    -e '/^[[:space:]]*EXPORT_SYMBOL/d' \
    -e '/^[[:space:]]*#define CREATE_TRACE_POINTS/d' \
    -e '/^[[:space:]]*#include <trace\/events\/jbd.h>/d' \
    -e '/^[[:space:]]*MODULE_LICENSE/d' \
    "$source"
  perl -0pi -e 's/\n\s*trace_jbd_[A-Za-z0-9_]+\s*\([^;]*?\);//gs' "$source"
done

sed -i \
  -e 's/static int __init journal_init(void)/int __init infiltratr_ext3_jbd_init(void)/' \
  -e 's/static void __exit journal_exit(void)/void __exit infiltratr_ext3_jbd_exit(void)/' \
  -e '/^module_init(journal_init);$/d' \
  -e '/^module_exit(journal_exit);$/d' \
  "$root/jbd_journal.c"

# mbcache is private to EXT3 xattrs and is initialised by the EXT3 module.
sed -i \
  -e 's/static int __init init_mbcache(void)/int __init infiltratr_ext3_mbcache_init(void)/' \
  -e 's/static void __exit exit_mbcache(void)/void __exit infiltratr_ext3_mbcache_exit(void)/' \
  -e '/^module_init(init_mbcache)$/d' \
  -e '/^module_exit(exit_mbcache)$/d' \
  -e '/^[[:space:]]*EXPORT_SYMBOL/d' \
  "$root/xattr.c"

# Wrap the original filesystem lifetime with its embedded cache and JBD
# lifetime so ext3.ko has exactly one module_init/module_exit pair.
perl -0pi -e 's/static int __init init_ext3_fs\(void\)/static int __init ext3_core_init_fs(void)/' "$root/super.c"
perl -0pi -e 's/static void __exit exit_ext3_fs\(void\)/static void __exit ext3_core_exit_fs(void)/' "$root/super.c"
perl -0pi -e 's/module_init\(init_ext3_fs\)\nmodule_exit\(exit_ext3_fs\)/int infiltratr_ext3_mbcache_init(void);\nvoid infiltratr_ext3_mbcache_exit(void);\nint infiltratr_ext3_jbd_init(void);\nvoid infiltratr_ext3_jbd_exit(void);\n\nstatic int __init init_ext3_fs(void)\n{\n\tint err = infiltratr_ext3_mbcache_init();\n\tif (err)\n\t\treturn err;\n\terr = infiltratr_ext3_jbd_init();\n\tif (err) {\n\t\tinfiltratr_ext3_mbcache_exit();\n\t\treturn err;\n\t}\n\terr = ext3_core_init_fs();\n\tif (err) {\n\t\tinfiltratr_ext3_jbd_exit();\n\t\tinfiltratr_ext3_mbcache_exit();\n\t}\n\treturn err;\n}\n\nstatic void __exit exit_ext3_fs(void)\n{\n\text3_core_exit_fs();\n\tinfiltratr_ext3_jbd_exit();\n\tinfiltratr_ext3_mbcache_exit();\n}\n\nmodule_init(init_ext3_fs)\nmodule_exit(exit_ext3_fs)/s' "$root/super.c"
sed -i 's/MODULE_DESCRIPTION("Second Extended Filesystem with journaling extensions")/MODULE_DESCRIPTION("Third Extended Filesystem")/' "$root/super.c"

# Match Common's pre-addition overflow discipline using the kernel-native
# helper.  Compute the proposed filesystem size once and consume only the
# validated result throughout ext3_group_extend().
sed -i '/^#include "ext3.h"$/a #include <linux/overflow.h>' "$root/resize.c"
perl -0pi -e 's/	ext3_grpblk_t add;\n/	ext3_grpblk_t add;\n	ext3_fsblk_t new_blocks_count;\n/' "$root/resize.c"
perl -0pi -e 's/	add = EXT3_BLOCKS_PER_GROUP\(sb\) - last;\n\n	if \(o_blocks_count \+ add < o_blocks_count\) \{\n		ext3_warning\(sb, __func__, "blocks_count overflow"\);\n		return -EINVAL;\n	\}\n\n	if \(o_blocks_count \+ add > n_blocks_count\)\n		add = n_blocks_count - o_blocks_count;\n\n	if \(o_blocks_count \+ add < n_blocks_count\)\n		ext3_warning\(sb, __func__,\n			     "will only finish group \("E3FSBLK\n			     " blocks, %u new\)",\n			     o_blocks_count \+ add, add\);/	add = EXT3_BLOCKS_PER_GROUP(sb) - last;\n\n	if (check_add_overflow(o_blocks_count, (ext3_fsblk_t)add,\n			       &new_blocks_count)) {\n		ext3_warning(sb, __func__, "blocks_count overflow");\n		return -EINVAL;\n	}\n\n	if (new_blocks_count > n_blocks_count) {\n		add = n_blocks_count - o_blocks_count;\n		new_blocks_count = n_blocks_count;\n	}\n\n	if (new_blocks_count < n_blocks_count)\n		ext3_warning(sb, __func__,\n			     "will only finish group ("E3FSBLK\n			     " blocks, %u new)",\n			     new_blocks_count, add);/s' "$root/resize.c"
sed -i \
  -e 's/o_blocks_count + add -1/new_blocks_count - 1/' \
  -e 's/o_blocks_count + add - 1/new_blocks_count - 1/' \
  -e 's/cpu_to_le32(o_blocks_count + add)/cpu_to_le32(new_blocks_count)/' \
  -e 's/o_blocks_count + add);/new_blocks_count);/g' \
  "$root/resize.c"

# EXT3 must contain its full historical feature set.  These EXT3-specific
# Kconfig symbols no longer exist in current kernels, so the external module
# owns them locally.  Generic kernel facilities such as quota remain governed
# by the target kernel.
cat > "$root/Makefile" <<'EOF'
# SPDX-License-Identifier: GPL-2.0
# Self-contained Infiltrator EXT3 kernel module.

obj-m += ext3.o

ext3-y := balloc.o dir.o file.o ialloc.o inode.o namei.o resize.o super.o \
          xattr.o jbd_checkpoint.o jbd_commit.o jbd_journal.o \
          jbd_recovery.o jbd_revoke.o jbd_transaction.o

ccflags-y += -I$(src)
ccflags-y += -DCONFIG_EXT3_FS_XATTR=1
ccflags-y += -DCONFIG_EXT3_FS_POSIX_ACL=1
ccflags-y += -DCONFIG_EXT3_FS_SECURITY=1
ccflags-y += -DCONFIG_EXT3_DEFAULTS_TO_ORDERED=1
EOF

rm -f \
  "$root/Kconfig" \
  "$root/acl.c" "$root/acl.h" \
  "$root/bitmap.c" \
  "$root/ext3_jbd.c" \
  "$root/fsync.c" "$root/hash.c" "$root/ioctl.c" \
  "$root/namei.h" "$root/symlink.c" \
  "$root/xattr.h" "$root/xattr_user.c" "$root/xattr_trusted.c" \
  "$root/xattr_security.c" "$root/mbcache.c" \
  "$root/include/linux/mbcache.h"
rm -rf "$root/include"

expected='Makefile
balloc.c
dir.c
ext3.h
file.c
ialloc.c
inode.c
jbd_checkpoint.c
jbd_commit.c
jbd_journal.c
jbd_recovery.c
jbd_revoke.c
jbd_transaction.c
journal.h
namei.c
resize.c
super.c
xattr.c'
actual="$(find "$root" -maxdepth 1 -type f -printf '%f\n' | sort)"
test "$actual" = "$expected"
test "$(find "$root" -type f | wc -l)" -eq 18

# EXT3 forensic invariants.
test "$(grep -R -h -o 'module_init[[:space:]]*(' "$root" | wc -l)" -eq 1
test "$(grep -R -h -o 'module_exit[[:space:]]*(' "$root" | wc -l)" -eq 1
grep -Fq 'MODULE_ALIAS_FS("ext3")' "$root/super.c"
grep -Fq 'error: no journal found.' "$root/super.c"
grep -Fq 'EXT3_MOUNT_JOURNAL_DATA' "$root/ext3.h"
grep -Fq 'EXT3_MOUNT_ORDERED_DATA' "$root/ext3.h"
grep -Fq 'EXT3_MOUNT_WRITEBACK_DATA' "$root/ext3.h"
grep -Fq 'EXT3_FEATURE_COMPAT_HAS_JOURNAL' "$root/ext3.h"
! grep -R -n 'register_filesystem.*ext[24]_fs_type' "$root"
! grep -R -n 'trace_jbd_' "$root"
! grep -R -n '#include <linux/jbd.h>\|#include <linux/mbcache.h>' "$root"
! grep -R -n '\bext4_' "$root"
grep -Fq 'check_add_overflow(o_blocks_count, (ext3_fsblk_t)add,' "$root/resize.c"
! grep -Fq 'o_blocks_count + add < o_blocks_count' "$root/resize.c"
