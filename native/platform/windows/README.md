# Windows native platform adapter

This directory is the Windows operating-system boundary for canonical
Filesystem Support engines.

It must contain only Windows integration: IFS/WDK lifecycle, IRP dispatch,
VCB/FCB/CCB ownership, Cache Manager and Memory Manager integration, Windows
locking, mount/dismount, block-device I/O adaptation, NTSTATUS translation and
driver package integration.

Filesystem-format semantics do **not** belong here. EXT2 superblock parsing,
inode rules, block mapping, allocation, directory semantics and other EXT2
logic must come from the same canonical EXT2 engine used by the Linux adapter.

The former ExtFS-for-Windows driver is preserved under
`archive/extfs-for-windows-v0.9.9/` as migration evidence. Its Windows
lifecycle patterns may be generalized here, but calls into the old
`core/extfs*.c` implementation must not be copied into the production path.

EXT2 is the first proving module. No Windows filesystem module is considered
installable until the shared canonical engine and this adapter pass the Windows
qualification gate.
