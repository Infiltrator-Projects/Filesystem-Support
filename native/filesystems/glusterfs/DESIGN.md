# GlusterFS Design and Ownership

## Classification

GlusterFS is a distributed network filesystem whose client composes remote
storage volumes. The current Debian client is userspace/FUSE.

This is exactly the class of filesystem for which Filesystem Support's shared
userspace-service boundary is preferred over inventing an out-of-tree kernel
module.

## Current state

There is no first-party GlusterFS client implementation in this repository.
The former `.gitkeep` has been removed; Debian's `glusterfs-client` remains
the current provider.

## Target architecture

If project-owned support is later admitted:

```text
native/filesystems/glusterfs/
  DESIGN.md
  core/                 portable Gluster client/protocol/namespace semantics
  userspace/            provider glue for shared userspace service
```

There is deliberately no project `kernel/` client unless a future decision
proves a kernel-specific implementation is technically necessary; even then it
must consume the same canonical client semantics rather than fork them.

## Ownership

The core may own Gluster-defined volume/client protocol, namespace routing,
replica/distribution policy visible to the client, failover/retry state and
metadata consistency semantics. Generic sockets, TLS, credentials, worker/event
loops and mount-service lifecycle belong in shared userspace infrastructure.

## Safety

Cluster endpoints and replies are untrusted. Authentication, transport
integrity, retry/idempotency, split-brain/partial-failure presentation and
write acknowledgement semantics require explicit contracts before first-party
mutation is claimed.

## Completion rule

The present entry is an external userspace-provider catalogue contract only.
