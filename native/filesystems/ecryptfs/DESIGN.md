# eCryptfs Design and Ownership

## Classification

eCryptfs is a stacked cryptographic filesystem. Its encrypted file/header/name
representation and cryptographic metadata are filesystem-defined semantics,
while the backing filesystem supplies physical allocation and durability.

Filesystem Support therefore treats eCryptfs as one canonical portable
filesystem/crypto engine with thin host integration rather than as Linux VFS
code.

## Current state

eCryptfs is currently **reference/import state**.

The source under `reference/linux/` is the pinned upstream Linux eCryptfs
implementation. It retains upstream licensing, provenance and source boundaries
and is not project-authored Filesystem Support code.

## Current directory contract

While eCryptfs remains in reference/import state, the filesystem root contains
only `DESIGN.md` and `reference/`, with the pinned Linux implementation below
`reference/linux/`. The future `core/`, `linux/` and `windows/` directories
appear only with independently authored implementation. `kernel/` and
`userspace/` are not valid alternate semantic trees.

CI enforces this present-state boundary and rejects accidental production
linkage of the imported reference source.

## Target layout

```text
native/filesystems/ecryptfs/
  DESIGN.md
  core/                 portable eCryptfs format/crypto semantics
  linux/                thin stacked-filesystem Linux adapter
  windows/              host adapter if a supported Windows mapping is viable
  reference/linux/      pinned upstream Linux evidence
```

## Canonical ownership

The future core may own eCryptfs-defined behaviour including encrypted-file
header/metadata interpretation, filename encryption representation, key/token
metadata formats, authenticated/validated encryption transforms supported by
the project, extent/page-to-lower-file mapping rules that are filesystem
semantics, and corruption/version validation.

Linux dentries/inodes, keyring APIs, page-cache objects, kernel threads,
misc-device messaging and lower-VFS calls are adapter/platform mechanisms and
must not define the portable filesystem model.

The backing filesystem remains responsible for its own on-disk allocation,
namespace durability and repair.

## Security boundary

Keys and plaintext are sensitive. A project implementation must define:

- supported cipher/KDF/signature/token formats explicitly;
- strict metadata length/version validation;
- no secret material in diagnostics/logging;
- bounded key lifetime and cleanup;
- authentication/integrity requirements before exposing plaintext where the
  format permits them;
- fail-closed behaviour for unavailable keys or unsupported crypto.

## Promotion sequence

1. document supported eCryptfs format/header/name variants;
2. implement bounded host-neutral metadata decoding;
3. validate against independent known-good encrypted files/directories;
4. implement read-only decryption through a userspace harness;
5. add a thin Linux stacked adapter over the same core;
6. add mutation only after key/write/durability behaviour is independently
   qualified;
7. consider other host adapters only if the same filesystem semantics map
   cleanly.

## Reference provenance

The refresh workflow pins Linux v6.12.107 and copies `fs/ecryptfs` into
`reference/linux/`. Refreshing that evidence must never overwrite future
project-owned `core/`, `linux/` or `windows/` code.
