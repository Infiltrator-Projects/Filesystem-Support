# Linux SFS adapter

The real ASFS Linux source is preserved unchanged in `reference/` as the
migration base. Its Linux 2.6-era VFS interfaces must be modernised before a
current-kernel module is claimed.

The final adapter will contain Linux-specific integration only; filesystem
semantics belong in `../core/`.
