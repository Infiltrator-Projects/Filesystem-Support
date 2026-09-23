# Native Filesystem Vision and Long-Term Architecture

## Purpose

Filesystem Support is one cross-platform product for discovering, installing
and ultimately providing a coherent catalogue of filesystem and storage
support.

The long-term goal is not to maintain a Linux implementation and a Windows
implementation of each disk format. For every real filesystem, Filesystem
Support owns **one canonical filesystem implementation** and exposes it through
operating-system adapters.

For conventional local filesystems:

```text
                      canonical filesystem engine
                         /                 \
                        /                   \
               Linux adapter            Windows adapter
                    |                         |
                 Linux VFS                Windows IFS
```

Linux should receive normal native VFS integration without requiring a custom
kernel. Windows should receive normal native filesystem-driver integration
through the WDK/IFS stack.

External kernel drivers, Debian packages, FUSE implementations and the former
standalone ExtFS-for-Windows implementation are transition/reference providers
while the canonical engines are incomplete.

## Why this project exists

The project addresses four related problems.

1. FUSE is useful interoperability infrastructure but is not the desired final
   architecture for ordinary local disk filesystems.
2. A collection of unrelated packages and utilities is not a coherent storage
   subsystem.
3. Maintaining separate filesystem algorithms per operating system creates
   duplicated correctness work and semantic drift.
4. Gaining filesystem support should not require replacing the stock Linux
   kernel or weakening the normal Windows driver trust model.

## Non-negotiable architectural principles

### 1. One filesystem implementation

Filesystem semantics are written once.

On-disk structures, mapping rules, allocation, extent handling, journaling,
checksums, directory rules, validation and recovery belong to the canonical
filesystem engine.

Linux VFS code and Windows IFS code adapt their operating systems to that
engine. They do not fork those semantics.

### 2. Native integration on each operating system

For conventional block-device and image filesystems, the intended endpoints
are:

- a normal out-of-tree Linux VFS module where the format is appropriate for
  kernel integration; and
- a normal native Windows filesystem driver built and qualified with the WDK.

The platform adapters may be completely different because Linux and Windows
have different kernel contracts. The filesystem implementation beneath them
must remain shared.

### 3. No custom Linux kernel requirement

Filesystem Support must not require users to patch, rebuild or replace their
distribution kernel merely to gain one of our filesystem drivers.

A target that can be delivered as an out-of-tree module should be delivered
that way.

### 4. Do not weaken Windows security to make deployment convenient

Production Windows filesystem drivers must follow the supported Microsoft
driver signing and Secure Boot path.

Test-signing remains useful for disposable development machines, but disabling
Secure Boot or enabling TESTSIGNING is not the intended production experience.

### 5. One product, not a separate application per filesystem

There must not be independent AFFS, EFS, EXT, HFS, NTFS and XFS applications.

The product remains **Filesystem Support**.

The implementation may contain many independently deployable modules, but
installation, updating, diagnostics, capability state and documentation belong
to the same subsystem and management experience.

### 6. Separate modules, shared infrastructure

Do not turn the catalogue into one giant kernel binary.

Where practical, each conventional filesystem remains independently loadable.
Shared platform infrastructure is promoted only when genuinely common and
stable.

Candidate shared facilities include bounded I/O, checked arithmetic, checksum
primitives, Unicode/name conversion, corruption diagnostics, mount-option
parsing, test-vector interfaces and common platform lifecycle helpers.

Filesystem-specific semantics remain with their filesystem.

### 7. FUSE and external providers are transitional for ordinary disk formats

Existing FUSE/kernel/package providers remain useful while our implementation
is incomplete and as interoperability/test oracles.

For a conventional local disk filesystem, the lifecycle is:

```text
external/reference provider
        ->
canonical parser and validation
        ->
native read-only adapters
        ->
safe native write support
        ->
feature-complete canonical engine
        ->
qualified Linux and Windows providers
        ->
external provider becomes optional
```

### 8. Not every catalogue entry is a disk filesystem

The management catalogue also contains network/distributed filesystems,
encrypted containers, overlays, archive/image access, cloud mounts, device
namespaces and tools-only entries.

“Own the whole catalogue” does not mean forcing OAuth, web APIs, MTP or archive
engines into kernel filesystem modules.

High-level remote/device protocols should use a coherent Filesystem Support
userspace service where that is the technically correct boundary.

## Product experience

On Linux, the eventual ordinary local-filesystem experience is:

```text
install Filesystem Support
        ->
install/build/sign qualified native modules
        ->
attach media
        ->
Linux requests filesystem
        ->
matching adapter/module loads
        ->
normal VFS mount
```

On Windows:

```text
install Filesystem Support
        ->
manager shows the common filesystem catalogue
        ->
select a qualified Windows filesystem module
        ->
signed driver package installs
        ->
attach media
        ->
Windows mounts through the normal filesystem stack
```

A filesystem that is not yet qualified for Windows can still appear in the
catalogue, but its installation action remains disabled. The interface must
never imply that unfinished support exists.

## Canonical engine and adapters

The concrete source/dependency rules are defined in
[`NATIVE_CODE_ARCHITECTURE.md`](NATIVE_CODE_ARCHITECTURE.md).

The Windows-specific destination and migration gates are defined in
[`WINDOWS_FILESYSTEM_ARCHITECTURE.md`](WINDOWS_FILESYSTEM_ARCHITECTURE.md).

The rule is simple:

> Filesystem Support owns what a filesystem means. The platform adapter owns
> how that filesystem participates in a particular operating system.

## Linux module lifecycle

Linux internal kernel interfaces change over time, so out-of-tree adapters must
be rebuilt and qualified for supported kernel versions.

Filesystem Support should detect installed/running kernels, validate matching
build headers, build the required modules, run qualification, sign modules when
required, install them under the correct kernel module tree and run `depmod`.

Secure Boot/module signature enforcement must be handled as a normal deployment
case rather than an exceptional manual procedure.

## Windows driver lifecycle

Windows filesystem support must have an equivalent first-class lifecycle:

1. build the platform adapter and selected canonical filesystem module for the
   supported architecture;
2. run compiler warnings-as-errors, static analysis and filesystem contract
   tests;
3. validate INF/package structure and PE architecture;
4. produce the driver catalogue from the exact staged binaries;
5. sign through the appropriate development or production trust path;
6. install through the Filesystem Support manager/installer;
7. verify service/driver registration and load state;
8. collect useful diagnostics on failure;
9. support clean removal/update semantics.

The mature ExtFS-for-Windows WDK, installer, signing and diagnostic work is
being migrated and generalized for this purpose rather than discarded.

## Native implementation maturity

Every target should expose an implementation state per platform. A practical
model is:

1. **Not implemented** — no canonical provider yet.
2. **Engine under development** — canonical parser/semantics are being built.
3. **Read-only experimental** — controlled native mounts work.
4. **Read-only validated** — broad corpus/corruption/interoperability testing
   has passed.
5. **Write experimental** — tightly controlled mutation exists.
6. **Read/write validated** — durability, recovery and destructive testing
   have passed.
7. **Feature-complete** — intended variants/features are implemented.
8. **Preferred** — Filesystem Support chooses the canonical provider by
   default.

Linux and Windows may be at different qualification states while still using
the same canonical filesystem semantics.

## Testing requirements

A native filesystem does not graduate merely because a sample volume mounts.

The corpus should cover, where meaningful:

- empty/minimal/maximal valid structures;
- nested directories and difficult names;
- boundary-size, sparse and fragmented files;
- alternate block/sector sizes;
- metadata/checksum variants;
- dirty/unclean states and recovery;
- intentionally malformed and truncated images;
- invalid/circular metadata references;
- full/out-of-space conditions;
- interrupted writes and crash boundaries;
- repeated mount/unmount;
- concurrency where permitted;
- interoperability with independent/native implementations.

Read paths require fuzz/corruption testing.

Write support additionally requires disposable destructive testing,
crash-consistency testing and an independent post-write verifier.

Platform qualification is separate from filesystem-semantic qualification:
Linux VFS correctness does not prove Windows IRP/FCB/cache correctness, and the
reverse is also true.

## Development order

**EXT2 is first.**

It is the proving filesystem for the canonical-engine plus dual-adapter model.
The existing Linux EXT2 source contains substantially more complete filesystem
semantics, while the former ExtFS portable core contributes host-neutral
validation, defensive checked geometry, tests and Windows integration
experience. Those inputs are to be reconciled into one implementation rather
than retained as competitors.

The immediate sequence is:

```text
EXT2 canonical engine
        ->
Linux adapter consumes it
        ->
Windows adapter consumes it
        ->
cross-platform qualification
        ->
EXT3
        ->
EXT4
```

After EXT2 proves the boundary, the rest of the catalogue can be scheduled by
complexity, practical need and available specifications/test corpora. The old
ROMFS-first roadmap is superseded.

## Relationship to InfiltratorFS

InfiltratorFS remains its own filesystem with its own on-disk design.

Filesystem Support can reuse proven generic engineering mechanisms and both
projects can improve genuinely common infrastructure, but compatibility
filesystem requirements must not distort the InfiltratorFS format, and
InfiltratorFS-specific assumptions must not leak into foreign-filesystem
engines.

Commonality is earned by identical requirements.

## Relationship to the current Linux package manager

The current Debian/FUSE/DKMS manager remains a transition layer and useful
fallback provider.

As canonical engines mature, catalogue state should distinguish:

```text
Canonical engine: validated
Linux native adapter: validated
Windows native adapter: in progress
External Linux provider: available fallback
```

This is more accurate than deleting an external provider as soon as development
starts.

## Relationship to the former ExtFS-for-Windows repository

ExtFS-for-Windows is being retired as a standalone implementation because
maintaining a second EXT2/3/4 engine conflicts with the one-engine rule.

Its final v0.9.9 source is preserved byte-for-byte under
`archive/extfs-for-windows-v0.9.9/`, and its architecture, tests, Windows
driver lifecycle, WDK packaging/signing knowledge and repository history are
being migrated into Filesystem Support.

The archived code is evidence/reference only and is excluded from production
builds.

The standalone repository must not be deleted until the explicit deletion
gates in
[`EXTFS_FOR_WINDOWS_MIGRATION.md`](EXTFS_FOR_WINDOWS_MIGRATION.md) have been
verified against the final repository state.

## Definition of success

For a conventional local filesystem, success means:

```text
                 one canonical filesystem engine
                      /               \
                     /                 \
            qualified Linux          qualified Windows
               adapter                  adapter
                 |                         |
          normal Linux VFS          normal Windows IFS
```

A filesystem bug is fixed once in the canonical engine and both operating
systems receive that semantic fix.

That is the architectural destination for the filesystem catalogue.
