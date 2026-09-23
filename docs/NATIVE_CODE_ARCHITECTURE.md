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

## One filesystem implementation, two OS wrappers

For each filesystem there is exactly **one filesystem implementation**.

`core/` is not merely a parser or a collection of helpers. It is the
filesystem: format interpretation, allocation, mapping, directory semantics,
metadata semantics, journaling/recovery rules, mutation logic and other
filesystem-defined behaviour belong there whenever they can be expressed
without an operating-system object.

The `linux/` and `windows/` directories are wrappers/adapters around that
same implementation. They must remain as thin as the host APIs allow.

A target shape is therefore:

```text
                    ext2/core/
              canonical EXT2 filesystem
                    /          \
                   /            \
          ext2/linux/          ext2/windows/
          thin VFS/KO          thin IFS/WDK
          adapter              adapter
```

There is no separately maintained "Linux EXT2" and "Windows EXT2". If the same
filesystem rule would otherwise be implemented in both wrappers, that rule
belongs in `core/`.

The same rule applies independently to EXT3, EXT4 and every filesystem promoted
from reference/import state into rewrite state.

During migration, substantial implementation may temporarily remain under
`linux/` because that is where the working reference implementation currently
lives. That is a migration condition, not the intended ownership boundary. The
direction of travel is to move/rewrite filesystem semantics into `core/` and
leave only unavoidable OS integration in the wrapper.

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

## Migration layout is not the target layout

The EXT rewrite may temporarily retain Linux-derived translation-unit names
such as `linux/file.c`, `linux/inode.c`, `linux/super.c` and `linux/dir.c`.
The old per-filesystem `kernel/` staging directories have been retired; these
remaining file boundaries are migration scaffolding, not architectural
requirements.

Keeping an inherited translation-unit boundary can be useful while replacing a
subsystem because it limits the amount of behaviour changed at one time and
makes qualification easier.  Once a subsystem has been independently rewritten,
its permanent location and file boundary must be chosen according to the
Filesystem Support architecture rather than according to the source layout that
was used as the reference implementation.

The target distinction is responsibility-based:

- filesystem-format semantics belong in the per-filesystem `core/`;
- Linux VFS, block-device, page-cache, module and kernel-lifetime glue belongs
  in the per-filesystem `linux/` adapter or reusable Linux platform layer;
- Windows IFS/WDK, IRP, cache-manager and driver-lifetime glue belongs in the
  per-filesystem `windows/` adapter or reusable Windows platform layer.

A source file may be split during migration when it mixes these responsibilities.
For example, a migration-era `linux/file.c` may contain both filesystem I/O
semantics and Linux VFS dispatch.  The final implementation should place shared
filesystem behaviour in `core/` and retain only Linux-specific dispatch in
`linux/`.

Conversely, do not split code merely to imitate this example tree.  A rewritten
subsystem may remain one cohesive source file when that produces the clearest
ownership and invariants.

The name, number and boundaries of source files are implementation decisions.
The stable architectural contracts are the canonical filesystem engine, the
OS-adapter boundary, and the independently deployable filesystem module.  For
Linux, a filesystem may still build exactly one `.ko` regardless of how many
source files implement its core and adapter.

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

## Source provenance and promotion model

Filesystem Support deliberately uses two source states.

### Reference/import state

Filesystems that have not yet entered an Infiltrator rewrite may use copied
upstream implementation source as their working baseline.  Those trees exist so
the project has a complete, working semantic reference while each filesystem is
waiting its turn for independent redesign.

Copied trees must retain their original licence, copyright and provenance
notices.  They must not be represented as project-authored source.

At the current development stage this reference/import state applies to filesystem implementations under `native/filesystems/` that have not been explicitly promoted. EXT2/3/4 and the five Amiga targets documented in `AMIGA_FILESYSTEMS.md` are excluded.

### Rewrite state

When a filesystem is selected for active Infiltrator development, its imported
implementation becomes temporary migration material.  The implementation is
then replaced subsystem by subsystem with code designed for the project's
canonical engine and OS-adapter architecture.

EXT2, EXT3 and EXT4 are currently in this rewrite state.

OFS, FFS, SFS, SFS2 and PFS3 are also in rewrite state. Their source bases,
provenance and five-way separation are defined in
[`AMIGA_FILESYSTEMS.md`](AMIGA_FILESYSTEMS.md). OFS and FFS begin from the
combined Linux AFFS implementation but are independent canonical filesystems;
SFS2 is likewise not an SFS compatibility mode.

For a rewritten unit, changing comments, names, file boundaries or Kbuild
layout is not sufficient.  The implementation itself must be replaced and
validated before inherited provenance can be removed from that unit.

Rewrite-state trees must not be refreshed automatically from upstream, because doing so could overwrite migration or project-authored work. Other reference trees may continue to be refreshed until that filesystem is explicitly promoted to rewrite state.

### Promotion rule

Promotion is deliberate and per filesystem:

1. preserve the copied reference tree and its legal provenance until the
   rewrite starts;
2. record the filesystem as being in rewrite state;
3. define the required format semantics and compatibility behaviour;
4. replace implementation units with project-authored code;
5. validate media compatibility, failure handling and platform integration;
6. remove inherited provenance only from units whose inherited implementation
   has actually been replaced; and
7. stop upstream refreshes for that filesystem once rewrite work has begun.

This gives the project working implementations now without confusing copied
reference source with the final Infiltrator codebase.

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
