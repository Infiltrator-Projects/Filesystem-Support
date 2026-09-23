# Canonical PFS3 core

This directory owns host-neutral PFS semantics for the PFS3 implementation.

The real PFS3 All-In-One implementation mounts the established `PFS\\1`
and `PFS\\2` disk formats; there is no invented `PFS\\3` on-disk magic.
The canonical core therefore starts with the real disk-type distinction and
the root-geometry checks required by the implementation: logical/reserved
block sizing, root-block cluster limits, reserved-area accounting, and the
rule that PFS1 media cannot advertise PFS2-only large-file or >1024-byte
reserved-block features.

