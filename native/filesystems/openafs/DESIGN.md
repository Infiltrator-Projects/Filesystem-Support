# OpenAFS Design and Ownership

## Classification

OpenAFS is a distributed network filesystem/client family. Its namespace,
volume, callback/cache and authentication semantics are not a local on-disk
filesystem format.

The Filesystem Support catalogue currently exposes the external Debian
OpenAFS client and DKMS module. The separate `openafs-fuse` catalogue entry is
an alternative provider for the same filesystem family, not a second
filesystem.

## Current implementation state

Filesystem Support contains no first-party OpenAFS implementation.

The current product path is the external `openafs-client` /
`openafs-modules-dkms` provider managed through the catalogue and action
backend. The former `.gitkeep` represented no implementation or ownership
boundary and is removed by this layout pass.

## Intended architecture

If OpenAFS is promoted into first-party development, the permanent shape is:

```text
native/filesystems/openafs/
  DESIGN.md
  core/                 canonical OpenAFS client/filesystem semantics
  userspace/            shared-service provider glue
  linux/                optional thin host integration if genuinely required
  windows/              optional thin host integration if genuinely required
  reference/            optional external implementation evidence with provenance
```

A DKMS or FUSE delivery mechanism must not create an independent OpenAFS
semantic engine.

## Canonical ownership

A future `core/` may own OpenAFS-defined behaviour such as:

- cell, volume and fileserver identity/state;
- fid/object identity and namespace operations;
- callback and cache-coherency semantics;
- volume-location and failover behaviour;
- protocol request/reply validation and retry/idempotency state;
- authentication/authorisation identities as represented by OpenAFS;
- lock and mutation semantics defined by the filesystem/client protocol;
- reconnect/recovery state required after server or network failure.

Kernel VFS objects, Linux module lifetime, page cache, platform credentials and
OS error translation belong only in a host adapter. Generic sockets, TLS or
credential-storage mechanics belong in shared userspace infrastructure when
their contracts are provider-neutral.

## Provider relationship

`openafs` and `openafs-fuse` are external provider identities for the same
filesystem family. They may remain available as compatibility/interoperability
providers while a canonical project implementation is incomplete.

The project must never duplicate protocol, namespace, cache or recovery
semantics merely because one provider uses a kernel module and another uses
FUSE.

## Safety and failure behaviour

Remote peers, messages, names, identifiers and capability/authentication state
are untrusted. Lengths and identifiers must be bounded, credentials must not be
logged, retries must respect operation idempotency, stale callbacks/cache state
must not be treated as authoritative, and ambiguous mutation outcome after
transport failure must fail conservatively.

## Completion rule

The current entry is complete only as an external OpenAFS provider catalogue
contract. It is not a project-authored OpenAFS filesystem engine.

A first-party implementation requires an explicit roadmap decision, independent
protocol/interoperability fixtures, failure/reconnect qualification and at least
one provider/adapter consuming the canonical core.
