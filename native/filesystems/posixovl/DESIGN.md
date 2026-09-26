# POSIXOVL Design and Ownership

## Classification

POSIXOVL is a userspace/FUSE metadata overlay for filesystems that do not
natively provide the POSIX ownership, permission and symbolic-link semantics
expected by Unix applications. It is not an on-disk filesystem format and it
must not become a second FAT, VFAT, exFAT or NTFS implementation.

The Filesystem Support catalogue currently delegates this capability to
Debian's `fuse-posixovl` package and classifies it as a userspace provider.
The underlying filesystem remains owned by its own canonical implementation.

## Current implementation state

Filesystem Support contains no first-party POSIXOVL implementation.

The current product path is the external `fuse-posixovl` provider managed
through the catalogue/action backend. The previous `.gitkeep` represented no
implementation, provenance or useful ownership boundary and is removed by this
layout pass.

## Target architecture

If a first-party equivalent is ever admitted, it belongs at the userspace
service boundary:

```text
native/filesystems/posixovl/
  DESIGN.md
  userspace/            POSIX metadata-overlay policy/provider, if implemented
```

There is deliberately no disk-format `core/` and no `kernel/` filesystem
module in this provider identity.

Generic mount-service lifetime, path validation, credential handling and
persistent side-metadata storage mechanisms should be shared when their
contracts are provider-neutral.

## Ownership rules

A future POSIXOVL implementation may own:

- mapping host POSIX uid/gid/mode semantics onto separately stored overlay
  metadata;
- symbolic-link representation where the backing filesystem cannot represent
  POSIX links directly;
- deterministic metadata lookup, update and removal rules;
- rename/link/unlink interaction between the presented namespace and the
  overlay metadata;
- validation and recovery of the overlay's own side metadata.

It must not implement the backing filesystem's directory format, allocation,
block mapping, journalling, repair or native security model.

## Safety and failure behaviour

The overlay must never report a POSIX metadata update as durable when the
backing object and side metadata are left in a contradictory state. Path
translation must stay inside the configured backing tree, identity values must
be range checked, and malformed or stale side metadata must fail explicitly
rather than silently granting broader permissions.

## Completion rule

The current entry is complete only as an external userspace-provider catalogue
contract. It is not a first-party filesystem implementation.

A future project-owned replacement requires an explicit roadmap decision,
userspace-service design, deterministic metadata/rename/link tests, crash and
stale-side-metadata recovery tests, and a deliberate transition away from the
external provider.
