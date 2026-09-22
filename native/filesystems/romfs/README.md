# ROMFS

ROMFS is the first native filesystem target because it is intentionally small
and read-only, making it suitable for proving the shared engine/VFS boundary
without mixing the architecture exercise with a complex allocator or recovery
protocol.

The first ROMFS milestone will add format structures, a userspace probe/parser,
independent fixtures, corruption tests, read-only traversal,
`infiltratr-romfs.ko`, VFS mount tests and side-by-side comparison with the
existing Linux provider.

No placeholder parser or fake kernel module is committed here.
