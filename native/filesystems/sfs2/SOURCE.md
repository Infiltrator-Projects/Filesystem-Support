# SFS2 source base

SFS2 is maintained as a separate filesystem because its on-disk format is not
1:1 compatible with SFS.

The pinned SFS2-capable implementation reference is Aaru Data Preservation
Suite's SmartFileSystem reader:

- upstream repository: `aaru-dps/Aaru`
- pinned commit: `818eac0d7b2832772ef5ff3133986b598e051b46`
- upstream path: `Aaru.Filesystems/SFS/`
- licence: LGPL-2.1-or-later, retained in each imported source file

The exact implementation is preserved under `reference/aaru/`. It explicitly
implements `SFS\\2` / structure version 4, including the different object
layout, 32-bit extent block counts and 48-bit file-size encoding.

The imported C# implementation is reference/migration input, not production
kernel code. The canonical SFS2 engine will be adapted from real implementation
behaviour and independently qualified. SFS2 filesystem rules must not be hidden
inside the SFS core.
