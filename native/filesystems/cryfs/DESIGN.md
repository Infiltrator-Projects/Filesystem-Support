# CryFS Design and Ownership

## Classification

CryFS is an encrypted userspace overlay filesystem designed for storing
encrypted content in an ordinary backing directory, including cloud-backed
storage. It is not a conventional block-device filesystem.

The current catalogue delegates this capability to Debian's `cryfs` package.

## Current state

There is no first-party CryFS implementation in Filesystem Support. The former
`.gitkeep` has been removed.

## Target architecture

If a first-party encrypted-overlay implementation is ever admitted, the correct
shape is:

```text
native/filesystems/cryfs/
  DESIGN.md
  core/                 portable encrypted-overlay semantics, if implemented
  userspace/            mount/provider integration
```

There is deliberately no `kernel/` directory. The encrypted object layout,
name mapping and authentication rules may justify a portable core, while host
mounting remains userspace service work.

## Ownership

A future core may own only CryFS-compatible encrypted-container/overlay
semantics: encrypted object naming/layout, authenticated metadata/data
transforms, directory/object mapping and deterministic failure behaviour.

Backing filesystem allocation, directory implementation and recovery remain
owned by the backing filesystem.

## Security

Keys, credentials and decrypted data are security-sensitive. Any first-party
implementation must avoid logging secrets, authenticate data before exposing
plaintext, reject tampered metadata, define key-derivation/version handling and
fail closed on unsupported formats.

## Completion rule

The present entry is an external userspace-provider contract only. It is not a
project-authored encrypted filesystem implementation.
