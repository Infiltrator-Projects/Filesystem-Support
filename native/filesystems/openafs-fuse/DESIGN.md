# OpenAFS via FUSE Provider Ownership

## Classification

`openafs-fuse` is an external userspace/FUSE provider for the OpenAFS
filesystem family. It is not a separate filesystem format and must not own an
independent OpenAFS implementation.

The catalogue currently exposes Debian's experimental `openafs-fuse` path for
systems where the normal OpenAFS client/kernel module is unsuitable. Its
provider limitations remain provider-specific; they do not redefine OpenAFS
filesystem semantics.

## One OpenAFS rule

Filesystem Support has one canonical ownership point for OpenAFS semantics:

```text
native/filesystems/openafs/
  core/                 canonical OpenAFS client/filesystem semantics
  userspace/            shared-service provider glue
  linux/                optional host integration
  windows/              optional host integration
```

A future first-party FUSE/userspace path must consume that same canonical
OpenAFS core. It must not implement a second protocol, namespace, callback,
cache, authentication or recovery engine here.

## Current implementation state

There is no project-authored source in this directory. The former `.gitkeep`
represented no implementation or ownership contract and is removed by this
layout pass.

The current product path remains the external `openafs-fuse` provider managed
through the catalogue/action backend.

## Intended directory shape

```text
native/filesystems/openafs-fuse/
  DESIGN.md
```

There is intentionally no `core/`, `linux/`, `windows/` or `kernel/`
implementation in this provider identity.

If project-owned userspace OpenAFS mounting is later implemented, it belongs
under the canonical `openafs/userspace/` adapter.

## Safety and failure behaviour

Provider availability is not evidence of full OpenAFS compatibility or safe
write support. The manager must preserve the provider's explicit experimental
and read-only limitations where applicable and must not infer capabilities from
package installation alone.

## Completion rule

This entry is complete only as an external provider catalogue contract. It is
never counted as an independent OpenAFS implementation.
