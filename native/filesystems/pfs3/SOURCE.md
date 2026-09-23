# PFS3 source base

PFS3 is a separate filesystem implementation.

The authoritative C implementation reference preserved here is PFS3
All-In-One:

- upstream repository: `tonioni/pfs3aio`
- pinned commit: `211f7f06aa29a3a2f9d8983f42beddab7ea60ce4`
- licence: upstream BSD 4-Clause, preserved verbatim as
  `reference/pfs3aio/LICENSE`

The complete top-level filesystem C/header implementation is retained under
`reference/pfs3aio/`.

The BSD-4-Clause source is not silently treated as GPL-2-compatible Linux
kernel code. It is implementation evidence and the migration base, not yet
production-linked into a Linux module. The production licence boundary remains
explicit while canonical semantics and host adapters are developed.

No PFS3 filesystem behaviour should be invented from a prose format summary
when the real upstream implementation already defines it.
