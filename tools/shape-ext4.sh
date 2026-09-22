#!/usr/bin/env bash
set -euo pipefail

root="${1:-native/filesystems/ext4/kernel}"

for required in \
  Makefile ext4.h acl.c acl.h balloc.c bitmap.c dir.c file.c fsync.c hash.c \
  inode.c symlink.c xattr.c xattr.h xattr_hurd.c xattr_security.c \
  xattr_trusted.c xattr_user.c mbcache.c include/linux/mbcache.h \
  jbd2_checkpoint.c jbd2_commit.c jbd2_journal.c jbd2_recovery.c \
  jbd2_revoke.c jbd2_transaction.c include/linux/jbd2.h; do
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

# Fold only genuinely small organisational units into their owning EXT4
# subsystem.  Large independent feature engines remain separate.
append_without_includes "$root/balloc.c" "$root/bitmap.c" "EXT4 bitmap helpers"
append_without_includes "$root/dir.c" "$root/hash.c" "EXT4 indexed-directory hash"
append_without_includes "$root/file.c" "$root/fsync.c" "EXT4 fsync"
append_without_includes "$root/inode.c" "$root/symlink.c" "EXT4 symlink inode operations"

append_without_includes "$root/xattr.c" "$root/xattr_hurd.c" "EXT4 Hurd xattr handler"
append_without_includes "$root/xattr.c" "$root/xattr_trusted.c" "EXT4 trusted xattr handler"
append_without_includes "$root/xattr.c" "$root/xattr_user.c" "EXT4 user xattr handler"
{
  printf '\n#ifdef CONFIG_EXT4_FS_SECURITY\n'
  append_without_includes /dev/stdout "$root/xattr_security.c" "EXT4 security xattr handler"
  printf '#endif /* CONFIG_EXT4_FS_SECURITY */\n'
} >> "$root/xattr.c"
{
  printf '\n#ifdef CONFIG_EXT4_FS_POSIX_ACL\n'
  append_without_includes /dev/stdout "$root/acl.c" "EXT4 POSIX ACL implementation"
  printf '#endif /* CONFIG_EXT4_FS_POSIX_ACL */\n'
} >> "$root/xattr.c"
append_without_includes "$root/xattr.c" "$root/mbcache.c" "EXT4 private metadata-block cache"

# xattr.h is the natural private home for the small ACL and metadata-cache
# declarations used by the merged xattr subsystem.
{
  printf '\n/* ---- EXT4 ACL declarations ---- */\n'
  cat "$root/acl.h"
  printf '\n/* ---- EXT4 private metadata-cache declarations ---- */\n'
  cat "$root/include/linux/mbcache.h"
} >> "$root/xattr.h"

find "$root" -maxdepth 1 -type f -name '*.c' -exec \
  sed -i \
    -e '/^[[:space:]]*#include[[:space:]]*"acl\.h"/d' \
    -e '/^[[:space:]]*#include[[:space:]]*<linux\/mbcache\.h>/d' \
    {} +

# This module is EXT4 only.  Delete upstream compatibility routing for EXT2
# and EXT3 while retaining the shared historical on-disk fields that EXT4
# legitimately uses.
sed -i \
  -e '/static inline int ext2_feature_set_ok(struct super_block \*sb);/d' \
  -e '/static inline int ext3_feature_set_ok(struct super_block \*sb);/d' \
  -e '/#define IS_EXT2_SB(sb)/d' \
  -e '/#define IS_EXT3_SB(sb)/d' \
  "$root/super.c"

perl -0pi -e 's/\n\tif \(\(ctx->opt_flags & MOPT_NO_EXT2\).*?\n\t\}\n\tif \(\(ctx->opt_flags & MOPT_NO_EXT3\).*?\n\t\}\n/\n/s' "$root/super.c"
perl -0pi -e 's/\n\tif \(IS_EXT2_SB\(sb\)\) \{.*?\n\t\}\n\n\tif \(IS_EXT3_SB\(sb\)\) \{.*?\n\t\}\n/\n/s' "$root/super.c"

perl -0pi -e 's/\tif \(test_opt\(sb, DAX_ALWAYS\)\) \{\n\t\tif \(IS_EXT2_SB\(sb\)\)\n\t\t\tSEQ_OPTS_PUTS\("dax"\);\n\t\telse\n\t\t\tSEQ_OPTS_PUTS\("dax=always"\);/\tif (test_opt(sb, DAX_ALWAYS)) {\n\t\tSEQ_OPTS_PUTS("dax=always");/s' "$root/super.c"
perl -0pi -e 's/if \(!IS_EXT3_SB\(sb\) && !IS_EXT2_SB\(sb\) &&\n\t    \(\(def_mount_opts & EXT4_DEFM_NODELALLOC\) == 0\)\)/if ((def_mount_opts & EXT4_DEFM_NODELALLOC) == 0)/s' "$root/super.c"

sed -i \
  -e '/static inline int ext2_feature_set_ok(struct super_block \*sb) { return 0; }/d' \
  -e '/static inline int ext3_feature_set_ok(struct super_block \*sb) { return 0; }/d' \
  "$root/super.c"

# All mount options in this tree are EXT4 options now.  The compatibility-only
# option classification flags are therefore meaningless.
sed -i \
  -e '/^#define MOPT_NO_EXT2/d' \
  -e '/^#define MOPT_NO_EXT3/d' \
  -e '/^#define MOPT_EXT4_ONLY/d' \
  "$root/super.c"
sed -i \
  -e 's/MOPT_EXT4_ONLY/0/g' \
  -e 's/MOPT_NO_EXT2/0/g' \
  -e 's/MOPT_NO_EXT3/0/g' \
  "$root/super.c"

# Remove obsolete EXT2/EXT3 mount-option spellings that were accepted only for
# compatibility, not as EXT4 functionality.
sed -i \
  -e '/fsparam_string[[:space:]]*("check".*Opt_removed)/d' \
  -e '/fsparam_flag[[:space:]]*("nocheck".*Opt_removed)/d' \
  -e '/fsparam_flag[[:space:]]*("reservation".*Opt_removed)/d' \
  -e '/fsparam_flag[[:space:]]*("noreservation".*Opt_removed)/d' \
  -e '/fsparam_u32[[:space:]]*("journal".*Opt_removed)/d' \
  "$root/super.c"

# These masks existed solely to decide whether the EXT4 implementation could
# masquerade as EXT2/EXT3.  EXT4's own support masks remain untouched.
perl -0pi -e 's/\n#define EXT2_FEATURE_COMPAT_SUPP.*?\n\n#define EXT4_FEATURE_COMPAT_SUPP/\n#define EXT4_FEATURE_COMPAT_SUPP/s' "$root/ext4.h"

# The signed-directory-hash flags are legitimate EXT4 on-disk flags; give them
# names owned by this implementation rather than carrying the old EXT2 prefix.
find "$root" -type f \( -name '*.c' -o -name '*.h' \) -exec \
  sed -i 's/EXT2_FLAGS_/EXT4_FLAGS_/g' {} +

# mbcache now lives inside xattr.o.  Its lifetime remains driven by the single
# EXT4 module entry point installed by the import workflow.
sed -i \
  -e '/^[[:space:]]*MODULE_AUTHOR("Jan Kara/d' \
  -e '/^[[:space:]]*MODULE_DESCRIPTION("Meta block cache/d' \
  -e '/^[[:space:]]*MODULE_LICENSE("GPL");/d' \
  "$root/xattr.c"

# Match Common's checked-addition contract in kernel space by using Linux's
# native overflow helper.  ext4_group_extend() computes the proposed size once
# and all later operations consume that validated value.
sed -i '/^#include <linux\/jiffies.h>$/a #include <linux/overflow.h>' "$root/resize.c"
perl -0pi -e 's/	ext4_grpblk_t add;\n/	ext4_grpblk_t add;\n	ext4_fsblk_t new_blocks_count;\n/' "$root/resize.c"
perl -0pi -e 's/	add = EXT4_BLOCKS_PER_GROUP\(sb\) - last;\n\n	if \(o_blocks_count \+ add < o_blocks_count\) \{\n		ext4_warning\(sb, "blocks_count overflow"\);\n		return -EINVAL;\n	\}\n\n	if \(o_blocks_count \+ add > n_blocks_count\)\n		add = n_blocks_count - o_blocks_count;\n\n	if \(o_blocks_count \+ add < n_blocks_count\)\n		ext4_warning\(sb, "will only finish group \(%llu blocks, %u new\)",\n			     o_blocks_count \+ add, add\);/	add = EXT4_BLOCKS_PER_GROUP(sb) - last;\n\n	if (check_add_overflow(o_blocks_count, (ext4_fsblk_t)add,\n			       &new_blocks_count)) {\n		ext4_warning(sb, "blocks_count overflow");\n		return -EINVAL;\n	}\n\n	if (new_blocks_count > n_blocks_count) {\n		add = n_blocks_count - o_blocks_count;\n		new_blocks_count = n_blocks_count;\n	}\n\n	if (new_blocks_count < n_blocks_count)\n		ext4_warning(sb, "will only finish group (%llu blocks, %u new)",\n			     new_blocks_count, add);/s' "$root/resize.c"
sed -i \
  -e 's/o_blocks_count + add - 1/new_blocks_count - 1/' \
  "$root/resize.c"

# Production source only: Kconfig presentation and KUnit source are not part of
# the filesystem implementation.
rm -f \
  "$root/Kconfig" "$root/.kunitconfig" \
  "$root/inode-test.c" "$root/mballoc-test.c"

rm -f \
  "$root/bitmap.c" "$root/fsync.c" "$root/hash.c" "$root/symlink.c" \
  "$root/acl.c" "$root/acl.h" \
  "$root/xattr_hurd.c" "$root/xattr_security.c" \
  "$root/xattr_trusted.c" "$root/xattr_user.c" \
  "$root/mbcache.c" "$root/include/linux/mbcache.h"

# One EXT4 module, with every real EXT4 feature engine retained.  ACL/security
# are owned by EXT4; encryption and verity remain conditional on the generic
# target-kernel frameworks they require.
cat > "$root/Makefile" <<'EOF'
# SPDX-License-Identifier: GPL-2.0
# Self-contained Infiltrator EXT4 kernel module.

obj-m += ext4.o

ext4-y := balloc.o block_validity.o dir.o ext4_jbd2.o extents.o \
          extents_status.o file.o fsmap.o ialloc.o indirect.o inline.o \
          inode.o ioctl.o mballoc.o migrate.o mmp.o move_extent.o namei.o \
          page-io.o readpage.o resize.o super.o sysfs.o xattr.o \
          fast_commit.o orphan.o jbd2_transaction.o jbd2_commit.o \
          jbd2_recovery.o jbd2_checkpoint.o jbd2_revoke.o jbd2_journal.o

ext4-$(CONFIG_FS_VERITY) += verity.o
ext4-$(CONFIG_FS_ENCRYPTION) += crypto.o

ccflags-y += -I$(src)/include
ccflags-y += -DCONFIG_EXT4_FS_POSIX_ACL=1
ccflags-y += -DCONFIG_EXT4_FS_SECURITY=1
EOF

# Forensic invariants.
test "$(find "$root" -type f | wc -l)" -eq 46
test "$(grep -R -h -o 'module_init[[:space:]]*(' "$root" | wc -l)" -eq 1
test "$(grep -R -h -o 'module_exit[[:space:]]*(' "$root" | wc -l)" -eq 1
grep -Fq 'MODULE_ALIAS_FS("ext4")' "$root/super.c"
grep -Fq 'ext4-$(CONFIG_FS_VERITY) += verity.o' "$root/Makefile"
grep -Fq 'ext4-$(CONFIG_FS_ENCRYPTION) += crypto.o' "$root/Makefile"
grep -Fq 'fast_commit.o' "$root/Makefile"
grep -Fq 'inline.o' "$root/Makefile"
grep -Fq 'extents.o' "$root/Makefile"
grep -Fq 'mmp.o' "$root/Makefile"
grep -Fq 'resize.o' "$root/Makefile"
! grep -R -n 'register_filesystem.*ext[23]_fs_type' "$root"
! grep -R -n 'IS_EXT[23]_SB\|ext[23]_feature_set_ok\|MOPT_NO_EXT[23]\|MOPT_EXT4_ONLY' "$root"
! grep -R -n 'EXT[23]_FEATURE_.*_SUPP' "$root"
! grep -R -n 'mounting ext[23] file system' "$root"
! grep -Eq 'bitmap\.o|fsync\.o|hash\.o|symlink\.o|acl\.o|mbcache\.o|xattr_(hurd|security|trusted|user)\.o' "$root/Makefile"
grep -Fq 'check_add_overflow(o_blocks_count, (ext4_fsblk_t)add,' "$root/resize.c"
! grep -Fq 'o_blocks_count + add < o_blocks_count' "$root/resize.c"
