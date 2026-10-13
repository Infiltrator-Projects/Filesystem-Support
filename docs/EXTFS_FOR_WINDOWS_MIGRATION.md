# ExtFS-for-Windows migration preservation ledger

## Purpose

This ledger records the engineering boundary used when the standalone ExtFS-for-Windows work was folded into Filesystem Support. It is provenance and migration evidence; it is not a second implementation authority and it must describe the repository as it exists now.

## Source baseline

Migration source:

- repository: `Infiltrator-Projects/ExtFS-for-Windows`;
- final reviewed main commit: `dda219be47033372217149bce8c8ee20f19ded0a`;
- tree: `76b127deaa8b5b4776774a9579ca6c791d9d6b38`;
- latest standalone release: `v0.9.9`, published 2026-09-22;
- Common pin at that baseline: `748e089ae175329471d4cf375522c44081371bd5`.

Migration destination evidence included Filesystem Support qualification commit `8e3e5bf6c299dece09641199004cc4c99ae98f25` and GitHub Actions build run `35806524701`.

## Responsibilities reviewed during migration

The standalone repository contained four materially different kinds of engineering work:

1. portable EXT2/EXT3/EXT4 parsing, validation and bounded mutation code;
2. Windows IFS lifecycle and request-dispatch code;
3. WDK/build/signing/installer machinery;
4. Windows diagnostics, Driver Verifier, host-readiness and filesystem-contract qualification.

Those responsibilities were deliberately not migrated as one opaque source tree. Filesystem semantics were reconciled against the canonical EXT family; Windows-specific mechanisms moved behind the Filesystem Support Windows boundary; obsolete standalone product/distribution mechanisms were retired.

## Durable decisions retained

The migration established or reinforced these contracts:

- portable filesystem semantics and Windows IFS plumbing are separate responsibilities;
- the canonical EXT engine is the only live filesystem-format authority;
- unsupported mutation layouts fail closed;
- durability barriers and recovery ordering are filesystem correctness requirements;
- Windows object, cache and lifecycle correctness require Windows-specific qualification;
- production Secure Boot support follows the normal Microsoft signing path rather than asking users to disable platform security;
- historical standalone installers do not remain as a second supported product or publication path.

## Classification rule

Every migrated responsibility ended in one of four states:

- **MIGRATED** — required functionality now lives in Filesystem Support;
- **MERGED** — useful semantics were reconciled into the canonical implementation rather than copied as a duplicate;
- **ARCHIVED IN HISTORY** — provenance remains recoverable from repository/release history but is not retained as live build input;
- **RETIRED** — deliberately obsolete, with the replacement boundary documented.

No live Filesystem Support build may depend on the old portable `core/extfs*.c` implementation or on an EXT-specific standalone installer.

## Current live destination

The live engineering ownership points are:

```text
native/filesystems/ext2/core/       canonical EXT2 semantics
native/filesystems/ext2/windows/    Windows EXT2 adapter/driver
native/platform/                    host-neutral and platform mechanisms
windows/build/                      WDK build/package validation
windows/test/                       Windows qualification
windows/manager/                    Windows product surface
docs/windows/                       Windows lifecycle/qualification contracts
```

EXT3 and EXT4 retain their own canonical format boundaries and provenance ledgers. Similarity between EXT generations does not permit one driver's persistent semantics to become another driver's implementation by aliasing or compatibility registration.

## Migration closure

The standalone migration gate was closed on 2026-09-23. At closure, the standalone source tree, release metadata and issue/PR history were compared against the material carried into or intentionally retired by Filesystem Support. The Windows EXT2 path compiled the Filesystem Support canonical EXT2 core directly, the temporary compatibility facade was gone, and the standalone repository contained no unique live engineering dependency required to continue the product.

The present repository does **not** retain a byte-for-byte copy of the old standalone repository as a live `archive/` directory. Earlier migration notes described such a preservation copy at the time of closure; subsequent repository cleanup removed that redundant live-tree payload. Current provenance is therefore this ledger plus normal Git/release history, not an executable or buildable duplicate source tree inside `main`.

That distinction is intentional: deleting a migration payload after its unique engineering responsibilities have been classified is different from silently losing an implementation dependency. The source-boundary tests prohibit the old ExtFS core from returning as a Windows build dependency.

## Historical binary disposition

Standalone ExtFS-for-Windows release binaries are **RETIRED**. They were experimental/test-signed installers for the superseded standalone product and are not Filesystem Support release artefacts.

The supported release path is the Filesystem Support release pipeline. Reintroducing an old standalone installer, copied EXT implementation or separate release channel would create a second authority and requires an explicit architecture decision rather than an incidental restoration of migration material.

## Continuing status

Migration closure did not mean EXT2 or Windows filesystem work was complete. It established ownership: further EXT2 correctness work occurs in the canonical EXT2 core and its platform adapters, and Windows installability remains gated by the current Windows qualification contract. The old standalone repository is not a compatibility fallback.
