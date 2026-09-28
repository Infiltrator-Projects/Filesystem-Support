# SFS2 — native Linux read/write adapter
obj-m += sfs2.o
sfs2-y := core_bridge.o allocation.o mapping.o io.o namespace.o lifecycle.o
ccflags-y += -I$(src)
# SFS2 mutation paths are enabled. lifecycle.c still forces SB_RDONLY whenever
# redundant-root or transaction-marker validation finds unsafe on-disk state.
ccflags-y += -DCONFIG_IFS_SFS2_RW=1
ccflags-y += -DCONFIG_IFS_SFS2_DEFAULT_CODEPAGE=\"none\"
