# AVFS Design and Ownership

## Classification

AVFS is a userspace virtual-filesystem provider that exposes archives,
compressed files, disk images and remote resources through a filesystem-style
namespace. It is not one on-disk filesystem format.

The Filesystem Support catalogue currently delegates this capability to
Debian's `avfs` package and classifies it as a userspace provider.

## Current implementation state

Filesystem Support contains no first-party AVFS implementation. The previous
`.gitkeep` represented no code and has been removed.

Provider discovery and package actions remain in the catalogue/action backend.
This document exists to make the architectural boundary explicit.

## Target architecture

Any future first-party AVFS-equivalent belongs at the userspace boundary:

```text
native/filesystems/avfs/
  DESIGN.md
  userspace/            AVFS-specific namespace/provider policy, if admitted
```

There is deliberately no `kernel/` directory and no single disk-format
`core/`. AVFS composes multiple archive, image and remote protocols; forcing
those unrelated semantics into one kernel filesystem would violate the
canonical-engine rule.

Reusable archive readers, remote transports, bounded I/O, mount-service
lifetime and namespace mechanisms should live in shared userspace
infrastructure or in the canonical implementation of the actual underlying
format/protocol.

## Ownership rules

AVFS-specific code, if ever implemented, may own:

- the synthetic namespace used to expose heterogeneous resources;
- dispatch from a namespace path to the appropriate underlying provider;
- composition policy across archive, image and remote backends;
- lifecycle/error propagation across mounted or opened resources.

It must not duplicate ZIP, ISO, SSH, WebDAV or other underlying
format/protocol implementations merely because AVFS can expose them.

## Safety

Paths, URLs, archive names and remote metadata are untrusted input. A future
implementation must use structured provider calls, reject namespace traversal,
bound resource expansion and avoid shell command construction.

## Completion rule

The current entry is complete only as an external userspace-provider catalogue
contract. It is not a project-authored filesystem implementation.
