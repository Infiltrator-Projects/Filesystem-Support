# EXT4 canonical core

This directory is the canonical host-neutral EXT4 filesystem layer.

EXT4 remains in active rewrite state, but the core is already active
project-authored implementation rather than a placeholder. The current core
files are:

- `ext4_core.c`
- `ext4_core.h`

They own the EXT4 format/feature and other host-neutral semantics that have
already crossed the implementation-ownership boundary.

Additional filesystem and JBD2 semantics move here feature by feature only
after their implementation bodies are independently rewritten and qualified.
The Linux tree under `../linux/` is already recut by Filesystem Support
responsibility, but many large units remain materially inherited migration
implementation. The authoritative per-unit classification is maintained in
`../../../docs/EXT_SOURCE_PROVENANCE.md`.

Do not copy, mechanically transform, rename or recomment migration-era Linux
implementation into this directory. A rule belongs in the canonical core
because it is EXT4 filesystem semantics independent of the host OS, not because
a Linux source file was moved.

Linux VFS/module/page-cache/bio integration remains in `../linux/`. A future
Windows IFS/WDK adapter belongs in `../windows/` and must consume this same
canonical core rather than implement EXT4 again.
