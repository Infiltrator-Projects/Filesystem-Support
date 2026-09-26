# ROMFS

ROMFS is currently retained as **reference/import state**.

The repository-wide native filesystem architecture is defined by
[`docs/NATIVE_CODE_ARCHITECTURE.md`](../../../docs/NATIVE_CODE_ARCHITECTURE.md),
and ROMFS-specific ownership and future source layout are defined by
[`DESIGN.md`](DESIGN.md).

The earlier plan to use ROMFS as the first native architecture proof has been
superseded by EXT2. The pinned Linux implementation is kept unchanged under
`reference/linux/` as evidence only; it is not a Filesystem Support native
ROMFS implementation.
