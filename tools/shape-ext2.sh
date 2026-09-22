#!/usr/bin/env bash
set -euo pipefail

root="${1:-native/filesystems/ext2/kernel}"

for required in \
  Makefile ext2.h file.c inode.c xattr.c acl.c acl.h ioctl.c symlink.c \
  xattr.h xattr_user.c xattr_trusted.c xattr_security.c mbcache.c \
  include/linux/mbcache.h; do
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

prepend_after_spdx() {
  local target="$1"
  shift
  local tmp
  tmp="$(mktemp)"
  {
    head -n 1 "$target"
    printf '%s\n' "$@"
    tail -n +2 "$target"
  } > "$tmp"
  mv "$tmp" "$target"
}

# Consolidate the private EXT2 headers. These declarations describe one
# filesystem implementation and do not need three internal header boundaries.
{
  printf '\n/* ---- EXT2 ACL definitions ---- */\n'
  cat "$root/acl.h"
  printf '\n/* ---- EXT2 extended-attribute definitions ---- */\n'
  cat "$root/xattr.h"
  printf '\n/* ---- EXT2 private metadata-block cache definitions ---- */\n'
  cat "$root/include/linux/mbcache.h"
} >> "$root/ext2.h"

# file.c owns regular-file behaviour, including EXT2 ioctls and file attributes.
prepend_after_spdx "$root/file.c" \
  '#include <linux/capability.h>' \
  '#include <linux/compat.h>' \
  '#include <linux/fileattr.h>' \
  '#include <linux/mount.h>' \
  '#include <linux/uaccess.h>'
append_without_includes "$root/file.c" "$root/ioctl.c" "EXT2 file ioctl and file-attribute operations"

# inode.c owns inode behaviour, including both normal and fast symlinks.
append_without_includes "$root/inode.c" "$root/symlink.c" "EXT2 symlink inode operations"

# xattr.c owns the complete optional metadata subsystem: user/trusted/security
# xattrs, POSIX ACL storage and the private mbcache implementation.
prepend_after_spdx "$root/xattr.c" \
  '#include <linux/capability.h>' \
  '#include <linux/list.h>' \
  '#include <linux/list_bl.h>' \
  '#include <linux/module.h>' \
  '#include <linux/posix_acl_xattr.h>' \
  '#include <linux/sched.h>' \
  '#include <linux/spinlock.h>' \
  '#include <linux/string.h>' \
  '#include <linux/workqueue.h>'

append_without_includes "$root/xattr.c" "$root/xattr_user.c" "EXT2 user xattr handler"
append_without_includes "$root/xattr.c" "$root/xattr_trusted.c" "EXT2 trusted xattr handler"

{
  printf '\n#ifdef CONFIG_EXT2_FS_SECURITY\n'
  append_without_includes /dev/stdout "$root/xattr_security.c" "EXT2 security xattr handler"
  printf '#endif /* CONFIG_EXT2_FS_SECURITY */\n'
} >> "$root/xattr.c"

{
  printf '\n#ifdef CONFIG_EXT2_FS_POSIX_ACL\n'
  append_without_includes /dev/stdout "$root/acl.c" "EXT2 POSIX ACL implementation"
  printf '#endif /* CONFIG_EXT2_FS_POSIX_ACL */\n'
} >> "$root/xattr.c"

append_without_includes "$root/xattr.c" "$root/mbcache.c" "EXT2 private metadata-block cache"

# Every surviving translation unit gets its private EXT2 metadata declarations
# through ext2.h. The old separate xattr/ACL headers no longer exist.
find "$root" -maxdepth 1 -type f -name '*.c' -exec \
  sed -i '/^[[:space:]]*#include[[:space:]]*"xattr\.h"/d; /^[[:space:]]*#include[[:space:]]*"acl\.h"/d; /^[[:space:]]*#include[[:space:]]*<linux\/mbcache\.h>/d' {} +

# Eight implementation units, one private header and one Kbuild file.
cat > "$root/Makefile" <<'EOF'
# SPDX-License-Identifier: GPL-2.0
# Self-contained Infiltrator EXT2 kernel module.

obj-m += ext2.o

ext2-y := balloc.o dir.o file.o ialloc.o inode.o namei.o super.o
ext2-$(CONFIG_EXT2_FS_XATTR) += xattr.o

ccflags-y += -I$(src)
EOF

rm -f \
  "$root/acl.c" \
  "$root/acl.h" \
  "$root/ioctl.c" \
  "$root/symlink.c" \
  "$root/xattr.h" \
  "$root/xattr_user.c" \
  "$root/xattr_trusted.c" \
  "$root/xattr_security.c" \
  "$root/mbcache.c" \
  "$root/include/linux/mbcache.h"
rmdir "$root/include/linux" "$root/include" 2>/dev/null || true

# Structural invariants. EXT2 is deliberately compact and independent.
expected='Makefile
balloc.c
dir.c
ext2.h
file.c
ialloc.c
inode.c
namei.c
super.c
xattr.c'
actual="$(find "$root" -maxdepth 1 -type f -printf '%f\n' | sort)"
test "$actual" = "$expected"
test "$(find "$root" -type f | wc -l)" -eq 10

! grep -R -n '#include "xattr.h"\|#include "acl.h"\|#include <linux/mbcache.h>' "$root"
! grep -R -n 'trace_ext2_dio_' "$root"
! grep -R -n 'EXT3_DEFM_JMODE' "$root"
grep -Fq 'error: journalled filesystem is not EXT2' "$root/super.c"
grep -Fq 'ext2-$(CONFIG_EXT2_FS_XATTR) += xattr.o' "$root/Makefile"
! grep -Eq 'ioctl\.o|symlink\.o|acl\.o|mbcache\.o|xattr_(user|trusted|security)\.o' "$root/Makefile"
