# EncFS Design and Ownership

## Classification

EncFS is an encrypted userspace/FUSE overlay that stores encrypted files and
metadata in an ordinary backing directory. It is not a block-device
filesystem.

The current catalogue delegates this capability to Debian's `encfs` package.

## Current state

Filesystem Support contains no first-party EncFS implementation. The former
`.gitkeep` has been removed.

## Target architecture

If EncFS-compatible functionality is admitted later:

```text
native/filesystems/encfs/
  DESIGN.md
  core/                 portable EncFS encrypted-overlay semantics
  userspace/            provider/mount integration
```

There is deliberately no `kernel/` directory.

## Ownership

The portable core may own only EncFS-defined configuration/version handling,
filename transformation, per-file encryption/decryption and authenticated or
integrity behaviour actually defined by the supported EncFS format.

The backing filesystem remains responsible for allocation, directory storage,
durability and recovery.

## Security

Keys and plaintext must not be logged. Configuration and ciphertext metadata
are untrusted input. Unsupported format/cipher/KDF variants must fail closed,
and path/name decoding must not escape the mounted namespace.

## Completion rule

The current entry is an external userspace-provider catalogue contract only.
A first-party EncFS implementation is not claimed until its format support,
crypto policy, tests and userspace integration are independently complete.
