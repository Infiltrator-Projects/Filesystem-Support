# Amiga Smart File System (SFS) — out-of-tree Linux module.
#
# Linux-facing VFS interfaces are modernised for current kernels.

obj-m += sfs.o

sfs-y := canonical.o dir.o extents.o file.o inode.o namei.o nodes.o objects.o super.o \
         symlink.o adminspace.o bitfuncs.o

ccflags-y += -I$(src)
ccflags-y += -DCONFIG_ASFS_RW=1
ccflags-y += -DCONFIG_ASFS_DEFAULT_CODEPAGE=\"none\"
