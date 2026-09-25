# FileVault / FVDE Design and Ownership

## Classification

This catalogue entry represents Apple's FileVault encrypted-volume/container
formats exposed through libfvde tooling. It is an encryption/storage layer, not
a filesystem.

The current external provider is Debian's `libfvde-utils`, including
`fvdemount`.

## Current state

Filesystem Support contains no first-party FileVault/FVDE implementation. The
former `.gitkeep` represented no source and has been removed.

## Target architecture

A future implementation may own a portable encrypted-container core:

```text
native/filesystems/filevault/
  DESIGN.md
  core/                 portable supported FVDE metadata/crypto semantics
  userspace/            credential + decrypted-volume exposure
  reference/            optional evidence with explicit provenance
```

There is deliberately no filesystem `kernel/` implementation here.

The decrypted block/container contents are passed to the actual contained
filesystem implementation such as APFS or HFS+. FileVault code must not parse
or duplicate those filesystems.

## Security boundary

Any first-party implementation must explicitly define supported FileVault/FVDE
versions, metadata validation, key derivation/unwrapping and sector transforms.
Credentials and decrypted keys/plaintext must not be logged. Unsupported or
unauthenticated/tampered state must fail closed.

## Completion rule

The present entry is an external encrypted-volume provider contract only. It is
not evidence of a first-party APFS, HFS+ or FileVault implementation.
