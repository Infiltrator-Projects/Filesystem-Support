# EXT4 — Linux adapter build contract
obj-m += ext4.o

ext4-y := core_bridge.o storage_guard.o directory_io.o journal_durability.o \
          extent_tree.o extent_cache.o fast_commit_engine.o file_io.o \
          inode_allocation.o inline_data.o inode_adapter.o control.o \
          journal_core.o journal_transactions.o mapping_support.o \
          multiblock_allocation.o namespace_mutation.o orphan_recovery.o \
          writeback_io.o online_resize.o lifecycle.o volume_admin.o \
          extended_metadata.o security_support.o

ccflags-y += -I$(src)/include
ccflags-y += -DCONFIG_EXT4_FS_POSIX_ACL=1
ccflags-y += -DCONFIG_EXT4_FS_SECURITY=1
