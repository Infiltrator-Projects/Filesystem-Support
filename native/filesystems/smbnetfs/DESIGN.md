# SMBNetFS Design and Ownership

## Classification

SMBNetFS is a userspace/FUSE namespace provider that exposes SMB workgroups,
servers and shares beneath one mounted hierarchy. It is not a separate SMB
protocol or filesystem format.

The Filesystem Support catalogue currently delegates this capability to the
external `smbnetfs` package.

## Current implementation state

Filesystem Support contains no first-party SMBNetFS implementation. The current
product path is the external userspace provider managed through the
catalogue/action backend.

The previous `.gitkeep` represented no implementation or useful ownership
boundary and is removed by this layout pass.

## Target architecture

If equivalent functionality is later implemented, it belongs at the userspace
namespace/service boundary:

```text
native/filesystems/smbnetfs/
  DESIGN.md
  userspace/            SMB discovery/namespace composition policy, if admitted
```

There is deliberately no independent SMB `core/` and no `kernel/`
filesystem module in this provider identity.

SMB protocol/session/file semantics belong to the canonical SMB/CIFS engine
under `native/filesystems/cifs/`. SMBNetFS-specific code may compose and
browse those resources but must not create another SMB protocol stack.

## Ownership rules

A future implementation may own:

- workgroup/server/share discovery and namespace synthesis;
- mapping discovered SMB resources into one hierarchical mount;
- provider configuration and per-share routing;
- lifecycle/error propagation across multiple SMB sessions.

Authentication, SMB dialect negotiation, signing/encryption, leases, file
handles and ordinary SMB namespace/data operations remain with the canonical
SMB/CIFS implementation.

## Safety and failure behaviour

Remote names and discovery responses are untrusted. Namespace synthesis must
prevent path traversal/collision ambiguity, authentication failure must remain
explicit, and loss of one server/share must not corrupt unrelated mounted
namespace state.

## Completion rule

The current entry is complete only as an external userspace-provider catalogue
contract. It is not a first-party SMB filesystem engine.
