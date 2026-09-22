# Native Filesystem Vision and Long-Term Architecture

## Purpose

Filesystem Support is not intended to remain a graphical front end for a large collection of unrelated Debian packages.

The long-term goal is to implement the filesystem support ourselves and make it feel like a coherent part of the operating system.

For real on-disk filesystems, the preferred final form is a **native Linux VFS filesystem driver built as an out-of-tree loadable kernel module**, installed and managed by Filesystem Support. The user should not need a custom kernel, should not need to patch Debian's kernel source, and should not need a separate application for every filesystem.

The model is the same architectural idea already used by InfiltratorFS: install the driver suite, let Linux load the required filesystem module when needed, and expose the filesystem through the normal Linux mount/VFS path.

External Debian packages, existing kernel drivers and FUSE implementations are therefore primarily **compatibility and transition providers** while our native implementations are incomplete.

## Why this project exists

The project is driven by three related problems.

1. **FUSE is not the desired final architecture for ordinary disk filesystems.** It is useful as an interoperability mechanism and can remain a temporary fallback, but the end goal for conventional local filesystems is normal kernel/VFS integration.
2. **A multitude of unrelated utilities and implementations is not a coherent subsystem.** Users should not need to know which package, daemon, FUSE executable or helper belongs to which disk format.
3. **Kernel patching or maintaining a custom kernel is not acceptable as the normal installation model.** Our filesystem support must be installable against the running distribution kernel as external modules wherever technically possible.

The desired user experience is therefore:

```text
Install Filesystem Support once
        |
        +-- install native filesystem module suite
        +-- rebuild/sign modules for installed kernels when required
        +-- register module aliases and dependencies
        |
Insert / attach / mount storage
        |
Linux requests the filesystem type
        |
the relevant Infiltrator filesystem module loads
        |
normal Linux VFS mount
```

The filesystem should then behave as part of the system rather than as a separate application.

## Non-negotiable architectural principles

### 1. No custom kernel requirement

Filesystem Support must not require users to patch, rebuild or replace the Debian kernel merely to gain one of our filesystem drivers.

If a filesystem can be implemented as an out-of-tree module, that is the required deployment model.

A target that truly cannot be implemented without kernel changes must be identified explicitly rather than quietly turning Filesystem Support into a custom-kernel project.

### 2. Native VFS modules for real local filesystems

Conventional block-device and image filesystems should ultimately be implemented as native Linux filesystem modules.

Examples include:

- AFFS
- ADFS
- SGI EFS
- HFS and HFS+
- FAT and exFAT
- NTFS
- UFS
- Minix
- JFS
- XFS
- ext2/ext3/ext4
- F2FS
- ISO 9660
- UDF
- and the other genuine on-disk filesystem formats in the support matrix.

The final provider for these should not be a FUSE process merely because FUSE was easier during early development.

### 3. FUSE is transitional, not the destination

Existing FUSE implementations remain useful today because they give users working access before our implementations exist.

They may also be useful as reference interoperability targets and test oracles.

For ordinary local disk filesystems, however, the roadmap is:

```text
External/FUSE fallback
        ->
Native parser and validation
        ->
Native read-only VFS module
        ->
Native safe read/write VFS module
        ->
Feature-complete native provider
        ->
External fallback no longer required
```

Removing FUSE from a target is a milestone, not merely a packaging change.

### 4. One product, not 104 applications

There must not be an AFFS application, an EFS application, an HFS application, an NTFS application, and so on.

The product remains **Filesystem Support**.

The implementation may contain many independently loadable modules, but installation, updating, diagnostics, documentation and status belong to one coherent subsystem and one management application.

### 5. Separate modules, shared infrastructure

The final implementation should **not** be one enormous kernel module containing every filesystem.

A likely shape is:

```text
Filesystem Support
|
+-- userspace manager / installer / updater
|
+-- native filesystem module suite
    |
    +-- infiltratr-fs-core.ko
    +-- infiltratr-affs.ko
    +-- infiltratr-adfs.ko
    +-- infiltratr-efs.ko
    +-- infiltratr-hfs.ko
    +-- infiltratr-hfsplus.ko
    +-- infiltratr-fat.ko
    +-- infiltratr-exfat.ko
    +-- infiltratr-ntfs.ko
    +-- infiltratr-ufs.ko
    +-- infiltratr-minix.ko
    +-- ...
```

Each filesystem remains independently loadable so using one filesystem does not drag all other implementations into kernel memory.

Shared code belongs in a deliberately small kernel-side support layer only when it is genuinely common and safe to share.

Candidate shared facilities include:

- endian-safe decoding and encoding
- checked integer arithmetic
- bounded block and byte-range access
- block-cache helpers
- common checksum primitives
- Unicode and filename conversion helpers
- mount-option parsing
- common diagnostic and trace facilities
- corruption/structure validation helpers
- common read-only image/block-device abstraction where useful
- shared test-vector and fuzzing interfaces.

Filesystem-specific on-disk semantics must remain in the filesystem-specific driver.

### 6. Userspace Common is not automatically kernel Common

The existing Infiltratr Common library is a userspace/shared-project library.

Kernel code has different constraints and must not casually link or copy userspace abstractions into kernel space.

If multiple native filesystem modules need shared kernel code, that code should live in a specifically designed **kernel-safe filesystem core** with kernel-appropriate allocation, locking, error handling, APIs and testing.

Promotion into that core follows the same project rule used elsewhere: only promote behaviour that is genuinely common and at least as correct as the best individual implementation.

### 7. Clean implementation, not blind source copying

The purpose of owning these implementations is defeated if they become an unreviewed copy of dozens of third-party filesystem projects.

Development may use:

- published filesystem specifications
- standards
- format documentation
- independently created test images
- interoperability testing
- documented behaviour of other implementations
- source study where licensing permits and the licensing consequences are understood.

Code copied from another implementation must never be treated as consequence-free. Licensing must be intentional.

Where the project wants an independently owned implementation, the code should be implemented from the format/behaviour specification and validated against real media and independent implementations.

## Native module lifecycle

### Installation

Filesystem Support should own the lifecycle of its native modules.

The intended flow is:

```text
detect installed/running kernels
        |
verify matching kernel build headers
        |
build required module suite against each supported kernel
        |
run module tests/validation
        |
sign modules where signature enforcement requires it
        |
install under /lib/modules/<kernel>/...
        |
run depmod
        |
make modules available to normal Linux module loading
```

The user should not need to invoke the compiler or manually copy `.ko` files.

### Kernel updates

Linux internal kernel interfaces change over time, so out-of-tree drivers cannot be treated as a single binary that will work against every future kernel.

Filesystem Support must therefore treat kernel updates as a normal lifecycle event.

For each newly installed kernel:

1. detect that the new kernel exists;
2. determine whether the filesystem module suite already has a compatible build;
3. build the suite against that kernel's build interface;
4. run compile-time and automated driver tests;
5. sign the modules if required;
6. install the modules for that kernel;
7. run `depmod`;
8. report success or a clearly actionable failure.

A kernel API change is therefore an adaptation problem for our driver suite, not a reason to patch the distribution kernel.

### Secure Boot and module signing

Secure Boot must be treated as a first-class deployment case.

Filesystem Support should:

- detect whether module signature enforcement matters on the machine;
- maintain or use an appropriate signing identity;
- sign every built native filesystem module consistently;
- guide the user through trust/enrolment only when the platform requires it;
- verify the installed module signature before declaring deployment successful.

The user should not have to repeat an enrolment process for every filesystem module.

### Autoloading

Where normal Linux module aliasing can represent the filesystem cleanly, modules should be installed so that the system can request them automatically when the filesystem type is needed.

Manual **Load module** and **Unload module** controls remain valuable for diagnostics and administration, but they are not the desired everyday workflow.

The desired everyday workflow is simply:

```text
filesystem needed -> module requested -> module loaded -> VFS mount
```

## Safety model for native filesystem development

Filesystem readers and writers have very different risk profiles.

A buggy reader may crash, reject valid media or expose corrupt data.

A buggy writer may destroy irreplaceable media.

Native support must therefore advance through explicit maturity stages rather than jumping directly to read/write.

### Native implementation maturity states

Every real filesystem target should eventually carry a native-development state such as:

1. **External only** — current Debian/kernel/FUSE implementation is the only provider.
2. **Native planned** — format ownership and implementation plan are documented.
3. **Parser under development** — on-disk structures can be decoded in test code but are not a supported driver.
4. **Native read-only experimental** — loadable VFS module mounts controlled test images read-only.
5. **Native read-only validated** — broad corpus, corruption, fuzz and interoperability testing has passed.
6. **Native write experimental** — tightly controlled write support exists but is not the default.
7. **Native read/write validated** — write operations have durability, corruption and interoperability coverage.
8. **Native feature-complete** — supported format variants and intended tooling are complete.
9. **Native preferred** — Filesystem Support chooses our implementation ahead of external providers.

The application should eventually expose this state directly in the 104-target matrix and GUI.

## Testing requirements

A native filesystem module should not graduate merely because a sample disk mounts.

Each implementation should accumulate a corpus covering:

- empty filesystems
- smallest and largest valid structures
- nested directories
- long and unusual filenames
- boundary-size files
- sparse/extents where the format supports them
- timestamps and metadata variants
- allocation fragmentation
- alternate block/sector sizes
- endian variants where applicable
- dirty/unclean shutdown states
- intentionally malformed metadata
- truncated images
- checksum failures
- circular or invalid metadata references
- full filesystem conditions
- out-of-space writes
- interrupted writes
- mount/unmount repetition
- concurrent access where the filesystem permits it
- interoperability with original/native operating systems where practical.

Read-only implementations require fuzzing and corruption testing before being considered validated.

Write support additionally requires crash-consistency and destructive-test coverage using disposable images/devices.

No development filesystem driver should be tested first against valuable original media.

## Development order

The 104 entries are not equal in size or difficulty.

The project should deliberately harvest reusable infrastructure from simpler formats before tackling the largest filesystems.

A sensible early sequence is:

```text
ROMFS
-> ISO 9660
-> FAT12/16/32
-> Minix
-> CP/M tooling/native access model
-> AFFS
-> SGI EFS
-> ext2
-> HFS
-> UDF
-> exFAT
```

After the framework is proven, progress into more complex local filesystems such as:

```text
ext3/ext4
HFS+
UFS
JFS
NTFS
XFS
F2FS
```

The largest and most structurally complex systems should come later, after the shared block, validation, testing and recovery infrastructure is mature:

```text
Btrfs
APFS
ZFS
GFS2 / OCFS2
CephFS and other distributed systems
```

This order is a roadmap, not a permanent ranking. A filesystem may move earlier when a dependency, test corpus, specification or practical need makes it advantageous.

## The 104-entry catalogue is broader than 104 disk filesystems

The existing support matrix intentionally includes more than conventional on-disk filesystem formats.

It also contains:

- network filesystems
- distributed filesystems
- encrypted containers
- archive/image access layers
- overlays
- cloud storage mounts
- device namespaces such as MTP/PTP/AFC
- diagnostic virtual filesystems
- tools-only support.

The phrase **implement all 104 ourselves** therefore means:

> replace each external support path with an Infiltrator-owned implementation or subsystem wherever doing so is technically meaningful.

It does **not** mean forcing inappropriate functionality into a kernel filesystem module.

## What should remain userspace

High-level remote and device protocols often require facilities that should not be pulled into kernel space merely to avoid FUSE.

Examples include:

- cloud OAuth and rapidly changing web APIs;
- S3-style object-store APIs;
- WebDAV/HTTP application-layer behaviour;
- some MTP/PTP/AFC device stacks;
- archive/container manipulation;
- other protocols requiring large authentication, TLS or application-protocol stacks.

For these targets, the long-term replacement should be a **single coherent Infiltrator userspace storage service**, not dozens of unrelated FUSE applications.

That service can own:

- authentication
- TLS
- network retry/reconnect behaviour
- caching
- cloud/device protocol adapters
- credential integration
- updateable high-level protocol logic.

The key architectural rule remains the same: one coherent subsystem managed through Filesystem Support.

Do not move complex web/application protocol stacks into kernel space merely to satisfy an ideological "no userspace" target.

## Relationship to InfiltratorFS

InfiltratorFS remains its own filesystem with its own on-disk design and engineering priorities.

Filesystem Support may reuse proven generic engineering ideas from InfiltratorFS, and both projects may contribute genuinely generic improvements to shared infrastructure, but foreign-filesystem compatibility must not distort the InfiltratorFS on-disk design.

Likewise, InfiltratorFS-specific assumptions must not leak into compatibility drivers for FAT, AFFS, UFS, NTFS, XFS or other formats.

The architectural relationship is:

```text
                 shared engineering principles
                         /        \
                        /          \
             InfiltratorFS     Filesystem Support
                                  |
                           native compatibility
                           filesystem modules
```

Commonality is earned by identical requirements, not by proximity of repositories.

## Relationship to the current Debian provider manager

Version 0.3.0 represents the transition layer.

Today, Filesystem Support can:

- detect current kernel support;
- distinguish built-in drivers from loadable modules and missing drivers;
- install and safely remove Debian/FUSE/DKMS support packages;
- load and unload loadable modules;
- document all 104 support targets.

Those external providers remain useful until the corresponding native implementation reaches sufficient maturity.

As native implementations appear, the catalogue should grow an explicit native-provider field rather than deleting the external provider immediately.

During migration a target may therefore have both:

```text
Native provider: experimental read-only
External provider: available and currently preferred
```

Later:

```text
Native provider: validated read/write
External provider: optional fallback
Preferred provider: Infiltrator native
```

Finally, for a mature native implementation, the external package becomes unnecessary for ordinary operation.

## Definition of success

The long-term project succeeds when a Debian user can install Filesystem Support once and gain a coherent suite of filesystem capabilities without understanding the implementation history behind each format.

For a conventional local filesystem, success looks like this:

```text
attach media
-> Linux recognises/request filesystem type
-> our native module loads normally
-> filesystem mounts through Linux VFS
-> no FUSE process
-> no custom kernel
-> no separate filesystem application
-> no third-party filesystem package required
```

For high-level remote/device targets, success means the same coherent user experience through the shared Infiltrator storage service rather than a collection of unrelated tools.

That is the architectural destination for the entire 104-target programme.


## Source architecture

The concrete greenfield source layout and dependency boundaries are defined in
[`NATIVE_CODE_ARCHITECTURE.md`](NATIVE_CODE_ARCHITECTURE.md).

That document is authoritative for how Common, the platform-neutral native
engine, Linux VFS adapters, Defragmenter-derived engineering lessons and
per-filesystem implementations are separated in code.
