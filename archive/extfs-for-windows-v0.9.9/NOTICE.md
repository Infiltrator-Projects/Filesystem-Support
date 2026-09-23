<!-- SPDX-License-Identifier: GPL-3.0-or-later -->

# Technical sources and provenance

ExtFS 0.9.2 is an original implementation. No source code from Linux, Ext2Fsd, Paragon, WinFsp or Microsoft filesystem samples is included.

The ExtFS source, build files and project documentation are distributed under `GPL-3.0-or-later`. Version 0.9.1 was the licensing-standardisation checkpoint; 0.9.2 is the subsequent forensic-audit durability and qualification hardening checkpoint.

The ext and JBD2 on-disk formats were implemented from published technical material, including:

- https://docs.kernel.org/filesystems/ext4/
- https://docs.kernel.org/filesystems/ext4/journal.html
- https://github.com/torvalds/linux/blob/master/include/linux/jbd2.h

The Windows integration follows published Microsoft WDK/IFS interfaces:

- https://learn.microsoft.com/windows-hardware/drivers/ifs/
- https://learn.microsoft.com/windows-hardware/drivers/ifs/creating-an-inf-file-for-a-file-system-driver
- https://learn.microsoft.com/windows-hardware/drivers/ddi/ntifs/nf-ntifs-ioregisterfilesystem
- https://learn.microsoft.com/windows-hardware/drivers/ddi/wdm/nf-wdm-kequerysystemtimeprecise

Microsoft FASTFAT/CDFS are architectural references only; their source is not copied into ExtFS.

Copyright (c) 1993-2026 Shannon Smith.
