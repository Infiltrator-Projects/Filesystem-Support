# davfs2 Design and Ownership

## Classification

davfs2 exposes WebDAV resources as a filesystem from userspace. It is a remote
protocol provider, not an on-disk filesystem.

The current catalogue delegates this capability to Debian's `davfs2` package.

## Current state

Filesystem Support contains no first-party davfs2 implementation. The former
`.gitkeep` has been removed.

## Target architecture

```text
native/filesystems/davfs2/
  DESIGN.md
  userspace/            WebDAV namespace/provider glue, if implemented
```

There is deliberately no `kernel/` or local-disk `core/`.

Reusable HTTP/WebDAV transport, authentication, TLS, timeout and service
lifetime mechanisms belong in shared userspace infrastructure when their
contracts are not davfs2-specific.

## Ownership

A future first-party implementation may own WebDAV-to-filesystem mapping,
collection/directory semantics, property translation, locking behaviour where
supported, cache policy and explicit write/upload semantics. It must not
duplicate unrelated local filesystem implementations.

## Safety

Remote responses, paths, redirects, credentials and certificates are untrusted
inputs. Future code must bound response sizes, reject path escape, define TLS
verification policy and keep credentials out of logs.

## Completion rule

The current entry is an external userspace-provider catalogue contract only.
