# Native Code Architecture

## Architectural rule

Filesystem Support owns one canonical implementation of each filesystem.

Filesystem-specific semantics are not implemented once for Linux and again for
Windows. Linux and Windows are operating-system adapters around the same
filesystem engine.

The detailed Windows migration decision and the ExtFS preservation/deletion
gates are recorded in
[`WINDOWS_FILESYSTEM_ARCHITECTURE.md`](WINDOWS_FILESYSTEM_ARCHITECTURE.md) and
[`EXTFS_FOR_WINDOWS_MIGRATION.md`](EXTFS_FOR_WINDOWS_MIGRATION.md).

## Dependency direction

```text
                         filesystem catalogue
                                |
                                v
                    canonical filesystem engine
                       /                 \
                      /                   \
             Linux platform adapter   Windows platform adapter
                    |                         |
               Linux VFS/KO            Windows IFS/WDK
```

Userspace inspection and test harnesses call the same canonical filesystem
engine through host-neutral I/O contracts.

Infiltratr Common may supply genuinely generic userspace/portable primitives,
but neither filesystem semantics nor kernel adapters may depend on GUI,
package-manager or unrelated application code.

## What is canonical filesystem code?

The canonical per-filesystem engine owns facts dictated by the filesystem
format itself, including:

- on-disk structures, feature bits and compatibility rules;
- superblock, inode and directory interpretation;
- block mapping and allocation policy;
- extent or indirect-tree algorithms;
- journal record formats and filesystem recovery rules;
- metadata checksums;
- extended attributes and filesystem-specific metadata;
- corruption and range validation;
- filesystem-defined ordering and crash-consistency rules.

If Linux and Windows need the same rule, that rule belongs here.

## What belongs in an OS adapter?

An OS adapter translates native operating-system contracts into the canonical
engine. It must not become a second filesystem implementation.

The Linux adapter owns Linux-specific concerns such as:

- VFS registration and mount lifecycle;
- `struct inode`, `struct file`, folios/page cache and block-device APIs;
- Linux locking and work/lifetime rules;
- module registration, aliases and kernel error translation.

The Windows adapter owns Windows-specific concerns such as:

- `DriverEntry` and filesystem registration;
- IRP dispatch and Windows create/read/write/query/set semantics;
- VCB/FCB/CCB lifetime and synchronization;
- Cache Manager and Memory Manager integration;
- volume verification, lock/dismount and removable-media lifecycle;
- NTSTATUS translation;
- WDK packaging, INF/catalogue production and signing.

Format knowledge must not be hidden inside either adapter merely because the
first implementation happened to be written there.

## Shared engine contract

The lowest shared layer is explicit and host-neutral.

`IfsIo` provides bounded positioned I/O. A filesystem parser must not know
whether the bytes come from an image, a Linux block device, a Windows volume,
a fuzz buffer or a qualification harness.

`IfsByteReader` provides bounds-checked decoding over already-read records.

`IfsFilesystemDescriptor` is the small ABI-bearing identity/capability
contract for a native engine. It contains no GTK, package-manager, Linux VFS or
Windows WDK object pointers.

As write support grows, the host contract may expose only the mechanisms that
are truly required by filesystem algorithms, for example durable flush/barrier
operations, bounded scratch storage, time and carefully defined synchronization
hooks. OS objects remain outside the canonical engine.

## Source layout direction

The target layout is:

```text
native/
  core/                              filesystem-neutral mechanisms
  filesystems/
    ext2/
      core/                          canonical EXT2 semantics
      linux/                         EXT2-specific Linux bridge, if required
      windows/                       EXT2-specific Windows bridge, if required
    ext3/
      core/
      linux/
      windows/
    ...
  platform/
    linux/                           reusable Linux adapter/framework
    windows/                         reusable Windows adapter/framework
    userspace/                       image/device inspection adapter

windows/
  manager/                           Windows Filesystem Support application
  build/                             WDK/build/sign/package orchestration
  installer/                         product/module installation
  test/                              Windows-specific qualification
```

Not every filesystem needs format-specific files under both adapter
directories. Prefer reusable platform infrastructure; create filesystem-specific
adapter glue only where the OS contract genuinely needs it.

## Common policy

Infiltratr Common remains authoritative for genuinely generic mechanisms that
are safe in the relevant build environment.

Reuse Common for identical contracts such as checked arithmetic, deterministic
ASCII/UTF-8 handling, exact userspace I/O and parsing helpers when doing so does
not introduce an invalid kernel dependency.

If a filesystem engine develops a stronger generic primitive, compare it
forensically with Common. Improve the generic implementation when appropriate,
then remove the duplicate. Do not create families of near-equivalent helpers.

Kernel builds must use only code proven safe for their kernel environment.
Userspace Common is not automatically kernel-safe.

## Imported Linux source

The current `native/filesystems/*/kernel` trees are valuable, mature semantic
references and, where licensing and build constraints permit, implementation
sources. They are not the final cross-platform boundary by themselves because
Linux VFS types and helpers are embedded throughout them.

For a filesystem promoted to the canonical cross-platform architecture, split
format semantics from Linux integration deliberately. Do not mechanically
translate Linux APIs into fake portability wrappers or maintain a second
Windows rewrite.

The strongest implementation wins at the semantic level regardless of whether
a particular check or algorithm originated in the imported Linux code, the
former ExtFS portable core, a specification or later project work.

## ExtFS-for-Windows migration rule

The former ExtFS portable core is migration evidence, not a second production
engine.

Its host-neutral checked geometry, malformed-media rejection, traversal logic,
durability discipline, tests and Windows IFS work must be reconciled into the
appropriate canonical or Windows-platform layers. The frozen v0.9.9 snapshot
under `archive/` is never linked into production builds.

A duplicate ExtFS algorithm may be removed only after the canonical
replacement has equivalent or stronger tests and both platform paths consume
that replacement.

## Shared kernel/platform core policy

Do not create one enormous Linux module or one enormous Windows driver merely
because Filesystem Support covers a large catalogue.

Filesystem modules should remain independently deployable where the operating
system benefits from that isolation. Shared platform infrastructure is
acceptable when its interface has been proven stable and when sharing it does
not create a single failure/version-skew point for every filesystem.

Source sharing and binary packaging are separate decisions: the same canonical
filesystem source can be compiled into more than one OS-specific module without
duplicating the source implementation.

## Development and qualification harnesses

`fsinspect` is the generic userspace inspection harness. It should call
canonical engines rather than spawn one utility per filesystem.

Qualification must include independently manufactured fixtures, malformed
input, sanitizer/static-analysis passes where applicable, cross-implementation
differential checks and destructive disposable-media tests for writers.

Mutation success requires an independent read-only verification pass. Durable
write transactions need explicit recovery boundaries.

## First implementation: EXT2

EXT2 is the architectural proof.

It must demonstrate:

1. one canonical EXT2 format/semantic implementation;
2. host-neutral parsing and malformed-media validation;
3. complete intended EXT2 block mapping and metadata behaviour;
4. Linux VFS integration through the Linux adapter;
5. Windows IFS integration through the Windows adapter;
6. the same canonical tests running independently of either kernel;
7. Linux qualification;
8. Windows WDK/static/runtime qualification;
9. no surviving independently maintained EXT2 algorithm in the Windows layer.

EXT3 and EXT4 follow after this boundary is proven. Other filesystems can then
adopt the same architecture without rediscovering the platform split.

If EXT2 exposes a bad abstraction, fix the abstraction rather than preserving
it for compatibility. The cross-platform native-engine API is still free to
improve before a stable release contract is declared.
