# MooseFS Design and Ownership

## Classification

MooseFS is a distributed network filesystem. Its client-side protocol,
namespace and consistency semantics are not a local on-disk filesystem and
should not be forced into a project out-of-tree kernel module.

## Current implementation state

Filesystem Support contains no first-party MooseFS client implementation. The
current product path is an external provider managed through the catalogue and
action backend. The former `.gitkeep` represented no implementation and is
removed by this layout pass.

## Intended architecture

```text
native/filesystems/moosefs/
  DESIGN.md
  core/                 portable MooseFS client/protocol semantics, if implemented
  userspace/            provider glue for the shared userspace service
```

There is deliberately no local-block-filesystem `linux/` implementation or
project `.ko` unless a future platform requirement proves a host-specific
adapter is necessary. Any such adapter must consume the same canonical client
core.

## Ownership

A future core may own MooseFS-defined namespace, metadata/session protocol,
chunk/file mapping, replica/location information exposed to the client,
failover/retry behaviour and consistency/recovery state.

Generic sockets, TLS/credentials where applicable, worker/event loops and
mount-service lifetime belong in shared userspace infrastructure when reusable.

## Safety and failure behaviour

Remote peers and metadata are untrusted. Message lengths, object/chunk
identifiers, retry/idempotency and partial-failure behaviour must be bounded and
explicit. A successful transport exchange is not by itself evidence that a
mutation is durably committed.

## Completion rule

The current entry is complete only as an external distributed-filesystem
provider contract. It is not a project-authored MooseFS engine.
