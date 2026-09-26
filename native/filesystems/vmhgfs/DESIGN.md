# VMware HGFS Design and Ownership

## Classification

VMware HGFS is a host/guest shared-folder protocol presented as a filesystem.
It is not a local on-disk filesystem format.

The Filesystem Support catalogue currently delegates this capability to
`vmhgfs-fuse` from open-vm-tools and treats it as a userspace provider.

## Current implementation state

Filesystem Support contains no first-party HGFS implementation. The current
product path is the external userspace provider managed through the
catalogue/action backend.

The previous `.gitkeep` represented no implementation or useful ownership
boundary and is removed by this layout pass.

## Target architecture

If project-owned HGFS support is admitted:

```text
native/filesystems/vmhgfs/
  DESIGN.md
  core/                 portable HGFS protocol/namespace/session semantics
  userspace/            VMware guest transport and mount-provider integration
  reference/            optional external evidence with explicit provenance
```

There is deliberately no project `kernel/` filesystem implementation.

## Canonical ownership

A future `core/` may own HGFS message encoding/decoding, capability
negotiation, shared-folder identity, namespace operations, file-handle state,
metadata mapping and transport-independent error/cancellation semantics.

VMware backdoor/vsock/guest transport, FUSE request translation and process
lifetime belong in userspace/platform adapters.

## Safety and failure behaviour

The host is a trust boundary. Message sizes, identifiers, names and file
handles must be validated, path traversal outside an exported share must be
prevented, and ambiguous host mutation outcomes must not be reported as
confirmed durable success.

## Completion rule

The current entry is complete only as an external userspace-provider catalogue
contract. It is not a first-party HGFS filesystem implementation.
