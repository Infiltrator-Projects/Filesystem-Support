# SSHFS Design and Ownership

## Classification

SSHFS is a userspace/FUSE provider that presents a remote SFTP namespace as a
mounted filesystem. It is not a local disk format and does not justify a
project kernel filesystem module.

The Filesystem Support catalogue currently delegates this capability to the
external `sshfs` package.

## Current implementation state

Filesystem Support contains no first-party SSHFS implementation. The current
product path is the external userspace provider managed through the
catalogue/action backend.

The previous `.gitkeep` represented no implementation or useful ownership
boundary and is removed by this layout pass.

## Target architecture

If project-owned SSHFS-compatible functionality is admitted later:

```text
native/filesystems/sshfs/
  DESIGN.md
  core/                 optional portable SFTP namespace/file semantics
  userspace/            SSH/SFTP transport and mount-provider integration
```

There is deliberately no `kernel/` filesystem implementation.

Generic SSH transport, credential/key handling, cancellation, retries and
userspace mount-service lifetime should be shared where their contracts are
provider-neutral.

## Ownership rules

A future implementation may own the filesystem-facing mapping of SFTP
operations: namespace traversal, file handles, attributes, symlinks, rename and
failure mapping. It must not invent stronger durability, locking or atomicity
than the remote SFTP server actually provides.

## Safety and failure behaviour

Remote names, attributes and responses are untrusted. Host-key/authentication
failure must fail closed, path translation must remain within the selected
remote root, and uncertain remote mutation outcome must not be reported as a
confirmed durable write.

## Completion rule

The current entry is complete only as an external userspace-provider catalogue
contract. It is not a first-party SSHFS implementation.
