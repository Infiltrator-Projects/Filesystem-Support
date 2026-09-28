# SFS2 — native Linux read-only qualification adapter
obj-m += sfs2.o
sfs2-y := core_bridge.o allocation.o mapping.o io.o namespace.o lifecycle.o
ccflags-y += -I$(src)
# Mutation helpers remain compiled because current responsibility-cut units mix
# read-side lookup with those helpers. lifecycle.c forces SB_RDONLY until the
# SFS2 transaction/writer path passes destructive interoperability qualification.
ccflags-y += -DCONFIG_IFS_SFS2_RW=1
ccflags-y += -DCONFIG_IFS_SFS2_DEFAULT_CODEPAGE=\"none\"
