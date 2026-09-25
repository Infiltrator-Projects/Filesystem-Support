# gocryptfs Design and Ownership

## Classification

gocryptfs is an encrypted userspace overlay filesystem. It stores encrypted
files and metadata in an ordinary backing filesystem and exposes a decrypted
namespace through FUSE.

The current catalogue delegates this capability to Debian's `gocryptfs`
package.

## Current state

Filesystem Support contains no first-party gocryptfs implementation. The former
`.gitkeep` has been removed.

## Target architecture

A future compatible implementation may legitimately contain a portable
encrypted-overlay core:

```text
native/filesystems/gocryptfs/
  DESIGN.md
  core/                 portable encrypted-overlay/name/data semantics
  userspace/            mount/provider integration
```

There is deliberately no kernel filesystem module here. The backing
filesystem's format/allocation/recovery semantics remain with that filesystem.

## Security

Any first-party implementation must explicitly define supported format
versions, key derivation, authenticated encryption, filename transformation and
tamper handling. Keys/plaintext must not be logged; unauthenticated or
unsupported state fails closed.

## Completion rule

The present entry is an external userspace-provider catalogue contract only.
