# SFS implementation ownership

SFS is an independent Filesystem Support implementation.

Its canonical filesystem semantics live in `core/`. Linux and Windows are
host adapters around that SFS-owned core and do not provide alternate
filesystem implementations.

The active SFS implementation is project-authored. No imported or reference
implementation is part of the production tree.

SFS and SFS2 remain separate filesystems and must not share filesystem-specific
parsing, allocation, mapping, namespace, metadata, recovery or mutation code.
