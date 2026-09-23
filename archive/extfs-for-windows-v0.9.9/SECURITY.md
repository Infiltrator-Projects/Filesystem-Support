<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Security and safety boundary

ExtFS remains an experimental kernel filesystem driver. A defect can crash Windows or corrupt a test filesystem. Use only backed-up/disposable media and a disposable test machine/VM until runtime qualification is complete.

0.9.2 is the forensic-audit hardening checkpoint for the bounded 0.9 feature set. The ext2 direct-resize path now requires a host durability barrier before any metadata-changing resize begins. If the host cannot flush writes to stable storage, the operation is refused before the dirty superblock or any other filesystem byte is written. Once mutation begins, the dirty superblock is flushed before mutation, all mutation is flushed before the clean marker, and the clean marker is flushed before success is reported. Failure at a durability boundary leaves the mounted view dirty/fail-closed.

The bounded ext4 resizer still requires 32-byte group descriptors, `metadata_csum`, a supported clean internal JBD2 journal, and at most one allocation group change per resize. Bitmap/group/inode/superblock and external extent-leaf checksums are validated or rebuilt before journal commit. Newly allocated data is zeroed and flushed before growth metadata becomes durable.

0.9.2 adds destructive qualification against disposable real ext2/ext3/ext4 images followed by reopen, content comparison and `e2fsck`, in addition to synthetic failure injection. Windows runtime mutation tests remain opt-in and must be performed only on a disposable backed-up test volume.

Depth greater than one, more than one external leaf, sparse or unwritten allocation, 64-bit/flex_bg layouts, dirty journals, external inode xattr blocks and unknown write-sensitive features remain refused by the bounded resizer. Namespace mutation, paging writes and exclusive volume-lock ownership are not yet implemented.
