# Linux kernel adapter

This directory is the Linux VFS/block-device side of the native engine.

It deliberately starts with **no fake adapter implementation**. The first real
adapter will be introduced with the first real native filesystem module
(ROMFS), so every kernel-facing helper exists because a driver actually needs
it and is tested in that context.

The platform-neutral sources under `native/core/` are written so they can be
compiled by Kbuild. Their public headers use Linux kernel types and unaligned
byte helpers when `__KERNEL__` is defined, while userspace builds use
Infiltratr Common.

Kernel-specific ownership belongs here or in
`native/filesystems/<id>/kernel/`: VFS registration, fs_context, block I/O,
inode/dentry/superblock/file operations, locking, module aliases and lifecycle.

On-disk decoding, structural validation and format-specific traversal do not
belong here merely because Linux is the first supported kernel.
