# S3FS Design and Ownership

## Classification

S3FS is a userspace/FUSE presentation layer over S3-compatible object storage.
It is not a local on-disk filesystem format.

The Filesystem Support catalogue currently delegates this capability to the
external `s3fs` package and classifies it as a userspace provider.

## Current implementation state

Filesystem Support contains no first-party S3FS implementation.

The current product path is the external provider managed through the
catalogue/action backend. The previous `.gitkeep` represented no
implementation or useful ownership boundary and is removed by this layout pass.

## Target architecture

If a first-party S3-backed filesystem is ever admitted, it belongs at the
userspace-service boundary:

```text
native/filesystems/s3fs/
  DESIGN.md
  userspace/            S3 namespace/provider policy, if implemented
```

There is deliberately no disk-format `core/` and no `kernel/` filesystem
module in this provider identity.

Generic HTTP/TLS, credential, retry, rate-limit, object-cache and service
lifetime mechanisms belong in shared userspace infrastructure when reusable.

## Ownership rules

A future implementation may own:

- mapping object keys into a mounted hierarchical namespace;
- directory synthesis and marker-object policy;
- metadata representation that S3 itself does not natively provide;
- multipart upload and read/write cache policy;
- rename/copy/delete semantics over an object store;
- retry/idempotency behaviour and explicit handling of uncertain remote writes.

It must not pretend S3 provides POSIX atomic rename, locking or directory
semantics where the backing service does not.

## Safety and failure behaviour

Remote keys, metadata and responses are untrusted input. Authentication
failures, ambiguous mutation outcome, stale cache state and provider
inconsistency must remain explicit. A local cache update must never be reported
as durable remote publication until the provider contract says it is.

## Completion rule

The current entry is complete only as an external userspace-provider catalogue
contract. It is not a first-party filesystem implementation.
