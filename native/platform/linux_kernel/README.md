# Linux kernel adapter

This directory owns reusable Linux VFS/block-device adapter source that is genuinely filesystem-neutral.

Shared source here is compiled directly into each filesystem module that consumes it. It does **not** build a shared kernel module and does not create a private runtime ABI between filesystem drivers. Each filesystem remains independently loadable, unloadable and deployable as its own `.ko`.

For example, OFS and FFS may share kernel-safe VFS compatibility, buffer, locking and other adapter primitives while `ofs.ko` still recognises and mounts only OFS and `ffs.ko` still recognises and mounts only FFS. Format-specific parsing, allocation, metadata and mutation rules remain in each filesystem's canonical core.

The platform-neutral sources under `native/core/` are written so they can be compiled by Kbuild. Their public headers use Linux kernel types and unaligned byte helpers when `__KERNEL__` is defined, while userspace builds use Infiltratr Common.

Kernel-specific reusable ownership belongs here. Filesystem-specific VFS glue belongs under `native/filesystems/<id>/linux/`.

On-disk decoding, structural validation and format-specific traversal do not belong here merely because Linux is the first supported kernel.
