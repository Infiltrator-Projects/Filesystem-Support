# onedriver Design and Ownership

## Classification

onedriver exposes Microsoft OneDrive cloud storage through a userspace/FUSE
namespace. It is a remote cloud-service provider, not an on-disk filesystem
format and not a candidate native block-filesystem kernel module.

The Filesystem Support catalogue currently delegates this capability to the
external Debian `onedriver` package and classifies it as a userspace provider.

## Current implementation state

Filesystem Support contains no first-party onedriver implementation.

The current product path is the external provider managed through the catalogue
and action backend. The former `.gitkeep` represented no implementation or
ownership boundary and is removed by this layout pass.

## Intended architecture

If first-party OneDrive support is ever admitted, it belongs at the shared
userspace-service boundary:

```text
native/filesystems/onedriver/
  DESIGN.md
  core/                 portable OneDrive service/object semantics, if justified
  userspace/            namespace/provider integration
```

There is deliberately no `kernel/`, Linux VFS filesystem engine or project
`.ko` here.

A portable `core/` is warranted only if the project owns substantial
OneDrive-specific protocol/object semantics that need to be shared across
platforms. Generic HTTP/TLS, OAuth credential handling, request scheduling,
caching, retry/backoff, durable local state and mount-service lifetime belong in
shared Filesystem Support userspace infrastructure when their contracts are
provider-neutral.

## Ownership

A future first-party implementation may own OneDrive-specific behaviour such as:

- drive/item identity and hierarchy mapping;
- path-to-object translation and rename/move semantics;
- remote metadata and timestamp interpretation;
- upload/download session behaviour;
- change/delta tracking and cache-coherency inputs;
- conflict, replacement and delete semantics defined by the service;
- explicit offline/stale-state presentation.

It must not duplicate the local filesystem that stores its cache or any
unrelated cloud-provider implementation.

## Safety and failure behaviour

Remote responses, names, identifiers, lengths and redirects are untrusted.
Credentials and refresh tokens must never be logged. Mutating requests require
explicit retry/idempotency rules so transport failure cannot silently duplicate
or lose operations. Partial uploads/downloads must not be presented as complete,
and namespace mapping must reject path escape and ambiguous collisions.

## Completion rule

The current entry is complete only as an external cloud/userspace-provider
catalogue contract. It is not a project-authored filesystem implementation.

A first-party replacement requires an explicit roadmap decision, deterministic
service/namespace tests, failure/retry qualification and a deliberate transition
away from the external provider.
