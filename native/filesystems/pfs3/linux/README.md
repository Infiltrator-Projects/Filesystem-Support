# PFS3 Linux adapter

This directory contains the native Linux VFS adapter for the project-owned PFS3
core.

The adapter currently mounts and reads supported PFS media, including directory
traversal, anode-chain mapping, soft links, validated hard-link identity,
linear/split anode numbering, super-index traversal and large-file decoding
where the canonical core admits the layout.  The current Linux adapter is
still deliberately read-only; that is a real implementation boundary, not a
compile-time qualification claim.

Full writable parity requires the original PFS3 allocation, metadata
publication and postponed-operation/recovery semantics to be represented in
the project-owned implementation and then exercised on genuine media.  Until
that gate is passed, the module must continue to fail closed rather than
silently corrupting PFS media.

The preserved PFS3aio tree under `../reference/pfs3aio/` is non-shipping
behavioural evidence for the incremental rewrite. It is never linked into the
GPL Linux module.
