# Canonical FFS core

This directory is the destination for host-neutral FFS
filesystem semantics extracted from the real upstream Linux AFFS implementation.

It is intentionally not populated with speculative replacement code. A
subsystem moves here only when upstream-backed behaviour has been preserved and
independently qualified. Linux and Windows will consume the same canonical
implementation.
