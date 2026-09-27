# SFS — Linux adapter build contract
obj-m += sfs.o
sfs-y := core_bridge.o allocation.o mapping.o io.o namespace.o lifecycle.o
ccflags-y += -I$(src)
ccflags-y += -DCONFIG_IFS_SFS_RW=1
ccflags-y += -DCONFIG_IFS_SFS_DEFAULT_CODEPAGE=\"none\"
