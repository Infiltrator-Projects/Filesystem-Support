# Filesystem Support

Filesystem Support is a native Linux Mint/Ubuntu desktop utility for discovering and enabling filesystem implementations without requiring users to remember package names or kernel modules.

The program presents filesystems by human name, detects the support already present on the running system, and will provide an explicit install action for missing support.

## Initial scope

The catalogue starts with EXT2/3/4, SGI XFS, Btrfs, F2FS, IBM JFS, NILFS2, FAT, exFAT, Amiga AFFS, Apple HFS/HFS+, ISO 9660, UDF, SquashFS, UFS, OpenZFS, VMFS6, CP/M, SMB/CIFS and NFS.

## Shared library pin

Filesystem Support is pinned to Infiltratr Common **1.19.22**, commit `302c44eb7436803dee020667453a9a0681da8bbf`.

## Design

The application separates its filesystem catalogue, read-only support probing, installation backend and GTK shell so new filesystem definitions do not require GUI-specific branches.

## Licence

GPL-3.0-or-later.
