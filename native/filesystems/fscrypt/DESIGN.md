# fscrypt Ownership and Integration

## Classification

fscrypt is not a filesystem. It is the Linux filesystem-encryption policy/API
family plus user-facing management tooling used by filesystems that implement
fscrypt support, including EXT4, F2FS and UBIFS.

The catalogue currently exposes Debian's `fscrypt` tooling as a tools-only
capability.

## Current state

Filesystem Support contains no first-party fscrypt management implementation.
The former `.gitkeep` represented no code and has been removed.

## Ownership rule

Filesystem-specific on-disk encryption policy metadata and integration remain
with the owning canonical filesystem engine and its platform adapter.

For example:

- EXT4 encryption feature interpretation belongs to EXT4;
- F2FS encryption feature interpretation belongs to F2FS;
- UBIFS encryption feature interpretation belongs to UBIFS.

Generic reusable encryption-policy vocabulary or key-management mechanisms may
live in shared Filesystem Support platform infrastructure only when their
contract is genuinely filesystem-neutral.

## Intended shape

```text
native/filesystems/fscrypt/
  DESIGN.md
```

There is deliberately no `core/` filesystem engine and no `kernel/`
filesystem module here.

If project-owned fscrypt management tooling is later admitted, it should live
under a general tool/service boundary rather than pretending to be a
filesystem.

## Security

Key identifiers, credentials and raw key material require explicit lifecycle
and logging policy. Filesystem Support must not imply that installing the
`fscrypt` command makes an unsupported filesystem encryption-capable.

## Completion rule

This entry is complete as a tools/provider catalogue contract only. Encryption
feature completeness is evaluated in each owning filesystem.
