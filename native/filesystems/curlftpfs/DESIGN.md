# CurlFtpFS Design and Ownership

## Classification

CurlFtpFS is a userspace/FUSE provider that exposes FTP servers as a filesystem
namespace. It is a remote protocol adapter, not an on-disk filesystem.

The current catalogue delegates this capability to Debian's `curlftpfs`
package.

## Current state

Filesystem Support contains no first-party CurlFtpFS implementation. The former
`.gitkeep` represented no code and has been removed.

## Target architecture

```text
native/filesystems/curlftpfs/
  DESIGN.md
  userspace/            FTP namespace/provider glue, if implemented
```

There is deliberately no `kernel/` and no local-filesystem `core/`.

If FTP protocol functionality becomes first-party, reusable transport,
credential, URL, timeout and service-lifetime logic belongs in shared userspace
infrastructure. CurlFtpFS-specific code owns only the mapping between FTP
operations/listings and filesystem-style namespace semantics.

## Safety

Server responses, paths and credentials are untrusted. Future code must bound
listing/data sizes, reject path escape, avoid shell command construction,
validate TLS policy where used and keep credentials out of logs.

## Completion rule

The current entry is an external userspace-provider catalogue contract only.
