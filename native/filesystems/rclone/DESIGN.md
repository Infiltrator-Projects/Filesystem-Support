# Rclone Remote Mount Design and Ownership

## Classification

Rclone mount is a userspace/FUSE presentation layer over remote object and file
storage providers. It is not one filesystem format and it must not become a
kernel filesystem merely to resemble local-disk entries in this catalogue.

The Filesystem Support catalogue currently delegates this capability to the
external `rclone` package and classifies it as a userspace provider.

## Current implementation state

Filesystem Support contains no first-party rclone remote-mount implementation.

The current product path is the external provider managed through the
catalogue/action backend. The previous `.gitkeep` represented no source,
provenance or useful ownership boundary and is removed by this layout pass.

## Target architecture

If project-owned remote-mount functionality is ever admitted, it belongs at
the shared userspace-service boundary:

```text
native/filesystems/rclone/
  DESIGN.md
  userspace/            rclone-compatible remote namespace/provider policy,
                        if deliberately implemented
```

There is deliberately no disk-format `core/` and no `kernel/` filesystem
module in this provider identity.

Provider-neutral HTTP/object-storage transports, credential handling, retries,
rate limiting, cache plumbing and service lifetime belong in shared userspace
infrastructure where their contracts are genuinely reusable.

## Ownership rules

A future implementation may own only the remote-filesystem presentation
semantics admitted for this provider, such as:

- mapping remote object/file identities into a mounted namespace;
- directory synthesis for object stores that do not have native directories;
- rename/copy/delete policy over provider capabilities;
- cache and consistency policy visible through the mounted namespace;
- explicit treatment of providers that cannot provide atomic rename or strong
  read-after-write behaviour;
- retry/idempotency rules for remote mutations.

It must not duplicate an underlying provider protocol merely because the
provider can be mounted through rclone. Protocol-specific implementations
belong in their appropriate reusable transport/provider layer.

## Safety and failure behaviour

Remote names, metadata, credentials and responses are untrusted input.
Authentication failures, ambiguous mutation outcome, stale cache state and
unsupported provider semantics must remain explicit. A failed or uncertain
remote write must never be reported as durably committed merely because a local
cache operation succeeded.

## Completion rule

The current entry is complete only as an external userspace-provider catalogue
contract. It is not a first-party filesystem implementation.

A future project-owned replacement requires an explicit roadmap decision,
supported-provider contract, deterministic namespace/cache tests and
interoperability/failure testing against independent remote providers.
