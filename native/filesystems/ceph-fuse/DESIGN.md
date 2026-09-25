# CephFS via FUSE Provider Ownership

## Classification

`ceph-fuse` is an external userspace provider identity for the CephFS
distributed filesystem. It is not a separate filesystem format.

The catalogue currently delegates this access path to Debian's `ceph-fuse`
package.

## One CephFS rule

Filesystem Support must not implement CephFS once for a kernel client and again
for a FUSE client. CephFS protocol, namespace, metadata and consistency
semantics belong to one canonical CephFS implementation if the project later
promotes CephFS into first-party development.

The delivery mechanism is an adapter/provider choice, not a filesystem
identity.

## Current state

There is no first-party code in this directory. The former `.gitkeep`
represented no implementation and has been removed.

## Intended shape

```text
native/filesystems/ceph-fuse/
  DESIGN.md
```

This provider-specific directory intentionally owns no `core/` and no
`kernel/`.

A future project CephFS implementation should live under the canonical CephFS
filesystem identity and use the shared userspace-service boundary for
distributed/network operation unless a separately justified host adapter is
required.

## Safety

The manager must report this provider's availability and limitations without
equating package installation with full CephFS feature or failure-mode
qualification by Filesystem Support.

## Completion rule

This entry is complete as an external provider catalogue contract only. It must
not be counted as a project-authored CephFS engine.
