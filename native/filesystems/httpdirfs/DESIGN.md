# httpdirfs Design and Ownership

## Classification

httpdirfs is a userspace/FUSE provider that exposes files discovered through an
HTTP directory/index as a filesystem-style namespace. It is a remote protocol
adapter, not an on-disk filesystem format.

Filesystem Support therefore treats httpdirfs as a userspace provider identity,
not as a candidate kernel filesystem module.

## Current implementation state

Filesystem Support contains no first-party httpdirfs implementation.

The current product path is an external provider managed through the filesystem
catalogue and action backend. This directory exists only to record the correct
ownership and future architecture.

## Intended layout

```text
native/filesystems/httpdirfs/
  DESIGN.md
  userspace/            HTTP namespace/provider glue, if ever implemented
```

There is deliberately no disk-format `core/`, no `linux/` VFS filesystem
implementation and no project `.ko` for this provider.

If a project-owned provider is later admitted, reusable HTTP/TLS, URL parsing,
credential, timeout, caching and service-lifetime mechanisms belong in shared
Filesystem Support userspace infrastructure when their contracts are genuinely
provider-neutral.

httpdirfs-specific code may own only the mapping from remote HTTP resources and
index/listing information into a filesystem-style namespace.

## Safety and failure behaviour

Remote servers, redirects, paths, filenames, lengths and content metadata are
untrusted input.

A future implementation must:

- validate URLs and redirect policy;
- reject path traversal and namespace escape;
- bound response, listing and file sizes;
- define TLS verification and credential handling explicitly;
- avoid shell command construction;
- surface partial or stale remote state rather than inventing local certainty;
- keep credentials and sensitive headers out of logs.

## Completion rule

The current entry is complete only as an external userspace-provider catalogue
contract. It is not a project-authored filesystem implementation.

A first-party replacement would require an explicit roadmap decision,
deterministic protocol/namespace tests and a deliberate transition away from the
external provider.
